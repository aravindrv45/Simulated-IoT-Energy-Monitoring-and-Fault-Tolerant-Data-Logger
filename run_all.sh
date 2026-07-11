#!/bin/bash

# --- Start Gateway Node ---
cd gateway-node && wokwi-cli . &
echo "Started Gateway Node"

# --- Start Slave Node ---
# We go back to the root first, then into the slave-node folder
cd ../slave-node && wokwi-cli . &
echo "Started Slave Node"

# --- Start the Bridge script ---
sleep 3
cd ../bridge && python3 bridge.py