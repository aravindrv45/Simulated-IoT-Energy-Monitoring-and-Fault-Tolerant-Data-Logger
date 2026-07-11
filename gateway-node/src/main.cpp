#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ModbusMaster.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "esp_task_wdt.h"

#define OUTAGE_PIN 4
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WiFiClient espClient;
PubSubClient mqttClient(espClient);
ModbusMaster energyMeter; 

// ---- Cloud Stack Constants ----
const char* ssid = "Wokwi-GUEST";
const char* password = "";
const char* mqttServer = "broker.hivemq.com";
const int mqttPort = 1883;
const char* mqttTopicTelemetry = "industrial/gateway-01/telemetry";
const char* mqttTopicStatus    = "industrial/gateway-01/status";
const char* mqttTopicAlerts    = "industrial/gateway-01/alerts";

// ---- Advanced Filter & Calibration Boundaries ----
const float alpha = 0.25;
float f_V = 0.0, f_A = 0.0, f_T = 0.0, f_G = 0.0;
bool filtersReady = false;

struct SystemTelemetry {
  uint32_t seq;
  float voltage;
  float current;
  float power;
  float temperature;
  float vibration;
  uint32_t ts;
};

QueueHandle_t dataQueue;
SemaphoreHandle_t storageMutex;
volatile bool netOnline = false;
volatile bool lastNetOnline = false;
uint32_t sequenceNum = 0;
int storedCount = 0;

void refreshDisplay(float v, float a, float t, float g, const char* label, int cached) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  // Dashboard Status Ribbon
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.printf("NET: %s", label);
  display.setCursor(82, 0);
  display.printf("BUF:%d", cached);
  display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

  // Structural Telemetry Layout Grid
  display.setCursor(0, 15);
  display.printf("Line: %.1fV | %.2fA", v, a);
  display.setCursor(0, 31);
  display.printf("Core Temp: %.1f C", t);
  display.setCursor(0, 47);
  display.printf("Bearing G: %.2f g", g);
  
  // Highlighting Abnormal Parameters Screen Alerts
  if (t > 75.0 || g > 1.5) {
    display.fillRect(0, 56, 128, 8, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(4, 56);
    display.print("!!! CRITICAL ALARM !!!");
  }
  display.display();
}

