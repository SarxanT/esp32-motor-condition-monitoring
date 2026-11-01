"""IoT predictive maintenance demo gateway: MQTT -> ThingSpeak + ThingSpeak TalkBack -> MQTT.

Start ONCE and leave running alongside the two Wokwi simulations.
No separate command_sender.py or manual sensor upload is required.
Only SIMULATED cloud-controlled RGB LED commands are supported; never motor actuation.
"""
import csv
import json
import math
import os
import threading
import time
from datetime import datetime, timezone

import paho.mqtt.client as mqtt
import requests

# --------- 1. MQTT topics (must match the two ESP32 sketches) ---------
ROOT = 'motor-lab/CHANGE_TO_A_UNIQUE_NONPERSONAL_ID'
BROKER = 'broker.emqx.io'
DATA_TOPICS = {1: f'{ROOT}/node1/data', 2: f'{ROOT}/node2/data'}
ACK_TOPICS = {1: f'{ROOT}/node1/ack', 2: f'{ROOT}/node2/ack'}
COMMAND_TOPICS = {1: f'{ROOT}/node1/cmd', 2: f'{ROOT}/node2/cmd'}
BOOT_TOPICS = {1: f'{ROOT}/node1/boot', 2: f'{ROOT}/node2/boot'}

# --------- 2. Cloud configuration -------------------------------------
# One-time setup: copy config.example.json to config.json and enter keys.
# config.json stays on YOUR computer; never upload it to public projects.
CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'config.json')
try:
    with open(CONFIG_PATH, encoding='utf-8') as handle:
        CONFIG = json.load(handle)
except FileNotFoundError:
    CONFIG = {}

def setting(name, default=''):
    """Read an environment override or a local configuration value."""
    return str(os.getenv(name) or CONFIG.get(name, default)).strip()

WRITE_KEY = setting('TS_WRITE_KEY')
TALKBACK = {
    1: {'id': setting('TB_NODE1_ID', '0'), 'key': setting('TB_NODE1_KEY')},
    2: {'id': setting('TB_NODE2_ID', '0'), 'key': setting('TB_NODE2_KEY')},
}
UPLOAD_EVERY = 15.5       # Free ThingSpeak: never start updates closer than 15.5s
TALKBACK_EVERY = 2.0     # Cloud command polling cadence
FRESH_FOR = 6.0          # A node must have sent data recently
MAX_PAIR_SKEW = 3.5      # Maximum gateway-arrival-time difference between nodes
ALLOWED = {'LED_GREEN', 'LED_YELLOW', 'LED_RED', 'LED_BLUE'}

# --------- 3. Thread-safe latest readings -----------------------------
lock = threading.Lock()
latest = {1: None, 2: None}
last_seen = {1: 0.0, 2: 0.0}
sequence = {1: 0, 2: 0}      # Increment on each valid MQTT message
uploaded_sequence = {1: 0, 2: 0} # Snapshot last successfully uploaded
connected = threading.Event()
pending_ack = {1: None, 2: None}  # Track the one active cloud command per node.
ACK_TIMEOUT = 12.0              # Log a missing ESP32 acknowledgment.
NODE_COMMAND_FRESH_FOR = 10.0   # Do not consume a TalkBack command while node offline.
upload_session = requests.Session()
talkback_session = requests.Session()
# The latest SUCCESSFULLY UPLOADED ThingSpeak condition is the authority.
# A command is applied only if it matches this cloud-published condition.
STATUS_COMMANDS = {0: 'LED_GREEN', 1: 'LED_YELLOW', 2: 'LED_RED'}
desired = {1: None, 2: None}
desired_since = {1: 0.0, 2: 0.0}
last_applied = {1: None, 2: None}
fallback_queued_for = {1: None, 2: None}
last_fallback_time = {1: 0.0, 2: 0.0}
FALLBACK_GRACE = 8.0  # Allow React -> ThingHTTP to act first.
FALLBACK_RETRY = 22.0 # Prevent repeated queue insertion on transient failures.

