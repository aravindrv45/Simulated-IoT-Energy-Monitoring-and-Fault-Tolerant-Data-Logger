#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("--- SYSTEM BOOT ---");
}

void loop() {
  int rawCurrent = analogRead(34);
  int rawVoltage = analogRead(35);

  float current = (rawCurrent / 4095.0) * 100.0;
  float voltage = (rawVoltage / 4095.0) * 440.0;

  Serial.print("Current: ");
  Serial.print(current);
  Serial.print("A, Voltage: ");
  Serial.print(voltage);
  Serial.println("V");

  delay(1000);
}