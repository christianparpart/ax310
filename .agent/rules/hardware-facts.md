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
* **Bytes 6 and 7 are not understood and are not part of the colour.** Two records
  with the same button and the same colour differ in them, so they are neither a
  checksum of the record nor derived from it.

**There is no brightness field.** The vendor scales the colour host-side: its
slider at minimum sent `0x19` on the lit channel and at maximum `0xff`. `0x19` is
25, the same floor its panel-brightness slider uses.

**The selectors are clockwise where `Button` is row-major:**

    0x3c  top-left       0x3d  top-right
    0x3f  bottom-left    0x3e  bottom-right

so `FirstButtonSelector + index` lights the wrong two and the mapping is a table.

The colour path has since been driven on hardware with `ax310_probe --button` and
the result accepted, so the selector table is confirmed **in the output
direction**: naming a button lights that button.

That says nothing about the input direction. `ButtonBits` runs `0x08, 0x04, 0x02,
0x01` -- reversed against `Button`'s order, for no recorded reason -- and is a
separate table that no test has touched. It is now easy to settle, though, and
without a VM: light one button a colour the others do not have, press it, and see
which bit arrives. The output mapping being confirmed is what makes that
unambiguous, because it identifies the button being pressed.

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

**Byte 4 in solid mode is not a parameter.** It carries `0x7d`, `0xcd`, `0xf8` or
`0xfb`, and it is neither of the two things it could have been:

* Not brightness. `b100` and `b0`, the two ends of the slider, both sent `0xfb`
  while the colour travelled the whole distance.
* Not colour. Clicking through all ten of the vendor's presets sent `0xfb` for
  every one of them, twenty-two records with ten different colours in them.

It moved only transiently, from `0xcd` to `0xfb` in the middle of a drag with
nothing else written. Treated as stale struct memory, the same as bytes 6 and 7 of
a `0xc0` record, and not read again.

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

**Which end of the wheel is which is not settled.** Read as red-green-blue the
sequence runs red to green to blue; read as blue-green-red it is the same wheel
walked backwards. Every test so far has been symmetric between the two: green is
`00 c4 00` either way, white is white, and the one asymmetric hardware test that
was run -- a "blue" pulse -- was only ever watched for whether it pulsed. Driving
`ff 00 00` at the strip and looking at it settles it in one command.

All six modes have since been driven on hardware with `ax310_probe --surround`,
along with `off`, so the mode table and the colour bytes are confirmed and not
merely replayed.

A note on how the first sweep read: the mode captures were taken at the vendor's
default colour, which is blue, not the red the capture plan asked for. Reading
`00 00 ff` as red-in-some-other-byte-order would have inverted the channel order
for every record here. Green settled it.
