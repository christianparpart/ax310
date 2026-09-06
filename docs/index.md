---
layout: default
title: Overview
description: >-
  A C++23 userspace driver and Qt/QML interface for the AVerMedia Live Streamer
  AX310 audio control deck, on Linux, reverse-engineered from its USB traffic.
---
<!-- SPDX-License-Identifier: Apache-2.0 -->
# AX310 on Linux

The **AVerMedia Live Streamer AX310** is a six-knob mixing deck with a 5&Prime;
touch panel, sold with Windows and macOS software and no Linux support. Its USB
protocol is undocumented.

This project works that protocol out and builds an open replacement for the
vendor's app: a Qt-free C++23 driver, a desktop mixer, and an interface rendered
onto the deck's own screen.

![The deck's 800×480 touch panel](images/deck-panel.png)

## What works

| Area | State |
| --- | --- |
| **Mixer** | six tracks × two independent mixes, read and written |
| **Mix control** | creator ↔ audience switching; Single/Dual mode located, not driven |
| **Effects** | reverb and compressor parameters, on both screens; equaliser and gate located |
| **Screen** | 800×480 JPEG frames pushed to the deck's panel |
| **Touch** | press / move / release, coordinates verified |
| **Knobs & buttons** | turns, pushes, capacitive touch, four function buttons |
| **Meters** | six channels, one per track, decoded live |
| **Audio routing** | each mixer track as its own PipeWire device |

## The wire specification

The register map, report layout, framed commands and their parameters are
published as the **[wire specification](wire-protocol.html)** — and it is not
written, it is *generated*, out of the tables in the driver that implement the
protocol.

That is deliberate. Earlier in this project the audio meters were described three
different ways in three files at the same time, and each description had been
written by someone who believed it. A document kept alongside code drifts from it;
a document projected out of code cannot. `ctest` fails when the two have parted,
and the claims a table cannot hold — an endianness, a contiguity — are tests
rather than sentences.

## What it looks like

The hardware control is a knob inside an LED ring, so every track is a ring
rather than a fader. The thin outer arc is the **other** mix, so both are
readable per track without switching; the inner arc is the live meter with a
peak-hold tick. Blue and orange are the deck's own LED colours for the creator
and audience mixes — they say which mix you are in rather than decorating it.

![The desktop window](images/desktop.png)

## Getting it

```sh
git clone https://github.com/christianparpart/ax310
cd ax310
cmake --preset clang-debug && cmake --build --preset clang-debug
ctest --preset clang-debug
./out/build/clang-debug/src/ax310_app
```

Needs a C++23 compiler, Qt 6, CMake and Ninja; hidapi and Catch2 are fetched
automatically. `cpack -G RPM` builds a package that also installs the udev rule
and the PipeWire configuration, neither of which is optional — without the udev
rule the deck cannot be opened, and without the PipeWire fragments it presents
one six-channel device instead of six tracks.

## How it was worked out

Almost everything here came from capturing the vendor software performing **one
action at a time** and diffing against an idle baseline, then confirming each
guess on the hardware before naming anything after it.

That order matters, and the notes record where it was not followed. A register was
named after screen brightness on strong evidence from captures alone; the deck
disagreed, and it dims the knob rings. The meters were read wrong three times
running, every time because only the first of six pairs was being decoded — and
that pair is the microphone, which hears whatever is played into the room, so it
appeared to answer every track in turn.

Those mistakes are kept, in `.agent/rules/`, because they are more reusable than
the conclusions.

## Where it is

Early, and honest about it. The parts above work and are tested — 138 tests,
none of which need the hardware. What is not done: the microphone monitor toggle,
the panel's on/off and brightness registers, and the noise gate, echo and
equaliser parameters. Attaching to a deck replays a captured sequence that turns
out to be somebody's saved configuration, so it is not side-effect-free.

- [Capabilities, against how much is mapped](capabilities.html)
- [How the vendor software is captured](capture-session.html)
- [Source on GitHub](https://github.com/christianparpart/ax310)
