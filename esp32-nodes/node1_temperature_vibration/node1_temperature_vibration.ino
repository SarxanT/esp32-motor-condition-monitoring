// Node 1: Temperature and vibration
// Wokwi ESP32 firmware; communicates through MQTT via the Python gateway.

/*
  Node 1: CLOUD-AUTHORITATIVE LEDs, no local health classification.
  Sensors: temperature (DS18B20), vibration indicator (MPU6050).
  MQTT sensor -> Python gateway -> ThingSpeak React/ThingHTTP -> TalkBack
  -> Python gateway -> MQTT command -> RGB LED -> acknowledgment.
  RGB common-anode, COM to 3V3, R=25 G=26 B=27.
*/

#include <WiFi.h>
#include <Wire.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <math.h>

// ============================================================
// 1. NETWORK SETTINGS
// These values tell the ESP32 WHICH Wi-Fi network and MQTT
// server to use. Keep the MQTT topic identical to the topic
// used by gateway.py (PROJECT_ID + /node1/data).
//
// The public broker is ONLY a message router. This sketch does
// not write to ThingSpeak directly.
// ============================================================
const char* WIFI_NAME = "Wokwi-GUEST";
const char* WIFI_PASSWORD = "";

// Public test broker: use synthetic/demo measurements only.
const char* MQTT_BROKER = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_TOPIC = "motor-lab/CHANGE_TO_A_UNIQUE_NONPERSONAL_ID/node1/data";
// Gateway sends *only simulated diagnostic LED commands* to this topic.
const char* MQTT_COMMAND_TOPIC = "motor-lab/CHANGE_TO_A_UNIQUE_NONPERSONAL_ID/node1/cmd";
// Node confirms which command it processed (gateway displays acknowledgments).
const char* MQTT_ACK_TOPIC = "motor-lab/CHANGE_TO_A_UNIQUE_NONPERSONAL_ID/node1/ack";
const char* MQTT_BOOT_TOPIC = "motor-lab/CHANGE_TO_A_UNIQUE_NONPERSONAL_ID/node1/boot";

// millis() uses milliseconds: 1000 ms = one second.
// Sensor reading, screen printing, and MQTT sending have
// INDEPENDENT intervals. Their events need not occur together.
const unsigned long PUBLISH_INTERVAL_MS = 2000;
const unsigned long PRINT_INTERVAL_MS   = 2000;
const unsigned long SENSOR_INTERVAL_MS  = 1000;
const unsigned long WIFI_RETRY_MS       = 5000;
const unsigned long MQTT_RETRY_MS       = 3000;
const unsigned long PUBLISH_RETRY_MS    = 3000;
// The retry values are MINIMUM waiting periods between attempts,
// not guarantees that a network connection succeeds in that time.

// ==================== SENSOR + RGB PINS =====================
#define TEMP_PIN 4
#define RED_PIN 25
#define GREEN_PIN 26
#define BLUE_PIN 27

// ============================================================
// 3. GLOBAL OBJECTS AND MEASUREMENTS
// OneWire + DallasTemperature communicate with DS18B20.
// Adafruit_MPU6050 communicates with the motion sensor over I2C.
// WiFiClient is the network transport used by PubSubClient.
//
// Global measurements hold the most recent sample. Both the
// Serial Monitor and MQTT use these SAME variables, but at
// different times according to their schedules.
// ============================================================
OneWire oneWire(TEMP_PIN);
DallasTemperature tempSensor(&oneWire);
Adafruit_MPU6050 mpu;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

float temperature = 0.0f;
float vibration = 0.0f;

// Publish immediately when MQTT connects; then every 2 seconds.
// After a failed publish, retry in 3 seconds instead of waiting 15.
bool publishPending = false;
bool sensorReady = false;
bool hasPublished = false;
unsigned long lastSuccessfulPublish = 0;
unsigned long lastPublishAttempt = 0;

// ======================= CLOUD RGB LED ========================
const bool RGB_COMMON_ANODE = true;  // Node 1: COM -> 3V3
// This LED displays ONLY the last received cloud command.
// No sensor threshold/health calculation runs on the ESP32.
enum LedState { WAIT_CLOUD, LED_HEALTHY, LED_WARNING, LED_ALARM, LED_TEST_BLUE };
LedState cloudLed = WAIT_CLOUD;

void rgbWrite(bool red, bool green, bool blue) {
  digitalWrite(RED_PIN,   (red   ^ RGB_COMMON_ANODE) ? HIGH : LOW);
  digitalWrite(GREEN_PIN, (green ^ RGB_COMMON_ANODE) ? HIGH : LOW);
  digitalWrite(BLUE_PIN,  (blue  ^ RGB_COMMON_ANODE) ? HIGH : LOW);
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
}

