#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "esp_task_wdt.h"

// ---- Wi-Fi (Wokwi virtual network) ----
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// ---- MQTT broker ----
const char* mqttServer = "broker.hivemq.com";
const int mqttPort = 1883;
const char* mqttTopicTelemetry = "wokwi/energy-meter/telemetry";
const char* mqttTopicControl   = "wokwi/energy-meter/control";
const char* mqttTopicStatus    = "wokwi/energy-meter/status";   // NEW: retained + LWT-backed online/offline
const char* mqttTopicAlerts    = "wokwi/energy-meter/alerts";   // NEW: spike/anomaly events
const char* mqttTopicHealth    = "wokwi/energy-meter/health";   // NEW: periodic diagnostics for HMI (buffer/link state)
const char* mqttClientId       = "esp32-energy-meter-01";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// ---- Calibration constants ----
float currentOffset = 0.0;
float currentGain = 1.0;
float voltageOffset = 0.0;
float voltageGain = 1.0;

// ---- Filter state ----
float filteredCurrent = 0.0;
float filteredVoltage = 0.0;
const float alpha = 0.2;
bool filterInitialized = false;

// ---- Anomaly thresholds (NOW MUTABLE — settable from HMI dashboard) ----
float MAX_CURRENT = 100.0;
float MAX_VOLTAGE = 440.0;

// ---- Data structure passed between tasks ----
struct SensorReading {
  uint32_t seq;
  float current;
  float voltage;
  uint32_t timestamp;
};

// ---- Circular buffer for store-and-forward ----
#define BUFFER_SIZE 20
SensorReading circularBuffer[BUFFER_SIZE];
int bufHead = 0;
int bufCount = 0;
uint32_t seqCounter = 0;

// ---- RTOS objects ----
QueueHandle_t sensorQueue;
TaskHandle_t sampleTaskHandle;
TaskHandle_t processTaskHandle;
SemaphoreHandle_t bufferMutex;

// ---- Network Resilience state ----
volatile bool networkAvailable = false;
volatile bool lastNetworkAvailable = false;

// ---- Outage control toggle (Hardware Interrupt & Software MQTT Support) ----
#define OUTAGE_BUTTON_PIN 4
volatile bool simulateOutage = false;
uint32_t lastButtonPress = 0;

// Hardware button ISR
void IRAM_ATTR onOutageButtonPress() {
  uint32_t now = millis();
  if (now - lastButtonPress > 300) { // debounce
    simulateOutage = !simulateOutage;
    lastButtonPress = now;
  }
}

// ---- Publish retained online/offline status ----
void publishStatus(bool online) {
  if (mqttClient.connected()) {
    mqttClient.publish(mqttTopicStatus, online ? "online" : "offline", true /* retained */);
  }
}

// ---- Publish a spike/anomaly alert to the dashboard ----
void publishAlert(const char* reason, float current, float voltage) {
  if (!mqttClient.connected()) return;
  char payload[160];
  snprintf(payload, sizeof(payload),
           "{\"type\":\"%s\",\"current\":%.2f,\"voltage\":%.2f,\"ts\":%u}",
           reason, current, voltage, millis());
  mqttClient.publish(mqttTopicAlerts, payload);
  Serial.print("[MQTT ALERT] ");
  Serial.println(payload);
}

// ---- Very small JSON value grabber (avoids pulling in ArduinoJson for one field) ----
bool extractFloatField(const char* json, const char* key, float* outVal) {
  char pattern[32];
  snprintf(pattern, sizeof(pattern), "\"%s\"", key);
  const char* p = strstr(json, pattern);
  if (!p) return false;
  p = strchr(p, ':');
  if (!p) return false;
  *outVal = atof(p + 1);
  return true;
}

