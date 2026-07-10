# ESP32 IoT Energy Monitoring & Fault-Tolerant Data Logger

A simulated ESP32 firmware project for real-time sensor monitoring, built and tested entirely using Wokwi's ESP32 simulator (no physical hardware required).

## Overview

This project implements an IoT sensor node that reads analog sensor data (simulating current and voltage sensors), processes it with filtering and calibration, and transmits it reliably over MQTT — with local buffering to handle network interruptions.

## Features

- Real-time ADC sensor sampling
- Signal filtering (exponential moving average) and calibration
- Anomaly/outlier detection on sensor readings
- FreeRTOS-based task architecture (separate sampling and transmission tasks)
- Local circular buffer for store-and-forward during network outages
- Data integrity via sequence numbering and timestamps
- Modbus TCP communication (with a Python-simulated Modbus slave)
- MQTT publish to a public broker
- OTA (Over-The-Air) firmware update support
- Hardware watchdog and basic fault-state handling

## Tech Stack

ESP32 (simulated via Wokwi) · C/C++ (Arduino framework) · FreeRTOS · Modbus TCP · MQTT · PlatformIO · Python · Node-RED

## How to Run

1. Install PlatformIO in VS Code
2. Build the firmware: `pio run`
3. Download Wokwi CLI and set your `WOKWI_CLI_TOKEN` environment variable
4. Run: `wokwi-cli --timeout 15000 --serial-log-file output.log`