// ============================================================
// 5. CONNECTIVITY - RETRY WITHOUT A PERMANENT WAIT LOOP
// maintainWiFi() and maintainMQTT() are called repeatedly from
// loop(). When already connected, they return immediately.
// When disconnected, they attempt connections after a retry gap.
// IMPORTANT: Individual Wi-Fi or MQTT connection attempts may
// still take some real time; this isn't fully asynchronous I/O.
// ============================================================
// ============ RECEIVE CLOUD COMMAND (NO LOCAL DECISION) ========
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (String(topic) != MQTT_COMMAND_TOPIC) return;
  String command;
  for (unsigned int i=0; i<length; ++i) command += (char)payload[i];
  command.trim();
  if (command == "LED_GREEN") {
    cloudLed = LED_HEALTHY;
  } else if (command == "LED_YELLOW") {
    cloudLed = LED_WARNING;
  } else if (command == "LED_RED") {
    cloudLed = LED_ALARM;
  } else if (command == "LED_BLUE") {  // manual test only
    cloudLed = LED_TEST_BLUE;
  } else {
    Serial.print("Unknown cloud command: "); Serial.println(command);
    return;
  }
  Serial.print("Node 1 CLOUD COMMAND received: "); Serial.println(command);
  updateRGB();
  if (mqttClient.connected()) mqttClient.publish(MQTT_ACK_TOPIC, command.c_str());
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  static unsigned long lastAttempt = 0;
  if (lastAttempt != 0 && millis() - lastAttempt < WIFI_RETRY_MS) return;

  lastAttempt = millis();
  Serial.println("Connecting to Wokwi Wi-Fi...");
  WiFi.begin(WIFI_NAME, WIFI_PASSWORD, 6);
}

void maintainMQTT() {
  if (WiFi.status() != WL_CONNECTED || mqttClient.connected()) return;

  static unsigned long lastAttempt = 0;
  if (lastAttempt != 0 && millis() - lastAttempt < MQTT_RETRY_MS) return;

  lastAttempt = millis();
  String clientID = "ESP32-Node1-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  Serial.print("Connecting to MQTT... ");

  if (mqttClient.connect(clientID.c_str())) {
    Serial.println("connected!");
    // MQTT subscriptions are session-based; subscribe after every reconnect.
    // Resynchronize LED state after each Wokwi restart/reconnection.
    mqttClient.publish(MQTT_BOOT_TOPIC, "BOOT", false);
    if (mqttClient.subscribe(MQTT_COMMAND_TOPIC)) {
      Serial.print("Subscribed to commands: ");
      Serial.println(MQTT_COMMAND_TOPIC);
    } else {
      Serial.println("Command subscription failed.");
    }
    publishPending = true;  // Schedule first publish once sensor data is ready.
    lastPublishAttempt = 0;
    Serial.print("Topic: ");
    Serial.println(MQTT_TOPIC);
  } else {
    Serial.print("failed; MQTT state = ");
    Serial.println(mqttClient.state());
  }
}

// ============================================================
// 6. READ WOKWI SENSOR VALUES
// This function is scheduled once per second by loop().
// The sensor values do NOT originate from MQTT or ThingSpeak:
// the ESP32 reads its own simulated components in Wokwi.
// ============================================================
void readSensors() {
  // DS18B20 gives the simulated sensor temperature in Celsius.
  tempSensor.requestTemperatures();
  float reading = tempSensor.getTempCByIndex(0);
  if (reading != DEVICE_DISCONNECTED_C && reading >= -55.0f && reading <= 125.0f) {
    temperature = reading;
  } else {
    Serial.println("Warning: invalid DS18B20 reading; keeping last value.");
  }

  // MPU6050 returns x/y/z acceleration in m/s^2.
  sensors_event_t accel, gyro, sensorTemp;
  mpu.getEvent(&accel, &gyro, &sensorTemp);

  const float x = accel.acceleration.x;
  const float y = accel.acceleration.y;
  const float z = accel.acceleration.z;
  const float totalAcceleration = sqrtf(x * x + y * y + z * z);

  // Coursework-only vibration indicator: deviation from gravity.
  // This is NOT a true sampled vibration RMS measurement.
  vibration = fabsf(totalAcceleration - 9.81f);
}

// ================= PRINT RAW SENSOR READINGS ===================
void printStatus() {
  Serial.print("Node 1 temperature: "); Serial.print(temperature, 2);
  Serial.print(" C | vibration: "); Serial.print(vibration,3);
  Serial.println(" | health: calculated in PYTHON; LED: cloud command only");
}

