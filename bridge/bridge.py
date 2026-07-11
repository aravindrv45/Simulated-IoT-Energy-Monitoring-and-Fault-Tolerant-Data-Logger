"""
bridge.py -- relays raw Modbus RTU bytes between two Wokwi RFC2217 ports.

Stands in for the physical RS-485 wire between the gateway (ESP32 #1) and
the slave (ESP32 #2), which now live in two separate Wokwi simulations.

Usage:
    1. Start the gateway-node simulation (wokwi-cli in gateway-node/)
    2. Start the slave-node simulation   (wokwi-cli in slave-node/)
    3. Run this script:  python bridge.py
    4. Watch this console -- every relayed frame is printed with a
       timestamp and byte count, so you can confirm traffic is flowing
       even without a readable Serial Monitor on either node.
"""

import serial
import threading
import time
from datetime import datetime

GATEWAY_URL = "rfc2217://localhost:4000"
SLAVE_URL = "rfc2217://localhost:4001"
BAUD = 9600
CONNECT_RETRY_SECONDS = 2


def ts():
    return datetime.now().strftime("%H:%M:%S.%f")[:-3]


def connect(url, label):
    while True:
        try:
            port = serial.serial_for_url(url, baudrate=BAUD, timeout=0)
            print(f"[{ts()}] connected to {label} ({url})")
            return port
        except Exception as e:
            print(f"[{ts()}] waiting for {label} ({url}) -- {e}")
            time.sleep(CONNECT_RETRY_SECONDS)


def relay(src, dst, label):
    while True:
        try:
            n = src.in_waiting
            if n:
                data = src.read(n)
                dst.write(data)
                print(f"[{ts()}] {label}: {len(data)} bytes  {data.hex(' ')}")
            else:
                time.sleep(0.005)
        except Exception as e:
            print(f"[{ts()}] {label} error: {e}")
            time.sleep(1)


def main():
    print("Modbus RTU bridge starting...")
    gateway = connect(GATEWAY_URL, "gateway (port 4000)")
    slave = connect(SLAVE_URL, "slave (port 4001)")
    print("Both ends connected. Relaying Modbus RTU frames. Ctrl+C to stop.\n")

    threading.Thread(target=relay, args=(gateway, slave, "gateway -> slave"), daemon=True).start()
    threading.Thread(target=relay, args=(slave, gateway, "slave -> gateway"), daemon=True).start()

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nStopping bridge.")


if __name__ == "__main__":
    main()
