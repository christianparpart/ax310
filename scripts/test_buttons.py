#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import os
import sys
import glob

def find_1310_device():
    for uevent_path in glob.glob('/sys/class/hidraw/*/device/uevent'):
        with open(uevent_path, 'r') as f:
            if '07CA' in f.read() and '1310' in open(uevent_path).read():
                hidraw_name = uevent_path.split('/')[4]
                return f'/dev/{hidraw_name}'
    return None

dev_path = find_1310_device()
if not dev_path:
    print("Could not find the 07CA:1310 interface.")
    print("Make sure the AX310 is fully awake (its screen is on).")
    sys.exit(1)

print(f"Found AX310 Control Interface at {dev_path}")
print("Please press the physical buttons on the AX310 now!")
print("Waiting for events... (Press Ctrl+C to stop)")

try:
    with open(dev_path, 'rb', buffering=0) as f:
        while True:
            data = f.read(64)
            if data:
                print("Received: " + " ".join(f"{b:02x}" for b in data))
except PermissionError:
    print(f"Permission denied to read {dev_path}. Run with sudo!")
except KeyboardInterrupt:
    print("\nDone.")
