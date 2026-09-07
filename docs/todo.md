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
- ~~**The mic monitoring toggle.**~~ There was never a register. Captured: the
  vendor's monitor widget writes `0x27` — the creator block's Mic slot — to full
  or to zero, and sends `0x22 = 0x02` alongside, which does not change between the
  two. So monitoring is the microphone's level in the mix you hear, the audience
  mix is untouched, and the driver could already do it. The panel's Monitor tile
  is live.
- ~~**Turn the IPS panel off.**~~ Captured: tapping the vendor's panel-off widget
  sends `01 0a ff`, once, and an idle capture of the same length sends nothing at
  all. `ax310_probe --panel-off` replays it. **Not yet confirmed on the hardware**
  — the VM held the deck when it was found, so nobody has watched the panel go
  dark in response to our own send.
  Turning it back **on** needs no command: the deck wakes itself, and the capture
  of doing so contains zero host-to-device traffic. Worth having for its own sake — a deck left on a desk
  overnight should not be showing a bright panel — and it may be the same register
  as brightness.
- ~~**Screen brightness.**~~ `01 0a <percent>`, captured from the vendor's slider
  at both ends: `0x19` at minimum and `0x64` at maximum. `setScreenBrightness` is
  implemented and clamps to that range, and **confirmed on the hardware**: the
  panel dims and brightens, and the knob rings do not move, which is what
  separates it from `0x1e`.
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
- ~~**`0xc0` / `0xe0` record writes.**~~ Both decoded. `0xc0` colours the
  function buttons and the knob rings, `0xe0` drives the surround strip in six
  modes. Neither is restored across connect, which for `0xe0` matters: its "off"
  is a black solid, so a strip left off stays off across a replug.
- ~~**Surround brightness reaches the strip twice.**~~ It does not. Byte 4 is the
  frequency and nothing else; brightness is applied to the colour, `0x19` to
  `0xff`, in every mode and on every light this device has.
- ~~**`ShutdownPayloads` has never been decoded.**~~ Both sequences are now
  written command by command. Shutdown is short and almost entirely legible:
  darken the four buttons, dim and darken the rings, turn the surround strip off,
  set `DisplayPower` to `0x02` — bracketed twice by two writes in groups `0x04`
  and `0x09` that remain unidentified.
- **Three command groups are unidentified** — `0x03`, `0x04` and `0x09`, each
  written once or twice with a constant value and never read. `0x01` (firmware),
  `0x0a` (display), `0x10` (properties) and `0xa0` (serial) are now known. The
  three that remain are the only writes in either sequence that are neither
  interrogation nor somebody's settings, which makes them the best remaining
  candidates for whatever actually wakes the hardware.
- **Framed command `0x9a`** — seen once, in the startup capture, carrying `0x00`
  and sent immediately before `CompressorEnable`. It is not in `InitPayloads`,
  which came from an older capture, and no effect capture triggers it. The
  position suggests a paired enable; nothing else does.
- **The counts assume every indexing enumeration is dense from zero.** They are
  derived by walking values from zero and stopping at the first gap, which is
  cheap enough for a header included everywhere. `Enumerators_test.cpp` compares
  that walk against an exhaustive search for all ten, so a hole fails the build
  rather than silently truncating.
- **`Enumerators.hpp` depends on compiler diagnostic strings.** It lists an
  enumeration's enumerators by reading `__PRETTY_FUNCTION__` / `__FUNCSIG__`,
  because C++23 has no way to ask. A self-test asserts the trick still works, and
  `src/checks/` cross-checks it against P2996 on the one compiler that has
  reflection. Delete both once P2996 is available everywhere — the standard
  version then becomes the implementation rather than the second opinion. The
  MSVC branch of the marker is written from documentation and has only ever been
  exercised by CI.
- **`PreservedAddresses` reads `0x11` with length 3; the vendor reads it with 1.**
  The vendor *writes* three bytes there, so three is a legal write length, but no
  capture shows a three-byte read. Since the deck does not clear its reply buffer,
  a three-byte read of a one-byte register would capture one real byte and two
  belonging to the previous answer — and the restore path would then write those
  two back. Worth checking against hardware with `--dump` before trusting it.
- **`0x11 = 0x01`, `0x12 = 0x08`, `0x13 = 0x01`, `0x22 = 0x02`** — the values are
  now known from the startup replies; what they mean is not.
