# What we actually know about the AX310

Established means confirmed on the device or arithmetically verified. Everything
else is marked. Full reasoning lives in `AGENT.md`; this is the summary.

## Bus

Two USB devices, one physical unit, both present at once — no re-enumeration
between them.

| | |
| --- | --- |
| `07ca:1310` interface 0 | **the deck** — vendor protocol, this is the one |
| `07ca:0310` interface 4 | consumer-control page (media keys), not ours |

Payloads are 64 bytes; a feature report is 65, with `0x00` in front as the report
id. **The missing report-id byte is what made the first 74 init writes all fail.**

## Command grammar

```
[0x01 SET | 0x81 GET]  0x10  <address>  <length>  <values...>
[0xfe]  0x00  <length>  <command>  <body...>  <checksum>
```

Framed-family length counts the whole command; the checksum is the low byte of
everything from the length onward. All 24 captured commands verify.

Byte 2 of the property form is an **address in a register space**, not a
selector — consecutive addresses return overlapping windows of one byte stream,
and reading around `0xd0` spells out the serial number.

**A read reply echoes address and length back**: `[report id][0x81][0x10][address]
[length][values...]`. Bytes past the stated length are the remains of the previous
reply — trust the echoed length and nothing beyond it.

## Addresses

| Address | Meaning | Confidence |
| --- | --- | --- |
| `0x27` | six per-knob LED ring levels, `0x00..0x14` | established, readable back |
| `0x1e` | knob **ring** brightness — not the screen | established on hardware |
| `0x21` | selects which rings light | observed, encoding unknown |
| `0x14` | `0x00` lights every ring | observed |
| `0x27..0x2c` | **creator mix** per-track levels, `base + track` | established |
| `0x2e..0x33` | **audience mix** per-track levels, same layout | established |
| `0x35` | written with `0x27` when the Mic level is dragged | established |
| `0x2e` | the **second mix's** six levels | established |
| `0x1d` | brackets a settings change, `01` before and `00` after | established |
| `0x15` | monitored mix: `00` creator, `01` audience | established |
| `0x35` | answers one value, not six | observed |
| `0x0f` | written only by shutdown; answers zeroes, useful for scrubbing | observed |
| `0x16` | **do not write** — wedged the deck | established the hard way |

## The 0xfe family is a DSP

The spec lists exactly four microphone effects — **noise gate, reverb, compressor,
equaliser** — and the captured sequence contains exactly four single-byte commands
that behave like enables. Which is which is not yet established.

| Command | Body | Meaning |
| --- | --- | --- |
| `0xb2` | none | apply/commit |
| `0x85` `0x87` `0x88` `0x9c` | 1 byte | enables; `0x85` observed with both `00` and `01` |
| `0x94` | 21 bytes | **delay-effect parameters**; see the field map below |
| `0x9e` | 22 bytes | parameters |
| `0x9f` | 17 bytes | parameters; repeated `0x0e83` pair — compressor attack/release? |
| `0xa3` | 23 bytes | **eight-band biquad EQ**, established |

**The bands are the user's equaliser and carry the gain.** Band index is slider
index: 0..7 for 50, 100, 250, 500, 1000, 4000, 8000, 16000 Hz, confirmed by the
8 kHz slider moving band 6 and nothing else. `b1` is always exactly `a1`, the
peaking-biquad signature; a band whose `b` equals its `a` throughout is at 0 dB.

**Only bands 1..6 are writable**: 100, 250, 500, 1000, 4000, 8000 Hz. Bands 0 and
7 are a fixed high-pass and low-pass and have one value each across every capture.
The vendor UI has eight sliders, so **50 Hz and 16 kHz drive nothing** -- dragging
either re-sends all eight bands unchanged. Confirmed by predicting it before
testing.

`0x94` body: decay at 1 (moves for time *and* room size), damp at 2, level at 3,
diffusion at 8, type at 14 (`0` none, `1` reverb, `2` echo), room size at 18-19
little-endian. Byte 6 differs between reverb and echo and is the likeliest home
for echo's delay.

`0xa3` body is `[band 0..7][0x01][enable]` then five little-endian signed 32-bit
Q30 coefficients (`0x40000000` = 1.0) in order `b0 b1 b2 a1 a2`. Verified three
ways: bands 2 and 4 have `b` exactly equal to `a` (unity, i.e. flat); band 7's
numerator is the textbook `1, 2, 1` lowpass shape; and that band's coefficients
sum to `2.1410` on both sides, a DC gain of exactly 1.

