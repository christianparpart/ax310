#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
import subprocess
import argparse
import sys
import re
import threading
import time

ax310_addresses = set()
lock = threading.Lock()

def monitor_lsusb():
    global ax310_addresses
    while True:
        current_addrs = set()
        try:
            result = subprocess.run(["lsusb"], capture_output=True, text=True)
            for line in result.stdout.splitlines():
                if "07ca:1310" in line or "07ca:0310" in line:
                    match = re.search(r'Bus (\d+) Device (\d+)', line)
                    if match:
                        bus = int(match.group(1))
                        dev = int(match.group(2))
                        current_addrs.add((bus, dev))
        except Exception:
            pass
        
        with lock:
            if current_addrs != ax310_addresses:
                new_addrs = current_addrs - ax310_addresses
                for bus, dev in new_addrs:
                    print(f"\n[SYSTEM] AX310 detected at Bus {bus}, Device {dev}")
                ax310_addresses = current_addrs
                
        time.sleep(1) # poll every second

def main():
    parser = argparse.ArgumentParser(description="Capture USB payloads for AX310")
    parser.add_argument("--interface", default="usbmon1", help="USB monitor interface")
    args = parser.parse_args()

    # Start the lsusb monitoring thread so we can survive replugs/power cycles
    t = threading.Thread(target=monitor_lsusb, daemon=True)
    t.start()
    
    # Give it a second to find initial devices
    time.sleep(1.2)

    # We capture Control(2), Bulk(3), and Interrupt(1) traffic. Exclude Isochronous(0) audio traffic.
    display_filter = "usb.transfer_type == 0x01 || usb.transfer_type == 0x02 || usb.transfer_type == 0x03"

    print(f"Capturing live USB traffic on {args.interface}. Press Ctrl+C to stop...")
    print("Traffic will be clustered by time and saved to 'ax310_capture.log'.\n")

    tshark_cmd = [
        "sudo", "tshark", "-i", args.interface,
        "-Y", display_filter,
        "-T", "fields",
        "-e", "frame.time_epoch",
        "-e", "usb.bus_id",
        "-e", "usb.device_address",
        "-e", "usb.endpoint_address",
        "-e", "usb.capdata",
        "-e", "usbhid.data",
        "-e", "usb.data_fragment",
        "-l" # Line buffered
    ]

    log_file = open("ax310_capture.log", "a")
    
    last_packet_time = 0.0
    last_payloads = {}
    repeat_counts = {}

    try:
        process = subprocess.Popen(tshark_cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        for line in process.stdout:
            line = line.strip()
            if not line:
                continue
            
            parts = line.split('\t')
            if len(parts) >= 5:
                timestamp_str = parts[0]
                bus_id_str = parts[1]
                dev_addr_str = parts[2]
                ep_addr = parts[3]
                capdata = parts[4]
                usbhid_data = parts[5] if len(parts) > 5 else ""
                data_frag = parts[6] if len(parts) > 6 else ""
                
                try:
                    bus = int(bus_id_str)
                    dev = int(dev_addr_str)
                    pkt_time = float(timestamp_str)
                except ValueError:
                    continue

                # Filter against known AX310 addresses dynamically
                with lock:
                    is_ax310 = (bus, dev) in ax310_addresses

                payload = capdata or usbhid_data or data_frag
                if is_ax310 and payload:
                    # Replace colons with spaces for easier reading
                    capdata_clean = payload.replace(':', ' ')
                    
                    # Determine direction from endpoint address (MSB)
                    try:
                        ep = int(ep_addr, 16)
                        direction = "IN " if (ep & 0x80) else "OUT"
                        ep_label = f"{direction} EP_{ep&0x7F:02X}"
                    except ValueError:
                        ep_label = f"??? EP_{ep_addr}"

                    # Clustering
                    if pkt_time - last_packet_time > 1.0:
                        cluster_header = f"\n========== EVENT CLUSTER @ {time.strftime('%H:%M:%S')} =========="
                        print(cluster_header)
                        log_file.write(cluster_header + "\n")
                        # Reset deduplication on new cluster
                        last_payloads.clear()
                        repeat_counts.clear()

                    last_packet_time = pkt_time

                    # Deduplication
                    dedup_key = ep_label
                    if last_payloads.get(dedup_key) == capdata_clean:
                        repeat_counts[dedup_key] = repeat_counts.get(dedup_key, 0) + 1
                        if repeat_counts[dedup_key] == 10:
                            msg = f"  [{ep_label}] ... (repeating identical payload, suppressing)"
                            print(msg)
                            log_file.write(msg + "\n")
                        elif repeat_counts[dedup_key] < 10:
                            pass # still outputting first few repeats just in case
                        else:
                            continue # suppressed
                    else:
                        last_payloads[dedup_key] = capdata_clean
                        repeat_counts[dedup_key] = 0

                    if repeat_counts[dedup_key] < 10:
                        msg = f"[{ep_label}] {capdata_clean}"
                        print(msg)
                        log_file.write(msg + "\n")
                        log_file.flush()
                        
    except KeyboardInterrupt:
        print("\nCapture stopped.")
        process.terminate()
        log_file.close()
        sys.exit(0)

if __name__ == "__main__":
    main()
