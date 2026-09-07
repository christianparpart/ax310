# ax310 — AVerMedia Live Streamer AX310 driver

A C++23 userspace driver for the AVerMedia Live Streamer AX310 control deck, plus
a Qt/QML demo application that drives it. The device is not publicly documented:
its HID protocol is being reverse-engineered from USB captures, and `scripts/`
holds the tooling that produced what we know so far. `docs/capabilities.md`
tracks what the vendor's own documentation says the deck can do against how much
of it we have — the target list this effort is working through.

`docs/todo.md` is the internal todo list, ordered by what unblocks the most.

**Read `.agent/rules/` before doing hardware work.** It holds the established
protocol facts, how to capture and decode a single action, what must never be
written to the deck, and the mistakes already made here. It exists so none of that
is re-derived.

The project is young — most of the rules below describe the shape the code is
being moved *towards*, and the current sources do not satisfy all of them yet.
"Known deviations" at the end names each gap explicitly. **New code is held to
the rules; touched code moves towards them.**

## Comments say what the code is

Never write what the code used to be, what a thing was called before, or what an
earlier version of a comment claimed. Git records that, and a comment about a
state the repository has left goes stale in a way nothing detects -- it was never
true of the code in front of the reader.

`scripts/check-comment-history.py` enforces this and runs as a CTest entry.

The exception is not about code at all. A **refuted reading of the hardware** --
"byte 4 is not brightness, and here is the capture that made it look like it was"
-- stops the next reader deriving the same wrong answer from the same evidence,
and this project has spent whole sessions on exactly that. Those belong in
`.agent/rules/hardware-facts.md`, beside the evidence they warn about, not in a
header where somebody is reading to find out what the code does.

## The hardware

The deck presents **two USB devices at once** — `07ca:0310` and `07ca:1310` —
and they coexist permanently. They are two functions of one physical unit, not
two states of one device; nothing re-enumerates, and there is no "mode" to
detect. Verified from the report descriptors:

| Device | Interface | What it is |
| --- | --- | --- |
| `07ca:1310` | 0, usage page `0xffa0` | **The deck.** INPUT 58 bytes, OUTPUT 1024 bytes, FEATURE 64 bytes |
| `07ca:0310` | 4, usage page `0x000c` | Consumer Control — play/pause, next, volume, mute. The audio side's media keys, and nothing the driver wants |

**The vendor interface declares no report IDs.** hidapi still prefixes every
transfer with a report-id byte, so that byte is always `0x00` and every buffer is
one longer than its payload: a 64-byte command goes out as 65, a 1024-byte screen
chunk as 1025. Getting this wrong fails in the worst possible way — hidapi reads
the first payload byte as a report id, the kernel rejects the transfer, and the
deck simply stays asleep. That is precisely what happened: all 74 initialisation
payloads were being rejected, silently, for the whole life of the project.

**The deck comes up asleep and must be initialised on every attach.** Before the
initialisation sequence it has a dark screen and sends *no input reports at all*.
After it, the screen lights and reports stream continuously. There is no state to
test for: an uninitialised deck is indistinguishable from an initialised one
until you wait for a report that never comes.

What the deck exposes:

- 4 physical buttons, one bit each in byte 0 (`0x08` top-left, `0x04` top-right,
  `0x02` bottom-left, `0x01` bottom-right).
- **Every input report is checksummed.** Byte 0x39 carries the low eight bits of
  every preceding byte summed. Verified against 164 distinct reports from two
  independent captures with no exceptions, so a report that fails it is corrupt
  and is dropped rather than decoded into something plausible.
- 6 rotary knobs. Byte 6 is the push bitmask, byte 7 the capacitive-touch
  bitmask, bytes 8–13 one counter per knob.
  **Those counters are relative, not positions** — knob 1 has been observed
  reporting `0x4d` while the ring has only 21 levels — so a turn is the signed
  8-bit difference from the previous report, and the absolute level is the
  driver's to keep. The counter also moves on knob pushes and button presses
  (seen going `0x0f` → `0x2f` → `0x4f` across a push and a button), so a change
  only counts as a turn while that knob's touch bit is set.
- **Six audio meters from byte `0x12`**, one per track in the deck's printed knob
  order, each a stereo pair of 16-bit **big-endian** values — the opposite
  endianness from the touch coordinates in the same report. Full scale `0x7fff`
  and linear: a tone at 0.7 of full amplitude reads `0x5999`, which is 70%.

  Confirmed against the hardware from the host side as well as from captures:
  `setup-audio.sh --identify` plays a tone into each per-track sink and reads
  48% on that track and 0% on the other five, three times out of three.

  The meters are **pre-fader** — they read the same at 0% as at 100% — so they
  identify a channel and cannot confirm a level register.

  This entry said something different three times. The long version, including
  why, is under "the report" below; it is worth reading before trusting any
  future statement about these bytes.
- An 800×480 touch screen on the OUTPUT endpoint (EP `0x02`, 1024 bytes). Frames
  go out as JPEG (baseline, 4:2:0, JFIF — exactly what Qt produces) in chunks of
  `[seq][end][end][4×0][len:le16][sum:le16][payload…]`, where `sum` is the
  16-bit unsigned sum of the chunk's payload bytes.

  **The two bytes after the sequence number mark the last chunk of a frame**:
  `0x01` on the final chunk, `0x00` on every other. Verified across 49 of the 50
  complete frames in the vendor capture. Without them the deck accepts every
  chunk, reports no error, and never puts the frame on screen — which is exactly
  how it behaved for the whole life of the project.

  The frame must be **exactly 800×480** in real pixels. This is a trap worth
  knowing: a grab comes back at whatever size the display's device pixel ratio
  imposes, so on a desktop at 125% an 800×480 window grabs 1000×600 and on a 2×
  one it grabs 1600×960. Asking for the right size does not help —
  `QQuickItem::grabToImage(QSize)` takes its target in *logical* pixels and Qt
  multiplies by the same ratio. The frame is resized after the grab instead,
  which is the only form that holds for every ratio, fractional ones included. A
  wrongly sized frame is accepted by the transport and then silently ignored by
  the deck, so it looks exactly like a dead screen.

  **The panel is rendered in a window the compositor never maps.** It is created
  and never shown, and `QQuickWindow::grabWindow()` renders it; posted mouse
  events still reach it, so the deck's touch screen works. Two things follow that
  are not obvious. Moving the window off-screen instead does not work under
  Wayland, where a client does not get to say where its windows go — that is how
  the panel came to appear on the desktop as a window nobody could click. And
  every grab of an unmapped window builds a throwaway RHI, so a `Shape` drawn by
  the shader-based `CurveRenderer` renders the first frame and nothing
  afterwards; `GeometryRenderer` triangulates on the CPU and survives. The
  rendering tests pin the software scene graph and cannot see either problem,
  which is why there is a native-backend CTest entry that can.
  Touches come back in the input report as an event of type `0x10`, with x and y
  at bytes 2–5 as **little-endian** pixel coordinates — the opposite endianness
  from the audio meters in the same report, which is not a typo. Verified across
  1367 touches from the deck: read little-endian every one landed inside
  800×480; read big-endian none did.

  **Contact is signalled by the report type, not by any flag.** While a finger is
  down the deck sends screen-touch reports and **suppresses its ordinary
  heartbeat** — idle it reports 10 to 15 times a second, and eight seconds of
  dragging carried nine ordinary reports in one run and none in another, where
  eighty would be expected. That is why the lift is the first ordinary report
  afterwards: the deck really does go quiet for the touch, so the rule reads a
  signal rather than getting lucky.

  **A held finger repeats, at about eleven reports a second.** Two measurements
  once disagreed about this; a deliberate motionless hold settled it, producing
  228 reports from one contact with a constant coordinate. That agrees with the
  older count of 45 across 4.8 seconds. The reading that said a still finger goes
  unreported came from single-report contacts assumed to be holds, which were
  taps.

  Byte 1 carries flags nobody has explained. It was read as a contact flag, and
  that was wrong in an expensive way: a finger held still reports `0x00` for the
  whole press, and so does **every two-finger gesture**, so stationary taps and
  all two-finger input produced no events at all. Only a single dragging finger
  sets it non-zero — which is why dragging was the one thing that appeared to
  work. Values seen with a finger down: `0x00`, `0x10`, `0x14`, `0x18`, `0x1c`,
  `0x48`, `0x49`. It is not speed and it is not finger count; both were tested
  against captures and refused.

  **Two fingers report one coordinate.** No second contact point appears
  anywhere in the report, and the knob bytes are untouched during a gesture, so
  multi-touch looks simply not to be exposed on this interface.

  The driver derives press / move / release from all this, and suppresses the
  repeats a held finger sends five times a second.
