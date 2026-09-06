# SPDX-License-Identifier: Apache-2.0
log_file = '../ax310_capture.log'

with open(log_file, 'r') as f:
    for line in f:
        if line.startswith('[OUT EP_02]'):
            hex_str = line.strip().split(' ', 2)[2]
            packet = bytes.fromhex(hex_str)
            if b'\xff\xd8\xff' in packet:
                idx = packet.find(b'\xff\xd8\xff')
                print(f"Found JPEG start at index {idx} in packet:")
                print(f"Packet hex: {packet[:32].hex()}")
                print(f"Header: {packet[:12].hex()}")
                print(f"Payload starts: {packet[12:20].hex()}")
                break
