#include <Arduino.h>
#include <DHT.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>

#define POT_V_PIN 34
#define POT_C_PIN 35
#define DHT_PIN 23
#define DHT_TYPE DHT22

#define BTN_THERMAL_PIN 25
#define BTN_VIBE_PIN 26

DHT dht(DHT_PIN, DHT_TYPE);
Adafruit_MPU6050 mpu;
bool mpuReady = false;

// Global Modbus Holding Registers Matrix Array Map
// [0]=Voltage(x10), [1]=Current(x100), [2]=Power(W), [3]=Temp(x10), [4]=Vibe(x100)
uint16_t modbusRegisters[5];

uint16_t calculateModbusCRC(uint8_t *buffer, int length) {
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < length; i++) {
    crc ^= (uint16_t)buffer[i];
    for (int bit = 8; bit != 0; bit--) {
      if ((crc & 0x0001) != 0) {
        crc >>= 1;
        crc ^= 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, 16, 17); // Hardware mapping RX2=16, TX2=17
  
  pinMode(POT_V_PIN, INPUT);
  pinMode(POT_C_PIN, INPUT);
  pinMode(BTN_THERMAL_PIN, INPUT_PULLUP);
  pinMode(BTN_VIBE_PIN, INPUT_PULLUP);

  dht.begin();
  if(!mpu.begin()) {
    Serial.println("[SLAVE INITIALIZER] Warning: MPU6050 simulation hook missing or unaligned.");
    mpuReady = false;
  } else {
    mpuReady = true;
  }
  Serial.println("[SLAVE INITIALIZER] Industrial Modbus Instrumentation Node Armed.");
}

void loop() {
  // 1. Process Baseline Telemetry Data Ingestion
  float voltageVal = (analogRead(POT_V_PIN) / 4095.0) * 440.0;
  float currentVal = (analogRead(POT_C_PIN) / 4095.0) * 100.0;
  float powerVal = voltageVal * currentVal;
  
  float tempVal = dht.readTemperature();
  if (isnan(tempVal)) tempVal = 24.5; // Fail-safe fallback value

  sensors_event_t a, g, temp;
  if (mpuReady) {
    mpu.getEvent(&a, &g, &temp);
  } else {
    // MPU6050 never initialized -- fall back to a resting-gravity reading
    // instead of leaving a/g/temp as uninitialized stack memory.
    a.acceleration.x = 0.0;
    a.acceleration.y = 0.0;
    a.acceleration.z = 9.81;
  }
  
  // Compute absolute composite acceleration vector magnitude minus raw earth gravitational baseline
  float totalAcceleration = sqrt(a.acceleration.x * a.acceleration.x + 
                                 a.acceleration.y * a.acceleration.y + 
                                 a.acceleration.z * a.acceleration.z);
  float vibeVal = abs(totalAcceleration - 9.81);
  if (vibeVal < 0.05 || isnan(vibeVal)) vibeVal = 0.02; // Eliminate floating environmental simulation noise

  // 2. Process Intentional Manual Fault Injection Overrides
  if (digitalRead(BTN_THERMAL_PIN) == LOW) {
    tempVal = 88.4 + random(0, 40) / 10.0; // Overrides register map into runaway thermal hazard state
    Serial.println("[FAULT INJECTOR] Active State: Forcing runaway thermal warning payload.");
  }
  
  if (digitalRead(BTN_VIBE_PIN) == LOW) {
    vibeVal = 2.85 + random(0, 150) / 100.0; // Overrides register map into bearing destruction vibrations
    Serial.println("[FAULT INJECTOR] Active State: Injecting heavy micro-shock harmonic damage.");
  }

  // 3. Serialize Data Structs Cleanly Into Standard 16-Bit Register Bitmaps
  modbusRegisters[0] = (uint16_t)(voltageVal * 10.0);
  modbusRegisters[1] = (uint16_t)(currentVal * 100.0);
  modbusRegisters[2] = (uint16_t)(powerVal);
  modbusRegisters[3] = (uint16_t)(tempVal * 10.0);
  modbusRegisters[4] = (uint16_t)(vibeVal * 100.0);

  // 4. Evaluate and Process Inbound Serial Queries Matching Request Struct Dimensions
  if (Serial2.available() >= 8) {
    uint8_t queryFrame[8];
    Serial2.readBytes(queryFrame, 8);

    // Filter by Device Station ID 1 and Read Function Code 03
    if (queryFrame[0] == 0x01 && queryFrame[1] == 0x03) {
      uint16_t registerPointer = (queryFrame[2] << 8) | queryFrame[3];
      uint16_t requestedLength = (queryFrame[4] << 8) | queryFrame[5];

      if (registerPointer == 0x0000 && requestedLength <= 5) {
        uint8_t responseSize = 5 + (requestedLength * 2);
        uint8_t responseFrame[responseSize];

        responseFrame[0] = 0x01; // Station Identity Echo
        responseFrame[1] = 0x03; // Function Code Echo
        responseFrame[2] = requestedLength * 2; // Exact data payload bytes length count

        int offset = 3;
        for (int i = 0; i < requestedLength; i++) {
          responseFrame[offset++] = modbusRegisters[registerPointer + i] >> 8;
          responseFrame[offset++] = modbusRegisters[registerPointer + i] & 0xFF;
        }

        uint16_t integrityCheckSum = calculateModbusCRC(responseFrame, responseSize - 2);
        responseFrame[responseSize - 2] = integrityCheckSum & 0xFF;
        responseFrame[responseSize - 1] = integrityCheckSum >> 8;

        Serial2.write(responseFrame, responseSize);
      }
    }
  }
  delay(15);
}