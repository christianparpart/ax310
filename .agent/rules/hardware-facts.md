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
