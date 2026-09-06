#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
echo "Capturing raw USB traffic on usbmon1..."
echo "Please start the Windows VM and let Creator Central connect."
echo "Turn the volume knobs to change the LED colors/brightness."
echo "Press Ctrl+C when you are done."
sudo dumpcap -i usbmon1 -w /tmp/ax310_leds.pcap
