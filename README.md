# Two-Node IoT Motor-Condition Monitoring — Experimental Source Draft

**PUBLIC SOURCE DRAFT — NOT YET VERIFIED END TO END.** This project is a separate, more advanced coursework prototype, preserved as a source snapshot for Week 3+ review. It simulates motor measurements; **it does not contain an AI predictive-maintenance model, physical motor protection, or a validated industrial system.**

## What is here

- **Node 1 (Wokwi ESP32):** simulated DS18B20 temperature, MPU6050-derived vibration, RGB diagnostic LED.
- **Node 2 (Wokwi ESP32):** simulated current and RPM input controls, RGB diagnostic LED.
- **Python gateway:** MQTT ingest and validation, cross-node fusion, ThingSpeak HTTP updates, TalkBack command handling, and CSV logging.
- **Configuration example:** placeholders only. Actual ThingSpeak secrets must stay in local `gateway/config.json`.

## Architecture

```text
Wokwi ESP32 Node 1 --MQTT--+
                           +--> Public demo MQTT broker --> Python gateway
Wokwi ESP32 Node 2 --MQTT--+                               |
                                                            +--> ThingSpeak fields (HTTPS)
ThingSpeak React / TalkBack -- HTTPS polling --> Gateway --+--> MQTT LED diagnostic commands
```

The public MQTT broker carries **synthetic/demo measurements only**. Public brokers on unencrypted port 1883 are not appropriate for private sensor data, device control, or production equipment.

## Before using this draft

1. Replace `CHANGE_TO_A_UNIQUE_NONPERSONAL_ID` consistently across both sketches and gateway scripts. Use a unique topic prefix that does not contain your name or student identifier.
2. Copy `gateway/config.example.json` to local `gateway/config.json` and fill in **fresh, newly issued** ThingSpeak/TalkBack credentials. The example ID values (`0`) are placeholders and do not function.
3. Review the public MQTT security model and choose an authenticated broker before working with real hardware or private information.
4. Open each `diagram.json` with its corresponding `.ino` in Wokwi. These are ESP32 sketches, **not Arduino Mega 2560 sketches**.
5. Install the Python requirements in an isolated local environment using `pip install -r gateway/requirements.txt`.
6. Read [architecture notes](docs/ARCHITECTURE.md), then review [readiness gates](docs/PUBLISHING_GATES.md) before claiming the system is working.

## ThingSpeak channel reference

The owner supplied [ThingSpeak channel 3521581](https://thingspeak.mathworks.com/channels/3521581) for later integration. Channel fields, ownership, and live operation have **not been verified**. The public code does not include a write key. See [channel notes](docs/THINGSPEAK_CHANNEL.md).

## Validation status

- Python scripts: syntax compilation checked, **not executed against a broker**. GitHub CI also runs static tests.
- Wokwi JSON files: parse checked, **not simulated**.
- ESP32 sketches: **not yet verified by a successful compile at the time of publication**. GitHub CI is configured to compile both.
- ThingSpeak/TalkBack and cross-node behavior: **not verified** in this session.

## Origin

Refurbished from the author's original coursework files. Historical/legacy gateway iterations and a diagnostic utility with mismatched commands were deliberately omitted from this candidate to avoid presenting incompatible approaches as a single runnable setup.