void storeToFlashFallback(SystemTelemetry &packet) {
  if (xSemaphoreTake(storageMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    File logFile = LittleFS.open("/offline_telemetry.dat", FILE_APPEND);
    if (logFile) {
      logFile.write((uint8_t*)&packet, sizeof(SystemTelemetry));
      logFile.close();
      storedCount++;
      Serial.println("[GATEWAY CORE] Link broken. Local data packet saved to LittleFS memory storage.");
    }
    xSemaphoreGive(storageMutex);
  }
}

// ---- Modbus Polling Engine Task ----
void modbusTask(void *param) {
  esp_task_wdt_add(NULL);
  Serial2.begin(9600, SERIAL_8N1, 16, 17); // Hardware mapping RX2=16, TX2=17
  energyMeter.begin(1, Serial2);

  for (;;) {
    esp_task_wdt_reset();
    
    // Command request for 5 consecutive registers starting at 0x0000
    uint8_t transaction = energyMeter.readHoldingRegisters(0x0000, 5);
    
    if (transaction == energyMeter.ku8MBSuccess) {
      float rawV = energyMeter.getResponseBuffer(0) / 10.0;
      float rawA = energyMeter.getResponseBuffer(1) / 100.0;
      float rawW = energyMeter.getResponseBuffer(2);
      float rawT = energyMeter.getResponseBuffer(3) / 10.0;
      float rawG = energyMeter.getResponseBuffer(4) / 100.0;

      // Digital Smoothing Signal Pipeline (EMA)
      if (!filtersReady) {
        f_V = rawV; f_A = rawA; f_T = rawT; f_G = rawG;
        filtersReady = true;
      } else {
        f_V = (alpha * rawV) + ((1.0 - alpha) * f_V);
        f_A = (alpha * rawA) + ((1.0 - alpha) * f_A);
        f_T = (alpha * rawT) + ((1.0 - alpha) * f_T);
        f_G = (alpha * rawG) + ((1.0 - alpha) * f_G);
      }

      // Check Machine Anomalies and Alert Real-time Broker
      if (f_T > 75.0 || f_G > 1.5) {
        Serial.printf("[EDGE SHIELD] Anomaly triggered! Temp: %.1f C, Vibe: %.2fg\n", f_T, f_G);
        if (mqttClient.connected()) {
          char alertPayload[128];
          snprintf(alertPayload, sizeof(alertPayload), "{\"status\":\"CRITICAL\",\"temp\":%.1f,\"vibe\":%.2f}", f_T, f_G);
          mqttClient.publish(mqttTopicAlerts, alertPayload);
        }
      }

      SystemTelemetry currentMetrics = {
        .seq = sequenceNum++, .voltage = f_V, .current = f_A, .power = rawW, 
        .temperature = f_T, .vibration = f_G, .ts = (uint32_t)millis()
      };

      xQueueSend(dataQueue, &currentMetrics, pdMS_TO_TICKS(20));
      refreshDisplay(f_V, f_A, f_T, f_G, netOnline ? "ONLINE" : "OFFLINE", storedCount);

    } else {
      Serial.printf("[GATEWAY COMMS] Modbus exception on wire network: 0x%02X\n", transaction);
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

// ---- MQTT Transmit and Cache Recovery Task ----
void cloudTask(void *param) {
  esp_task_wdt_add(NULL);
  SystemTelemetry outbound;

  for (;;) {
    esp_task_wdt_reset();
    mqttClient.loop();

    bool linkDropForced = (digitalRead(OUTAGE_PIN) == LOW);
    netOnline = (WiFi.status() == WL_CONNECTED) && mqttClient.connected() && !linkDropForced;

    if (netOnline != lastNetOnline) {
      if (netOnline) {
        Serial.println("[MQTT CORE] Cloud connection online. Restoring pipeline.");
        mqttClient.publish(mqttTopicStatus, "online", true);
      } else {
        Serial.println("[MQTT CORE] Connection offline. Redirecting stream to non-volatile local disk storage.");
        if(mqttClient.connected()) mqttClient.publish(mqttTopicStatus, "offline", true);
      }
      lastNetOnline = netOnline;
    }

    if (xQueueReceive(dataQueue, &outbound, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (netOnline) {
        char jsonBuffer[256];
        snprintf(jsonBuffer, sizeof(jsonBuffer), 
                 "{\"seq\":%u,\"V\":%.1f,\"A\":%.2f,\"W\":%.1f,\"temp\":%.1f,\"vibe\":%.2f,\"ts\":%u}",
                 outbound.seq, outbound.voltage, outbound.current, outbound.power, outbound.temperature, outbound.vibration, outbound.ts);
        mqttClient.publish(mqttTopicTelemetry, jsonBuffer);
        Serial.print("[CLOUD TRANSMIT] "); Serial.println(jsonBuffer);
      } else {
        storeToFlashFallback(outbound);
      }
    }

    // Flush Backlogged Storage Files Out Safely Upon Internet Restoral
    if (netOnline && LittleFS.exists("/offline_telemetry.dat")) {
      if (xSemaphoreTake(storageMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        File logFile = LittleFS.open("/offline_telemetry.dat", FILE_READ);
        if (logFile) {
          Serial.println("[RECOVERY PROCESS] Flushing structural database backlogs to server network...");
          while (logFile.available()) {
            SystemTelemetry backup;
            logFile.read((uint8_t*)&backup, sizeof(SystemTelemetry));
            char jsonBuffer[280];
            snprintf(jsonBuffer, sizeof(jsonBuffer), 
                     "{\"seq\":%u,\"V\":%.1f,\"A\":%.2f,\"W\":%.1f,\"temp\":%.1f,\"vibe\":%.2f,\"ts\":%u,\"buffered\":true}",
                     backup.seq, backup.voltage, backup.current, backup.power, backup.temperature, backup.vibration, backup.ts);
            mqttClient.publish(mqttTopicTelemetry, jsonBuffer);
          }
          logFile.close();
          LittleFS.remove("/offline_telemetry.dat");
          storedCount = 0;
          Serial.println("[RECOVERY PROCESS] Flush complete. Local non-volatile database cleared.");
        }
        xSemaphoreGive(storageMutex);
      }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(OUTAGE_PIN, INPUT_PULLUP);
  Wire.begin(21, 22); // Your exact SDA/SCL Pins configuration
  
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println("OLED init failed"); 
  }
  display.clearDisplay();
  display.display();

  if (!LittleFS.begin(true)) { Serial.println("LittleFS Mount Failed"); return; }

  esp_task_wdt_init(3, true);
  storageMutex = xSemaphoreCreateMutex();
  dataQueue = xQueueCreate(20, sizeof(SystemTelemetry));

  WiFi.begin(ssid, password);
  mqttClient.setServer(mqttServer, mqttPort);

  xTaskCreatePinnedToCore(modbusTask, "ModbusTask", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(cloudTask, "CloudTask", 4096, NULL, 1, NULL, 0);
}

void loop() { vTaskDelay(1000 / portTICK_PERIOD_MS); }