**The captured "init sequence" is not an initialisation.** It is one user's saved
configuration — mixer levels and the whole DSP chain — and replaying it imposes
that on any deck. Which payloads actually wake the hardware is unknown, and the
sequence is all-or-nothing today.

## Input report

58 bytes, **mixed-endian**, decoded field by field from named offsets — never
overlaid with a packed struct.

| Offset | Field |
| --- | --- |
| `0x00` | event type / buttons |
| `0x01` | touch flags — meaning unknown, **not** a contact flag |
| `0x02`, `0x04` | touch X, Y — **little**-endian |
| `0x06`, `0x07` | knob push, knob touch bitmasks |
| `0x08` | six knob values |
| `0x12` | **six** audio meters, one per track, each a 16-bit **big**-endian stereo pair |
| `0x39` | checksum |

Traps that cost real time:

- **Knob values are wrapping counters, not positions.** Accumulate signed 8-bit
  deltas, gated on the touch bit.
- **Contact is signalled by report *type*, not by byte 1.** A held finger and
  every two-finger gesture report `0x00` there.
- **Two fingers report one coordinate.** Multi-touch is not exposed.
- **An all-zero report is interleaved between real ones** and must be dropped, or
  every held button releases and every touched knob lets go.

## Screen

800×480 JPEG, chunked at 1025 bytes with a 13-byte header and 1012 of payload.
**Bytes 1 and 2 of the final chunk's header are `0x01`** — the end-of-frame
marker, without which the deck accepts the whole transfer and displays nothing.

`grabToImage(QSize)` takes *logical* pixels and Qt multiplies by the device pixel
ratio, so a request for 800×480 comes back 1000×600 at 125% scaling. Resize after
the grab.

## Six tracks

**The meters are per track, at `0x12`, four bytes each: left then right, 16-bit
big-endian, in knob order.** Confirmed with the vendor's per-track peak view on, a
live microphone, and music on System: those two pairs moved and the other four
were zero.

**The deck always sends all six.** Toggling the vendor's peak view sends it
nothing; that setting only changes what the application draws. Every capture ever
taken contained the full block -- the ones with music on Game and on Chat light
Game and Chat, which is what confirmed the knob-order mapping.

Three earlier readings died, all from the same mistake: decoding only the first
pair, which is the microphone. A live microphone hears whatever is played into the
room, so its meter appeared to answer every track in turn.

They **can** identify a channel: one meter per track, and only the track carrying
signal lights. They **cannot** confirm a volume register, because they sit
pre-fader -- the meter reads the same at 0% as at 100%. That one still needs an
ear. And beware the obvious test: playing a tone through headphones next to a live
microphone puts the tone into the mix through the microphone, which looks exactly
like the channel responding.

The knobs are printed on the deck, left to right: **Mic, Line In, Console, System,
Game, Chat**. The first three are physical inputs; the last three are the host's
digital tracks. The playback map is aux0/1 System, aux2/3 Game, aux4/5 Chat —
confirmed by ear first and since re-confirmed by measurement, because the meters
answer it directly (`setup-audio.sh --identify`).

The four capture pairs are identified too: aux0/1 the **creator** mix, aux2/3 the
**audience** mix, aux4/5 the **microphone**, aux6/7 host playback **pre-fader**.

Six knobs, six meters, four function buttons — all three confirmed against the
vendor spec. The manual names four tracks (System, Game, Chat, Mic); the other two
are unnamed, with the optical console input the obvious candidate. **Meter index
is knob order**, Mic first and Chat last, established five separate ways.

Two mixes: **Creator** (headphones) and **Audience** (what the stream captures).
In **Single Mix** they are identical; in **Dual Mix** they hold independent levels
for all six tracks. The mix toggle is captured and driven — `0x15` inside the
`0x1d` fence — and `Device::selectMix()` is it. **Single/Dual is located and not
driven**: the vendor writes `0x21` (`0x80` Single, `0x00` Dual) alongside `0x22`,
but `0x21` also selects which knob rings light, and which of the two it is doing
has never been settled on the hardware. In Dual Mix the MIC knob drives the
creator mix *and* the chat mic together.

## The touch flags byte at report offset 0x01

Still unnamed, but no longer shapeless. Measured over 136 reports and 6 contacts,
with `ax310_probe --touches`:

- **Every contact begins with `0x00`.** Six of six, and no other value ever
  started one.
- **The change is one-way.** `0x00 -> 0x1c` was seen once; `0x1c -> 0x00` never.
  Within a contact the value latches on and stays -- all 71 `0x1c` reports in
  that run belonged to a single contact, while five other contacts stayed at
  `0x00` throughout.