// HMI Remote Dashboard Subscriber Callback
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, mqttTopicControl) != 0 || length == 0) return;

  char msg[192];
  unsigned int n = min(length, (unsigned int)(sizeof(msg) - 1));
  memcpy(msg, payload, n);
  msg[n] = '\0';

  // Legacy single-char outage toggle (still supported)
  if (n == 1 && (msg[0] == '1' || msg[0] == '0')) {
    simulateOutage = (msg[0] == '1');
    Serial.println(simulateOutage
      ? ">>> REMOTE COMMAND: Outage Injected via HMI Dashboard"
      : ">>> REMOTE COMMAND: Outage Cleared via HMI Dashboard");
    return;
  }

  // JSON command support: {"cmd":"outage","state":1}  or  {"cmd":"threshold","maxCurrent":90,"maxVoltage":420}
  if (strstr(msg, "\"cmd\"") == NULL) return;

  if (strstr(msg, "\"outage\"")) {
    float state = 0;
    if (extractFloatField(msg, "state", &state)) {
      simulateOutage = (state != 0);
      Serial.println(simulateOutage
        ? ">>> REMOTE COMMAND: Outage Injected via HMI Dashboard"
        : ">>> REMOTE COMMAND: Outage Cleared via HMI Dashboard");
    }
  } else if (strstr(msg, "\"threshold\"")) {
    float newMaxCurrent, newMaxVoltage;
    bool gotCurrent = extractFloatField(msg, "maxCurrent", &newMaxCurrent);
    bool gotVoltage = extractFloatField(msg, "maxVoltage", &newMaxVoltage);
    if (gotCurrent) MAX_CURRENT = newMaxCurrent;
    if (gotVoltage) MAX_VOLTAGE = newMaxVoltage;
    Serial.print(">>> REMOTE COMMAND: Thresholds updated -> MAX_CURRENT=");
    Serial.print(MAX_CURRENT);
    Serial.print(" MAX_VOLTAGE=");
    Serial.println(MAX_VOLTAGE);
  }
}

// ---- Connect to Wi-Fi with a timeout ----
void connectWiFi() {
  Serial.print("Connecting to Wi-Fi");
  WiFi.begin(ssid, password);

  uint32_t startAttempt = millis();
  const uint32_t WIFI_TIMEOUT_MS = 15000;

  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startAttempt > WIFI_TIMEOUT_MS) {
      Serial.println("\nWiFi connection FAILED (timeout) — running offline.");
      return;
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Wi-Fi connected, IP = ");
  Serial.println(WiFi.localIP());
}

// ---- Connect / reconnect to MQTT broker (non-blocking style) ----
// NOTE: mqttClient.connect() is itself a BLOCKING call (DNS lookup + waiting
// for CONNACK). Against a public broker this can occasionally take a couple
// of seconds. If that happens right before the 3-second watchdog deadline,
// ProcessTask never makes it back to esp_task_wdt_reset() in time and the
// chip reboots mid-connect — which is why status/health/control never came
// up on the very first boot. Two guards fix this without touching the
// intentional 3-second watchdog budget:
//   1. setSocketTimeout() bounds how long connect() can block internally.
//   2. A cooldown between attempts stops back-to-back blocking retries from
//      compounding, and we feed the watchdog immediately after the call
//      returns rather than waiting for the next loop iteration.
#define MQTT_RECONNECT_COOLDOWN_MS 3000
uint32_t lastMqttAttempt = 0;

bool connectMQTT() {
  if (mqttClient.connected()) return true;
  if (WiFi.status() != WL_CONNECTED) return false;

  uint32_t now = millis();
  if (now - lastMqttAttempt < MQTT_RECONNECT_COOLDOWN_MS) {
    return false; // still cooling down from the last attempt
  }
  lastMqttAttempt = now;

  Serial.print("Connecting to MQTT broker...");
  // LWT: if this client disconnects uncleanly, the broker publishes "offline"
  // (retained) on mqttTopicStatus on our behalf — this is what lets the
  // dashboard detect a Wokwi sim stopping abruptly, not just a clean disconnect.
  bool ok = mqttClient.connect(
    mqttClientId,
    NULL, NULL,                 // no username/password
    mqttTopicStatus, 1, true,   // willTopic, willQos, willRetain
    "offline"                   // willMessage
  );
  esp_task_wdt_reset(); // feed the watchdog immediately — connect() just blocked for a while

  if (ok) {
    Serial.println("connected");
    mqttClient.subscribe(mqttTopicControl);
    publishStatus(true); // announce online only AFTER a successful connect
    return true;
  } else {
    Serial.print("failed, rc=");
    Serial.println(mqttClient.state());
    return false;
  }
}

