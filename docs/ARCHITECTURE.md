# System architecture and honest scope

## Sensors and messages

| Node | Inputs | Output topics | Interpretation |
|---|---|---|---|
| ESP32 Node 1 | DS18B20 + MPU6050 (Wokwi) | `/node1/data`, `/node1/ack`, `/node1/boot` | Temperature, motion proxy, and diagnostic LED feedback |
| ESP32 Node 2 | Two Wokwi potentiometers | `/node2/data`, `/node2/ack`, `/node2/boot` | Simulated motor current and RPM values; these are **not physical current/RPM sensors** |
| Python gateway | Both MQTT feeds | ThingSpeak Fields 1–4, 6–8; `/node1/cmd`, `/node2/cmd` | Synchronizes readings and forwards allowed simulated RGB LED commands |

Cloud upload is batched through the gateway to avoid simultaneous nodes independently writing the same ThingSpeak channel. Approximate 15.5-second pacing is used in code; this is a design choice, not evidence of a successful cloud test.

## Scope limits

- Motor condition is based on classroom thresholds, not trained machine-learning inference.
- Vibration is an acceleration-derived proxy; current and RPM are simulated rather than measured.
- The Python gateway provides logic and transport, but no claim of industrial reliability or fault detection accuracy is made.
- The diagnostic LED simulates cloud-controlled status feedback; it is not connected to motor power or protection.
- MQTT is presently configured for a shared public test broker with no transport encryption or authentication. **No real-device actuation.**

## Planned next iteration

- Replace public MQTT with broker credentials and TLS for any non-demo usage.
- Store channel keys in private configuration only; rotate the previously embedded credentials.
- Compile each ESP32 sketch, run both Wokwi simulations, and capture reproducible demonstration evidence.
- Add deterministic tests for the Python gateway's payload validation, field mapping, synchronization, and retry logic.
- Introduce actual motor measurements and clearly labeled AI/predictive analysis only after data quality and evaluation are established.
