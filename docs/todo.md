# Internal todo

Ordered by what unblocks the most. Kept here rather than in a head, so a session
that ends mid-thought loses nothing.

---

## 1. Audio routing — *working; packaging left*

`scripts/setup-audio.sh` selects the Pro Audio profile and splits the deck into
per-track devices. Every mapping is confirmed on the hardware, not inferred:

| Sink | Channels | Knob |
| --- | --- | --- |
| `ax310_system` | aux0/1 | System |
| `ax310_game` | aux2/3 | Game |
| `ax310_chat` | aux4/5 | Chat |

| Source | Channels | Carries |
| --- | --- | --- |
| `ax310_creator` | aux0/1 | the creator mix — headphones |
| `ax310_audience` | aux2/3 | the audience mix — the stream |
| `ax310_mic` | aux4/5 | the microphone |
| `ax310_loopback` | aux6/7 | host playback, pre-fader |

~~Left:~~ **Done.** `packaging/wireplumber/51-ax310.conf` states a `pro-audio`
preference for `07ca:0310` and renames the two ALSA nodes to `ax310_raw_out` and
`ax310_raw_in`, which is what lets `packaging/pipewire/ax310-split.conf` be one
fixed file rather than one generated per machine. `setup-audio.sh` installs those
same two files into the user's own directories, and `--regenerate` rebuilds the
fragment from the track tables in the script, which remain its single source of
truth. `cmake/Packaging.cmake` installs all of it plus a udev rule, and `cpack -G
RPM` builds. All verified on this machine, install and remove.

Left, and smaller:

- ~~**A licence.**~~ Apache-2.0. `LICENSE` and `NOTICE` are in the tree and in the
  package, every first-party file carries an SPDX identifier, and the RPM declares
  it. `cmake/` already carried Apache-2.0 headers from the import it came from, so
  the tree is now consistent rather than mixed.
- ~~**`--identify` can stop needing an ear.**~~ Done. It plays a tone into each
  sink and reads the deck's own meters; each one peaks its own track at 48% and
  every other track at 0%. That reproduces the ear's answer exactly, which is the
  first independent confirmation the routing map has had.
- **`--identify-mixes` still needs an ear**, and always will: it asks which mix
  feeds the headphones, and the meters are pre-fader, so muting a mix does not
  move them.

---

## 2. Per-track volume address map — *done*

Volume is **per address**, not one port with a selector, and the addresses turned
out to be two contiguous six-byte blocks rather than a scatter: `0x27` is the
creator mix's base and `0x2e` the audience mix's, each `base + track` in the
deck's printed knob order. `protocol::levelAddressOf()` is the whole map, all
twelve of them, and they read back.

---

## 3a. Remaining DSP gaps — *reverb and equaliser done*

Mapped: the equaliser's six writable bands and their frequencies, the reverb's
five sliders, the compressor's threshold and ratio, and every effect's enable.

Left, in order of value:

- **Echo's three sliders** — level, time, delay. Echo shares `0x94` with reverb, so
  only the fields that differ need finding; byte 6 is the lead.
- **The noise gate's four sliders** — threshold, attack, hold, release, in `0x9e`.
- **The compressor's remaining three** — attack, release, output gain, in `0x9f`.
  The repeated `0x0e83` at bytes 4..7 is where attack and release should be.
- **The scale of the fields we have.** Threshold reads as whole dB and the Q8
  hypothesis predicts a fractional slider position makes the low byte non-zero.
  Untested.

---

## 3. Finish mapping the DSP enables

Two of five are now established by capture: `0x85` is **reverb**, `0x9b` is the
**compressor**. Three enable-shaped commands remain — `0x87`, `0x88`, `0x9c` —
against three remaining effects: **noise gate**, **echo** and **equaliser**.

The vendor UI offers five effects, not the four the datasheet lists, and each has
sliders worth capturing once the enables are named:

| Effect | Sliders |
| --- | --- |
| Noise gate | threshold, attack, hold, release |
| Compressor | threshold (**byte 9, dB**), ratio (**byte 11**), attack, release, output gain |
| Reverb | level, time, room size, damp, diffusion, **plus presets** |
| Echo | level, time, delay, **plus presets** |
| Equaliser | 3 bass, 3 mid, 3 treble |

The equaliser's nine sliders against our **eight** `0xa3` band slots is an open
question worth settling early — three bands of three controls each would fit the
labels better than nine bands do.

---

## 4. Separate waking the deck from configuring it

`connect()` replays a captured sequence that is somebody's saved configuration.
Property registers are now snapshotted and restored, but the DSP chain has no
read-back we know of, so the equaliser and whichever enable is the reverb are still
imposed on any deck this attaches to.

