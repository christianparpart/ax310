# SPDX-License-Identifier: Apache-2.0
import os
import sys

for dev in os.listdir('/sys/class/hidraw'):
    with open(f'/sys/class/hidraw/{dev}/device/uevent', 'r') as f:
        uevent = f.read()
        if '07CA:1310' in uevent:
            print(f"Found 1310 at /dev/{dev}")
            os.system(f"sudo hexdump -C /dev/{dev} & PID=$!; sleep 5; kill $PID")
