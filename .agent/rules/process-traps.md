# Traps already fallen into

**A batched edit script that asserts at the end discards every edit.** The pattern

```python
s = s.replace(a, b); s = s.replace(c, d)
assert something
p.write_text(s)          # never runs when the assert fires
```

has silently thrown away all pending edits **five times** in this project. Apply
edits one at a time, write after each, and verify against the file.

**Verify a hypothesis on the hardware before naming anything after it.** `0x1e`
was the screen-brightness candidate on strong capture evidence — the only property
both sequences write, to different values. It dims the knob rings. Right register,
wrong subsystem, and the name would have misled every reader afterwards.

**Check the reply is fresh, not stale.** The deck leaves the remainder of its
previous reply in the buffer, so bytes past the answer's stated length are
garbage from the last read. Interleave a read of `0x0f` (answers zeroes) to make
staleness obvious.

**A verification that passes in the wrong conditions proves nothing.** The frame
size fix was verified at device-pixel-ratio 1; the user's display runs at 1.25 and
was never going to honour it. Reproduce the user's conditions or say the check was
partial.

**A listening test measures the whole chain, not the deck.** There is a second
audio interface upstream of the AX310 here, with its own compressor. So a
dynamics artefact heard in the headphones may belong to that box, and an AX310
setting that seems to do nothing may be masked by it. Prefer a capture: it shows
what the host actually sent and no downstream gear can colour it.

Listening is still sound when the AX310's own control is the only thing moved --
an AX310 equaliser slider changing what you hear proves that equaliser is live,
whatever else is in the chain.

**Ask what the hardware is doing before theorising.** Both the meter decode and
the touch-flags decode survived a plausible reading of the captures and died the
moment somebody played a tone or held a finger still. Captures under-determine
meaning; the device settles it.

**A green test suite says nothing about what Qt printed.** QML defects — a
binding loop, an assignment Qt cannot make, a property that is not there — are
reported to the message handler and then survived: the binding stays dead, the
scene still renders, every rendering test still passes. The ring gauges shipped
sized by evaluation order this way, and the application printed the same warning
once per gauge per repaint while CI stayed green. `GuiRender_test.cpp` now
installs a `qInstallMessageHandler` and fails on anything above debug level, so
the test suite hears what the terminal was already saying.

**The renderer the tests use is not the renderer the deck's frames use.** The
suite pins Qt's software scene graph so it needs no display and no GPU; the deck's
frames are grabbed from a window the compositor never maps, which goes through the
RHI and builds a throwaway one per grab. `Shape.CurveRenderer` is shader-based and
holds GPU resources across frames, so it drew the first frame and nothing after
it -- the panel went out with its numerals, legends and tiles intact and every
ring arc gone. Under QPainter both renderers draw the arcs identically, down to
the byte: regenerating the screenshots after the fix produced no diff at all.
`Shape.GeometryRenderer` triangulates on the CPU and survives every grab. There is
now a native-backend CTest entry that fails on this, and it is the only test in
the suite that can.

**`pkill -f` matches the shell that runs it.** The pattern is tested against every
process's full command line, and the command line of the shell executing `pkill -f
"http.server 8787"` contains that string — so the shell kills itself, the rest of
the compound command never runs, and the failure looks like the *last* thing in
the line went wrong rather than the first. It has happened three times in this
project, most recently taking a heredoc with it. Kill by PID instead:

```sh
pid=$(ss -lptn 'sport = :8787' | grep -oP 'pid=\K[0-9]+' | head -1)
[ -n "$pid" ] && kill "$pid"
```

`pkill -x name` is safe where the process has a distinct executable name; `-f` is
the dangerous one.

**A hedged observation is not a finding, and the hedge is the load-bearing part.**
The deck's owner said the screen "seems to" turn off, "at least when we don't send
some JPEG frames" — and within minutes that was written into hardware-facts.md as
"the screen turns itself off after a short time when no frames are sent", with a
paragraph of consequences built on top. It was wrong: the last frame stays on the
panel indefinitely. The hedge carried the actual information — that somebody was
guessing at a cause for something they had noticed — and dropping it turned their
guess into the project's fact. Quote the observation, or ask what settles it, but
do not promote it while removing the words that said it was uncertain.
