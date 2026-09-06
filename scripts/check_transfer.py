# SPDX-License-Identifier: Apache-2.0
import sys

log_file = '../ax310_capture.log'
packets = []

with open(log_file, 'r') as f:
    for line in f:
        if line.startswith('[OUT EP_02]'):
            hex_str = line.strip().split(' ', 2)[2]
            try:
                packets.append(bytes.fromhex(hex_str))
            except:
                pass
            if len(packets) > 100:
                break

for i, p in enumerate(packets):
    if len(p) >= 12:
        print(f"Packet {i}: seq={p[0]:02x} len={p[8]|(p[9]<<8)} b10={p[10]:02x} b11={p[11]:02x}")
    if p[0] == 0 and i > 0:
        break
