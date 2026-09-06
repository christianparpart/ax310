# SPDX-License-Identifier: Apache-2.0
import sys
print("std::vector<std::vector<uint8_t>> initPayloads = {")
with open("/tmp/init_payloads_ordered.txt") as f:
    for line in f:
        line = line.strip()
        if len(line) < 128: continue
        bytes_str = ", ".join(f"0x{line[i:i+2]}" for i in range(0, len(line), 2))
        print(f"    {{{bytes_str}}},")
print("};")