- Per-knob LED rings, level `0…0x14`, all six carried in one command.

**The command protocol has a grammar**, recovered from the captured init and
shutdown sequences rather than guessed:

```
[0x01 SET | 0x81 GET]  0x10  <property>  <length>  <values…>
[0xfe]  0x00  <length>  <body…>  <checksum>
```

The first form is a property read or write — the same property id appears with
`0x81` carrying zeroes and with `0x01` carrying a value. 39 of the 51 captured
property commands carry nothing past their stated length; the twelve that do are
all `0xc0`/`0xe0`, where the length is a **10-byte record size** and the payload
holds several records.

The second form frames differently: the length counts the whole command and the
last byte is the low byte of the body summed. **All 24 such commands in the
captures verify**, and everything past the stated length is zero.

**Byte 2 is an address, not a property selector.** Every one of the 256 ids
answers a read, and consecutive ids return overlapping windows of one byte
stream: reading around `0xd0` spells out `5311291500056`, the deck's serial
number as `lsusb` reports it. So the deck exposes a register space, and byte 3
is how many bytes to read or write there.

**Reads work, so the device can be asked rather than guessed at.** Send an
`0x81` command and then `hid_get_feature_report`; the answer echoes the address
and length back, which is what tells a real reply from a stale buffer.

Confirmed against the hardware by writing a value, watching, and restoring it:

| Address | What it does |
| --- | --- |
| `0x1e` | **knob LED ring brightness** — `0x01` extinguishes the rings, `0x0d` at init |
| `0x21` | selects **which** rings light — `0x10` leaves only knob 1 lit; also tracks the mixer mode |
| `0x14` | writing `0x00` lights every ring; init writes `0x01` |
| `0x27` | the six per-knob LED levels — the **display**, not the mixer |
| `0x2b` | **the Game track's volume**, `0x00..0x14`; volume is per-address |
| `0x2e` | **the second mix's six levels** |
| `0x1d` | brackets a settings change: `0x01` before, `0x00` after |
| `0x15` | **which mix is monitored** — `0x00` creator, `0x01` audience |
| `0x0f` | written only by shutdown |

`0x1e` was the screen-brightness candidate on the strength of the captures — the
only property both sequences write, and to different values. The hardware said
otherwise: it dims the **knob rings** and leaves the panel alone. Right register,
wrong subsystem, and a good argument for testing a hypothesis on the device
before naming anything after it.

`0x27` is confirmed as what the rings display: a staircase written to it steps
the rings, and the same staircase written to `0x2e` changes nothing visible.

**Capturing the vendor software settled four registers at once.** Four one-action
captures — enable Dual Mix, disable it, switch creator to audience, drag one
track's volume — produced between eight and fifteen commands each, against an idle
baseline of **zero**. Creator Central sends nothing when untouched, so every
command in an action capture is that action.

- **The per-track levels are two contiguous six-byte blocks, one per mix**, based
  at `0x27` and `0x2e` and laid out in the deck's printed knob order: Mic, Line In,
  Console, System, Game, Chat. So `0x2a` is System's level in the first mix
  because it is `0x27 + 3`, and dragging the Mic slider writes `0x27` because Mic
  is track 0. Written with length 7 at a base, one command carries all six.

  This was recorded for a while as "a different address per track, and Mic is an
  exception". It is not an exception and they are not unrelated addresses — one
  block, indexed. Reading `0x27..0x2c` and `0x2e..0x33` shows both blocks whole.

  **They control audio, not just the rings.** Muting System in the first block
  silences a System tone in the mix on the deck's first capture pair; writing
  `0x31`, the same track in the second block, silences it on the second pair and
  leaves the first alone. The deck's own meters cannot show this — they are
  pre-fader — which is why this went unconfirmed for so long.
- **`0x2e` is the second mix's levels.** Enabling Dual Mix writes it; disabling
  Dual Mix, switching the monitored mix and dragging a volume all do not. That is
  the register a second independent mix needs and nothing else does.
- **`0x1d` brackets a settings change**, `0x01` before and `0x00` after. Every
  mode change is wrapped in it; volume drags are not, which fits something that
  streams continuously. The init sequence never writes it.
- **`0x15` selects the monitored mix**, `0x00` creator and `0x01` audience. Only
  creator-to-audience has been captured; the reverse is inferred.

Two things fall out. **Our captured init sequence was recorded in Single Mix,
monitoring the creator mix** — it writes `0x21 = 0x80`, `0x22 = 0x12` and
`0x15 = 0x00`, and disabling Dual Mix writes exactly that same `0x21`/`0x22` pair.
And the `0xc0` record's bytes 3 and 5 swap `00 7d ff` to `ff 7d 00` when the
monitored mix changes, which is a pair of complementary gains — the monitor
crossfading between the two mixes.

Reads also confirm the reply layout: `[report id][0x81][0x10][address][length]`
then the values, echoing address and length back. Bytes past the answer's length
are **stale** — the deck leaves the remainder of its previous reply in the buffer
— so a reader must trust the echoed length and nothing beyond it.