void updateNetworkState() {
  bool linkUp = (WiFi.status() == WL_CONNECTED) && !simulateOutage;
  bool wasConnected = mqttClient.connected();
  networkAvailable = linkUp && (wasConnected || connectMQTT());

  if (networkAvailable != lastNetworkAvailable) {
    if (networkAvailable) {
      Serial.println(">>> NETWORK RESTORED — resuming fast-path telemetry, flushing backlog");
    } else {
      Serial.println(">>> NETWORK LOST — switching to store-and-forward buffering");
      // Manual outage (button/HMI) or Wi-Fi loss while MQTT session is still
      // technically alive won't trigger the broker's LWT, so publish it ourselves.
      if (wasConnected) publishStatus(false);
    }
    lastNetworkAvailable = networkAvailable;
  }
}

void bufferPush(SensorReading &reading) {
  if (xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    circularBuffer[bufHead] = reading;
    bufHead = (bufHead + 1) % BUFFER_SIZE;
    if (bufCount < BUFFER_SIZE) {
      bufCount++;
    } else {
      Serial.println("BUFFER FULL: oldest reading overwritten");
    }
    xSemaphoreGive(bufferMutex);
  }
}

void buildPayload(SensorReading &reading, char *out, size_t outSize) {
  snprintf(out, outSize,
           "{\"seq\":%u,\"current\":%.2f,\"voltage\":%.2f,\"ts\":%u}",
           reading.seq, reading.current, reading.voltage, reading.timestamp);
}

bool transmitReading(SensorReading &reading) {
  if (!networkAvailable) {
    return false;
  }

  char payload[128];
  buildPayload(reading, payload, sizeof(payload));

  bool ok = mqttClient.publish(mqttTopicTelemetry, payload);
  if (ok) {
    Serial.print("[MQTT TX] ");
    Serial.println(payload);
  } else {
    Serial.println("[MQTT TX] publish failed");
  }
  return ok;
}