- **So it is not a counter**, which was the standing guess. A counter cycles;
  this does not. `0x10`, `0x14`, `0x18` and `0x1c` are `0x10 | (n << 2)` for
  n = 0..3 and look exactly like a two-bit field, which is what made the guess
  attractive -- but an earlier run counted them 94, 50, 12 and 166 times, and no
  counter is that uneven. A saturating ramp is, with `0x1c` as the top.

A second run, 638 reports across 11 contacts, sharpened it:

- **Every transition goes up, and none goes down.** `0x00 -> 0x10`, `0x00 -> 0x14`,
  `0x00 -> 0x1c` and `0x10 -> 0x1c` were all seen; in 627 transitions there was not
  one downward step and no contact ever returned to `0x00`. Ordered
  `0x00 < 0x10 < 0x14 < 0x1c`, the value is monotonic within a contact and
  saturates -- but it can skip levels rather than walking each one.
- **`0x00` is the not-moving state.** It moved in 36 of 156 reports; the other
  values moved in 76 to 93 per cent of theirs.
- **Instantaneous speed is not what picks the level.** One contact sat at `0x14`
  for 155 reports and another went straight to `0x1c`, and the mean step was 2
  pixels in both.

That reading -- an accumulator, distance or time since the contact began -- was
then measured and **refuted**. A run of one slow short drag and one fast long
swipe flipped after 28 px / 471 ms and 17 px / 47 ms respectively. Neither a
fixed distance nor a fixed duration fires at both, and the two times differ by an
order of magnitude.

The same run put the level ordering in doubt. The **slow** gesture settled at
`0x1c` and the **fast** one at `0x14`, which is backwards for a magnitude that
saturates -- and `0x14` carried a mean step of 40 pixels there against 2 in the
run before it, so the level does not track speed either.

What survives is that the value classifies the contact from how it begins, and is
fixed early. `0x14` and `0x1c` differ in one bit, `0x08`. The candidate is
press-then-drag against immediate flick -- the slow contact dwelled 471 ms before
moving and the fast one 47 ms. **Two contacts cannot establish that**, and it is
recorded here as the surviving guess rather than a finding.

That was measured too, and **refuted**: fifteen contacts containing five
deliberate flicks and five deliberate press-drags produced no `0x14` at all. The
`0x08` bit is not flick against press-drag. Three readings of that bit have now
died -- counter, accumulator, gesture class -- and it is left unexplained rather
than given a fourth.

~~What the same run did establish: the deck goes silent while a finger is still.~~
**Withdrawn, and the opposite is true.** A deliberate motionless hold, asked for
and performed as one, produced **228 reports from a single contact** -- about
eleven a second, with one of them carrying any movement. That agrees with the
older measurement of 45 reports across a 4.8-second hold and settles the conflict
in its favour: a held finger repeats, and `Device.cpp` has said so all along.

The single-report contacts that produced the retracted claim were brief taps, not
holds. Nobody said which gesture made which contact and the run was not designed
to say; the shape was read out of the numbers and the reading was wrong. This is
the third claim in one session promoted from data whose provenance was assumed --
see process-traps.md.

The question it raised is still worth having asked: `Device::dispatchEvent` treats the first **non-touch** report
as the finger lifting, and the deck streams meter reports continuously. If those
interleave with touch reports, every drag is delivered as press, release, press.
~~Measured: 9 of them across 15 gestures, so a drag really is delivered as press,
release, press.~~ **Withdrawn.** Every one of those reports was all-zero, and
`Device::poll` drops all-zero reports before anything sees them -- the deck
interleaves them and decoding one would release every held button. They never
reach `dispatchEvent`, so no spurious release was ever demonstrated.

The mistake was in the instrument: `--touches` counted reports the driver
discards, and the count was then written up as a driver defect. It now applies the
same filter, so what it counts is what `dispatchEvent` would actually act on.

**And the concern is now largely disposed of, by arithmetic rather than by
argument.** Idle, the deck reports at roughly ten to fifteen a second -- that is
what `--meters` measures. Eight seconds of dragging should therefore carry about
eighty non-touch reports. Two runs carried nine and zero.

So **the deck stops its heartbeat while a finger is on the glass**, and resumes
after the lift. That makes `dispatchEvent`'s rule -- the first non-touch report
ends the touch -- not a guess that happens to work but a reading of something the
deck actually does. It also explains the all-zero reports: they are occasional
fillers during a touch, not the suppressed heartbeat.

