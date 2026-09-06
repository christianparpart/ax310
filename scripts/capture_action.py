#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Capture the USB traffic of exactly one action, and decode it.

The existing capture is 203 MB of one long session and it only covers startup,
which is why so many commands are still unknown: everything is mixed together and
nothing can be attributed to anything. This records **one action at a time**, with
idle margins either side, so the commands it prints are the commands that action
sent. Run it once per action and the answers fall out by comparison.

It captures the whole usbmon bus rather than one device address, because the deck
changes address whenever it re-enumerates or a VM takes it; the decoder filters by
command grammar instead.

Usage:
    scripts/capture_action.py dual-mix-on
    scripts/capture_action.py mic-gain --bus 1
    scripts/capture_action.py smoke --run 'scripts/probe_dsp.py --set 85 01'
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys
import time

VENDOR_ID = "07ca"
SETTLE_SECONDS = 1.5


def find_bus():
    """The usbmon bus the deck is on."""
    listing = subprocess.run(["lsusb"], capture_output=True, text=True, check=False).stdout
    for line in listing.splitlines():
        if VENDOR_ID in line:
            found = re.search(r"Bus (\d+)", line)
            if found:
                return int(found.group(1))
    return None


def find_decoder():
    """The built ax310_decode binary, whichever preset produced it.

    The decoder is C++ rather than a script on purpose: it shares Protocol.hpp
    with the driver, so it cannot drift from the driver's own idea of what a
    register is called. The cost is that it has to be built first.
    """
    root = pathlib.Path(__file__).resolve().parent.parent
    candidates = sorted(root.glob("out/build/*/src/tools/ax310_decode"))
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("name", help="what you are about to do, e.g. dual-mix-on")
    parser.add_argument("--bus", type=int, help="usbmon bus number (default: where the deck is)")
    parser.add_argument("--run", help="run this instead of waiting for Enter (for testing)")
    parser.add_argument("--dir", default="captures", help="where to put the pcap")
    args = parser.parse_args()

    bus = args.bus or find_bus()
    if bus is None:
        print("no 07ca device on the bus; pass --bus if it is claimed by a VM", file=sys.stderr)
        return 1

    # The device node, not /sys/module: usbmon is built into some kernels, where
    # it works fine but appears in neither lsmod nor /sys/module.
    if not pathlib.Path(f"/dev/usbmon{bus}").exists():
        print(f"no /dev/usbmon{bus} -- run: sudo modprobe usbmon", file=sys.stderr)
        return 1

    target = pathlib.Path(args.dir)
    target.mkdir(exist_ok=True)
    pcap = target / f"{args.name}.pcap"

    # tshark runs as root but the redirect happens as us, which is what keeps the
    # file writable in a directory root cannot reach.
    #
    # stdin must be detached. tshark reads the terminal while capturing, so with
    # stdin inherited it swallows the Enter this script is waiting for -- and puts
    # the tty in raw mode on the way, which staircases every line printed after.
    # The session looks hung and the terminal needs a reset.
    with pcap.open("wb") as sink:
        capture = subprocess.Popen(
            ["sudo", "-n", "tshark", "-i", f"usbmon{bus}", "-w", "-", "-q", "-F", "pcap"],
            stdout=sink, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        try:
            time.sleep(SETTLE_SECONDS)
            print(f"\n  RECORDING  '{args.name}'  (bus {bus})\n")
            if args.run:
                subprocess.run(args.run, shell=True, check=False)
            else:
                print("  Do the ONE thing now -- nothing else.")
                input("  Then press Enter. ")
            time.sleep(SETTLE_SECONDS)
        finally:
            subprocess.run(["sudo", "-n", "kill", "-INT", str(capture.pid)], check=False)
            capture.wait(timeout=15)

    size = os.path.getsize(pcap)
    print(f"\n  saved {pcap} ({size // 1024} KiB)\n")

    decoder = find_decoder()
    if decoder is None:
        print("  ax310_decode is not built -- the pcap is saved, decode it with:", file=sys.stderr)
        print("    cmake --build --preset clang-debug", file=sys.stderr)
        print(f"    <build>/src/tools/ax310_decode {pcap}", file=sys.stderr)
        return 0

    subprocess.run([str(decoder), str(pcap)], check=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
