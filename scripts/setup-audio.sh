#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Split the AX310's six playback channels into per-track stereo sinks, so
# applications can be routed to individual mixer tracks.
#
# The deck presents one 6-channel playback device and one 8-channel capture
# device. PipeWire's default profile maps those onto a surround layout, which
# both hides the tracks and wastes half the channels -- so every application ends
# up in the same place and the six knobs control six copies of one thing.
#
# This selects the Pro Audio profile, which exposes the raw channels, and adds
# loopback sinks that present each stereo pair as its own device.
#
# It installs the same two files a package installs, from packaging/, into the
# user's own configuration directories -- so development and the package run
# identical configuration rather than two things that drift. Idempotent and
# reversible: --remove puts it back, and neither needs root.
#
#   --regenerate  rebuild packaging/pipewire/ax310-split.conf from the track
#                 tables below, which are its single source of truth

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PACKAGING="$HERE/../packaging"

FRAGMENT_NAME="ax310-split.conf"
USER_CONF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/pipewire/pipewire.conf.d"
FRAGMENT="$USER_CONF_DIR/$FRAGMENT_NAME"

RULES_NAME="51-ax310.conf"
USER_RULES_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/wireplumber/wireplumber.conf.d"
RULES="$USER_RULES_DIR/$RULES_NAME"

# The names the WirePlumber fragment gives the deck's two ALSA nodes. Generated
# names carry the serial number, which would make the split fragment correct on
# exactly one machine.
RAW_SINK="ax310_raw_out"
RAW_SOURCE="ax310_raw_in"

# The deck's six knobs are labelled, left to right: Mic, Line In, Console, System,
# Game, Chat. Only the last three are host playback -- the others are physical
# inputs -- which is why six playback channels make three stereo pairs and the
# vendor's manual lists exactly three playback devices.
#
# **Confirmed on the hardware** twice over, and --identify re-confirms it in about
# fifteen seconds. Do not weaken this back to a guess.
#
# First by ear, because the meters were misread at the time as one stereo mix.
# They are not: there is one per track, and only the track carrying signal lights.
# So --identify now plays a tone into each sink and reads the deck's own meters --
# 48% on the named track and 0% on every other one, all three times. What the
# meters still cannot do is confirm a level register: they sit pre-fader and read
# the same at 0% as at 100%, which is why --identify-mixes still needs an ear.
PLAYBACK_TRACKS=("System:AUX0,AUX1" "Game:AUX2,AUX3" "Chat:AUX4,AUX5")

# The capture pairs, established by playing a tone into a playback track and
# recording each pair while muting tracks in one mix or the other:
#
#   aux0/1  the CREATOR mix -- what the streamer hears; the 0x27 block sets it
#   aux2/3  the AUDIENCE mix -- what the stream captures; the 0x2e block sets it
#   aux4/5  the microphone; no host playback reaches it
#   aux6/7  host playback pre-fader; no knob affects it
#
# Creator against Audience was settled by ear with --identify-mixes: muting System
# in the 0x27 block silenced the headphones and muting it in 0x2e did not. All four
# are confirmed.
CAPTURE_TRACKS=("Creator:AUX0,AUX1" "Audience:AUX2,AUX3" "Mic:AUX4,AUX5" "Loopback:AUX6,AUX7")

die() { echo "error: $*" >&2; exit 1; }

find_card() {
    pactl list cards 2>/dev/null | awk '/Name: alsa_card.*AX310/ { print $2; exit }'
}

restart_audio() {
    systemctl --user restart pipewire.service pipewire-pulse.service wireplumber.service
    # The daemons need a moment before the new nodes exist to be queried.
    for _ in $(seq 20); do
        pactl info >/dev/null 2>&1 && return 0
        sleep 0.25
    done
    die "PipeWire did not come back after a restart"
}

find_probe() {
    # Newest, not alphabetically first: several presets build this, and picking
    # ax310_probe out of whichever directory sorts first silently ran a binary
    # from before --level existed.
    local probe
    probe=$(ls -1t "$HERE/../out/build/"*/src/tools/ax310_probe 2>/dev/null | head -1)
    [[ -x "${probe:-}" ]] || die "ax310_probe is not built -- cmake --build --preset clang-debug"
    echo "$probe"
}

