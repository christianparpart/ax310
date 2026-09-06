# Splitting the deck into per-track devices

`scripts/setup-audio.sh` does this; the notes here are the things that were not
obvious and cost time.

## The channels are real, the split is software

The USB descriptors declare **one** six-channel playback interface and **one**
eight-channel capture interface. No operating system gets three playback devices
from this hardware -- AVerMedia's Windows driver splits them in software too, and
looks native only because the vendor ships it. So a software split here is not a
workaround, it is the same thing Windows does.

## The profile is the whole problem

PipeWire's default maps a 3-channel surround layout onto the six channels, which
hides the tracks *and* wastes half of them. The `pro-audio` card profile exposes
`aux0..aux5` and `aux0..aux7` raw. Everything else follows from that.

## Confirmed channel map

| Playback | | Capture | |
| --- | --- | --- | --- |
| aux0/1 | System | aux0/1 | creator mix (headphones) |
| aux2/3 | Game | aux2/3 | audience mix (the stream) |
| aux4/5 | Chat | aux4/5 | microphone |
| | | aux6/7 | host playback, pre-fader |

All verified on the hardware, none inferred. Playback by tone-and-knob; capture by
muting a track in one mix block and watching which pair went quiet.

## Traps

- **Never test channel routing by playing a multichannel file at the raw node.**
  A six-channel file declares `FL FR FC LFE RL RR`, the raw ports are `aux0..aux5`,
  and PipeWire resolves the mismatch by remixing -- collapsing every pair onto the
  same channels. It looks exactly like a hardware finding. Test through the split
  sinks, which carry `stream.dont-remix`.
- **The deck's meters answer one of these questions and not the other.** There is
  one per track and it lights only when that track has signal, so they *can*
  identify a channel: play into a sink and see which meter moves. They are
  **pre-fader**, so they cannot tell you whether a level register works -- the
  meter reads the same at 0% as at 100%. That still needs an ear or a
  capture-side measurement.

  This entry used to say the meters read one stereo mix and could not help at
  all. That was wrong, and the routing here was identified by ear because of it.
  `setup-audio.sh --identify` now measures it instead: a tone in each sink peaks
  its own track at 48% and every other track at 0%, which reproduces the ear's
  answer exactly. `ax310_probe --meters` is the same reading, by hand.
- **Loopback nodes default to `node.virtual = true`**, and desktops hide those
  behind a "show virtual devices" toggle. Set it false: these are the only route to
  real hardware tracks.
- **`wpctl status` will not list the split sinks** because they carry a
  `node.link-group` and WirePlumber classifies them as filter chains. That is
  cosmetic -- `pipewire-pulse` exposes them fine, which is what desktop applets
  actually read. Check with `pactl`, not `wpctl`.
