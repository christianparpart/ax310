# SPDX-License-Identifier: Apache-2.0
import usb.core
import usb.util
import sys

dev = usb.core.find(idVendor=0x07ca, idProduct=0x0310)
if dev is None:
    print("Device not found")
    sys.exit(1)

for cfg in dev:
    print(f"ConfigurationValue: {cfg.bConfigurationValue}")
    for intf in cfg:
        print(f"  Interface: {intf.bInterfaceNumber}, Alt: {intf.bAlternateSetting}")
        for ep in intf:
            print(f"    Endpoint: {hex(ep.bEndpointAddress)}")