**Do not write addresses the captured sequences do not write.** Writing `0x01`
to `0x16` wedged the deck — every read after it failed and it needed a power
cycle. Reads across the whole space are harmless; writes outside the vendor's
own set are not, and sweeping them blind is not a technique this project should
use again. The reliable way to learn a command is to capture the vendor software
performing the action, which is where the init sequence came from.

**Ring colour encodes the mix** — blue for the creator mix, orange for the
audience mix. There is no hardware control for it: the vendor's software draws a
button on the deck's own screen, receives the touch through the ordinary input
report, and then sends a command. So the toggle is host-driven and the command
exists, but it was never in our capture, which recorded startup only.

**The deck also has a mono-mix mode.** Beyond choosing which mix you monitor,
the vendor's software can collapse the two into one, so the streamer and the
audience hear the same thing. Whether the microphone is treated the same way in
that mode is not established. Like the audience/creator toggle this is a
software-side switch with no hardware control, so the command exists and is not
in our capture.

The two mixes do appear on the **capture** side. The deck presents eight capture
channels as four stereo pairs, and with a tone playing, pairs `(FL,FR)` and
`(FC,LFE)` carry identical content. That has two readings and the capture cannot
tell them apart: two mixes that happen to be configured identically, or a deck in
mono-mix mode presenting one mix twice. The `0x94` blocks favour the first — our
capture writes the same parameters to both selector values — but the question is
open, and which pair is creator and which audience is unsettled either way.

**The deck carries a DSP, and the captured sequence configures it.** The `0xfe`
family is not miscellaneous: sorted by command byte it is plainly an audio
pipeline being set up, and the deck is known to offer at least a room reverb and
a compressor.

| Command | Body | Reading |
| --- | --- | --- |
| `0xb2` | none | apply or commit; always follows an `0x81 0x01` |
| `0x85` | 1 byte | **delay-effect enable**, shared by reverb and echo |
| `0x94` | 21 bytes | **delay-effect parameters**; body byte 14 is `0` none, `1` reverb, `2` echo |
| `0x9b` | 1 byte | **compressor enable**; **not in the init sequence at all** |
| `0x9f` | 17 bytes | **compressor parameters** |
| `0x9e` | 22 bytes | **noise gate parameters** |
| `0x87`, `0x88`, `0x9c` | 1 byte | **equaliser enables**, always written together |
| `0xa3` | 23 bytes | **an eight-band biquad equaliser** — see below |

Every one of these was established by capturing the vendor software toggling one
effect at a time. Five effects, and the DSP is smaller than five blocks: **reverb
and echo share one enable and one parameter block**, distinguished by body byte 14
of `0x94`. Switching to echo sends `0x85 = 01` and a body differing from reverb's
in that byte and one parameter.

That byte has now been read wrong twice, which is worth recording as a caution
rather than hiding. First as a *mix* selector — two blocks differing only there,
in a device with exactly two mixes. Then as a mirror of the enable, which fitted
every value seen until the echo capture produced a third. Both were consistent
with the evidence at the time. **A two-valued field looks like a flag until it
takes a third value.**

**The reverb's five sliders are mapped**, each to its own body byte of `0x94`:

| Slider | Body byte |
| --- | --- |
| decay | 1 — moves for **time** *and* **room size** |
| damp | 2 |
| level | 3 |
| diffusion | 8 |
| room size | 18–19, 16-bit little-endian, alongside byte 1 |

Level, damp and diffusion are plain 8-bit controls, each seen spanning `0x00` to
`0xff`. Byte 1 being shared is not a muddle: the feedback gain a reverb needs for a
given decay time depends on the delay length, and the delay length *is* the room
size — so the host computes the coefficient and sends the size beside it.

Mapped by dragging one slider at a time. The two preset captures could not do it,
because a preset restores a whole saved slider set at once and the app stores it
host-side; **there is no preset concept on the wire at all.**

The three equaliser enables are always written together: `0x01` immediately before
the eight bands go out, `0x00` while another effect is configured with the
equaliser off. Three flags for a UI that splits the equaliser into bass, mid and
treble is a tidy fit and an untested one — nothing has been seen to write them
separately.

The vendor's UI offers **five** effects, not the four the datasheet lists: noise
gate, compressor, reverb, **echo**, and equaliser. Counting enables against
effects proved to be a dead end twice over — the datasheet undercounts, `0x9b` was
missing from our sequence, reverb and echo share a block, and the equaliser has
three enables to itself. Every mapping here came from a capture instead.

**The init sequence turns the user's reverb on, and this is no longer an
inference.** Payload 21 writes `0x85 = 00`, payload 23 writes `0x85 = 01`, and
payload 25 is byte-identical to the parameter block the vendor software sends when
reverb is switched on. Payload 30 is likewise byte-identical to the compressor's.

`0x94`'s body byte 14 **mirrors the reverb enable**. The three blocks in the init
sequence sit in this order: `0x85 = 00`, a block with byte 14 clear, `0x85 = 01`,
a block with byte 14 set, then a third with it still set and seven parameter bytes
changed. The correlation with the enable is exact.

It was read as a *mix* selector before the command was identified — two blocks
differing in exactly one byte, in a device with exactly two mixes, is a seductive
shape — and that reading was used as evidence the deck was in Single Mix. **That
argument is void.** The conclusion survives on other grounds: the init sequence
writes `0x21 = 0x80` and `0x22 = 0x12`, which is precisely what *disabling* Dual
Mix writes, and `0x15 = 0x00` for the creator mix.

`0x9f` may work the same way. Its two blocks differ in body byte 0 — `0x00` and
`0x80` — and only the `0x00` one matched the captured compressor. The noise gate
is the other dynamics processor and takes threshold, attack, hold and release, so
byte 0 selecting which processor is configured is the hypothesis worth testing
against a `gate-on` capture.

**Effect parameters do travel live, one byte per control.** Dragging the
compressor's threshold slider sends three `0x9f` blocks differing in **body byte 9
alone** -- `0xfb`, `0xfc`, `0xe6`, against `0xee` from an earlier capture. Read as
signed dB those are -5, -4, -26 and -18, which is exactly a compressor threshold's
range. (Signed byte or the low half of a Q8 value at bytes 8..9 cannot yet be told
apart: byte 8 has always been `0x00` and every value seen was a whole dB.)

That matters beyond the compressor: it rules out the idea that the vendor software
batches parameter changes behind some trigger we had not pulled. It does not. **So
the equaliser is genuinely the anomaly**, not an example of a general rule.

**`0xa3` carries the equaliser, gain included.** Dragging the 8 kHz treble slider
changes band 6 and only band 6, so the bands are the user's equaliser and their
index is the slider's: `0`..`7` for 50, 100, 250, 500, 1000, 4000, 8000 and
16000 Hz.

The structure confirms it. In every band ever seen, **`b1` is exactly equal to
`a1`** -- the signature of a canonical peaking biquad, where both terms are the
same `-2cos w`. And a band whose `b` equals its `a` throughout is unity, which is
a slider sitting at **0 dB**, not an inert section.

