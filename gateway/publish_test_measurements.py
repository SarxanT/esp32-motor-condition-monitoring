"""Publish example sensor readings to exercise the MQTT gateway.

This utility simulates both nodes; it does not use physical sensors.
"""

import json
import time
import paho.mqtt.client as mqtt

BROKER = "broker.emqx.io"
PORT = 1883
PROJECT_ID = "CHANGE_TO_A_UNIQUE_NONPERSONAL_ID"  # Must match gateway.py.

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.connect(BROKER, PORT, 60)
client.loop_start()

# Simulated Node 1 data
node1 = {
    "temperature": 36.5,
    "vibration": 0.25,
    "condition": 0
}

# Simulated Node 2 data
node2 = {
    "current": 1.15,
    "rpm": 1180,
    "condition": 0,
    "mode": 0
}

topic1 = f"motor-lab/{PROJECT_ID}/node1/data"
topic2 = f"motor-lab/{PROJECT_ID}/node2/data"

client.publish(topic1, json.dumps(node1), qos=1).wait_for_publish()
print("Node 1 message sent")

time.sleep(1)

client.publish(topic2, json.dumps(node2), qos=1).wait_for_publish()
print("Node 2 message sent")

client.disconnect()
client.loop_stop()
