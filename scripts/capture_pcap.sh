#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
echo "Capturing raw USB traffic on usbmon1..."
echo "Please start the Windows VM and let Creator Central initialize the AX310."
echo "Press Ctrl+C when you see the screen turn on/initialize."
sudo tshark -i usbmon1 -w /tmp/ax310_init.pcap
