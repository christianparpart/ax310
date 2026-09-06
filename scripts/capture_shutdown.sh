#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
echo "Capturing raw USB traffic on usbmon1..."
echo "Please close the Creator Central app or shut down the Windows VM."
echo "Press Ctrl+C when you see the device gracefully shut down."
sudo tshark -i usbmon1 -w /tmp/ax310_shutdown.pcap