**Audio was playing during the zero run**, so the meters were not zero and a
heartbeat arriving mid-drag would have been counted. None did. The suppression is
observed directly, not inferred from rates, and there is no audio-dependent
failure in the touch path.

`0x48` and `0x49` are a separate shape -- bits 6, 3 and 0 -- and appear rarely.
They are **not** the two-finger case: the deck's owner reports the panel simply
does not track more than one finger, which agrees with `Protocol.hpp`'s standing
note that two-finger input produces no events at all. What they do mean is still
open.

## The panel keeps the last frame indefinitely

Stop sending frames and the deck goes on showing the last one it received, for as
long as you leave it. It does **not** blank on its own.

So stopping frames is not how the vendor software blanks the screen, and the
panel-power register is still unfound. An earlier version of this entry said the
opposite -- that the screen sleeps without frames -- which came from hardening a
hedged observation ("seems to be", "at least when we don't send frames") into a
fact within minutes of hearing it. The deck's owner then said plainly that the
last frame persists. Twice in this project a guess has been written down as
established and had to be taken back out; this is the second.

**Touch is reported with no application running at all.** Every touch measurement
so far was taken with `ax310_probe` alone and nothing pushing frames, and the deck
sent throughout -- 638 reports in one thirty-second run. Nothing has to be started
first to gather touch data.

The deck's owner mentioned the panel not always recognising a finger. That is
their observation and it is recorded as one rather than as a device fact: no
measurement has separated it from an ordinary capacitive screen missing a light
touch, and it is not the app, which was never running.

## The panel's brightness: group 0x0a, not a property

    01 0a <level>       write
    81 0a 00 00         read, and the answer puts the level back in byte 2

A group of its own, three bytes, and the level rides in the **address** slot
rather than a value slot -- which is why there is no room for a value and why the
length stays zero. Confirmed from both directions: writes of `19` and `64` matched
the vendor slider at 25% and 100%, and a read during the vendor's startup answered
`81 0a 64` while it was displaying 100%.

`0xff` blanks the panel. The captured init writes `0xaa`, which is outside the
25..100 the slider produces and is not the blanking sentinel; what it does is
unknown.

**A refuted reading worth keeping.** `0x1e` looks like screen brightness from the
captures alone, and the case was strong: the init writes `0x0d` and the shutdown
`0x09`, exactly the shape of a display being dimmed on the way out. It is not.
On hardware that register dims the knob LED rings, and writing `0x01` extinguishes
them. The screen's brightness was never in the property space at all, which is why
searching the property addresses for it could only ever fail.

## The microphone registers: 0x1f, 0x20, 0x23

Five one-action captures of the vendor software, all three addresses already in
`PreservedAddresses` and none of them ever named:

| capture            | `0x20` | `0x23` | `0x1f` |
| mic type XLR       | `0e`   | `00`   | `38`   |
| mic type + phantom | `0f`   | `08`   | `00`   |
| mic type 6.3 mm    | `0e`   | `00`   | `00`   |
| gain to minimum    | --     | `08`   | `00`   |
| gain to maximum    | --     | `00`   | `38`   |

**`0x1f` is the gain**, and the strongest reading here: the slider at its floor
sends `0x00` and at its ceiling `0x38`, so the range is 0 to 56.

**`0x20` bit 0 is phantom power.** XLR and 6.3 mm both send `0x0e` and only the
+48 V position sends `0x0f`. That the two non-phantom types are indistinguishable
suggests the deck detects the connector itself and only the phantom choice is a
setting -- which would mean "microphone type" is one host-controlled bit, not a
three-way selector.

**`0x23` is not resolved.** It is `0x08` for phantom and for gain-at-minimum, and
`0x00` for the other three. It cannot be a function of the gain alone: 6.3 mm sent
gain `0x00` with `0x23 = 0x00`, where gain-to-minimum sent the same gain with
`0x23 = 0x08`.

Also unexplained: switching to phantom sent gain `0x00` when the gain had been
`0x38` a capture earlier and nobody moved it. Dropping the gain before applying
+48 V would be a sensible thing for the vendor to do, but that is a guess about
intent and not a measurement.

**`0x1f` is confirmed and named `MicGain`.** Held at each end while speaking, the
microphone's own meter followed -- and the meter was the right instrument rather
than an ear, because there is a second interface upstream of this deck with its
own compressor which an ear cannot separate from the deck's preamp.

`0x20` is **not** confirmed. Its phantom-power reading rests on the captures
alone, and testing it means putting +48 V on whatever microphone is connected,
which is not something to do casually.