write_tone() {
    python3 - "$1" <<'TONE'
import math, struct, sys, wave
w = wave.open(sys.argv[1], "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
frames = bytearray()
for n in range(48000 * 6):
    v = int(16000 * math.sin(2 * math.pi * 440 * n / 48000))
    frames += struct.pack("<hh", v, v)
w.writeframes(bytes(frames)); w.close()
TONE
}

if [[ "${1:-}" == "--identify" ]]; then
    # Plays a tone into each split sink in turn and reads the deck's own meters to
    # see which track it landed on. No ears involved.
    #
    # This used to play the tone and ask which knob answered, because the meters
    # were misread at the time as a single stereo mix. They are one per track: a
    # tone in a sink lights exactly its own meter, which is what makes the whole
    # question measurable. What the meters still cannot do is confirm a level
    # register -- they are pre-fader and read the same at 0% as at 100%.
    #
    # The tone must go through the split sinks, not straight at the device. A
    # six-channel file declares FL/FR/FC/LFE/RL/RR, the raw node's ports are
    # aux0..aux5, and PipeWire resolves that mismatch by remixing -- which
    # silently collapses all three pairs onto the same channels and makes every
    # one of them answer the same knob. The loopbacks carry stream.dont-remix for
    # exactly this reason.
    [[ -f "$FRAGMENT" ]] || die "run $0 without --identify first, to create the sinks"
    probe=$(find_probe)

    tone=$(mktemp --suffix=.wav)
    trap 'rm -f "$tone"' EXIT
    write_tone "$tone"

    # The deck's knobs, left to right, which is the order ax310_probe prints its
    # peaks in.
    knobs=(Mic "Line In" Console System Game Chat)

    printf '\n  %-14s %-10s %s\n' "sink" "answered" "peaks, per knob"
    for entry in "${PLAYBACK_TRACKS[@]}"; do
        name="${entry%%:*}"
        sink="ax310_${name,,}"

        pw-cat --playback --target "$sink" "$tone" >/dev/null 2>&1 &
        tone_pid=$!
        sleep 0.5
        peaks=$("$probe" --meters 4 2>/dev/null | awk '/^peak /{ $1=""; sub(/^ /,""); print }')
        wait $tone_pid 2>/dev/null || true

        [[ -n "$peaks" ]] || die "the deck sent no reports -- is another program holding it?"

        # The loudest track, and only if it is loud enough to mean something: a
        # live microphone hears the tone through the headphones, so a bare maximum
        # would sometimes name the microphone.
        best=$(echo "$peaks" | awk '{ m = 0; i = 0; for (k = 1; k <= NF; ++k) if ($k > m) { m = $k; i = k }; print (m >= 10 ? i : 0) }')
        if [[ "$best" -gt 0 ]]; then answered="${knobs[$((best - 1))]}"; else answered="nothing"; fi
        printf '  %-14s %-10s %s\n' "$sink" "$answered" "$peaks"
    done

    echo
    echo "  Each sink should have answered a different track. If the names do not"
    echo "  match PLAYBACK_TRACKS at the top of this script, correct them there and"
    echo "  re-run with --regenerate."
    exit 0
fi

if [[ "${1:-}" == "--identify-mixes" ]]; then
    # Which capture pair is the Creator mix and which the Audience mix. Creator is
    # the one you hear, so this mutes a track in one mix at a time and asks. The
    # capture pairs themselves were identified by measurement; only this last step
    # needs an ear.
    [[ -f "$FRAGMENT" ]] || die "run $0 without arguments first, to create the sinks"
    probe=$(find_probe)
    # Captured, not piped. ax310_probe exits 1 when it prints usage, and under
    # `set -o pipefail` that failure becomes the pipeline's -- so a piped grep
    # reports "stale" for a perfectly good binary.
    probe_usage=$("$probe" 2>&1 || true)
    [[ "$probe_usage" == *"--level"* ]] \
        || die "$probe predates --level; rebuild it with: cmake --build --preset clang-debug"

    # Save what the levels actually are. Restoring a hardcoded value would leave
    # the user's mix louder or quieter than they set it, which is the sort of thing
    # a diagnostic must never do.
    before1=$("$probe" --read 2a 1 | tr -d ' ')
    before2=$("$probe" --read 31 1 | tr -d ' ')
    tone=$(mktemp --suffix=.wav)
    restore() {
        "$probe" --level 1 System "$before1" >/dev/null 2>&1 || true
        "$probe" --level 2 System "$before2" >/dev/null 2>&1 || true
    }
    trap 'rm -f "$tone"; restore' EXIT
    python3 - "$tone" <<'PY'
import math, struct, sys, wave
w = wave.open(sys.argv[1], "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
frames = bytearray()
for n in range(48000 * 3600):
    frames += struct.pack("<hh", *([int(14000 * math.sin(2 * math.pi * 440 * n / 48000))] * 2))
    if n > 48000 * 60: break
w.writeframes(bytes(frames)); w.close()
PY
    # Both mixes up, so the tone is audible whichever one feeds the headphones.
    "$probe" --level 1 System 14 >/dev/null
    "$probe" --level 2 System 14 >/dev/null
    pw-cat --playback --target ax310_system "$tone" >/dev/null 2>&1 &
    tone_pid=$!
    trap 'kill $tone_pid 2>/dev/null; rm -f "$tone"; restore' EXIT

    echo
    echo "  A tone is playing. You should hear it now."
    read -r -p "  Press Enter when you can hear it. " || true
    "$probe" --level 1 System 00 >/dev/null
    read -r -p "  MIX 1 muted. Did the tone stop? Press Enter. " || true
    "$probe" --level 1 System 14 >/dev/null
    "$probe" --level 2 System 00 >/dev/null
    read -r -p "  MIX 1 restored, MIX 2 muted. Did the tone stop this time? Press Enter. " || true
    restore
    echo
    echo "  Whichever mute silenced your headphones is the CREATOR mix."
    echo "  Rename it in CAPTURE_TRACKS at the top of this script and re-run."
    exit 0
fi

generate_fragment() {
    cat <<'HEADER'
# SPDX-License-Identifier: Apache-2.0
# AVerMedia Live Streamer AX310: split the deck into per-track devices.
#
# Generated by scripts/setup-audio.sh --regenerate from the track tables in that
# script, which are the single source of truth for these mappings. Every one of
# them was confirmed on the hardware rather than inferred; docs/capabilities.md
# records how.
#
# The deck presents one six-channel playback device and one eight-channel capture
# device. Its six knobs are Mic, Line In, Console, System, Game and Chat, and only
# the last three are host playback -- so six playback channels make three stereo
# sinks, and the eight capture channels make four stereo sources.
#
# This targets ax310_raw_out and ax310_raw_in, which are the deck's ALSA nodes
# renamed by the WirePlumber fragment beside this one. Without that rename the
# names carry the deck's serial number.
HEADER
    echo "context.modules = ["
    for entry in "${PLAYBACK_TRACKS[@]}"; do
        name="${entry%%:*}"
        pair="${entry##*:}"
        cat <<EOF
  { name = libpipewire-module-loopback
    args = {
      node.description = "AX310 $name"
      capture.props = {
        node.name        = "ax310_${name,,}"
        node.description = "AX310 $name"
        media.class      = Audio/Sink
        audio.position   = [ FL FR ]
        # Presented as a real device, not a virtual one. A loopback node defaults
        # to node.virtual = true, and desktops hide those behind a "show virtual
        # devices" toggle -- which would mean asking somebody to go looking for
        # their own sound card.
        #
        # The claim is honest. These are the only way to reach the deck's three
        # playback tracks: the kernel sees one six-channel device because that is
        # what the USB descriptors declare, so splitting it is software work on
        # every platform. The vendor's own Windows driver does exactly this and
        # its three devices look native there too.
        node.virtual     = false
        device.class     = sound
      }
      playback.props = {
        node.name          = "ax310_${name,,}_out"
        audio.position     = [ ${pair//,/ } ]
        target.object      = "$RAW_SINK"
        stream.dont-remix  = true
        node.passive       = true
        node.dont-reconnect = false
      }
    }
  }
EOF
    done
    for entry in "${CAPTURE_TRACKS[@]}"; do
        name="${entry%%:*}"
        pair="${entry##*:}"
        cat <<EOF
  { name = libpipewire-module-loopback
    args = {
      node.description = "AX310 $name"
      capture.props = {
        node.name          = "ax310_${name,,}_in"
        audio.position     = [ ${pair//,/ } ]
        target.object      = "$RAW_SOURCE"
        stream.dont-remix  = true
        node.passive       = true
      }
      playback.props = {
        node.name        = "ax310_${name,,}_source"
        node.description = "AX310 $name"
        media.class      = Audio/Source
        audio.position   = [ FL FR ]
        node.virtual     = false
        device.class     = sound
      }
    }
  }
EOF
    done
    echo "]"
}

if [[ "${1:-}" == "--regenerate" ]]; then
    generate_fragment > "$PACKAGING/pipewire/$FRAGMENT_NAME"
    echo "wrote $PACKAGING/pipewire/$FRAGMENT_NAME"
    exit 0
fi

if [[ "${1:-}" == "--remove" ]]; then
    rm -f "$FRAGMENT" "$RULES"
    # Restarted first, then the profile put back. The other order looks more
    # natural and does not work: WirePlumber writes its state file lazily, so a
    # restart issued straight after a profile change loses the change and comes
    # back on the profile it had stored before.
    restart_audio
    card=$(find_card) || true
    if [[ -n "${card:-}" ]]; then
        pactl set-card-profile "$card" "output:analog-surround-21+input:analog-surround-71" || true
    fi
    echo "removed $FRAGMENT and $RULES, and restored the surround profile"
    exit 0
fi

[[ -f "$PACKAGING/pipewire/$FRAGMENT_NAME" ]] \
    || die "$PACKAGING/pipewire/$FRAGMENT_NAME is missing -- run $0 --regenerate"
[[ -f "$PACKAGING/wireplumber/$RULES_NAME" ]] \
    || die "$PACKAGING/wireplumber/$RULES_NAME is missing"

# Refuse to install a fragment that no longer matches the tables above, rather
# than installing a stale one and letting the mismatch surface as a track that
# answers the wrong knob.
if ! diff -q <(generate_fragment) "$PACKAGING/pipewire/$FRAGMENT_NAME" >/dev/null; then
    die "$PACKAGING/pipewire/$FRAGMENT_NAME is out of date -- run $0 --regenerate"
fi

card=$(find_card)
[[ -n "$card" ]] || die "no AX310 sound card found -- is the deck plugged in and not claimed by a VM?"

mkdir -p "$USER_CONF_DIR" "$USER_RULES_DIR"
install -m 0644 "$PACKAGING/wireplumber/$RULES_NAME" "$RULES"
install -m 0644 "$PACKAGING/pipewire/$FRAGMENT_NAME" "$FRAGMENT"

# The rule states a preference for a device WirePlumber has not seen before; this
# deck it has, and a profile it already stored wins over the rule. So the profile
# is also set outright, which is what makes the first run work on this machine as
# well as on a fresh one.
pactl set-card-profile "$card" pro-audio

restart_audio

# The fragment names two nodes the WirePlumber rule is supposed to have renamed.
# If the rename did not happen, the loopbacks come up with nothing behind them --
# three sinks that accept audio and drop it, which is indistinguishable from a
# muted deck. Better to say so.
# Waited for rather than checked once: restart_audio returns as soon as the
# daemon answers, which is before it has finished creating nodes.
for node in "$RAW_SINK" "$RAW_SOURCE"; do
    found=""
    for _ in $(seq 20); do
        if pactl list short 2>/dev/null | grep -qw "$node"; then found=yes; break; fi
        sleep 0.25
    done
    [[ -n "$found" ]] \
        || die "$node did not appear -- the WirePlumber rule in $RULES did not take"
done

echo "installed $RULES and $FRAGMENT"