This was recorded here for several commits as "the bank is fixed", on the strength
of three captures where the 50 Hz slider moved and nothing changed. Two supporting
arguments were offered and both were wrong the same way: bands 2 and 4 being
pass-throughs was read as evidence of fixed sections when it means those sliders
are centred, and bands 5 and 6 carrying `enable = 0` was read as "nobody would
disable 4 kHz and 8 kHz" when band 6 demonstrably still responds to its slider.
**A negative result from one control is not a property of the protocol.**

The 50 Hz silence has since been explained, and by taking the negative result
seriously rather than generalising from it. Across every capture, **bands 0 and 7
have exactly one value each** while bands 1 to 6 take several, and the two fixed
ones have numerators `(1, -2, 1)*K` and `(1, 2, 1)*K` -- a high-pass and a
low-pass band-limiting the chain. So only six bands are adjustable, against eight
sliders in the vendor UI. Counting back from the one band we measured directly
(8 kHz moves band 6) puts bands 1..6 at 100, 250, 500, 1000, 4000 and 8000 Hz, and
leaves 50 Hz and 16 kHz with nothing to write. The silence was the protocol being
consistent, not the capture failing.

Both predictions were then tested and both held: dragging 16 kHz re-sends all
eight bands with **none** changed, and dragging 100 Hz moves **band 1 alone**. The
map is

| band | 1 | 2 | 3 | 4 | 5 | 6 |
|------|---|---|---|---|---|---|
| Hz   | 100 | 250 | 500 | 1000 | 4000 | 8000 |

with 50 Hz and 16 kHz driving nothing on the wire.

`0xa3` is decoded, not guessed. Its body is
`[band 0..7] [0x01] [enable] [b0][b1][b2][a1][a2]`, five little-endian signed
32-bit coefficients in **Q30** — `0x40000000` is 1.0. Three independent checks
agree: bands 2 and 4 have `b` exactly equal to `a`, which is algebraically unity
and so a flat band; band 7's numerator is `0.535, 1.070, 0.535`, the textbook
`1, 2, 1` lowpass shape; and for that band `Σb == Σa == 2.1410`, meaning DC gain
of exactly 1. A wrong word size or scale factor does not produce a unity-gain
filter by coincidence. Six of the eight bands are enabled and all of them sit
near flat, which is what a lightly-adjusted user EQ looks like.

The ordering — disable, write parameters, enable — is what makes the whole
sequence legible as *configuration* rather than initialisation, and is the
evidence behind deviation 1. `ax310_probe --effect` toggles a confirmed enable,
and `ax310_probe --dump` reads back every register the handshake overwrites; every
frame either sends is built from Protocol.hpp rather than hand-assembled.

The report is **decoded field by field from named offsets**, not overlaid with a
packed struct. `protocol::decodeReport()` returns a value type that aliases
nothing, so there is no lifetime to get wrong and no layout for a compiler to
disagree about — and, more usefully, the endianness has to be written down at
each field rather than being implied. That matters here because the report is
**mixed-endian**: touch coordinates little-endian, audio meters big-endian, in
the same 58 bytes. Fields whose meaning is unknown are simply absent from the
decoded type; the bytes are still on the wire, and naming them would only invite
code to trust a guess.

**The meters are one per track, each a stereo pair.** Six of them from report
offset `0x12`, four bytes apiece -- left then right, both 16-bit big-endian -- in
the deck's printed knob order.

Established with the vendor's software switched to its **per-track peak view**,
with a live microphone and music on System and nothing else connected. The Mic and
System pairs moved and the other four sat at zero, and the stereo layout shows in
the pairs themselves: the microphone is mono and its two sides agree to within ten
counts, while System carries stereo music and its two differ by about a thousand.

**The deck always sends all six.** Toggling the vendor's peak view sends the deck
nothing at all -- zero commands in a capture of that toggle -- so it only changes
what the application draws. Re-reading every capture with the right layout shows
the block was always complete: the microphone capture lights Mic, the two
music-on-a-different-track captures light Game and Chat respectively, and the
System ones light System. Each capture lights exactly the track that was in use,
which is five independent confirmations of the mapping.

**This file was wrong about the meters three times before that**, and the reason
is worth keeping. Every earlier reading came from decoding only the first pair,
which is the microphone -- and a live microphone hears whatever is played into the
room. So the microphone's meter appeared to answer every track in turn, and looked
first like one meter per playback channel, then like a single pre-fader stereo
mix, then like one meter per mix. The last of those survived until a capture with
the System track at 100% in one mix and 0% in the other left both values still
tracking each other.

**The deck interleaves an all-zero report between real ones.** It carries no
event and must be dropped before decoding — read literally it releases every held
button and lets go of every touched knob, then takes them all back on the next
report.

Input reports are 58 bytes (65 with the report-id byte when a transport pads to
the full report length). Anything in the report we cannot yet explain stays named
`reserved` in the decoder rather than being silently ignored.

## Layout

```
src/ax310/          The driver library, with its own CMakeLists.txt.
    Types.hpp         Vocabulary: Button, KnobId, Touch, TouchPhase,
                      ConnectionState, DeviceMode, DeviceError.
    Protocol.hpp      Wire layout: report structs, bit tables, chunk geometry.
    Commands.hpp      The captured vendor command sequences, as data.
    Event.hpp         DeviceEvent variant and IDeviceListener.
    IHidTransport.hpp The USB seam. HidApiTransport.{hpp,cpp} implements it and
                      is the only file that includes <hidapi.h>.
    IClock.hpp        The time seam, plus SystemClock.
    ILogger.hpp       The logging seam, plus NullLogger and StderrLogger.
    Device.{hpp,cpp}  Connect, decode, chunk. Owns no thread.
src/app/            The Qt layer. DeviceBridge.{hpp,cpp} adapts the driver to
                    Qt's object model; main.cpp wires everything together.
src/gui/            QML — Main.qml (desktop control panel), ScreenUI.qml (the
                    800x480 surface grabbed, JPEG-encoded and pushed to the deck).
scripts/            Reverse-engineering tooling: USB capture, log analysis, JPEG
                    extraction, checksum search, protocol probes. Python 3, ad-hoc.
cmake/              Shared build modules (see "Building").
```

**`src/ax310` is Qt-free, and the build is what enforces that.** The library
target links hidapi and nothing else, and carries no AUTOMOC, so a Qt include
added to any file under `src/ax310` fails to compile rather than being caught in
review. Qt lives in `src/app` and `src/gui`; `DeviceBridge` is the one class that
sees both worlds.

## Load-bearing design principles

The first four are the project's reason for existing in this form — the driver
is being written this way on purpose, not incidentally. The rest come from the
shared Contour C++ guidelines (`/contour-workflows:cpp-guidelines`), which apply
here in full. Deviate only with a strong, explicitly written justification,
recorded at the declaration.

### Dependency injection