`0x23` was written with both of its observed values and **nothing observable
happened** -- no change to the microphone, the rings, the panel or the mix. It
stays unnamed, and it is now a register with two known values and no known
effect.

What would settle `0x23`: one capture of the gain slider at a **middle** position.
If `0x1f` takes an intermediate value the gain reading is confirmed continuous,
and whatever `0x23` does at neither extreme is the clue it has not given yet.

## The function buttons' colour: 0xc0, ten bytes, one button at a time

    00  <button>  01  <r> <g> <b>  ??  ??  <lit>  80

Fourteen records of the vendor software, one action each.

* **Byte 1 selects the button**, and the four are `0x3c` to `0x3f` -- known to be
  exactly those because turning all four off writes all four in one burst.
* **Bytes 3, 4, 5 are red, green, blue.** Driving one button to each primary gave
  `ff 00 00`, `00 ff 00`, `00 00 ff`.
* **Byte 8 lights it**: `0x1f` on, `0x00` off. Off writes black *and* clears this,
  so a colour of zero on its own is not what the vendor sends.
* **Bytes 6 and 7 are not understood, and they are not inert.** Two records with
  the same button and the same colour differ in them, so they are neither a
  checksum of the record nor derived from it. They are not passive either --
  see below.

**There is no brightness field.** The vendor scales the colour host-side: its
slider at minimum sent `0x19` on the lit channel and at maximum `0xff`. `0x19` is
25, the same floor its panel-brightness slider uses.

**The selectors are clockwise where `Button` is row-major:**

    0x3c  top-left       0x3d  top-right
    0x3f  bottom-left    0x3e  bottom-right

so `FirstButtonSelector + index` lights the wrong two and the mapping is a table.

The selector table is confirmed **in the output direction**, by driving all four
in one pass to red, green, blue and yellow and reading the deck: each colour
appeared under the button the table names. One pass rather than four, so the
answer cannot be an artefact of writes landing out of order.

That pass was spaced, and the spacing is load-bearing: four consecutive records
at `0xc0` with no gap between them do **not** all take effect. See the section
below, and a deck that attaches with one button lit in the last record's colour
and the other three dark.

### Records sent back to back are not all applied

The shutdown sequence darkens the four buttons with four records to `0xc0`, one
per selector, and sent with no gap between them the deck applies only some: a
shutdown leaves one button still lit, and which one differs between runs. There is
nothing wrong with the records -- the same bytes work when they are spaced.

The handshake already carries a 10 ms gap per payload, with the note that the deck
does not come back without it. Given the same gap the shutdown leaves exactly the
state it asks for, checked by lighting all four buttons first so a record that
failed to land would be visible.

So the pacing is not a property of the handshake, it is a property of the deck.
Anything sending a run of records should space them, and a burst to one address is
where it shows first.

### Bytes 6 and 7 decide whether a colour lights at all

The vendor's initialisation writes all four buttons and **only two of them light**:
the two it writes `ff 00 00`. The two it writes `00 37 ff` stay dark. Both records
are otherwise identical in shape, and both carry `f9 3d` in bytes 6 and 7.

Driving the same selector with the same `00 37 ff` and `00 1d` in that pair --
one byte pair changed, nothing else -- lights it blue. So:

| colour | bytes 6,7 | result |
| --- | --- | --- |
| `ff 00 00` | `f9 3d` | lit, red |
| `00 37 ff` | `f9 3d` | **dark** |
| `00 37 ff` | `00 1d` | lit, blue |

The pair therefore gates or shifts a colour rather than riding alongside it. It is
not a simple on/off, because red lights under `f9 3d` and blue does not.

`0xf9` there is worth holding beside two other unexplained bytes with the same
look: byte 6 of a knob-ring record is `0xf8`, and byte 4 of a surround record is
`0xf8` at rest -- the one whose effects were described as "interesting colour
effects that I cannot describe yet". Whether the three are one field is untested.

**What this means for writing code:** use `buttonColourRecord()`, whose pair is a
combination observed lighting a button, and do not copy bytes 6 and 7 out of a
capture. `DefaultButtonColours` goes out through that builder at every connect,
which is why all four light where the vendor's own replay leaves two dark.

### The input direction, settled the same way

`ax310_probe --inputs` lights one button at a time and reads the byte that
arrives, so a press is identified by the light rather than by the table being
checked. Confirmed on the deck, at report offset `0x00`:

| button | bit |
| --- | --- |
| top-left | `0x08` |
| top-right | `0x04` |
| bottom-left | `0x02` |
| bottom-right | `0x01` |

which is `ButtonBits` as written -- backwards against `Button`'s order, and that
is simply what the hardware does.

