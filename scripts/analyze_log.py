#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import sys

def main():
    unique_payloads = {}
    with open("ax310_capture.log", "r") as f:
        for line in f:
            if "[IN  EP_01]" in line:
                payload = line.split("] ")[1].strip()
                if payload != "00" * 58 and payload != "00" * 64:  # ignore all zeroes
                    unique_payloads[payload] = unique_payloads.get(payload, 0) + 1

    with open("ax310_summary.txt", "w") as f:
        for p, count in sorted(unique_payloads.items(), key=lambda x: x[1], reverse=True):
            f.write(f"{count:4d} {p}\n")

if __name__ == "__main__":
    main()