// ============================================================
// 8. PUBLISH NODE 1 VALUES THROUGH THE MQTT BROKER
// Creates one JSON message with temperature and vibration;
// the Python gateway calculates and uploads health condition.
//
// Data route:
// Wokwi ESP32 -> broker.emqx.io -> gateway.py subscriber.
//
// A 'true' return from publish() confirms that the library
// accepted the outgoing message. Python printing NODE 1
// RECEIVED confirms the subscriber actually got the message.
// ============================================================
void publishSensorData() {
  if (!mqttClient.connected()) {
    Serial.println("MQTT upload skipped: broker not connected.");
    return;
  }

  JsonDocument doc;
  doc["temperature"] = temperature;
  doc["vibration"] = vibration;

  String payload;
  serializeJson(doc, payload);

  lastPublishAttempt = millis();
  if (mqttClient.publish(MQTT_TOPIC, payload.c_str())) {
    lastSuccessfulPublish = millis();
    hasPublished = true;
    publishPending = false;
    Serial.print("MQTT published: ");
    Serial.println(payload);
  } else {
    publishPending = true;
    Serial.println("MQTT publish failed; will retry in 3 seconds.");
  }
}

// ============================================================
// 9. STARTUP
// setup() runs once after reboot. Initialize Serial, the LED,
// both sensors, and MQTT settings. Wi-Fi connection is started;
// the main loop will keep checking its progress.
// ============================================================
void setup() {
  Serial.begin(115200);

  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);
  rgbOff();
  testRGBAtStartup();

  Serial.println("\n================================");
  Serial.println("NODE 1 - TEMPERATURE + VIBRATION");
  Serial.println("Network: MQTT to Python Gateway");
  Serial.println("================================");

  tempSensor.begin();
  Wire.begin(21, 22);

  if (!mpu.begin()) {
    Serial.println("ERROR: MPU6050 NOT FOUND");
    while (true) delay(100);
  }
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(onMqttMessage);
  // Limit the time a stalled broker can block a connection attempt.
  mqttClient.setSocketTimeout(2);
  mqttClient.setKeepAlive(30);
  maintainWiFi();
  Serial.println("Node 1 ready!");
}

// ============================================================
// 10. MAIN LOOP — THE SCHEDULER
// loop() runs repeatedly. millis() helps run each activity at
// its own frequency WITHOUT using delay(15000).
//
// TIMING SUMMARY (assuming network is available):
// - ~every 1 second: readSensors() only
// - ~every 2 seconds: printStatus() to ESP32 Serial Monitor
// - upon MQTT connection: publish the latest sensor values ASAP
// - ~every 2 seconds after successful publication: publish
// - every loop pass: update LED and service the MQTT connection
//
// The Python gateway receives publications asynchronously via
// the broker. Screen printing and Python receipt are NOT linked.
// ============================================================
void loop() {
  // Sensor sampling remains local. Health classification does NOT.
  // Timers are static so they remember timestamps across loops.
  // Subtracting unsigned millis() values also handles wraparound.
  static unsigned long lastSensor = 0;
  if (millis() - lastSensor >= SENSOR_INTERVAL_MS) {
    lastSensor = millis();
    readSensors();
    sensorReady = true;
  }
  updateRGB();

  // Keep MQTT connection alive and process network traffic.
  maintainWiFi();
  maintainMQTT();
  // MQTT loop keeps the connection alive and processes packets.
  // Call it frequently instead of sleeping for 15 seconds.
  if (mqttClient.connected()) mqttClient.loop();

  // Print measurements every 2 seconds.
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= PRINT_INTERVAL_MS) {
    lastPrint = millis();
    printStatus();
  }

  // One immediate upload after (re)connection, then every 2 seconds.
  // MQTT publishing does not share ThingSpeak's 15-second channel limit.
  // If a publish fails, retry in 3 seconds while still reading sensors.
  if (mqttClient.connected() && sensorReady) {
    const unsigned long now = millis();
    // First successful connection/reconnection schedules a send.
    const bool firstOrReconnect = publishPending || !hasPublished;
    // Only a SUCCESSFUL publish resets the 15-second countdown.
    const bool regularUpload = hasPublished &&
      (now - lastSuccessfulPublish >= PUBLISH_INTERVAL_MS);
    // After a failed attempt, wait 3 seconds before trying again.
    // A connection may still fail independently of our timers.
    const bool retryReady = lastPublishAttempt == 0 ||
      (now - lastPublishAttempt >= PUBLISH_RETRY_MS);

    if ((firstOrReconnect || regularUpload) && retryReady) {
      publishSensorData();
    }
  }

  // Small loop pause reduces busy-waiting. The 15-second interval
  // is NOT implemented by this delay; it is controlled by millis().
  delay(20);
}