The knob pushes, at offset `0x06`, are `KnobBits` as written: `0x01` Mic, `0x02`
Line In, `0x04` Console, `0x08` System, `0x10` Game, `0x20` Chat. The knobs cannot
be lit one at a time -- the ring record colours all six -- so their identity comes
from the legend printed on the deck, and the walk takes the pushes in the order
they arrive rather than asking for one per round.

Two more things the same walk settled:

* **A release is reported**, roughly 300 ms after the press, as a report with the
  byte back to zero. That is what lets the driver's rising edge re-arm; without it
  every second press of the same button would be lost.
* **Two buttons held together arrive as one report with both bits set** -- `0x09`
  for top-left and bottom-right. The bitmask reading is what the hardware does.

**A trap worth keeping, because it cost three runs.** A walk that asks for one
press per round is only as good as the pacing of the person at the deck: a press
landing just after its round's deadline satisfies the *next* round, and every
later press is then credited to the wrong control. Read that way, one slow start
reported `bottom-left` as `0x08`, which is `top-left`'s bit. Two controls cannot
share a bit, so a repeat in the results is a mis-press rather than a finding --
the tool checks for that now and refuses to publish a verdict from such a walk.

The cue matters as much as the check. The lit button is what tells somebody at the
deck what to press, and the walk's phases are told apart by the lights alone:
one lit, then all four dark, then all four lit. Writing the newly lit button
*after* darkening the others makes every transition flash all-four-dark, which is
the next phase's cue -- so the lit one is written first.

## Switching the mix takes three writes, not one

`SelectedMix` (`0x15`) alone moves the audio. It does **not** move the knob rings:
they keep the colour and the levels of the mix that was on before, which from the
desk looks exactly like a switch that did not happen -- the audio changes, the
panel's rendering changes, the hardware does not.

The vendor's own switch, captured whole, is one settings transaction:

    SET 0x1d = 01     begin
    SET 0x1e = 0d     knob LED brightness, already at that value
    SET 0xc0 = 01 c0 0a ff 7d 00 fd 00 1f 80    ring colour, orange
    SET 0x21 = 01     KnobLedSelect
    SET 0x22 = 01     unrecorded
    SET 0x27 = 14     the creator mix's Mic level
    SET 0x15 = 01     SelectedMix = audience
    SET 0x1d = 00     end

Three of those are the switch. The ring **colour** has to be written because the
record carries no mix -- the deck applies it to whichever mix is selected -- so a
switch that does not say the new colour leaves the old one glowing. `0x21` is what
moves the **levels** the rings display. `0x15` moves the audio.

Reading `0x21` across three captures gives both directions, and one byte carrying
two things:

| action | `0x21` | ring colour | `0x15` |
| --- | --- | --- | --- |
| switch to the audience mix | `0x01` | `ff 7d 00` orange | `0x01` |
| Dual Mix on | `0x00` | `00 7d ff` blue | `0x00` |
| Dual Mix off, back to single | `0x80` | `00 7d ff` blue | `0x00` |

so `0x80` is a single creator mix, `0x01` a single audience mix, `0x00` Dual Mix --
and `0x80` is what init writes, which is why a freshly attached deck monitors the
creator mix.

**Confirmed on the hardware:** with the colour, `0x21` and `0x15` inside one fence,
both the ring colour and the ring levels follow the switch in both directions.

Two writes in the vendor's sequence are deliberately not replayed. `0x27 = 0x14`
is the creator mix's Mic level -- somebody's setting caught in the capture, and
replaying it would overwrite the level the person at the deck had chosen. `0x22`
has no recorded meaning; the rings follow without it, so it stays unwritten until
something shows what it does.

### A ring colour that did not land, and two explanations for it

**Measured:** a deck brought up on the audience mix showed `0x15 = 01`,
`0x21 = 01`, an orange panel, orange tiles, and six **blue** rings at 50%. Blue
at 50% is what the handshake imposes, so the ring-colour record that should have
replaced it did not take effect.

**Not measured, and both still open.** Two things could produce that, and this
deck has not been asked which:

1. *The order.* A record carries no mix, so the deck may apply it to whichever
   mix is selected when it arrives -- in which case a colour sent before the
   switch paints the mix being left, one step behind forever.
2. *The spacing.* The record went out with no gap on either side, between a
   property write and the fence close.

The captures argue **against** the first. `switch-mix.pcap`, read with
`ax310_decode`, is the vendor doing this successfully:

```
  0              SET 0x1d = 01          fence open
  1   +43.817ms  SET 0x1e = 0d          knob LED brightness
  2  +289.473ms  SET 0xc0  01 c0 0a ff 7d 00 fd 00 1f 80    orange, BEFORE the switch
  3  +126.507ms  SET 0x21 = 01
  4  +248.094ms  SET 0x22 = 01
  5  +248.667ms  SET 0x27 = 14
  6   +43.276ms  SET 0x15 = 01          the switch
  7   +43.000ms  SET 0x1d = 00          fence close
```

The vendor writes the colour four commands *ahead* of `0x15` and it works. So
"the deck applies a record to the mix selected when it arrives" cannot be the
whole story, and the ordering explanation is an inference this capture
contradicts rather than a finding.

What the capture does show is scale. The vendor never leaves less than 43 ms
between any two commands here, and gives that record 289 ms ahead of it and 126
after. Across every capture in this project, two `0xc0` records are never closer
than about 250 ms; the only gaps under 5 ms are a record followed by a *read*.
This driver was sending the record with none at all.

Against that: 10 ms was enough to make the four button records all land, which is
measured and is recorded above. So a gap of some size is needed and 10 ms can be
sufficient -- which leaves it unclear why this one record failed.

Both changes are in the driver now, the order and a gap, and **neither is
established as the fix.** What settles it is a deck: switch back and forth
several times and watch whether the rings follow, then take the order back to the
vendor's and watch again.

### Single mix mode collapses the playback tracks across both mixes

`0x21` is not only which mix the rings show. It is the mixer *mode*, and the mode
decides whether the two level blocks are independent at all.

Measured with `ax310_probe`, no application running. Two distinct blocks were set:

```
creator  0x27   12 0c 0a 0a 0c 0e
audience 0x2e   0e 0c 0a 08 06 04
```

Writing `0x21 = 0x80` -- single, monitoring the creator mix, which is what
`Device::selectMix` sends -- left the creator block alone and changed the
audience block to:

```
audience 0x2e   0e 0c 0a 0a 0c 0e
                         ^^^^^^^^ the creator block's last three, copied
```

Tracks 3, 4 and 5 are **System, Game and Chat** -- the three host playback tracks
from `scripts/setup-audio.sh`. Tracks 0, 1 and 2 are Mic, Line In and Console,
the physical inputs, and they kept their own values. So a single mix means one
mix *of the host's audio*: the deck copies the monitored mix's playback levels
over the other block, and leaves the inputs per-mix.

With `0x21 = 0x00` -- Dual Mix -- the same two blocks were set again and stayed
independent, across repeated reads.

**This is why per-mix volumes do not survive a switch.** `KnobLedSelectForMix`
holds `{ 0x80, 0x01 }` and nothing else, so every `selectMix` writes a single-mix
value, and `connect()` calls `selectMix` too. `0x21` is in `PreservedAddresses`,
so the handshake restores whatever mode the deck was in -- and then the adoption
step immediately overwrites it with Single. A deck the user had put in Dual Mix
comes back Single, and the first switch flattens the playback tracks.

Two mixes that are independent is what the README promises, and it needs
`0x21 = 0x00`. The driver cannot currently write it: see `docs/todo.md` on
`MixId` having two enumerators where the deck has three states.

## The knob rings' colour: the same 0xc0, with byte 0 choosing the bank

    01  c0  0a  <r> <g> <b>  ??  ??  1f  80

Four records. Byte 0 is what separates this from a button record: `0x00` a
function button, `0x01` the knob rings. Bytes 1 and 2 then read as a first light
index and a count -- one light at `0x3c` for a button, ten from `0xc0` for the
rings -- which fits every record captured and has **not** been tested by writing
anything else. Bytes 3-5 are red, green, blue, proven with the three primaries.

**The mix is not in the record.** The vendor offers a ring colour per mix, and
"red on the creator mix" and "red on the audience mix" are byte-identical
captures. The deck colours whichever mix is selected, so the colour cannot be
aimed -- a caller wanting the other mix's colour selects that mix first.

The vendor writes `KnobLedBrightness` (`0x1e`) immediately before every one of
these, with the value already in the register. Nothing has shown the colour
depends on it, so the driver does not replay it.

Bytes 6 and 7 were `f8 00` in all four captures. That is not evidence of meaning:
the button records carry `bc 1d`, `00 1d` and `00 00` in the same two bytes, so
the pair is the same unexplained padding there as here.

Driven on hardware with `ax310_probe --knob-colour`: the rings take the colour.

## The surround strip: 0xe0, ten bytes, six modes

    01  <mode>  01  20  <rate>  00  00  <r> <g> <b>

