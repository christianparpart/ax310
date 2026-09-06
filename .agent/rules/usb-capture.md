# Capturing what the vendor software sends

The one technique that reliably produces answers here. Nearly every unimplemented
feature is a host-sent command, and the existing 203 MB capture cannot supply
them: it covers startup only, and everything in it is mixed together so nothing
can be attributed to anything.

**Capture one action at a time.** `scripts/capture_action.py` exists for this and
does the whole loop — record, wait, stop, decode.

## Setup, once per boot

```sh
sudo modprobe usbmon
```

usbmon may be **built into the kernel**, in which case it appears in neither
`lsmod` nor `/sys/module` but works fine. Test for `/dev/usbmon<bus>`, not for the
module.

## The gotchas, all of which cost time here

- **tshark cannot write into a directory root cannot reach.** It runs as root and
  the scratchpad is mode 700. Capture to stdout and let the shell redirect:
  `sudo tshark -i usbmon1 -w - -q -F pcap > out.pcap`.
- **Force `-F pcap`.** tshark writes pcapng by default and the decoder reads
  classic pcap.
- **The 8-byte setup packet lives at offset 40 *inside* the 64-byte usbmon
  header**, not in front of the data. The payload starts at offset 64 for control
  and interrupt transfers alike. Getting this wrong silently yields zero results.
- **`usb.capdata` is empty** for these transfers, and `usb.setup.bRequest` does
  not match. Do not go field-hunting in tshark; `src/tools/UsbmonCapture.cpp`
  parses the pcap directly.
- **Capture the whole bus, not one device address.** The address changes whenever
  the deck re-enumerates or a VM claims it. The decoder filters by command grammar.
- **Take only `'S'` (submit) records** and skip endpoints with bit 7 set; those
  are the deck talking back, and they flood.

## Host capture sees VM traffic

With libvirt USB passthrough (`--hostdev 07ca:1310`), QEMU reaches the device
through the host's USB stack, so **host-side usbmon sees everything the Windows
guest sends**. Nothing needs installing inside the VM.

## Reading the result

```sh
out/build/clang-debug/src/tools/ax310_decode captures/thing.pcap
out/build/clang-debug/src/tools/ax310_decode captures/idle.pcap captures/thing.pcap
```

`scripts/capture_action.py` runs it for you and finds it under whichever preset
built it. **The decoder is C++ on purpose**: it shares `Protocol.hpp` with the
driver, so it cannot print a register name the driver has since renamed. A script
holding its own copy of that table drifts, and the drift is silent.

**Creator Central is silent at idle.** Six seconds of it running untouched
produced zero commands — only screen chunks. So every command in an action capture
is that action, and diffing is a fallback rather than a routine step.

**Screen chunks swamp everything.** The vendor software redraws the deck's panel
at ~30 fps, which is 1024-byte output reports at roughly 2 MB/s. Filter outbound
transfers of `SCREEN_CHUNK_SIZE` and count them rather than dropping them silently,
or a capture that is all screen looks like a capture that found nothing.
