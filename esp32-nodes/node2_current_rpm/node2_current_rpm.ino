// Node 2: Current and RPM
// Wokwi ESP32 firmware; communicates through MQTT via the Python gateway.

/*
  Node 2: CLOUD-AUTHORITATIVE LEDs, no switch or local health classification.
  Current potentiometer GPIO34, RPM potentiometer GPIO35; RGB R25/G26/B27.
  Node 2 RGB common-cathode, COM to GND.
  MCU scales and filters readings, gateway calculates motor health.
  Only ThingSpeak TalkBack commands control the LED (except boot self-test).
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// ============================================================
// 1. NETWORK AND TOPICS
// The Project identifier must match gateway.py and Node 1.
// data: Node 2 -> gateway, cmd: PC -> Node 2, ack: Node 2 -> PC
// Public MQTT broker: non-sensitive simulated data only.
// ============================================================
const char* WIFI_NAME = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";
const char* MQTT_BROKER = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_DATA_TOPIC = "motor-lab/CHANGE_TO_A_UNIQUE_NONPERSONAL_ID/node2/data";
const char* MQTT_COMMAND_TOPIC = "motor-lab/CHANGE_TO_A_UNIQUE_NONPERSONAL_ID/node2/cmd";
const char* MQTT_ACK_TOPIC = "motor-lab/CHANGE_TO_A_UNIQUE_NONPERSONAL_ID/node2/ack";
const char* MQTT_BOOT_TOPIC = "motor-lab/CHANGE_TO_A_UNIQUE_NONPERSONAL_ID/node2/boot";

// Separate schedules: sensing, printing and MQTT publishing.
const unsigned long SENSOR_INTERVAL_MS = 1000;
const unsigned long PRINT_INTERVAL_MS = 2000;
const unsigned long PUBLISH_INTERVAL_MS = 2000;
const unsigned long WIFI_RETRY_MS = 5000;
const unsigned long MQTT_RETRY_MS = 3000;
const unsigned long PUBLISH_RETRY_MS = 3000;

// ============================================================
// 2. INPUT PINS AND OPERATING STATES
// Both potentiometers produce ADC values from 0 to 4095.
// Do not confuse their simulated values with live motor current.
// ============================================================
#define CURRENT_PIN 34
#define RPM_PIN 35
#define RED_PIN 25
#define GREEN_PIN 26
#define BLUE_PIN 27

float current = 0.0f;
float rpm = 0.0f;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

// Upload immediately after sensor data and MQTT connection,
// then every 2 s after a successful publish; retry failures.
bool sensorReady = false;
bool publishPending = false;
bool hasPublished = false;
unsigned long lastSuccessfulPublish = 0;
unsigned long lastPublishAttempt = 0;

// ===================== CLOUD RGB LED ========================
const bool RGB_COMMON_ANODE = false; // Node 2: COM -> GND
// This LED displays ONLY the last received cloud command.
// No sensor threshold/health calculation runs on the ESP32.
enum LedState { WAIT_CLOUD, LED_HEALTHY, LED_WARNING, LED_ALARM, LED_TEST_BLUE };
LedState cloudLed = WAIT_CLOUD;

void rgbWrite(bool red, bool green, bool blue) {
  // Node 2 uses common-cathode RGB: HIGH means ON.
  // Change RGB_COMMON_ANODE only if your actual LED wiring is different.
  digitalWrite(RED_PIN,   red   != RGB_COMMON_ANODE ? HIGH : LOW);
  digitalWrite(GREEN_PIN, green != RGB_COMMON_ANODE ? HIGH : LOW);
  digitalWrite(BLUE_PIN,  blue  != RGB_COMMON_ANODE ? HIGH : LOW);
}
void rgbOff() { rgbWrite(false, false, false); }
void updateRGB() {
  switch (cloudLed) {
    case LED_HEALTHY:   rgbWrite(false, true, false); break;
    case LED_WARNING:   rgbWrite(true, true, false); break;
    case LED_ALARM:     rgbWrite((millis()/500UL)%2UL == 0UL, false, false); break;
    case LED_TEST_BLUE: rgbWrite(false, false, true); break;
    default:            rgbOff(); break;  // awaiting ThingSpeak command
  }
}
void testRGBAtStartup() {
  Serial.println("RGB self-test: RED > GREEN > BLUE > YELLOW");
  rgbWrite(true,false,false); delay(300);
  rgbWrite(false,true,false); delay(300);
  rgbWrite(false,false,true); delay(300);
  rgbWrite(true,true,false); delay(300);
  rgbOff();
  Serial.println("LED OFF: waiting for ThingSpeak TalkBack command");
  Serial.println("[CHECK] Common-cathode RGB: COM->GND, R->GPIO25, G->GPIO26, B->GPIO27");
}

// =====================================================
// READ SIMULATED CURRENT AND RPM
// Seven-sample median reduces brief ADC spikes.
// A zero must persist for three 1-second samples before acceptance.
// This filters a transient reset but still detects sustained zero as ALARM.
// =====================================================
int medianADC(int pin) {
  int a[7];
  for (int i = 0; i < 7; i++) { a[i] = analogRead(pin); delayMicroseconds(200); }
  for (int i = 1; i < 7; i++) {
    int x = a[i], j = i - 1;
    while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; }
    a[j + 1] = x;
  }
  return a[3];
}

int zeroCurrentCount = 0, zeroRPMCount = 0;
bool currentInitialized = false, rpmInitialized = false;

void readSensors() {
  int rawCurrent = medianADC(CURRENT_PIN);
  int rawRPM = medianADC(RPM_PIN);
  if (rawCurrent <= 2) zeroCurrentCount++; else zeroCurrentCount = 0;
  if (rawRPM <= 2) zeroRPMCount++; else zeroRPMCount = 0;
  if (rawCurrent > 2 || zeroCurrentCount >= 3 || !currentInitialized) {
    current = rawCurrent * (3.0f / 4095.0f);
    currentInitialized = true;
  } else Serial.println("Node 2: ignoring transient current ADC zero");
  if (rawRPM > 2 || zeroRPMCount >= 3 || !rpmInitialized) {
    rpm = rawRPM * (1800.0f / 4095.0f);
    rpmInitialized = true;
  } else Serial.println("Node 2: ignoring transient RPM ADC zero");
}

// ================== PRINT RAW SENSOR MEASUREMENTS =============
void printStatus() {
  Serial.print("Node 2 current: "); Serial.print(current,2);
  Serial.print(" A | RPM: "); Serial.print(rpm,0);
  Serial.println(" | health: calculated in PYTHON; LED: cloud command only");
}

// ============ RECEIVE CLOUD COMMAND (NO LOCAL DECISION) ========
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (String(topic) != MQTT_COMMAND_TOPIC) {
    Serial.print("[CLOUD] Ignored topic: ");
    Serial.println(topic);
    return;
  }

  String command;
  command.reserve(length + 1);
  for (unsigned int i = 0; i < length; ++i) {
    command += (char)payload[i];
  }
  command.trim();

  if (command == "LED_GREEN") {
    cloudLed = LED_HEALTHY;
  } else if (command == "LED_YELLOW") {
    cloudLed = LED_WARNING;
  } else if (command == "LED_RED") {
    cloudLed = LED_ALARM;
  } else if (command == "LED_BLUE") {
    cloudLed = LED_TEST_BLUE;
  } else {
    Serial.print("[CLOUD] Rejected unsupported command: ");
    Serial.println(command);
    return;
  }

  // Force the LED output immediately, then continue updating it in loop().
  updateRGB();
  Serial.print("[CLOUD] NODE 2 RECEIVED: ");
  Serial.println(command);
  Serial.print("[CLOUD] LED STATE: ");
  switch (cloudLed) {
    case LED_HEALTHY:   Serial.println("GREEN"); break;
    case LED_WARNING:   Serial.println("YELLOW"); break;
    case LED_ALARM:     Serial.println("BLINKING RED"); break;
    case LED_TEST_BLUE: Serial.println("BLUE"); break;
    default:            Serial.println("OFF"); break;
  }

  // Acknowledgment confirms that the command was accepted and applied
  // by the sketch; it cannot prove a real-world LED is electrically working.
  if (mqttClient.connected()) {
    bool ackSent = mqttClient.publish(MQTT_ACK_TOPIC, command.c_str());
    Serial.println(ackSent ? "[CLOUD] ACK published" : "[CLOUD] ACK publish FAILED");
  }
}

// ============================================================
// 7. NETWORK MAINTENANCE
// Retry connections at intervals without an endless while loop.
// One connect attempt can still block briefly due to networking.
// Subscribe to cmd again after each successful MQTT reconnect.
// ============================================================
void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  static unsigned long lastAttempt = 0;
  if (lastAttempt != 0 && millis() - lastAttempt < WIFI_RETRY_MS) return;

  lastAttempt = millis();
  Serial.println("Node 2: connecting to Wi-Fi...");
  WiFi.begin(WIFI_NAME, WIFI_PASSWORD, 6);
}

void maintainMQTT() {
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) return;
  static unsigned long lastAttempt = 0;
  if (lastAttempt != 0 && millis() - lastAttempt < MQTT_RETRY_MS) return;

  lastAttempt = millis();
  String clientID = "ESP32-Node2-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("Node 2: connecting to MQTT... ");

  if (mqttClient.connect(clientID.c_str())) {
    Serial.println("connected!");
    // Resynchronize LED state after each Wokwi restart/reconnection.
    mqttClient.publish(MQTT_BOOT_TOPIC, "BOOT", false);
    if (mqttClient.subscribe(MQTT_COMMAND_TOPIC)) {
      Serial.print("Subscribed: ");
      Serial.println(MQTT_COMMAND_TOPIC);
    } else {
      Serial.println("MQTT command subscription failed.");
    }
    publishPending = true;
    lastPublishAttempt = 0;
  } else {
    Serial.print("Failed; MQTT state: ");
    Serial.println(mqttClient.state());
  }
}

// ============================================================
// 8. TRANSMIT LATEST NODE 2 VALUES AS A JSON MQTT MESSAGE
// Python gateway subscribes to MQTT_DATA_TOPIC and receives:
// {"current": 1.2, "rpm": 1200, "mode": 0}.
// Condition is calculated later in Python, not in Node 2.
// publish() returning true does not prove Python received it.
// ============================================================
void publishSensorData() {
  if (!mqttClient.connected()) {
    Serial.println("Node 2 publish skipped: MQTT disconnected.");
    return;
  }

  JsonDocument doc;
  doc["current"] = current;
  doc["rpm"] = rpm;
  doc["mode"] = 0; // Legacy ThingSpeak Field 8, fixed value; no mode switch

  String payload;
  serializeJson(doc, payload);

  lastPublishAttempt = millis();
  if (mqttClient.publish(MQTT_DATA_TOPIC, payload.c_str())) {
    lastSuccessfulPublish = millis();
    hasPublished = true;
    publishPending = false;
    Serial.print("NODE 2 MQTT published: ");
    Serial.println(payload);
  } else {
    publishPending = true;
    Serial.println("Node 2 MQTT publish failed; retry in 3s.");
  }
}

// ============================================================
// 9. SETUP: RUNS ONCE AFTER RESET
// ============================================================
void setup() {
  Serial.begin(115200);

  pinMode(CURRENT_PIN, INPUT);
  pinMode(RPM_PIN, INPUT);
  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);
  rgbOff();
  testRGBAtStartup();

  Serial.println("\n================================");
  Serial.println("NODE 2 - CURRENT + RPM + MQTT");
  Serial.println("================================");

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);
  mqttClient.setSocketTimeout(2);
  mqttClient.setKeepAlive(30);
  maintainWiFi();
  Serial.println("Node 2 initialized.");
}

// ============================================================
// 10. MAIN LOOP
// 1 sec: read the two potentiometers (no local classification).
// 2 sec: print readings to Wokwi serial monitor.
// 2 sec: publish JSON (or immediately after reconnect).
// Every pass: RGB LED, Wi-Fi and MQTT maintenance, commands.
// ============================================================
void loop() {
  static unsigned long lastSensor = 0;
  if (millis() - lastSensor >= SENSOR_INTERVAL_MS) {
    lastSensor = millis();
    readSensors();         // Two potentiometers simulate measurements.
    sensorReady = true;
  }
  updateRGB();

  maintainWiFi();
  maintainMQTT();
  if (mqttClient.connected()) mqttClient.loop();

  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= PRINT_INTERVAL_MS) {
    lastPrint = millis();
    printStatus();
  }

  if (mqttClient.connected() && sensorReady) {
    const unsigned long now = millis();
    const bool firstOrReconnect = publishPending || !hasPublished;
    const bool regular = hasPublished &&
      (now - lastSuccessfulPublish >= PUBLISH_INTERVAL_MS);
    const bool retryReady = lastPublishAttempt == 0 ||
      (now - lastPublishAttempt >= PUBLISH_RETRY_MS);

    if ((firstOrReconnect || regular) && retryReady) {
      publishSensorData();
    }
  }
  delay(20);
}