Thirteen records covering all seven of the vendor's modes and both ends of its
colour, brightness and frequency controls. This closes `0xe0`, the last record
address that was unaccounted for.

* **Byte 1 is the mode**, and the six run four apart rather than one:

      0x24  scrolling rgb    0x30  blinking rgb
      0x28  pulsing rgb      0x34  solid
      0x2c  blinking         0x38  pulsing

  What the low two bits are for is unknown; every captured record has them clear.
* **Bytes 7, 8, 9 are red, green, blue.** Proven with green (`00 ff 00`) against
  blue (`00 00 ff`). The three hue-cycling modes ignore the colour and the vendor
  still fills it with white rather than leaving it stale.
* **Byte 4 is the frequency, and only in the modes that animate.** The slider's
  ends gave `0x01` and `0x0a`, and holding it still while dragging brightness the
  whole way left it at `0x0a` in every record.

**"Off" is not a mode.** The vendor's off sends solid (`0x34`) with a black
colour, byte for byte. Since nothing restores `0xe0` on connect, a strip left
black stays black across a replug and looks exactly like one that does not work.

**No light on this device has a brightness field.** Brightness is applied to the
colour before it is sent, spanning `0x19` to `0xff` per channel -- on the strip in
solid and in pulsing alike, and on the buttons and the rings the same way. Five
captures settled it: at both ends of the brightness slider byte 4 was `0xfb`
while the colour travelled the whole distance. A hand-placed midpoint gave `0x85`,
which a straight line between the ends puts at 47%, so the curve is taken as
linear -- consistent with the evidence rather than measured from it, because a
dragged slider cannot distinguish a line from a gentle curve.

The dark channels stay at zero when dimmed; the floor is not a colour shift, or
a dimmed red would wash out to pink.

**Byte 4 in solid mode is unexplained, and the deck does read it.** The vendor
puts `0x7d`, `0xcd`, `0xf8` or `0xfb` there, and what it puts there tracks neither
of the two things it could have:

* Not brightness. `b100` and `b0`, the two ends of the slider, both sent `0xfb`
  while the colour travelled the whole distance.
* Not colour. Clicking through all ten presets sent `0xfb` for every one of them,
  twenty-two records with ten different colours in them.

Those two say what the *vendor's software* does with the byte. They do not say
what the *deck* does with it, and the two came apart here: driving the byte by
hand produces colour effects on the strip that have not been described yet.

So the vendor never varies it in a way a capture could show, and the deck reacts
to it anyway. An earlier version of this entry concluded from those same two
observations that the byte was stale struct memory -- a claim about the device
drawn from evidence that only covered the host. Worth remembering as its own
trap: "the vendor never exercises this field" and "this field does nothing" are
different statements, and only the first one is in a capture.

An earlier version of this entry said byte 4 was brightness in solid, on a single
capture in which brightness and byte 4 happened to move together. Two things
moving in one capture is not one causing the other.

## The vendor's ten surround presets

Clicked one at a time, in the order the palette lays them out, with the eleventh
click returning to the first and reproducing its bytes exactly:

    c4 00 00    c4 5f 00    00 c4 00    00 c4 5f    00 c4 c4
    00 62 c4    00 00 c4    5f 00 c4    c4 00 c4    c4 60 c4

A hue wheel, and the presets peak at `0xc4` rather than `0xff` -- the brightness
slider scales them up to `0xff`, which is the same rule as everywhere else.

**The order is red-green-blue**, settled on hardware: `ff 00 00` driven at the
strip is red. So the palette above starts at red and runs forward through the
wheel.

It is worth recording how nearly this was missed. Nothing had tested it. Every
check that had been run was symmetric between red-green-blue and blue-green-red --
green is `00 c4 00` in either reading, white is white either way, and the one
asymmetric case that reached hardware was a pulse watched for *whether* it pulsed
rather than for its colour. A hue wheel read backwards is still a hue wheel, so
the ten presets could not settle it either. One command and one look did.

All six modes have since been driven on hardware with `ax310_probe --surround`,
along with `off`, so the mode table and the colour bytes are confirmed and not
merely replayed.

A note on the mode sweep's colour. Those records carry `00 00 ff`, which with the
confirmed order is blue, though the capture plan had asked for red. Nothing drawn
from them depends on it -- they were read for the mode selector in byte 1, which
is the same whatever colour is alongside it -- but it is the reason the channel
order stayed unverified for as long as it did, and the reason to state a record's
colour from the bytes rather than from what was meant to be clicked.