# --------- 4. Persistent history and MQTT traffic accounting ----------
# No extra Python packages required. Files survive gateway restarts.
HISTORY_PATH = os.path.join(os.path.dirname(CONFIG_PATH), 'motor_history.csv')
STATS_PATH = os.path.join(os.path.dirname(CONFIG_PATH), 'network_statistics.csv')
# ESP32 electrical power is estimated, not measured by Wokwi.
# These are assumptions for the course report, not motor current readings.
ESP32_VOLTAGE = 3.3
ESP32_AVG_CURRENT_A = 0.100  # 100 mA per ESP32 (assumed)
ESP32_COUNT = 2
ESTIMATED_POWER_W = ESP32_VOLTAGE * ESP32_AVG_CURRENT_A * ESP32_COUNT
GATEWAY_START_TIME = time.monotonic()
HISTORY_COLUMNS = [
    'timestamp_utc', 'thingspeak_entry', 'temperature_c', 'vibration_m_s2',
    'current_a', 'rpm', 'node1_condition', 'node2_condition', 'mode',
    'node1_mqtt_sequence', 'node2_mqtt_sequence'
]
STATS_COLUMNS = [
    'timestamp_utc', 'interval_seconds', 'node1_messages', 'node2_messages',
    'total_messages', 'payload_bits_per_second', 'mqtt_bits_per_second_estimated',
    'esp32_power_w_estimated', 'esp32_energy_wh_since_start_estimated'
]
# Estimates MQTT fixed header + topic UTF-8 bytes + topic-length prefix.
# Excludes TCP/IP, Wi-Fi, retransmissions, TLS (not used), and ACKs.
# QoS0 is used by the ESP32 sensor publishers.
traffic = {1: {'messages': 0, 'payload_bytes': 0, 'mqtt_bytes': 0},
           2: {'messages': 0, 'payload_bytes': 0, 'mqtt_bytes': 0}}
traffic_previous = {1: dict(traffic[1]), 2: dict(traffic[2])}
TRAFFIC_REPORT_EVERY = 60.0
file_lock = threading.Lock()


def utc_timestamp():
    """Return the current UTC time for measurements and event logs."""
    return datetime.now(timezone.utc).isoformat(timespec='seconds')


def append_csv(path, columns, row):
    # Only write a header for new/empty files, never overwrite existing data.
    """Append a measurement row to a CSV log file."""
    with file_lock:
        with open(path, 'a', newline='', encoding='utf-8') as handle:
            writer = csv.DictWriter(handle, fieldnames=columns)
            if handle.tell() == 0:
                writer.writeheader()
            writer.writerow(row)


def mqtt_publish_packet_bytes(topic, payload_length):
    # MQTT 3.1.1 PUBLISH QoS0: fixed header 1 byte + remaining-length varint
    """Estimate MQTT packet size for network usage statistics."""
    remaining = 2 + len(topic.encode('utf-8')) + payload_length
    varint_length = 1
    value = remaining
    while value >= 128:
        varint_length += 1
        value //= 128
    return 1 + varint_length + remaining


def save_traffic_interval(seconds):
    # Compare counters with the previous minute to get a rate, not a total.
    """Persist collected traffic statistics for the current interval."""
    with lock:
        snapshot = {n: dict(traffic[n]) for n in (1, 2)}
        previous = {n: dict(traffic_previous[n]) for n in (1, 2)}
        for n in (1, 2):
            traffic_previous[n] = dict(snapshot[n])

    diff = {n: {key: snapshot[n][key] - previous[n][key]
                for key in snapshot[n]} for n in (1, 2)}
    total_msgs = diff[1]['messages'] + diff[2]['messages']
    payload_bytes = diff[1]['payload_bytes'] + diff[2]['payload_bytes']
    mqtt_bytes = diff[1]['mqtt_bytes'] + diff[2]['mqtt_bytes']
    payload_bps = payload_bytes * 8 / seconds
    mqtt_bps = mqtt_bytes * 8 / seconds

    # Power is assumed constant. Energy grows with elapsed running time.
    runtime_hours = (time.monotonic() - GATEWAY_START_TIME) / 3600
    energy_wh = ESTIMATED_POWER_W * runtime_hours

    append_csv(STATS_PATH, STATS_COLUMNS, {
        'timestamp_utc': utc_timestamp(),
        'interval_seconds': round(seconds, 2),
        'node1_messages': diff[1]['messages'],
        'node2_messages': diff[2]['messages'],
        'total_messages': total_msgs,
        'payload_bits_per_second': round(payload_bps, 2),
        'mqtt_bits_per_second_estimated': round(mqtt_bps, 2),
        'esp32_power_w_estimated': round(ESTIMATED_POWER_W, 3),
        'esp32_energy_wh_since_start_estimated': round(energy_wh, 5),
    })

    # One small CMD summary per minute; normal gateway logs stay unchanged.
    print(f'[{datetime.now():%H:%M:%S}] NETWORK (last {seconds:.0f}s) | '
          f'N1: {diff[1]["messages"]}, N2: {diff[2]["messages"]} messages | '
          f'payload: {payload_bps:.1f} bit/s | '
          f'MQTT est.: {mqtt_bps:.1f} bit/s | '
          f'ESP32 est.: {ESTIMATED_POWER_W:.2f} W, {energy_wh:.4f} Wh',
          flush=True)