Anything touching I/O, time, randomness, the filesystem, the USB bus, or any
other ambient resource is reached through an interface — never through a concrete
type, a singleton, or a free function with hidden state. `hid_open`, `hid_write`,
`hid_read_timeout`, `std::this_thread::sleep_for` and `std::chrono::steady_clock`
are all ambient resources.

Define the seam first, then inject it (by reference or `unique_ptr` at
construction). The seams:

- `IHidTransport` — enumerate / open / write / read / feature-report. The one
  place hidapi is named, implemented by `HidApiTransport`.
- `IClock` — `now()` and `sleepFor()`. The initialisation handshake's delays and
  the reconnect wait are behaviour worth testing without spending real seconds.
- `ILogger` — the driver never writes to stderr itself. `NullLogger` and
  `StderrLogger` ship with it.
- `IDeviceListener` — where decoded events go, so the driver does not know Qt
  exists.

`Device` takes all four by reference at construction and holds nothing else.
Substituting a `FakeHidTransport` that replays one of this tree's captures, a
`ManualClock`, a `NullLogger` and a recording listener exercises enumeration
order, the handshake, report decoding and frame chunking with no device attached.

If you find yourself wanting a global, a mutable `static`, or a direct
`hid_*()`/`::sleep()` call inside driver logic, that is the signal to introduce
or reuse a seam.

### Data-driven design

Behaviour is described by data; code interprets that data. Adding a command, a
button, a knob, or an error code should be *adding a row to a table*, not editing
logic in several places.

- **One source of truth per concept.** The init sequence, the shutdown sequence,
  the command opcodes, the button bit → identity mapping: each exists exactly
  once, as data.
- **No hand-rolled repetition.** Branches that differ only by a constant are a
  table in disguise. The 4-way `if (buttonId == 8) … else if (buttonId == 4) …`
  ladder currently in `Main.qml` is the canonical example of what not to write.
- **Built for extension.** The next command is a new descriptor, not a new `if`
  arm threaded through existing functions.
- **A table indexed by an enumerator carries a `static_assert`** that its extent
  matches the enum's count and that every row sits at its own enumerator's index.
  Never anchor the length on an enumerator by name; that guard only fires when
  nothing is wrong.

The captured payload blobs are the hard case here. Raw 64-byte arrays copied out
of a USB capture are acceptable *only* while their meaning is unknown, and each
must carry a comment saying what was observed. As soon as a field's meaning is
understood, it becomes a named constant or a builder function, and the blob goes.

### Coroutines

The device is an asynchronous, long-lived, reconnecting resource. Its I/O belongs
in coroutines, not in a hand-rolled thread with an `atomic<bool>` stop flag: a
read loop as a coroutine, `co_await`-able reads with a timeout, and a
cancellation path that is structured rather than polled.

`Device` deliberately owns no thread — `poll()` performs exactly one read cycle
and returns, which leaves the threading policy with the host and makes the driver
drivable from a test loop. Today `DeviceBridge` supplies that loop as a
`std::thread`; replacing it with a coroutine changes only the app layer.

Never let a coroutine outlive what it borrows, and never let a handle be owned by
two objects. A `Task<T>` takes ownership of its coroutine handle at the point it
is awaited, so a temporary cannot tear the coroutine down across a suspend point.

### RAII for every resource

`hid_device*`, the hidapi library init itself, threads, coroutine handles: each is
owned by a wrapper whose destructor releases it. No `hid_close` on a path that an
early return or a thrown exception can skip. No raw owning pointers, ever —
`std::unique_ptr` with a custom deleter for C handles.

### Configuration at construction time

**A constructed object is a usable object.** Collaborators, policy, limits and
tuning knobs are supplied to the constructor and fixed thereafter. No `init()`,
no default construction followed by a run of setters, no static knob poked at
startup. Configuration members are private and have no setter; prefer that
encapsulated immutability to `const` members, which delete assignment.

Configuration is not state: a setter that mutates what the object exists to
manage (the current LED level, the last decoded knob position) is fine; a setter
that installs a policy read once at startup is not. Ask whether two
differently-configured instances would be two objects or one object in two
states — two objects means constructor.

Setup that can fail belongs in a static factory returning
`std::expected<Device, DeviceError>`, not in a constructor that leaves a
half-built object behind. `DeviceImpl` today is the anti-pattern in full:
construct, then `connectDevice()`, then hope — with every method having to
tolerate a not-yet-connected state. The exceptions that apply here are Qt's
`Q_PROPERTY`/QML types in `src/app`, which the framework default-constructs and
then assigns to, and externally-driven geometry. Both are documented at the
declaration; neither reaches into `src/ax310`.

### Layering

`src/gui` → `src/app` → `src/ax310`, and never the other way. The driver library
does not know the application exists, the application does not reach around it
into hidapi, and neither depends on QML. The Qt-free rule above is the load-
bearing half of this; the direction of the arrows is the other half.

## C++ coding guidelines

The standard is **C++23**, `CMAKE_CXX_EXTENSIONS OFF`.

**Precedence when these disagree:** `.clang-tidy` and `.clang-format` first (they
are machine-enforced), then this file, then the shared Contour C++ guidelines,
then surrounding code. Where a guideline and neighbouring code disagree, follow
the neighbours and say so — do not reformat unrelated lines.

### Zero-warning policy

Warnings are errors in every preset (`PEDANTIC_COMPILER_WERROR=ON`) and every
enabled clang-tidy check is an error (`WarningsAsErrors: '*'`). A warning is a
build break, not a note. Fix the cause: no `NOLINT`, no `#pragma` mutes, and no
widening of `-Wno-error=…` without an explicitly justified reason written at the
site.

### Non-negotiable

- **Data-driven design** and **dependency injection**, as above.
- **No raw owning pointers.** `std::unique_ptr`/`std::shared_ptr` for ownership,
  RAII for resources. `new` in application code is a defect (Qt's
  parent-owned-`QObject` idiom in `src/app` is the one exception, and it is
  spelled with a parent argument, never a bare `new` whose result is posted
  somewhere and forgotten).
- **No C-style loops.** Range-based `for`, `std::views::iota`, `std::ranges`
  algorithms. An index arithmetic loop over a buffer is a `std::span` and a view.
- **`std::span` / `std::string_view`** at every boundary that takes a contiguous
  sequence. Never a pointer+length pair, never a raw array parameter.
- **`const` correctness** throughout — references, pointers, member functions,
  and locals (`misc-const-correctness` is on).
- **`[[nodiscard]]`** on every function whose return value carries the outcome —
  which is every `std::expected`-returning function.
- **`explicit`** on every single-argument constructor and conversion operator.
- **Fixed-width types at the wire boundary** (`std::uint8_t`, `std::uint16_t`),
  `std::size_t` for sizes. `-Wconversion` is on: every narrowing is an explicit,
  deliberate `static_cast`, and if you cannot say why it is safe, it is not.
- **Doxygen on every new public function, class, struct, and member:**
  ```cpp
  /// Short description.
  /// @param name Description.
  /// @return Description.
  ```
- **All changes covered by unit tests.** Aim to increase coverage with every
  change; see "Testing".