The real fix is knowing which payloads wake the hardware. Capturing Creator
Central's own startup — now that we can capture one action cleanly — would show
what it sends to a cold deck.

---

## 5. Move protocol tooling into `src/tools/` — *done*

**Done:** the capture decoder is now `src/tools/`, C++ against the library and
held to the gate. Validated against the python one it replaced on all nine
captures: every address, length, value and count identical, differing only in
register *names* — where the python copy was already stale, which is the drift
this was meant to stop, caught in the act.

`ax310_probe` replaces `probe_dsp.py` and the ad-hoc read scripts: `--dump` reads
every register the handshake overwrites, `--read` takes one, `--effect` toggles a
confirmed enable. It builds its frames from `Protocol.hpp`, so its checksums
cannot disagree with the driver's, and it deliberately does **not** go through
`Device` — `connect()` would reconfigure the deck on the way in, which is not what
a tool for looking at one should do.

Spent scripts removed: `probe_dsp.py`, `find_crc*.py`, `extract_jpegs*.py`, the
`jpegs/` directories, and a tracked `__pycache__` that should never have been
committed.

**Left:** add `0x87`, `0x88` and `0x9c` to the `--effect` allow-list once the
captures name them.

---

## 6. Smaller, known, not urgent

- ~~**The command that makes the deck send all six meters.**~~ There isn't one,
  and there is nothing left to find: the deck always sends all six, one per track,
  each a stereo pair from report offset `0x12`. Toggling the vendor's peak view
  sends the deck nothing at all. Every capture had held the answer from the start;
  what hid it was a decoder that read only the first pair — the microphone, which
  hears whatever is played into the room and so appeared to answer for every
  track in turn.
- **The mic monitoring toggle.** The manual's "Monitor" widget turns microphone
  playback into the headphones on and off, and it is the one control the GUI
  design ships as visibly inert — the deck panel reserves its tile so the row is
  not re-cut later, but it cannot be operated. One capture of that widget being
  toggled fills the gap and the tile simply lights up.
- ~~**Turn the IPS panel off.**~~ Captured: tapping the vendor's panel-off widget
  sends `01 0a ff`, once, and an idle capture of the same length sends nothing at
  all. `ax310_probe --panel-off` replays it. **Not yet confirmed on the hardware**
  — the VM held the deck when it was found, so nobody has watched the panel go
  dark in response to our own send.
  Turning it back **on** needs no command: the deck wakes itself, and the capture
  of doing so contains zero host-to-device traffic. Worth having for its own sake — a deck left on a desk
  overnight should not be showing a bright panel — and it may be the same register
  as brightness.
- **Screen brightness** — the register is still unidentified; `setScreenBrightness`
  is a stub. A `screen-brightness` capture would settle it.
- **Does a drag survive with audio playing?** `Device::dispatchEvent` treats the
  first non-touch report as the finger lifting. All-zero reports are dropped
  before that, and in silence every report interleaving a drag was all-zero — so
  nothing is broken today. But a report is only all-zero when nothing is
  happening: with audio the meters are non-zero, those reports survive the
  filter, and a drag would be delivered as press/release/press. One run of
  `ax310_probe --touches` with music playing settles it. An earlier version of
  this entry asserted the bug on a measurement that counted the dropped reports.
- **Touch flags byte** at report offset `0x01` — a monotonic value that latches
  per contact; three readings of the `0x08` bit have been measured and refuted.
- **`0xc0` / `0xe0` record writes** — not restored across connect, because their
  length field is a record size rather than a byte count.
- ~~**`cmake/` carries another project's files.**~~ Done. Five unreachable
  modules removed, the sixth replaced, every surviving cross-reference rewritten,
  and one real defect found in the process: the CPM bootstrap's stall bound had
  been empty since the variable was renamed. `.clang-tidy` was re-measured
  against this project rather than the one it came from — 38 suppressions that
  silenced nothing are gone.
- **The read loop** is a `std::thread` in `DeviceBridge`, not a coroutine.
- **Numeric quantities are bare `int`s** where a unit type belongs.
- ~~**No CI.**~~ `.github/workflows/ci.yml` runs all four presets on every push.
- **QML cannot name the driver's enums**, so they go through `qRegisterMetaType`.
- **The equaliser and noise gate have no interface**, because their parameters are
  not located yet. Each found field is one row in `protocol::Parameters` and
  appears in both screens with no new code -- the deck panel's effects page and
  the desktop window are both built from that table.
- **Single/Dual mix is not driven.** `0x21` takes `0x80` for Single and `0x00` for
  Dual, always alongside `0x22` and inside the `0x1d` fence, but the same address
  also selects which knob rings light and which of the two it is doing has never
  been settled on the hardware. The panel's tile is held and marked unverified
  rather than driven on a guess. One capture, or one careful write with the deck
  in view, settles it.