void flushBuffer() {
  if (xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

  int oldestIndex = (bufHead - bufCount + BUFFER_SIZE) % BUFFER_SIZE;
  while (bufCount > 0) {
    SensorReading reading = circularBuffer[oldestIndex];
    if (transmitReading(reading)) {
      oldestIndex = (oldestIndex + 1) % BUFFER_SIZE;
      bufCount--;
    } else {
      break;
    }
  }

  xSemaphoreGive(bufferMutex);
}

void sampleTask(void *param) {
  esp_task_wdt_add(NULL);

  for (;;) {
    esp_task_wdt_reset();

    int rawCurrent = analogRead(34);
    int rawVoltage = analogRead(35);

    float scaledCurrent = (rawCurrent / 4095.0) * 100.0;
    float scaledVoltage = (rawVoltage / 4095.0) * 440.0;

    scaledCurrent = (scaledCurrent * currentGain) + currentOffset;
    scaledVoltage = (scaledVoltage * voltageGain) + voltageOffset;

    if (!filterInitialized) {
      filteredCurrent = scaledCurrent;
      filteredVoltage = scaledVoltage;
      filterInitialized = true;
    } else {
      filteredCurrent = alpha * scaledCurrent + (1 - alpha) * filteredCurrent;
      filteredVoltage = alpha * scaledVoltage + (1 - alpha) * filteredVoltage;
    }

    if (filteredCurrent < 0 || filteredCurrent > MAX_CURRENT ||
        filteredVoltage < 0 || filteredVoltage > MAX_VOLTAGE) {
      Serial.println("ANOMALY: reading out of range, discarding");
      publishAlert("spike", filteredCurrent, filteredVoltage); // NEW: notify dashboard
      vTaskDelay(1000 / portTICK_PERIOD_MS);
      continue;
    }

    SensorReading reading;
    reading.seq = seqCounter++;
    reading.current = filteredCurrent;
    reading.voltage = filteredVoltage;
    reading.timestamp = millis();

    if (xQueueSend(sensorQueue, &reading, pdMS_TO_TICKS(100)) != pdTRUE) {
      Serial.println("WARNING: Queue full, reading dropped");
    }

    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

#define STATUS_INTERVAL_MS 3000
uint32_t lastStatusPrint = 0;

void printStatus() {
  uint32_t now = millis();
  if (now - lastStatusPrint < STATUS_INTERVAL_MS) return;
  lastStatusPrint = now;

  Serial.print("[STATUS] wifi=");
  Serial.print(WiFi.status() == WL_CONNECTED ? "UP" : "DOWN");
  Serial.print(" mqtt=");
  Serial.print(mqttClient.connected() ? "UP" : "DOWN");
  Serial.print(" simulateOutage=");
  Serial.print(simulateOutage ? "ON" : "OFF");
  Serial.print(" networkAvailable=");
  Serial.print(networkAvailable ? "YES" : "NO");
  Serial.print(" bufCount=");
  Serial.print(bufCount);
  Serial.print("/");
  Serial.print(BUFFER_SIZE);
  Serial.print(" seq=");
  Serial.println(seqCounter);

  if (mqttClient.connected()) {
    char payload[192];
    snprintf(payload, sizeof(payload),
             "{\"wifi\":%d,\"mqtt\":%d,\"outage\":%d,\"networkAvailable\":%d,"
             "\"bufCount\":%d,\"bufSize\":%d,\"seq\":%u,"
             "\"maxCurrent\":%.2f,\"maxVoltage\":%.2f}",
             WiFi.status() == WL_CONNECTED ? 1 : 0,
             mqttClient.connected() ? 1 : 0,
             simulateOutage ? 1 : 0,
             networkAvailable ? 1 : 0,
             bufCount, BUFFER_SIZE, seqCounter,
             MAX_CURRENT, MAX_VOLTAGE);
    mqttClient.publish(mqttTopicHealth, payload);
  }
}

void processTask(void *param) {
  esp_task_wdt_add(NULL);
  SensorReading reading;

  for (;;) {
    esp_task_wdt_reset();

    mqttClient.loop();
    updateNetworkState();
    printStatus();

    if (xQueueReceive(sensorQueue, &reading, pdMS_TO_TICKS(200)) == pdTRUE) {
      if (networkAvailable && bufCount == 0) {
        if (!transmitReading(reading)) {
          bufferPush(reading);
        }
      } else {
        bufferPush(reading);
      }
    }

    if (networkAvailable && bufCount > 0) {
      flushBuffer();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("--- SYSTEM BOOT ---");

  pinMode(OUTAGE_BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(OUTAGE_BUTTON_PIN), onOutageButtonPress, FALLING);

  esp_task_wdt_init(3, true);

  bufferMutex = xSemaphoreCreateMutex();

  connectWiFi();
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setSocketTimeout(2); // seconds — keeps connect()/read() well under the 3s watchdog budget

  sensorQueue = xQueueCreate(10, sizeof(SensorReading));
  if (sensorQueue == NULL) {
    Serial.println("ERROR: Failed to create queue");
    while (1);
  }

  xTaskCreatePinnedToCore(sampleTask, "SampleTask", 4096, NULL, 2, &sampleTaskHandle, 1);
  xTaskCreatePinnedToCore(processTask, "ProcessTask", 4096, NULL, 1, &processTaskHandle, 0);
}

void loop() {
  vTaskDelay(500 / portTICK_PERIOD_MS);
}