- **No new third-party dependencies** without strong justification, and every
  one is **pinned to a release tag**, never to a moving branch. Today: hidapi
  0.15.0 (via CPM) for the library, Qt6 (from the system) for the app, and
  nothing else. `fmt` was dropped once `std::format`/`std::print` covered it.

### Error handling

Fallible operations on the public API surface return
`std::expected<T, DeviceError>`; they do not return `bool`, do not return `void`
after printing to stderr, and do not throw. Chain with `and_then`, `or_else`,
`transform`, `transform_error` rather than nested `if`s. Reserve exceptions for
programmer errors — precondition violations and contract misuse.

`DeviceError` is an `enum class` with a message table, not a string. A caller
must be able to *branch* on "device not found" versus "write failed" without
parsing text.

### Type safety over primitives

A parameter typed `int` accepts every wrong value in the language. The API surface
uses strong types:

- `enum class` for anything with a fixed set of values — buttons, knob identity,
  event kind, error kind. Never a bare `int` id.
- A named type for a bounded quantity — brightness, LED level, a percentage —
  that validates on construction. `setScreenBrightness(int)` taking 3000 is a bug
  the type system should have caught.
- Bitmask enums are `enum class` with explicit operators, not `uint8_t` passed
  around raw. This is the one place a bit-position enum stays unscoped-in-spirit:
  the values are protocol-defined, so they are combined, not switched over.

**`enum class` over `bool`.** A `bool` in an API is an anonymous enum whose two
values are named after their representation instead of their meaning. It is a
finding as a parameter, as a member, and as a success/failure return.

- **Parameters.** `knobTouched(knobId, true)` says nothing at the call site, and
  `bool` converts from pointers and integers, so an argument meant for another
  overload is swallowed without a diagnostic. Write
  `enum class Touch : std::uint8_t { Released, Touched }` — off/absent/default at
  zero, so a zero-initialized value still means what `false` meant. A defaulted
  `bool` parameter is the worst form; prefer two named functions.
- **Returns.** `bool` is right when the function name asks the question —
  `isConnected()`, `contains()`, `empty()`, `explicit operator bool()`. It is a
  finding when it reports success or failure: `connectDevice()` returning `bool`
  throws away *why* it failed, and that is `std::expected<void, DeviceError>`.
- **Members.** Two or more `bool` members in a type are usually a state machine
  hiding in flags — `m_connected` plus `m_stopThread` is exactly that, and the
  states are one `enum class`, not a pair of independent switches. A `bool` that
  survives reads as a predicate: `_isConnected`, not `_connected`.
- **When you cannot**, say why at the declaration: a Qt signal/slot or virtual
  you do not own, a standard concept, or the wire boundary itself — the protocol
  has boolean bits, and they are converted to an `enum class` at the decoder and
  never carried inward as `bool`.

### Wire decoding (project-specific, and strict)

The protocol is bytes off a USB bus. Decoding them wrong is silent, and the
device is the only thing that can tell you.

- **Never `reinterpret_cast` a byte buffer to a struct.** It is undefined
  behaviour, it depends on padding and alignment we do not control, and UBSan
  will eventually say so. Decode field by field from a `std::span<std::byte const>`,
  or `std::memcpy`/`std::bit_cast` into a trivially-copyable type of exactly the
  right size.
- **Never type-pun through a `union`.** Reading a member that was not the one
  written is UB in C++ (it is legal in C, which is why protocol headers written
  in C look like this). Model an alternative-carrying report as a
  `std::variant`, or decode explicitly on the event-type byte.
- **Decode multi-byte fields explicitly.** The wire is little-endian; the host may
  not be. Read `uint16_t` through a two-byte helper, never by overlaying a struct.
- **No `#pragma pack` overlays as a parsing strategy.** Packed structs are
  acceptable as *documentation* of a layout, with a `static_assert` on
  `sizeof`, but the decoder does not cast onto them.
- **Validate before you trust.** Length, event type, and report size are checked
  against the buffer actually received; an unexpected size is an error value, not
  a `continue` with a printf.
- **`std::array`, not C arrays**, in protocol structs and everywhere else.
  (`cppcoreguidelines-avoid-c-arrays` is disabled in the inherited `.clang-tidy`;
  the rule still stands here.)

### Naming

`.clang-tidy` enforces this and it is a build error, not a review comment:

- Types, enums, enumerators, constants and `constexpr` variables: `CamelCase`.
- Functions, methods, parameters, locals, members: `camelBack`.
- Non-public data members: a **leading underscore** — `_handle`, `_connected`.
  Not `m_`. The existing `m_`-prefixed members are pre-existing and must be
  renamed when their file is touched.
- **No Hungarian notation and no prefixes** — no `g_` on globals, no `s_` on
  statics, no `k` on constants. A file-scope name is spelled like any other name
  of its kind. If a bare name reads wrong at the call site, that is the "inject
  it" rule telling you the state should not be ambient in the first place.
- The names the standard library binds to by spelling (`value_type`, `begin`,
  `push_back`, `std::formatter` specializations) cannot follow this and carry a
  `NOLINT(readability-identifier-naming)` with the reason at the declaration.

### Headers

- Headers are `.hpp`, sources `.cpp`. There are no `.h` files in `src/`.
- `#pragma once` is the house include guard.
- **Public headers are self-contained**: each compiles standalone, includes what
  it uses, and depends on no precompiled header and no particular include order.
- No `using namespace` at file scope in a header. Ever.
- Public symbols live in namespace `ax310` (protocol details in
  `ax310::protocol`).

### Tooling

- **`clang-format` and `clang-tidy` after every change.** Successive LLVM
  releases disagree with each other, so name the version explicitly rather than
  taking whatever `PATH` resolves to. The toolchain this tree is developed
  against is **LLVM 22**:
  ```sh
  clang-format-22 -i $(git ls-files '*.cpp' '*.h')
  cmake --preset clang-debug -DCLANG_TIDY_EXE=$(command -v clang-tidy-22 || command -v clang-tidy)
  cmake --build --preset clang-debug   # clang-tidy runs as part of the build
  ```
- **clang-tidy reports are fixed at the source. Never silence one with
  `NOLINT`** — the two exceptions are the standard-library-spelling names above,
  and each must carry its reason inline.
- `.clang-tidy`'s check list was inherited from a sibling project and its
  rationale comments still describe that codebase. The list itself is a sound,
  proven baseline; when a disabled check turns out to be one this project *wants*,
  re-enable it and rewrite the note. `HeaderFilterRegex` is set to
  `src/(ax310|app)/` — if you add a top-level source directory, add it there too
  or its headers go unlinted.
- Before calling a change done, build it with **both** `clang-debug` and
  `gcc-release`. One compiler at `-O0` hides a whole class of defect that a
  second standard library and an optimizer surface immediately.

## Building

Requires Qt6 (Core, Gui, Qml, Quick, Concurrent) from the system, plus hidapi and
fmt, which CPM fetches at configure time.

