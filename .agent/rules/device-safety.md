# Not breaking the deck

**Never write an address the captured sequences do not write.** Writing `0x01` to
address `0x16` wedged the device: every subsequent read failed and it took a power
cycle to recover. Blind sweeps across the register space are retired as a
technique.

**Reads are safe.** All 256 addresses answer a read and none of them has caused
harm. Reading is the first move for any question that a read can answer.

**Writing a value the vendor already writes is safe.** Every frame in
`src/ax310/Commands.hpp` was observed on the wire, so replaying one — or changing
only its value byte where that byte is already known to take both values — stays
inside what the hardware expects. `ax310_probe --effect` is built on exactly this
and refuses anything outside its allow-list, which holds only enables the hardware
has confirmed.

**Connecting is safer than it was, and still not free.** `Device::connect()`
replays the captured sequence, which is somebody's saved configuration rather than
an initialisation. It now snapshots the thirteen property registers that sequence
overwrites and writes them back afterwards — verified on hardware, knob levels and
ring brightness both survive a connect.

**But the DSP chain is still imposed.** The `0xfe` family has no read-back we know
of, so the equaliser and whichever enable is the reverb are applied to any deck
this attaches to. It has already been heard on a live call. Check before attaching
to a deck someone is using.

**Check whether the user is using the deck before writing to it.** Audio hardware
is not a private test fixture; it may be carrying a conversation right now.
