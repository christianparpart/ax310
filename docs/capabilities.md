---
layout: default
title: Capabilities
description: >-
  What the deck can do according to its vendor, and how much of it this project has mapped.
---
# What the AX310 can do, and how much of it we have

Compiled from the vendor's own documentation so the reverse-engineering effort
has a target list rather than a guess at one. Everything in the "What it is"
column is the vendor's claim; everything in "Ours" is what this project has
actually established, and the two are kept apart on purpose.

**Sources.** [Creator Central user manual, 15 July 2022][um] (49 pages, the
substantive one) and the [product page][pp]. Neither documents the wire protocol
— they describe the app, which is exactly what makes them useful: every feature
below is something the app achieves by sending the deck a command we have not yet
identified.

[um]: https://storage.avermedia.com/web_release_www/software/AX310_Creator_Central_UM_20220715.pdf
[pp]: https://www.avermedia.com/product-detail/AX310

## Hardware, as specified

| | |
| --- | --- |
| Interface | USB 2.0 Type-B, driver required |
| Mic in | XLR (balanced) / 6.3 mm (single-ended) ×1, phantom power +48 V switchable **in software** |
| Console in | Optical (Toslink) ×1 |
| Computer inputs | 3 digital tracks |
| Outputs | 3.5 mm TRS headphone and line out; Creator Mix / Audience Mix |
| Sampling | up to 96 kHz, 24-bit |
| Mic effects | **noise gate, reverb, compressor, equaliser** |
| Screen | 5″ IPS capacitive touch panel |
| Encoders | 6 (3 physical inputs, 3 digital inputs) |
| Function buttons | 4 |
| Lighting | RGB |

Two of these independently confirm decodes we had only inferred from captures:
**four function buttons** matches our `ButtonCount`, and **six encoders** matches
both `KnobCount` and the six audio meters we decode out of the input report.

## The six tracks

**The deck prints all six on its face**, left to right: **Mic, Line In, Console,
System, Game, Chat**. The first three are physical inputs and the last three are
the host's digital tracks -- which is why the deck exposes six playback channels
as three stereo pairs, and why the vendor's manual lists exactly three playback
devices.

The manual names the host-facing ones, as the virtual devices the driver
presents:

| Track | Host sees it as | Knob |
| --- | --- | --- |
| System | output — "System (Live Streamer AX310)" | System |
| Game | output — "Game (Live Streamer AX310)" | Game |
| Chat | output — "Chat (Live Streamer AX310)", set as default *communication* device | Chat |
| Mic | input — "Chat Mic (Live Streamer AX310)", the XLR/6.3 mm mic | Mic |

Plus two mix devices that are sums rather than tracks: **Audience Mix**, the input
a streaming app captures, and **Creator Mix**, what reaches the headphones. The
remaining two of the six tracks are not named in either source; the optical
console input is the obvious candidate for one of them. **Which meter index maps
to which track is not established** — that is a straightforward hardware
experiment (play a tone into one track, watch which of the six meters moves) and
worth doing.

## Single Mix vs Dual Mix

The mixer has two modes, and this is the setting behind several things we had
recorded as puzzles.