def on_connect(client, userdata, flags, reason_code, properties):
    """Subscribe to node topics after connecting to the MQTT broker."""
    if reason_code == 0:
        connected.set()
        for topic in (*DATA_TOPICS.values(), *ACK_TOPICS.values(), *BOOT_TOPICS.values()):
            client.subscribe(topic)
        print('MQTT connected; subscribed to both nodes and acknowledgments.', flush=True)
    else:
        connected.clear()
        print('MQTT connection rejected:', reason_code, flush=True)


def on_disconnect(client, userdata, disconnect_flags, reason_code, properties):
    """Report MQTT disconnections and reconnection status."""
    connected.clear()
    print('MQTT disconnected; reconnecting automatically.', flush=True)


def on_message(client, userdata, message):
    """Called by Paho's background network thread, not the main loop."""
    if message.topic in BOOT_TOPICS.values():
        node = next(n for n, topic in BOOT_TOPICS.items() if topic == message.topic)
        with lock:
            last_applied[node] = None
            pending_ack[node] = None
            fallback_queued_for[node] = None
            desired_since[node] = time.monotonic()
        print(f'[{datetime.now():%H:%M:%S}] Node {node} restarted; LED resync scheduled', flush=True)
        return
    if message.topic in ACK_TOPICS.values():
        node = next(n for n, topic in ACK_TOPICS.items() if topic == message.topic)
        command = message.payload.decode(errors='replace').strip()
        with lock:
            expected = pending_ack[node]
            if expected and expected['command'] == command:
                pending_ack[node] = None
                last_applied[node] = command
                match = True
            else:
                match = False
        if match:
            print(f'[{datetime.now():%H:%M:%S}] CONFIRMED | Node {node} executed {command}', flush=True)
        else:
            print(f'[{datetime.now():%H:%M:%S}] Node {node} ACK (not pending): {command}', flush=True)
        return
    if message.topic not in DATA_TOPICS.values():
        return
    node = next(n for n, topic in DATA_TOPICS.items() if topic == message.topic)
    try:
        payload = json.loads(message.payload.decode('utf-8'))
        required = ('temperature', 'vibration') if node == 1 else ('current', 'rpm', 'mode')
        if not isinstance(payload, dict):
            raise ValueError('Expected a JSON object')
        clean = {}
        for key in required:
            val = payload[key]
            if isinstance(val, bool) or not isinstance(val, (float, int)) or not math.isfinite(val):
                raise ValueError(f'Invalid numeric field {key}')
            clean[key] = val
        if node == 2 and clean['mode'] != 0:
            raise ValueError('Expected fixed mode=0 after switch removal')
        with lock:
            latest[node] = clean
            last_seen[node] = time.monotonic()
            sequence[node] += 1
            traffic[node]['messages'] += 1
            traffic[node]['payload_bytes'] += len(message.payload)
            traffic[node]['mqtt_bytes'] += mqtt_publish_packet_bytes(message.topic, len(message.payload))
        # Quiet mode: do not print every incoming 2-second MQTT sample.
    except (KeyError, ValueError, TypeError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        print(f'Rejected Node {node} payload: {exc}', flush=True)


def classify_node1(temp, vib):
    """Gateway-side DEMO thresholds, not validated industrial diagnostics."""
    if temp <= 45.0 and vib <= 0.60:
        return 0
    if temp <= 50.0 and vib <= 0.90:
        return 1
    return 2


def classify_node2(current, rpm):
    """Fixed operating state; no mode switch."""
    if 0.70 <= current <= 1.30 and 1050 <= rpm <= 1250:
        return 0
    if 0.50 <= current <= 1.50 and 950 <= rpm <= 1350:
        return 1
    return 2


def upload_to_thingspeak():
    """Upload only a COMPLETE, RECENT, NEW pair of Node 1 + Node 2.

    Important invariants:
    - Never upload a partial set of ThingSpeak fields.
    - Both nodes must have sent at least one NEW sample since last success.
    - Their latest MQTT arrival times must be close to each other.
    - A failed HTTP upload does not consume the pair.
    - The caller enforces ThingSpeak's >=15 second rate limit.
    """
    if not WRITE_KEY:
        return
    now = time.monotonic()
    with lock:
        if latest[1] is None or latest[2] is None:
            print('WAITING: need data from BOTH nodes; no upload.', flush=True)
            return
        if sequence[1] <= uploaded_sequence[1] or sequence[2] <= uploaded_sequence[2]:
            print('WAITING: need NEW data from BOTH nodes; no upload.', flush=True)
            return
        if any(now - last_seen[n] > FRESH_FOR for n in (1, 2)):
            print('WAITING: a node is stale; no partial upload.', flush=True)
            return
        if abs(last_seen[1] - last_seen[2]) > MAX_PAIR_SKEW:
            print('WAITING: node messages too far apart; no upload.', flush=True)
            return
        n1, n2 = dict(latest[1]), dict(latest[2])
        sent_sequence = dict(sequence)

    # Conditions are calculated HERE, never on either ESP32.
    status1 = classify_node1(n1['temperature'], n1['vibration'])
    status2 = classify_node2(n2['current'], n2['rpm'])
    # All 7 fields are always present in the same HTTP request.
    fields = {
        'api_key': WRITE_KEY,
        'field1': n1['temperature'],
        'field2': n1['vibration'],
        'field3': n2['current'],
        'field4': n2['rpm'],
        'field6': status1,
        'field7': status2,
        'field8': int(n2['mode']),
    }
    try:
        response = upload_session.post('https://api.thingspeak.com/update', data=fields, timeout=8)
        response.raise_for_status()
        entry = response.text.strip()
        if not entry.isdigit() or int(entry) == 0:
            print(f'ThingSpeak rejected COMPLETE batch: {entry!r}', flush=True)
            return
        with lock:
            for n in (1, 2):
                uploaded_sequence[n] = max(uploaded_sequence[n], sent_sequence[n])
        with lock:
            for node, status in ((1, status1), (2, status2)):
                new_command = STATUS_COMMANDS[status]
                if desired[node] != new_command:
                    desired[node] = new_command
                    desired_since[node] = time.monotonic()
                    fallback_queued_for[node] = None
                    print(f'[{datetime.now():%H:%M:%S}] CLOUD DESIRED | Node {node}: {new_command}', flush=True)
        print(f'[{datetime.now():%H:%M:%S}] ThingSpeak UPDATED | entry {entry} | Node 1={status1} | Node 2={status2} | ALL 7 fields', flush=True)
        # Store only confirmed cloud uploads: entry ID links CSV to ThingSpeak.
        try:
            append_csv(HISTORY_PATH, HISTORY_COLUMNS, {
                'timestamp_utc': utc_timestamp(), 'thingspeak_entry': entry,
                'temperature_c': n1['temperature'], 'vibration_m_s2': n1['vibration'],
                'current_a': n2['current'], 'rpm': n2['rpm'],
                'node1_condition': status1, 'node2_condition': status2,
                'mode': int(n2['mode']),
                'node1_mqtt_sequence': sent_sequence[1],
                'node2_mqtt_sequence': sent_sequence[2]
            })
        except OSError as exc:
            print(f'WARNING: ThingSpeak updated, but CSV save failed: {exc}', flush=True)
    except requests.RequestException as exc:
        print(f'Complete batch upload failed; retry after cooldown: {exc}', flush=True)


def publish_command(client, node, command, cmd_id):
    """Issue one MQTT command and await the ESP32 acknowledgment."""
    with lock:
        pending_ack[node] = {'command': command, 'sent_at': time.monotonic(), 'id': cmd_id}
    try:
        info = client.publish(COMMAND_TOPICS[node], command, qos=1, retain=False)
        info.wait_for_publish(timeout=5)
        if not info.is_published():
            raise RuntimeError('MQTT publication not confirmed')
        print(f'[{datetime.now():%H:%M:%S}] ThingSpeak -> MQTT | Node {node}: {command} | command #{cmd_id}', flush=True)
    except (RuntimeError, ValueError) as exc:
        with lock:
            if pending_ack[node] and pending_ack[node]['id'] == cmd_id:
                pending_ack[node] = None
        print(f'WARNING | Node {node}: TalkBack command {cmd_id} consumed; MQTT failed: {exc}', flush=True)


def poll_talkback(client, node):
    """Consume TalkBack commands, but never execute OLD status commands.

    React may enqueue stale colors. We compare them with the latest successfully
    uploaded cloud condition before publishing anything to the ESP32.
    """
    cfg = TALKBACK[node]
    if not cfg['key'] or not connected.is_set():
        return
    with lock:
        if pending_ack[node] is not None:
            return
        if latest[node] is None or time.monotonic() - last_seen[node] > NODE_COMMAND_FRESH_FOR:
            return
        expected = desired[node]
    if expected is None:
        return  # Wait for first successful COMPLETE ThingSpeak upload.

    url = f'https://api.thingspeak.com/talkbacks/{cfg["id"]}/commands/execute.json'
    try:
        response = talkback_session.post(url, data={'api_key': cfg['key']}, timeout=8)
        response.raise_for_status()
        if not response.text.strip():
            return
        payload = response.json()
        if not isinstance(payload, dict):
            print(f'TalkBack Node {node}: unexpected response', flush=True)
            return
        command = str(payload.get('command_string') or '').strip()
        if not command:
            return
        if command not in ALLOWED:
            print(f'DROPPED | Node {node}: unknown TalkBack command {command!r}', flush=True)
            return
        cmd_id = payload.get('id', '?')
        with lock:
            expected_now = desired[node]
            already_applied = last_applied[node] == command
        if command != 'LED_BLUE' and command != expected_now:
            print(f'STALE DROPPED | Node {node}: {command} #{cmd_id}; latest cloud={expected_now}', flush=True)
            return
        if command != 'LED_BLUE' and already_applied:
            print(f'DUPLICATE DROPPED | Node {node}: {command} #{cmd_id}', flush=True)
            return
        publish_command(client, node, command, cmd_id)
    except (requests.RequestException, RuntimeError, ValueError) as exc:
        print(f'TalkBack Node {node} polling error: {exc}', flush=True)


def repair_missing_cloud_commands():
    """Cloud-side backup when a React action fails or startup begins mid-state.

    Posts the CURRENT command into ThingSpeak TalkBack, never directly to MQTT.
    Thus ESP32 LEDs remain cloud-command-controlled. React can stay enabled.
    """
    now = time.monotonic()
    for node in (1, 2):
        cfg = TALKBACK[node]
        with lock:
            command = desired[node]
            fresh = latest[node] is not None and now - last_seen[node] <= NODE_COMMAND_FRESH_FOR
            should_queue = (bool(cfg['key']) and command is not None and fresh
                and last_applied[node] != command and pending_ack[node] is None
                and now - desired_since[node] >= FALLBACK_GRACE
                and (fallback_queued_for[node] != command or now - last_fallback_time[node] >= FALLBACK_RETRY))
        if not should_queue:
            continue
        # Mark BEFORE network I/O to prevent repeated queue insertion.
        with lock:
            fallback_queued_for[node] = command
            last_fallback_time[node] = time.monotonic()
        try:
            url = f'https://api.thingspeak.com/talkbacks/{cfg["id"]}/commands'
            response = talkback_session.post(url, data={
                'api_key': cfg['key'], 'command_string': command}, timeout=8)
            response.raise_for_status()
            print(f'[{datetime.now():%H:%M:%S}] CLOUD REPAIR QUEUED | Node {node}: {command} via TalkBack', flush=True)
        except requests.RequestException as exc:
            print(f'Cloud repair Node {node} failed: {exc}', flush=True)


def check_ack_timeouts():
    """Report lost ACKs; do NOT pretend a broker publish is device execution."""
    now = time.monotonic()
    for node in (1, 2):
        with lock:
            expected = pending_ack[node]
            if expected and now - expected['sent_at'] >= ACK_TIMEOUT:
                pending_ack[node] = None
            else:
                expected = None
        if expected:
            print(f'[{datetime.now():%H:%M:%S}] WARNING | no ESP32 ACK from Node {node} '
                  f'for {expected["command"]} (TalkBack #{expected["id"]}).', flush=True)


def talkback_worker(client, stop_event):
    """Poll commands independently so HTTP polling cannot delay uploads."""
    while not stop_event.wait(TALKBACK_EVERY):
        check_ack_timeouts()
        for node in (1, 2):
            if stop_event.is_set():
                return
            poll_talkback(client, node)
        repair_missing_cloud_commands()


def main():
    """Initialize the MQTT client and run the integrated IoT gateway."""
    print('SYNCHRONIZED AUTOMATIC IOT GATEWAY STARTED', flush=True)
    print('ThingSpeak upload:', 'ENABLED' if WRITE_KEY else 'DISABLED (TS_WRITE_KEY missing)', flush=True)
    print('Cloud updates: complete 7-field batches, >=15.5s apart.', flush=True)
    print('Gateway classifies motor health; RGB is controlled via ThingSpeak TalkBack (stale-command guard enabled).', flush=True)
    print('TalkBack credentials:', {n: bool(cfg['key']) for n, cfg in TALKBACK.items()}, flush=True)
    print('History CSV:', HISTORY_PATH, flush=True)
    print('Network CSV:', STATS_PATH, flush=True)
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id='py-gateway-CHANGE_TO_A_UNIQUE_NONPERSONAL_ID')
    client.on_connect, client.on_disconnect, client.on_message = on_connect, on_disconnect, on_message
    client.reconnect_delay_set(min_delay=1, max_delay=15)
    client.connect(BROKER, 1883, keepalive=60)
    client.loop_start()
    stop_event = threading.Event()
    talkback_thread = threading.Thread(target=talkback_worker, args=(client, stop_event), daemon=True)
    talkback_thread.start()

    # First upload as soon as both nodes arrive; subsequent HTTP attempts
    # begin no sooner than 15.5s after the previous HTTP attempt began.
    last_http_attempt = -UPLOAD_EVERY
    last_wait_notice = time.monotonic()
    last_traffic_report = time.monotonic()
    try:
        while True:
            now = time.monotonic()
            if now - last_traffic_report >= TRAFFIC_REPORT_EVERY:
                elapsed = now - last_traffic_report
                last_traffic_report = now
                try:
                    save_traffic_interval(elapsed)
                except OSError as exc:
                    print(f'WARNING: traffic CSV save failed: {exc}', flush=True)
            if WRITE_KEY and now - last_http_attempt >= UPLOAD_EVERY:
                with lock:
                    ready = (all(latest[n] is not None for n in (1, 2))
                             and all(sequence[n] > uploaded_sequence[n] for n in (1, 2))
                             and all(now - last_seen[n] <= FRESH_FOR for n in (1, 2))
                             and abs(last_seen[1] - last_seen[2]) <= MAX_PAIR_SKEW)
                if ready:
                    last_http_attempt = time.monotonic()
                    upload_to_thingspeak()
                elif now - last_wait_notice >= 30.0:
                    with lock:
                        missing = [str(n) for n in (1, 2) if latest[n] is None or now - last_seen[n] > FRESH_FOR]
                        skew = abs(last_seen[1] - last_seen[2]) if all(latest.values()) else None
                    reason = ('no recent data from Node ' + ', '.join(missing)) if missing else ('node messages not close enough' if skew is not None and skew > MAX_PAIR_SKEW else 'waiting for new pair')
                    print(f'[{datetime.now():%H:%M:%S}] Waiting: {reason}. No partial upload.', flush=True)
                    last_wait_notice = now
            time.sleep(0.1)
    except KeyboardInterrupt:
        print('Gateway stopped.', flush=True)
    finally:
        try:
            elapsed = time.monotonic() - last_traffic_report
            if elapsed >= 1.0:
                save_traffic_interval(elapsed)
        except OSError as exc:
            print(f'WARNING: final traffic save failed: {exc}', flush=True)
        stop_event.set()
        client.disconnect()
        client.loop_stop()
        upload_session.close()
        talkback_session.close()


if __name__ == '__main__':
    main()
