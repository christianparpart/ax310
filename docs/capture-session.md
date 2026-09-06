---
layout: default
title: Capturing the vendor software
description: >-
  How the AX310's undocumented commands are recovered: one action at a time, against an idle baseline.
---
# Running a capture session

How to get the commands we are missing out of the vendor software. Read
`.agent/rules/usb-capture.md` first if anything below misbehaves — it has the
gotchas.

The idea is simple and the discipline is the whole trick: **capture one action at
a time, and do nothing else while recording.** Every previous capture mixed
everything together, which is why we have 88 opaque payloads and no idea what most
of them mean.

---

## On the host, once

```sh
sudo modprobe usbmon
pkill -f ax310_app          # our driver must not be sending anything
virsh start AX310-Capture-VM
virt-viewer AX310-Capture-VM
```

The deck is passed through to the VM by `--hostdev`, but it stays on the host's
USB bus, so host-side capture sees everything Windows sends. **Nothing needs
installing inside the VM.**

## In the VM, once

1. Let Windows finish booting.
2. Start **Creator Central** and log in.
3. **Wait until the deck is fully up** — screen lit, knob rings on. The startup
   flood is already captured and would only bury the thing you are after.
4. Then stop touching everything.

---

## The loop, once per action

**On the host**, start the recorder and name what you are about to do:

```sh
scripts/capture_action.py dual-mix-on
```

It prints `RECORDING`. **Now switch to the VM, do that one thing, and nothing
else.** Come back to the host terminal and press Enter. It saves the pcap and
prints the commands it saw.

If it cannot find the deck because the VM has claimed it, add `--bus 1`.

**The idle baseline is already taken, and it is silent.** Six seconds of a fully
running Creator Central with nobody touching anything produced **zero commands** —
just 11309 screen chunks, the vendor software redrawing the deck's panel at about
thirty frames a second.

That is the best possible news for this exercise: **every command that appears in
an action capture is that action.** No diffing needed. The decoder skips the
screen chunks and counts them, so a capture that is all screen says so rather than
looking empty.

If a result ever does look noisy, the diff is still there:

```sh
out/build/clang-debug/src/tools/ax310_decode captures/idle.pcap captures/dual-mix-on.pcap
```

Expect roughly 2 MB of pcap per second of capture — almost all of it screen. Keep
each capture short.

---

## What to capture, in priority order

Names matter — use them exactly, since the decoder output is filed under them.

### Tier 1 — the mixer model (do these even if you do nothing else)

| Name | Do this in Creator Central |
| --- | --- |
| `dual-mix-on` | Audio Mixer → click **Enable Dual Mix** |
| `dual-mix-off` | switch back to Single Mix |
| `switch-mix` | tap the **Switch Mix** widget on the deck's own screen |
| `volume-game-up` | drag **one** track's volume (Game) up in the app |

Why these first: Dual Mix keeps six independent levels **per mix**, so there is a
mixer register we have not found — `0x27` only drives the LED rings. `0x2e` is the
strong candidate and `switch-mix` is the command we have hunted longest. Watch
`0x21` and `0x22`, which move together with the mixer mode.

### Tier 2 — the four microphone effects

The spec lists exactly four, and we have exactly four unidentified single-byte
enables (`0x85`, `0x87`, `0x88`, `0x9c`). One capture each maps them all.

| Name | Do this |
| --- | --- |
| `gate-on` / `gate-off` | Mic settings → Noise Gate toggle |
| `reverb-on` / `reverb-off` | Mic settings → Reverb toggle |
| `comp-on` / `comp-off` | Mic settings → Compressor toggle |
| `eq-on` / `eq-off` | Mic settings → Equalizer toggle |
| `reverb-amount` | drag the reverb amount slider only |

### Tier 3 — everything else

| Name | Do this |
| --- | --- |
| `screen-brightness` | Hardware Settings → touch panel brightness slider |
| `phantom-on` / `phantom-off` | Mic settings → +48V |
| `mic-gain` | drag the mic gain slider only |
| `monitor-on` / `monitor-off` | the Monitor widget |
| `rgb` | Hardware Settings → RGB lighting |
| `line-out` | Hardware Settings → Line Out source |

---

## Reading what comes back

The decoder prints one line per host-to-device command, in our grammar. A toggle
should show up as one or two commands that differ only in a value byte between the
`-on` and `-off` captures — that byte and that address are the answer.

Send me the printed output; the pcaps stay local and are gitignored. If a capture
prints nothing, the action probably went to the app rather than the deck, which is
itself worth knowing.

**Do not stop at the first plausible reading.** Two decodes in this project
survived the captures and died on the hardware. When a capture suggests a
register, confirm it by writing and watching before naming anything after it.