- **Single Mix** (the app's default) — one mix goes to headphones, line out and
  the stream. Audience Mix is then *identical* to Creator Mix.
- **Dual Mix** — two independent mixes with **separate volume levels for all six
  tracks**. Switched with the on-screen "Switch Mix" widget, which also selects
  which mix the knobs currently adjust.

Consequences for our decoding, stated plainly because they change how existing
evidence should be read:

1. Our capture shows two pairs of capture channels carrying identical content.
   AGENT.md offers two readings for that; the manual says Single Mix makes
   Audience identical to Creator, which makes **"the deck was in Single Mix"** the
   simpler explanation and the one to test first.
2. The init sequence writes `0x21 = 0x80` and `0x22 = 0x12`, which is exactly
   what *disabling* Dual Mix writes, plus `0x15 = 0x00` for the creator mix. So
   the capture was taken in Single Mix, monitoring creator. (An earlier argument
   from `0x94`'s body byte 14 was wrong — that byte mirrors the reverb enable.)
3. In Dual Mix the MIC knob drives the Creator Mix **and** the Chat Mic together.
   That is the documented behaviour behind an observation made mid-call: turning
   the mic knob changed what the far end heard. Not a bug in our driver, and not
   something our driver did — it is what the knob is for.

## Mic effects

The spec lists exactly four — **noise gate, reverb, compressor, equaliser** — and
the captured sequence contains exactly four single-byte commands that behave like
enables: `0x85`, `0x87`, `0x88`, `0x9c`. That is suggestive, not proof; the
mapping is what one capture per toggle settles — see `docs/capture-session.md`.

| Effect | Our state |
| --- | --- |
| Equaliser | **decoded** — `0xa3`, eight bands, `[band][0x01][enable]` then five Q30 biquad coefficients. Six bands enabled in our capture, all near flat. |
| Compressor | parameter block candidate `0x9f`, which carries a repeated 16-bit pair (`0x0e83` twice) where attack/release would sit. Unconfirmed. |
| Reverb | not identified. Confirmed present by ear: audible as a room effect on mic monitoring after our init ran. |
| Noise gate | not identified. |

## Feature inventory

`✅` works · `◐` partly · `❌` not started · `—` app-side, no device command

**Confirmed by ear on the hardware**, the host's three playback pairs are:

| Channels | Sink | Knob |
| --- | --- | --- |
| aux0, aux1 | `ax310_system` | System |
| aux2, aux3 | `ax310_game` | Game |
| aux4, aux5 | `ax310_chat` | Chat |

The four capture pairs, established by playing a tone into a playback track and
muting tracks in each mix block:

| Channels | Source | What it carries |
| --- | --- | --- |
| aux0, aux1 | `ax310_creator` | the **Creator mix** — what the streamer hears; set by the `0x27` block |
| aux2, aux3 | `ax310_audience` | the **Audience mix** — what the stream captures; set by the `0x2e` block |
| aux4, aux5 | `ax310_mic` | the microphone; no host playback reaches it |
| aux6, aux7 | `ax310_loopback` | host playback pre-fader; no knob affects it |

Settled by ear: with a tone playing, muting System in the `0x27` block silenced
the headphones and muting the same track in `0x2e` did not.

**These are software-split devices on every platform, including Windows.** The USB
descriptors declare one six-channel playback interface and one eight-channel
capture interface, so no operating system receives three separate playback
devices from the hardware. AVerMedia's Windows driver splits them in software
exactly as this does; it simply looks native because the vendor ships the driver.
Our nodes therefore declare `node.virtual = false`, so a desktop does not hide the
deck's own tracks behind a "show virtual devices" toggle.

| Feature | Where it lives | Ours |
| --- | --- | --- |
| Knob turn / push / touch | hardware → input report | ✅ decoded |
| Function buttons ×4 | hardware → input report | ✅ decoded |
| Touch panel input | hardware → input report | ✅ press/move/release, coordinates verified |
| Screen rendering | host → chunked JPEG | ✅ 800×480 |
| Six audio meters | hardware → input report | ✅ decoded, **one per track** in knob order, stereo pairs from `0x12` |
| Knob LED ring brightness | `0x1e` | ✅ confirmed on hardware |
| Knob LED ring levels | `0x27` | ✅ confirmed, and **readable back** |
| Which rings light | `0x21`, `0x14` | ◐ observed, encoding not worked out |
| Screen brightness | `01 0a <percent>` | ✅ captured and implemented; a family of its own, which is why `0x1e` looked right and was not |
| Panel off | `01 0a ff` | ✅ captured and implemented; the deck wakes itself, with no host command |
| RGB lighting control | ring registers, and `0xc0` for the buttons | ◐ ring colour tracks the mix; the four buttons take a full RGB each, captured and encoded, **untested on hardware** |
| Per-track volume | `0x27` and `0x2e` | ✅ two contiguous six-byte blocks, one per mix, `base + track`; all twelve read and written |
| Creator ↔ Audience switch | `0x15` inside the `0x1d` fence | ✅ driven, both directions |
| Single ↔ Dual Mix | `0x21` (`0x80` single / `0x00` dual), `0x22` | ◐ captured, **not driven** — `0x21` also selects which rings light and which it is doing is unsettled |
| Mic monitoring on/off | `0x27`, the creator block's Mic slot | ✅ captured and driven — the vendor's widget writes the microphone's creator-mix level and nothing else; there was never a separate register |
| Phantom power | `0x20` bit 0 | ◐ captured — `0x0f` with phantom, `0x0e` without; XLR and 6.3 mm are indistinguishable on the wire, so the deck likely senses the connector. Untested: it means putting +48 V on whatever is plugged in |
| Mic gain | `0x1f` | ✅ `0x00` to `0x38`, confirmed on the hardware against the microphone's own meter |
| Reverb | `0x85` enable, `0x94` body | ✅ five sliders driven from both screens |
| Compressor | `0x9b` enable, `0x9f` body | ◐ threshold and ratio driven; attack, release, output gain not located |
| Equaliser | `0xa3` bands | ◐ six writable bands decoded; its enable is a guess (`0x87`/`0x88`/`0x9c`), untested |
| Echo | shares `0x85`/`0x94` with reverb | ◐ selected by body byte 14; its three sliders not located |
| Noise gate | `0x9e` body | ❌ no separate enable seen; none of its four sliders located |
| Line-out source selection | host command | ❌ |
| Effect presets | — app | **app-side only**; ours to design freely |
| Hotkeys, widgets, 5 pages, swipe | — app | we render our own QML instead |
| OBS / SLOBS / Voicemod integration | — app | out of scope |
| Hotkey & audio profiles | — app | out of scope |
| Firmware update | — Assist Central | out of scope |

## What this changes about our priorities

The single most valuable thing the manual settles is that **the mixer volumes are
not `0x27`**. We confirmed `0x27` drives the LED rings and assumed it was
therefore the volume; the manual's Dual Mix description requires six levels *per
mix*, so there is a mixer register we have not found — and `0x2e`, the second
six-value array that init reads and never writes and that changes nothing
visible, becomes a much stronger candidate than "held but not shown".

Reading both registers on a live deck supports that. With a scrubbing read
between each (`0x0f` answers zeroes, which makes a stale buffer obvious), `0x27`
returns `14 14 14 14 14 14` and `0x2e` returns `14 0a 0a 0a 0a 0a`, repeatably.
Six values each, for six tracks, **held at different levels** — which is what a
second independent mix looks like and is hard to explain otherwise. It is not
proof: that needs the deck put into Dual Mix, the other mix's levels changed, and
`0x2e` read again, and the toggle is a command we do not have yet.

Second: nearly every unimplemented feature above is a **host-sent command that our
capture never recorded**, because the capture covered startup only. Capturing the
vendor software while it toggles each of these is the one technique that reliably
produces answers here, and `scripts/install_vm.sh` already exists for it.