`src/ax310/CMakeLists.txt` describes the library and `src/CMakeLists.txt` the
application; the Qt-free rule lives in the first of those as a fact about what
the target links, not as a comment.

Presets live in `CMakePresets.json` and build into `out/build/<preset>/`.

```sh
# Clang Debug with PEDANTIC + ASan + UBSan + clang-tidy — the default preset
cmake --preset clang-debug
cmake --build --preset clang-debug
ctest --preset clang-debug

# GCC, for the second compiler and second standard library
cmake --preset gcc-debug   && cmake --build --preset gcc-debug
cmake --preset gcc-release && cmake --build --preset gcc-release

# Sanitizers without the clang-tidy cost
cmake --preset clang-asan-ubsan && cmake --build --preset clang-asan-ubsan

# ThreadSanitizer — the read loop and the screen writer share a device handle,
# so run this whenever that code changes
cmake --preset clang-tsan && cmake --build --preset clang-tsan

# Coverage (needs llvm-profdata/llvm-cov at the SAME major version as clang).
# Writes out/build/clang-coverage/coverage/{html,coverage.lcov,percent.txt}
cmake --preset clang-coverage
cmake --build --preset clang-coverage
cmake --build --preset clang-coverage --target coverage

# Windows presets exist (cl-debug, cl-release, clangcl-debug, clangcl-release)
# and are host-conditioned. They have never been exercised.
```

**This project's own headers come first in an include block**, then third-party,
then Qt, then the standard library. `.clang-format`'s `IncludeCategories` is what
decides it and `scripts/check-include-order.py` is what enforces it, as a test.
Reorder with `scripts/fix-include-order.py`, which formats the include region and
nothing else -- a whole-file `clang-format` would drag in 178 lines of unrelated
wrapping, because `ColumnLimit` was inherited along with the rest of that config
and disagrees with how this code is written.

`llvm-include-order` is enabled but does not enforce this: it only checks that a
block is internally sorted, and cannot see a block in the wrong place.

**Nothing writes to stdout or stderr directly.** `IConsole` is the fourth
injected seam, beside `IHidTransport`, `IClock` and `ILogger`, and
`IConsole.cpp` is the only file in the tree that names either stream. A tool
takes one; `main()` gives it a `SystemConsole`; a test gives it a
`CapturingConsole` and asserts on what came out, including which of the two
streams it went to. `ConsoleLogger` writes through it too, which is what made
the log line itself testable.

That is a portability boundary as much as a testing one. The ostream overloads of
`std::print` are C++23's P2539 -- libstdc++ has them, MSVC's `<print>` does not --
and finding that out cost a build. One file to keep portable is better than
seven.

**`docs/` is a published website.** GitHub Pages serves it from `master` at
<https://christianparpart.github.io/ax310/>, with Jekyll. So `docs/_config.yml`,
`docs/_layouts/` and `docs/assets/` are load-bearing, a new page needs YAML front
matter to get the layout, and anything added under `docs/` is public the moment
it is pushed. `todo.md` is excluded in `_config.yml`, because working notes read
as promises once they are on a website.

**The wire specification is generated, not written.** `docs/wire-protocol.md` is
projected out of the tables in `Protocol.hpp` and `Types.hpp` by `ax310_spec`, and
`ctest` fails when the committed copy has parted from them. Do not edit it; change
the header and run `ax310_spec --write docs/wire-protocol.md`. Claims it makes
that no table can hold — endianness, contiguity, whether a parameter fits its
body — are `[spec]`-tagged tests in `Protocol_test.cpp` rather than sentences
anybody has to remember to check.

This file keeps what that document cannot: the evidence, the readings that were
wrong first, and what is still unknown.

`cmake/` modules and what is wired:

| Module | Wired | Purpose |
| --- | --- | --- |
| `portable/PedanticCompiler.cmake` | yes | `PEDANTIC_COMPILER`, `PEDANTIC_COMPILER_WERROR` |
| `portable/Sanitizers.cmake` | yes | `ENABLE_SANITIZER_{ADDRESS,UNDEFINED,THREAD}` |
| `portable/ClangTidy.cmake` | yes | `ENABLE_TIDY`, `CLANG_TIDY_EXE` |
| `Coverage.cmake` + `ProjectTargets.cmake` | yes | `ENABLE_COVERAGE`, the `coverage` target |
| `CPM.cmake` + `FetchTransferBound.cmake` | yes | dependency fetch, with a stall bound |
| `Packaging.cmake` | yes | install rules and CPack; `cpack -G RPM` |

Every module in `cmake/` is now reached from the build. The directory arrived
from another project and carried five that were not — `CompileCache.cmake`,
`Version.cmake`, `Utf8CodePage.cmake` and the two `MacOS*.cmake` — which have
been removed, along with the sixth, `Packaging.cmake`, which was replaced rather
than deleted because this project needed one. Their cross-references had rotted
into the modules that stayed, and one of them was load-bearing: `CPM.cmake` read
`FASTCACHED_FETCH_SILENCE_SECONDS` while `FetchTransferBound.cmake` had been
renamed to define `AX310_FETCH_SILENCE_SECONDS`, so the CPM bootstrap's
`INACTIVITY_TIMEOUT` was the empty string. CMake accepts that in silence, and an
empty bound is no bound.

Dependencies are added with `SYSTEM YES`, so their headers sit behind
`-isystem`: without it a dependency's own header is compiled under our warning
flags, and fmt's format-presentation enum alone trips `-Wduplicate-enum` into a
`-Werror` failure in code we do not maintain. The one Qt-generated file that
GCC's `-Wnull-dereference` false-positives on at `-O3` is exempted by name in
`src/CMakeLists.txt`; nothing first-party is.

Dependencies are fetched *before* the pedantic/sanitizer/tidy modules are
included, deliberately: `add_compile_options()` and `CMAKE_CXX_CLANG_TIDY` are
captured when a target is created, so third-party targets stay out of `-Werror`
and out of the linter. Keep that ordering when editing the top-level
`CMakeLists.txt`.

## Testing

Catch2 (pinned at v3.16.0 via CPM), one binary — `ax310_test` — declared in
`src/ax310/CMakeLists.txt` next to the library it proves, registered with
`catch_discover_tests`. `AX310_BUILD_TESTS` (default ON) gates both the Catch2
fetch and the target.

```sh
ctest --preset clang-debug          # 73 cases; also gcc-release, clang-asan-ubsan
ctest --preset clang-debug -R knob  # one group
```

`Foo.cpp` has `Foo_test.cpp` beside it. That naming is not cosmetic: the
`ignore_regex` in `scripts/coverage.sh` excludes `_test.cpp`, `test_main.cpp` and
`src/tests/` from the report, so a file named any other way silently counts
itself as covered production code.