- **`0x20` bits 1 and 2** are set in every capture and nothing is known to clear
  them. Two mic settings are accounted for; whatever these are is not in any pane
  that has been swept.
- **Byte 4 in solid mode does something, and we do not know what.** The vendor
  never varies it usefully — `0xfb` for all ten colour presets and at both ends of
  the brightness slider — but driving it by hand produces colour effects on the
  strip that have not been characterised yet. Wants a systematic sweep: hold the
  mode, colour and everything else fixed, step the byte through its range, and
  describe what the strip does at each value. No VM needed, only the deck and
  somebody looking at it.

  An earlier entry closed this as stale struct memory. That conclusion was drawn
  from captures, which can only show what the vendor's software writes, never what
  the deck does with what it receives.
- **Bytes 6 and 7 of a button-colour record gate the colour**, and this is now
  measured rather than suspected. The vendor's own initialisation lights only two
  of its four buttons: the two it writes `ff 00 00`. The two it writes `00 37 ff`
  stay dark, and both carry `f9 3d` in that pair. Changing only the pair, to the
  `00 1d` that `buttonColourRecord()` sends, lights the same colour on the same
  button. So red survives `f9 3d` and blue does not, which is not a simple enable.

  Hold this next to the entry above: `0xf9` here, `0xf8` in byte 6 of a knob-ring
  record, and `0xf8` in byte 4 of a surround record are three unexplained bytes of
  the same shape in the same family of records. One sweep that steps the pair with
  the colour held fixed, on a button and on the strip, may answer all three at
  once. No VM needed.
- ~~**The surround record's channel order is unverified.**~~ Settled on hardware:
  `ff 00 00` is red, so bytes 7-9 are R-G-B as written.
- ~~**Which selector is which physical button.**~~ Settled on hardware: all four
  driven in one pass to red, green, blue and yellow, each appearing under the
  button `ButtonColourSelectors` names. That pass was spaced: four consecutive
  records at `0xc0` sent with no gap do **not** all land, which is the deck
  attaching with only the last one lit. `Device::lightButtonsWithDefaults` paces
  them the way the handshake and the shutdown sequence are paced.
- **Per-mix knob colour needs the mix selected first** — the ring record carries
  no mix, so setting the audience mix's colour means selecting it, writing, and
  selecting back. Whether the deck keeps both colours or the driver must is
  untested; the colour path itself is confirmed on hardware.
- ~~**`ButtonBits` is unverified.**~~ Settled with `ax310_probe --inputs`, which
  lights one button at a time and reads the arriving byte, so the press is
  identified by the light rather than by the table under test. `0x08, 0x04, 0x02,
  0x01` is right; the reversal against `Button`'s order is what the hardware does.
  `KnobBits` was confirmed in the same walk, and two behaviours with it: a release
  is reported about 300 ms after the press, and two buttons held together arrive
  as one report with both bits set.

  Worth remembering from how long it took: asking for one press per round makes
  the result depend on the pacing of somebody who cannot see the terminal, and a
  press that misses its round is credited to the next control. The walk now retries
  a round rather than advancing, takes the knob pushes in arrival order, and
  refuses a verdict when two rounds report the same bit — which two controls
  cannot do, so it means a mis-press rather than a finding.
- **The shutdown sequence does not stop the report stream.** `--inputs` was run
  against a deck the driver had already shut down, and reports still arrived --
  so the probe's watch modes need the deck woken once, not the driver left
  running. What a *cold* deck does is unchanged and still the documented case:
  no reports until the handshake. Worth pinning down what the shutdown sequence
  actually stops, since it is the closest thing to the "wake without configuring"
  command that is still missing.
- **The mix colours should be configurable.** Today `protocol::MixRingColours`
  holds one pair -- the deck's own `#007DFF` and `#FF7D00` -- and the QML theme is
  held to it by a test, so the rings and the panel cannot disagree. What is not
  there is any way for somebody to choose their own. That wants a settings store,
  which this project does not have at all yet: where user configuration lives and
  how it persists is the decision to make first, and the mix colours are only its
  first customer. The knob-ring colour is per mix on the hardware too, so the
  driver already has the shape for it.
- **Nothing in the GUI reaches the lights** — button colours, ring colour and the
  surround strip are all driven by hand through `ax310_probe`. They are the first
  device features with no interface at all rather than a partial one.
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