**Test doubles live beside the interface they implement**, the way `NullLogger`
and `NullDeviceListener` already did: `ManualClock` in `IClock.hpp`,
`CapturingLogger` in `ILogger.hpp`, `RecordingListener` in `Event.hpp`,
`FakeHidTransport` in `FakeHidTransport.hpp`. They are library code and are
tested themselves (`Fakes_test.cpp`) — a fake nobody exercises does not report
its own bugs, it reports the subject's, wrongly. `src/tests/` holds only what is
shared and test-only: the `test_main.cpp` entry point, `ReportBuilder.hpp`, and
`Printers.hpp`.

`ReportBuilder` assembles an input report **by byte offset**, never by filling in
a `protocol::InputReport` and copying it. Building fixtures out of the struct
would assert that the struct agrees with itself, and would keep agreeing after a
field moved; the offsets are what pin the layout the captures actually show.

`Printers.hpp` gives Catch2 a `StringMaker` for each of the project's enums,
reusing `describe()` and `nameOf()` rather than repeating them. Without it a
failed comparison prints `{?} == {?}`.

**Every code area must be testable, and new code lands with tests.** If
something is hard to test, that is a design smell: inject the dependency and
extract the decision rather than skipping the test. Pure decisions — checksum,
chunking, report decoding, knob-delta arithmetic — belong in dependency-free
headers that need no device and no Qt. GUI-layer code that must be exercised
uses `Qt6::Test`, offscreen.

Tests must not sleep on wall-clock time; waits are bounded and driven by the
injected clock.

## Reporting a change

A change summary states its **performance impact** (if any), a **risk
assessment**, and **coverage** — what the new tests cover and which numbers
moved. Before calling a change done: run `clang-format`, build clean under
`clang-debug` *and* `gcc-release`, run the tests, and look for duplication the
change introduced (`/simplify`).

## Reverse engineering

`scripts/` is the lab notebook, not production code — Python 3, no dependencies
beyond the standard library and `lsusb`/`usbmon`.

- `capture_usb.py` / `capture_leds.sh` / `capture_pcap.sh` / `capture_shutdown.sh`
  record traffic (the device is watched for both `07ca:0310` and `07ca:1310`).
- `analyze_log.py` reduces a capture to unique input reports by frequency.
- `extract_jpegs*.py`, `check_jpeg_header.py`, `check_transfer.py` reconstruct
  screen frames from the outbound stream.
- `find_crc*.py` search for the chunk checksum algorithm.
- `gen_init.py` / `gen_shutdown.py` emit the payload tables in `Device.cpp`.
- `install_vm.sh` + `autounattend.xml` stand up a Windows VM to capture the
  vendor software's traffic.

Captures (`ax310_capture.log`, `ax310_summary.txt`, `lsusb_full.txt`) are
gitignored — they are large and machine-specific. **When a capture establishes a
protocol fact, write the fact down in this file's "The hardware" section.** A
200 MB log nobody can reproduce is not documentation.

## Known deviations

**The tree builds clean under its own gate.** `clang-debug` (PEDANTIC +
`-Werror` + ASan + UBSan + clang-tidy with every enabled check an error),
`gcc-release`, `clang-asan-ubsan` and `clang-tsan` all pass with zero
diagnostics and zero race reports. Keep it that way: a warning here is a build
break, not a note.

The tree violates the rules above in these specific ways. Each is debt with a
known fix, not a precedent to copy:

1. **Connecting restores the property registers, but not the DSP.** The captured
   sequence is somebody's saved configuration rather than an initialisation: it
   sets every knob level to 50% and configures a whole DSP chain. Both halves were
   felt during a live voice call — the microphone level moved for the far end, and
   the deck's room reverb switched on.

   `connect()` now reads all thirteen property registers the sequence overwrites
   (`protocol::PreservedAddresses`) before the handshake and writes them back
   after. Verified on the hardware: with the deck's knob levels at `0x14` and
   `0x1e` at `0x05`, a full connect leaves both exactly where they were, where the
   sequence writes `0x0a` and `0x0d` to them.

   **The DSP half is not fixed.** The `0xfe` family has no read-back we know of,
   so the equaliser and whichever enable is the reverb are still imposed on
   whatever deck this attaches to. Nor are the `0xc0`/`0xe0` record writes
   restored — their length field is a record size rather than a byte count, so
   reading one back is a different shape of operation. Attaching is much less
   destructive than it was and is still not free.

   The real fix is to know which payloads wake the hardware and which merely
   restore a stranger's taste, and that needs the vendor software captured while
   it performs each action. See `docs/capture-session.md`.

2. **The read loop is a `std::thread` in `DeviceBridge`**, not a coroutine. The
   driver itself is clean — it owns no thread — so this is one class to change.
3. **Numeric quantities are still bare `int`s.** `setScreenBrightness(int)` takes
   3000 without complaint, and `volume` is a percentage only by convention. The
   enums are done; the bounded quantities are not.
4. **Eighty-eight opaque payload blobs** in `Commands.hpp` — 74 init and 14
   shutdown 64-byte reports replayed from a capture, plus the
   `0x01 0x10 0x27 0x07` LED command prefix, none of them understood.
5. **`setScreenBrightness` is still a stub.** The register that dims the panel
   has not been found; `0x1e` turned out to be the knob rings. Bytes 0x2A-0x37 of
   the input report, the touch flags byte, the `0xc0`/`0xe0` record writes, the
   mix/colour register, and which of the two mixes is the audience one are the
   other open questions.
6. **The touch flags byte is unexplained, and four readings of it are dead.** It
   is fixed per contact and only ever counts up: every contact starts at `0x00`,
   and in roughly 1400 transitions none went down or returned to `0x00`. `0x00`
   is the not-moving state. It is *not* a counter (a counter cycles), not an
   accumulator of distance or time (one contact flipped at 28 px / 471 ms and
   another at 17 px / 47 ms), not a magnitude (the slow gesture reached the
   higher value), not a gesture class (five flicks and five press-drags produced
   no `0x14`), and not finger count (the panel tracks one finger). The driver
   ignores the byte and takes contact from the report type, which is what the
   hardware actually signals.
7. **QML cannot name the driver's enums.** They are declared in Qt-free headers,
   so moc never sees the enumerators and `Q_ENUM_NS` is unavailable;
   `registerDeviceMetaTypes()` makes them marshal, but the QML handlers still
   compare numbers. The fix is `Q_ENUM` mirrors on `DeviceBridge`, converted at
   that boundary.
8. **CI runs all four presets** on every push to master, rendering tests
   included -- see `.github/workflows/ci.yml`. What it does not yet cover is the
   coverage preset, and nothing exercises the hardware paths, which is deviation
   9. The former text of this entry follows, for the parts still true: the suite,
   the sanitizer
   presets and the coverage target are all in place and scripted; what is
   missing is a workflow that runs them on push.
9. **The hardware paths are unreachable from a test, by construction.**
    `HidApiTransport` and `StderrLogger` sit at 0% because they are the far side
    of the seams — the code whose whole job is to touch the USB stack and the
    terminal. That is the design working, but it does mean those two files are
    only ever exercised by running the app against a real deck.
