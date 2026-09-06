// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Types.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>

/// Wire layout of the AX310's HID reports.
///
/// The bit positions here are the single source of truth for how a Types.h
/// vocabulary value is represented on the bus: callers work in Button / KnobId
/// and never see a mask.
namespace ax310::protocol
{

/// Byte offsets within the 58-byte input report.
///
/// Written down as offsets and read one field at a time, rather than overlaid
/// with a packed struct. The struct was shorter and hid two things that each
/// cost this project a bug: that reading it meant casting a byte buffer to a
/// type no object of which exists there, and that the endianness is **mixed** --
/// touch coordinates little-endian, audio meters big-endian, in the same report.
inline constexpr std::size_t EventTypeOffset = 0x00;
inline constexpr std::size_t ButtonsOffset = 0x00;
inline constexpr std::size_t TouchFlagsOffset = 0x01;
inline constexpr std::size_t TouchXOffset = 0x02; ///< little-endian
inline constexpr std::size_t TouchYOffset = 0x04; ///< little-endian
inline constexpr std::size_t KnobPushOffset = 0x06;
inline constexpr std::size_t KnobTouchOffset = 0x07;
inline constexpr std::size_t KnobValuesOffset = 0x08;
inline constexpr std::size_t AudioMetersOffset = 0x12; ///< big-endian pairs
inline constexpr std::size_t ChecksumOffset = 0x39;

/// One input report, decoded.
///
/// A value type built by reading the bytes, not a view onto them: nothing here
/// aliases the buffer, so there is no lifetime to get wrong and no layout for a
/// compiler to disagree about. Fields whose meaning is unknown are simply not
/// here -- the bytes are still on the wire, and naming them would only invite
/// code to depend on a guess.
struct InputReport
{
    std::uint8_t buttons = 0;   ///< Button bitmask; meaningless on a touch report.
    std::uint8_t knobPush = 0;  ///< Knob push bitmask.
    std::uint8_t knobTouch = 0; ///< Knob capacitive-touch bitmask.

    /// Per-knob rotation counters. Relative, not positions -- see AGENT.md.
    std::array<std::uint8_t, KnobCount> knobValues {};

    /// One meter per track, the louder side of its stereo pair, raw against
    /// AudioLevelFullScale.
    std::array<int, 6> audioMeters {};

    bool isScreenTouch = false;  ///< Whether a finger is on the glass.
    std::uint8_t touchFlags = 0; ///< Byte 1 of a touch report; latches per contact, see below.
    int touchX = 0;
    int touchY = 0;
};

/// Whether a bit in one of the report's bitmasks is set.
///
/// A named pair rather than a bool, so a decoder table's row says which state it
/// produces an event for instead of carrying an unexplained `true`.
enum class BitState : std::uint8_t
{
    Clear,
    Set
};

/// Size of an input report on the control interface.
inline constexpr std::size_t ControlReportSize = 58;

/// Size of a report once a transport pads it to the full report length.
inline constexpr std::size_t PaddedReportSize = 64;

/// One outgoing command as the device expects it: always a full report.
using Payload = std::array<std::uint8_t, PaddedReportSize>;

/// hidapi prefixes every transfer with a report-id byte. This interface's report
/// descriptor declares no report IDs at all (verified: usage page 0xffa0, with an
/// INPUT of 58 bytes, an OUTPUT of 1024 and a FEATURE of 64, none numbered), so
/// that byte is always 0x00 and every buffer is one longer than its payload.
///
/// Getting this wrong is silent in the worst way: hidapi reads the first payload
/// byte as a report id, the kernel rejects the transfer, and the device simply
/// stays asleep. Every one of the 74 init payloads failed that way.
inline constexpr std::size_t FeatureReportSize = PaddedReportSize + 1;

/// A command payload framed for the wire: 0x00 then the 64 payload bytes.
using FeatureReport = std::array<std::uint8_t, FeatureReportSize>;

/// @param payload The 64-byte command.
/// @return @p payload with the report-id byte in front.
[[nodiscard]] constexpr FeatureReport frameFeatureReport(Payload const& payload) noexcept
{
    FeatureReport framed {};
    std::ranges::copy(payload, std::next(framed.begin(), 1));
    return framed;
}

/// AVerMedia's USB vendor id.
inline constexpr std::uint16_t VendorId = 0x07ca;

/// How to find the deck while it is in one particular mode.
struct ModeDescriptor
{
    std::uint16_t productId; ///< The product id it enumerates as.
    int interfaceNumber;     ///< The HID interface carrying the vendor protocol.
};

/// One descriptor per DeviceMode, indexed by the enumerator.
///
/// Both are on the bus at the same time -- they are two USB devices of one
/// physical unit, not two states of one device. Only Control carries the deck:
/// its interface 0 is the vendor interface (usage page 0xffa0). Base's HID
/// interface 4 is a plain Consumer Control page -- play/pause, volume, mute --
/// belonging to the audio side, and nothing the driver wants is there.
inline constexpr std::array<ModeDescriptor, DeviceModeCount> ModeDescriptors { {
    { .productId = 0x0310, .interfaceNumber = 4 }, // Base: media keys, not ours
    { .productId = 0x1310, .interfaceNumber = 0 }, // Control: the deck
} };

/// @param mode The mode to look up.
/// @return How to find the deck in @p mode.
[[nodiscard]] constexpr ModeDescriptor descriptorFor(DeviceMode mode) noexcept
{
    return ModeDescriptors[indexOf(mode)];
}

/// How a command payload's first byte reads.
///
/// Recovered from the captured init and shutdown sequences: the same property
/// appears with 0x81 carrying zeroes and with 0x01 carrying a value, which is
/// what a read and a write of one property look like.
enum class CommandKind : std::uint8_t
{
    Set = 0x01,
    Get = 0x81
};

/// Second byte of a property command. The only group seen so far.
inline constexpr std::uint8_t PropertyGroup = 0x10;

/// The four function buttons' colour, written one button at a time to `0xc0`.
///
/// Ten bytes, captured across fourteen records of the vendor software:
///
///     00  <button>  01  <r> <g> <b>  ??  ??  <lit>  80
///
///   * **byte 1 selects the button**, `0x3c` to `0x3f`. Turning all four off
///     writes all four selectors in one burst, which is how the set is known to
///     be exactly those and consecutive.
///   * **bytes 3, 4 and 5 are red, green and blue**, proven by driving one button
///     to each primary in turn: `ff 00 00`, `00 ff 00`, `00 00 ff`.
///   * **byte 8 lights the button**: `0x1f` on, `0x00` off. "Off" writes black
///     *and* clears this, so it is not merely a colour of zero.
///   * **bytes 6 and 7 are not understood, and are not part of the colour.** Two
///     records with the same button and the same colour, captured minutes apart,
///     differ in them -- so they are not a checksum of this record and not
///     derived from its contents. They carry something outside it.
///
/// There is no brightness field. The vendor scales the colour host-side and sends
/// the result: its slider at minimum sent `0x19` on the lit channel and at
/// maximum `0xff`. `0x19` is 25, the same floor its panel-brightness slider uses.
///
/// Which selector is which physical button is only half known: `0x3c` is the one
/// the vendor's grid calls top-left and `0x3e` the one it calls bottom-right.
/// `0x3d` and `0x3f` have not been assigned, and the deck's own physical order has
/// never been verified either -- ButtonBits runs `0x08, 0x04, 0x02, 0x01`, which
/// is reversed for no reason anybody has recorded.
inline constexpr std::uint8_t ButtonColourAddress = 0xc0;

/// Which selector addresses which button, indexed by Button.
///
/// The four selectors are consecutive but they are **not** in the enum's order.
/// Captured one button at a time, all four:
///
///     0x3c  top-left        0x3d  top-right
///     0x3f  bottom-left     0x3e  bottom-right
///
/// which is clockwise, where Button is row-major. `FirstButtonSelector + index`
/// would light the wrong two, so the mapping is a table.
///
/// A caveat worth keeping: these are the positions the *vendor's* grid gives them.
/// Whether Button's own enumerators match the deck's physical layout has never
/// been verified -- ButtonBits runs 0x08, 0x04, 0x02, 0x01, reversed for no
/// recorded reason -- so this maps a vendor label to a selector, and the last link
/// to a physical button is still assumed. Pressing each button and watching which
/// bit arrives would close it.
inline constexpr std::array<std::uint8_t, ButtonCount> ButtonColourSelectors {
    0x3c, // TopLeft
    0x3d, // TopRight
    0x3f, // BottomLeft
    0x3e, // BottomRight
};
static_assert(rowsInEnumeratorOrder(AllButtons));

/// @param button Which button.
/// @return The selector byte its colour record carries.
[[nodiscard]] constexpr std::uint8_t selectorFor(Button button) noexcept
{
    return ButtonColourSelectors[indexOf(button)];
}

/// Byte 8's two observed values.
inline constexpr std::uint8_t ButtonLit = 0x1f;
inline constexpr std::uint8_t ButtonDark = 0x00;

/// Builds one button-colour record.
///
/// The bytes nobody has explained are set to values that were observed together
/// with a lit button, rather than to zero, because replaying a shape that has been
/// seen is the rule this project writes commands under.
///
/// @param selector Which button, from ButtonColourSelectors.
/// @param red Red, 0 to 255.
/// @param green Green.
/// @param blue Blue.
/// @param lit Whether the button is lit at all.
/// @return The ten values to write to ButtonColourAddress.
[[nodiscard]] constexpr std::array<std::uint8_t, 10> buttonColourRecord(
    std::uint8_t selector, std::uint8_t red, std::uint8_t green, std::uint8_t blue,
    bool lit) noexcept
{
    return { 0x00,
             selector,
             0x01,
             red,
             green,
             blue,
             0x00,
             0x1d,
             lit ? ButtonLit : ButtonDark,
             0x80 };
}

/// The display command group: `[0x01][0x0a][level]`, and that is the whole of it.
///
/// A second family beside the property one, which is why the screen's brightness
/// was never found among the property addresses -- it was never there. `0x1e` was
/// once named for it on strong evidence from captures alone, and the hardware
/// disagreed: that register dims the knob rings.
///
/// Three captures of the vendor software settled the encoding between them, each
/// sending exactly one command where an idle capture of the same length sends
/// none at all:
///
///   | action                | command       | third byte |
///   | slider to minimum     | `01 0a 19`    | 25         |
///   | slider to maximum     | `01 0a 64`    | 100        |
///   | the panel-off widget  | `01 0a ff`    | 255        |
///
/// So the third byte is a **percentage**, the vendor's slider runs 25 to 100, and
/// `0xff` is a sentinel for off rather than a level.
inline constexpr std::uint8_t DisplayGroup = 0x0a;

/// The dimmest and brightest the vendor's own slider will send.
///
/// Values below the minimum are not refused by anything here, but they have never
/// been observed: the vendor stops at 25 and offers the off widget instead, which
/// suggests the panel does not usefully dim further. Device clamps to this range
/// rather than discovering what 1% does on somebody's deck.
inline constexpr int MinPanelBrightness = 25;
inline constexpr int MaxPanelBrightness = 100;

/// The value that turns the panel off, which is not a brightness.
inline constexpr std::uint8_t PanelOffLevel = 0xff;

/// Builds a display command.
/// @param level A percentage, or PanelOffLevel.
/// @return The payload to frame and send.
[[nodiscard]] constexpr Payload displayCommand(std::uint8_t level) noexcept
{
    Payload payload {};
    payload[0] = static_cast<std::uint8_t>(CommandKind::Set);
    payload[1] = DisplayGroup;
    payload[2] = level;
    return payload;
}

/// Addresses that must not be written.
///
/// Writing 0x01 to 0x16 wedged the deck: every subsequent read failed and it
/// took a power cycle to recover. It is not in either captured sequence, and
/// this is the evidence that being absent from them is a reason to leave an
/// address alone rather than an invitation to try it.
inline constexpr std::array<std::uint8_t, 1> DangerousAddresses { 0x16 };

/// Properties the deck exposes, as far as the captures show.
///
/// Named only where the evidence supports it. Several more ids appear in the
/// init sequence whose meaning is unknown, and they stay out of here rather than
/// being given a plausible name: an id named on a guess invites code to trust it.
enum class Property : std::uint8_t
{
    /// Written only by the shutdown sequence, to 0x02.
    DisplayPower = 0x0f,

    /// Brightness of the knob LED rings -- **not** the screen. Init writes 0x0d
    /// and shutdown 0x09; writing 0x01 extinguishes the rings, observed on the
    /// hardware. It was a screen-brightness candidate on the evidence of the
    /// captures alone, and the deck disagreed.
    KnobLedBrightness = 0x1e,

    /// Base of the **creator mix's** six per-track levels, `0x00..0x14` each.
    ///
    /// Confirmed by ear: with a tone playing, muting System here silences the
    /// headphones, and muting the same track in AudienceMixLevels does not. The
    /// creator mix is the one the streamer hears.
    ///
    /// The knob LED rings display this block, which is why it was called
    /// CreatorMixLevels for a long time. The rings show whichever mix the deck is
    /// monitoring, and SelectedMix says which that is.
    ///
    /// Read or written with length 7 it carries all six at once, in the deck's
    /// printed knob order: Mic, Line In, Console, System, Game, Chat. Written with
    /// length 1 at `base + track` it sets one -- which is why dragging the Mic
    /// slider in the vendor software writes `0x27` and dragging System writes
    /// `0x2a`. They are not different registers; Mic is simply track 0.
    ///
    /// Confirmed to control audio, not merely the LED rings: muting System here
    /// silences the tone in the mix captured on the deck's first capture pair.
    /// The deck's own meters cannot show this because they are pre-fader.
    CreatorMixLevels = 0x27,

    /// Which knobs light at all. Init writes 0x80; writing 0x10 leaves only the
    /// first knob's ring lit, so it selects rather than scales. The encoding is
    /// not worked out -- a six-knob mask would not be 0x80.
    ///
    /// It also tracks the mixer mode, which the ring-selection reading does not
    /// explain: the vendor software writes 0x80 for Single Mix, 0x00 for Dual Mix
    /// and 0x01 when switching to the audience mix, always alongside 0x22. So
    /// either the name is too narrow or this address carries two things.
    KnobLedSelect = 0x21,

    /// Init writes 0x01 here; writing 0x00 lights every ring. What it means
    /// beyond that is unknown.
    KnobLedModeAt14 = 0x14,

    /// Base of the **audience mix's** six per-track levels, laid out exactly like
    /// CreatorMixLevels: `base + track`, same knob order. This is the mix the
    /// stream captures; muting a track here leaves the headphones untouched.
    ///
    /// Confirmed against the hardware: writing `0x31`, which is this base plus
    /// System, silences a System tone in the mix on the deck's second capture pair
    /// and leaves the first pair untouched. So the deck really does carry two
    /// independent six-track mixes.
    ///
    /// Historical note on the name: this address was long recorded as an
    /// unexplained second array.
    ///
    /// Established by capturing the vendor software: enabling **Dual Mix** writes
    /// this array and nothing else writes it -- not disabling Dual Mix, not
    /// switching which mix is monitored, not dragging a volume. That is exactly
    /// the register a second independent mix needs and no other feature does.
    ///
    /// It reads back independently of CreatorMixLevels, which is what first
    /// suggested it: `14 0a 0a 0a 0a 0a` here against `14 14 14 14 14 14` there.
    /// Writing a staircase to it changes nothing visible, because the deck
    /// displays the mix it is currently monitoring and this is the other one.
    AudienceMixLevels = 0x2e,

    /// Brackets a settings change: `0x01` before, `0x00` after.
    ///
    /// Every mode change the vendor software makes is wrapped in this pair --
    /// enabling Dual Mix, disabling it, switching the monitored mix. Volume
    /// drags are **not** wrapped, which fits: those stream continuously and a
    /// fence per sample would be pointless. The captured init sequence never
    /// writes it at all.
    SettingsTransaction = 0x1d,

    /// Which mix is being monitored: `0x00` creator, `0x01` audience.
    ///
    /// Switching creator to audience writes `0x01` here, and the init sequence
    /// writes `0x00` -- so a deck this driver attaches to is left monitoring the
    /// creator mix. Only that one direction has been captured; the write for
    /// audience back to creator is inferred, not seen.
    SelectedMix = 0x15,

    // The per-track levels live in two contiguous six-byte blocks, one per mix,
    // based at CreatorMixLevels and AudienceMixLevels. See levelAddressOf().

    // The Mic track does not follow the pattern and so has no enumerator here:
    // dragging its slider writes nothing in the 0x2a block, but CreatorMixLevels and
    // KnobPropertyAt35 in pairs carrying the same value. Why the microphone is
    // special is not established -- it is the one track that is a capture rather
    // than a playback channel, which is the obvious guess and only a guess.
    //
    // The two remaining tracks have not been watched, so whether 0x2d continues
    // the block is unknown. It cannot simply run to 0x2f: 0x2e already carries
    // the second mix's six levels.

    /// Read with length 7 during init, never written, and it answers with one
    /// value rather than six.
    /// Microphone preamp gain, `0x00` to `0x38`.
    ///
    /// The vendor's slider sends `0x00` at its floor and `0x38` at its ceiling.
    /// **Confirmed on the hardware** rather than named from the capture: holding
    /// it at each end while speaking moved the microphone's own meter, which is
    /// pre-fader and so answers the preamp rather than the mix. That test was
    /// chosen over listening because there is a second interface upstream of this
    /// deck with its own compressor, and an ear cannot tell the two apart.
    MicGain = 0x1f,

    KnobPropertyAt35 = 0x35,
};

/// What the vendor's init sequence writes to Property::KnobLedBrightness.
///
/// Used as the top of the range because nothing better is known; the deck
/// accepted 0x14 as well, so the true ceiling has not been found.
inline constexpr int KnobLedBrightnessAtStartup = 0x0d;

/// Values a property command carries per knob.
inline constexpr std::size_t KnobPropertyLength = 7;

/// @param address The address to write, named or not.
/// @param values Its new value, one byte per element.
/// @return The full 64-byte payload, ready to be framed and sent.
///
/// Takes a raw address rather than a Property because most of the register space
/// has no name that the evidence supports, and code that has to preserve a
/// register it cannot identify still has to address it.
[[nodiscard]] constexpr Payload setPropertyAt(std::uint8_t address,
                                              std::span<std::uint8_t const> values) noexcept
{
    Payload payload {};
    payload[0] = static_cast<std::uint8_t>(CommandKind::Set);
    payload[1] = PropertyGroup;
    payload[2] = address;
    payload[3] = static_cast<std::uint8_t>(values.size());
    std::ranges::copy(values, std::next(payload.begin(), 4));
    return payload;
}

/// @param address The address to read, named or not.
/// @param length How many bytes the answer is expected to carry.
/// @return The full 64-byte payload.
[[nodiscard]] constexpr Payload getPropertyAt(std::uint8_t address, std::size_t length) noexcept
{
    Payload payload {};
    payload[0] = static_cast<std::uint8_t>(CommandKind::Get);
    payload[1] = PropertyGroup;
    payload[2] = address;
    payload[3] = static_cast<std::uint8_t>(length);
    return payload;
}

/// @param property The property to write.
/// @param values Its new value, one byte per element.
/// @return The full 64-byte payload, ready to be framed and sent.
[[nodiscard]] constexpr Payload setProperty(Property property, std::span<std::uint8_t const> values) noexcept
{
    return setPropertyAt(static_cast<std::uint8_t>(property), values);
}

/// @param property The property to read.
/// @param length How many bytes the answer is expected to carry.
/// @return The full 64-byte payload.
[[nodiscard]] constexpr Payload getProperty(Property property, std::size_t length) noexcept
{
    return getPropertyAt(static_cast<std::uint8_t>(property), length);
}

/// @param block Either Property::CreatorMixLevels or Property::AudienceMixLevels.
/// @param knob Which track's level to address.
/// @return The address of that track's level in that mix.
///
/// The two mixes are six-byte blocks laid out in the deck's printed knob order,
/// so one track's level is its block's base plus its index. Established on the
/// hardware in both blocks.
[[nodiscard]] constexpr std::uint8_t levelAddressOf(Property block, KnobId knob) noexcept
{
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(block) + indexOf(knob));
}

/// Offsets within a feature-report reply, which keeps its leading report-id byte.
inline constexpr std::size_t ReplyKindOffset = 1;
inline constexpr std::size_t ReplyGroupOffset = 2;
inline constexpr std::size_t ReplyAddressOffset = 3;
inline constexpr std::size_t ReplyLengthOffset = 4;
inline constexpr std::size_t ReplyValuesOffset = 5;

/// Picks the values out of a property read's reply, if it really is one.
///
/// The deck answers with the command header echoed back --
/// `[report id][0x81][0x10][address][length]` and then the values -- and leaves
/// **the remains of the previous reply** in the bytes past that length. So the
/// echo is not decoration: it is the only thing separating an answer from stale
/// rubbish, and every field of it is checked here.
///
/// @param reply What getFeatureReport() returned.
/// @param address The address that was asked for.
/// @return The values, or nothing if this is not that address's answer.
[[nodiscard]] constexpr std::optional<std::span<std::uint8_t const>> propertyReplyValues(
    std::span<std::uint8_t const> reply, std::uint8_t address) noexcept
{
    if (reply.size() <= ReplyValuesOffset)
        return std::nullopt;

    if (reply[ReplyKindOffset] != static_cast<std::uint8_t>(CommandKind::Get))
        return std::nullopt;

    if (reply[ReplyGroupOffset] != PropertyGroup || reply[ReplyAddressOffset] != address)
        return std::nullopt;

    std::size_t const length = reply[ReplyLengthOffset];
    if (length == 0 || reply.size() < ReplyValuesOffset + length)
        return std::nullopt;

    return reply.subspan(ReplyValuesOffset, length);
}

/// One register the init sequence overwrites, and how many bytes it carries.
struct PreservedAddress
{
    std::uint8_t address; ///< Where it lives in the register space.
    std::uint8_t length;  ///< How many bytes to read and put back.
};

/// Every property address the captured init sequence writes.
///
/// `0x2e`, the audience mix's levels, is deliberately **not** here. It looks like
/// an omission -- the creator block at `0x27` is preserved and its twin is not --
/// but neither the init nor the shutdown sequence writes `0x2e`, so there is
/// nothing to put back. Adding it would mean writing a register the vendor only
/// writes when enabling Dual Mix, on every connect, to restore a value nothing
/// had changed.
///
/// Read before the handshake and written back after, because that sequence is
/// somebody else's saved configuration rather than an initialisation. Most of
/// these have no name the evidence supports -- which is the argument for
/// restoring them, not against it: overwriting a register nobody can identify is
/// worse than overwriting one we understand, because the damage is unpredictable.
///
/// The `0xc0` and `0xe0` record writes are **not** here. Their length field is a
/// record size rather than a byte count and one command carries several records,
/// so reading one back is not the same shape of operation. They are still
/// overwritten on connect.
inline constexpr auto PreservedAddresses = std::to_array<PreservedAddress>({
    { .address = 0x11, .length = 3 },
    { .address = 0x14, .length = 1 },
    { .address = 0x15, .length = 1 },
    { .address = 0x1e, .length = 1 },
    { .address = 0x1f, .length = 1 },
    { .address = 0x20, .length = 1 },
    { .address = 0x21, .length = 1 },
    { .address = 0x22, .length = 1 },
    { .address = 0x23, .length = 1 },
    { .address = 0x27, .length = 7 },
    { .address = 0x35, .length = 1 },
    { .address = 0x3c, .length = 1 },
    { .address = 0x3d, .length = 1 },
});

/// The most bytes any preserved address carries, so a snapshot needs no heap.
inline constexpr std::size_t MaxPreservedLength = 7;

/// First byte of the deck's other command family, which frames differently:
/// [0xfe][0x00][length][body...][checksum], where length counts the whole
/// command and the checksum is the low byte of the body summed.
inline constexpr std::uint8_t FramedCommandMarker = 0xfe;

/// Bytes a framed command spends on framing: marker, zero, length, command and
/// the trailing checksum. The stated length counts all of them.
inline constexpr std::size_t FramedOverhead = 5;

/// Commands in the framed family, as far as the hardware has confirmed them.
///
/// The deck carries a DSP with at least five effects -- noise gate, compressor,
/// reverb, echo and equaliser -- each with an enable and a parameter block. Names
/// here were established by capturing the vendor software toggling one effect at
/// a time, not by reading the captures alone; the unnamed ones stay out.
enum class FramedCommand : std::uint8_t
{
    /// The delay-effect block on or off, shared by reverb and echo. Toggling
    /// reverb on writes `0x01` and off writes `0x00`; switching to echo writes
    /// `0x01` here too, and distinguishes itself in DelayEffectParameters.
    DelayEffectEnable = 0x85,

    /// Noise gate parameters, 22 bytes, byte-identical to init[27] and init[28].
    /// Sent on its own when the gate is toggled; no separate gate enable has been
    /// seen, so either this block carries one or the enable is still unfound.
    NoiseGateParameters = 0x9e,

    /// Three flags the vendor software always writes together, `0x01` before it
    /// sends the eight equaliser bands and `0x00` when another effect is being
    /// configured with the equaliser off. Three of them for an equaliser the UI
    /// splits into three groups -- bass, mid and treble -- is a tidy fit and an
    /// untested one: nothing has yet been seen to write them separately.
    EqualiserEnableA = 0x87,
    EqualiserEnableB = 0x88,
    EqualiserEnableC = 0x9c,

    /// Parameters for the delay-effect block, 21 bytes.
    ///
    /// **Body byte 14 selects which effect it is**: `0x00` none, `0x01` reverb,
    /// `0x02` echo. Reverb and echo share one block and one enable -- switching
    /// echo on sends `0x85 = 01` and a body with byte 14 at `0x02`, differing
    /// from the reverb body in exactly that byte and one parameter.
    ///
    /// The block reverb sends is byte-identical to init[25], which is how we know
    /// the captured sequence turns the user's reverb on.
    ///
    /// **Presets are an application feature, not a device one.** The vendor
    /// software stores a slider set per preset and pushes it as an ordinary
    /// parameter write -- selecting one sends a single block of this command and
    /// nothing else, with no preset index anywhere on the wire. So there is
    /// nothing to reverse-engineer there, and this project's own presets can be
    /// whatever it likes.
    ///
    /// The five reverb sliders are mapped; see the delayEffect namespace for the
    /// offsets. Byte 6 is not among them -- it differs between reverb and echo and
    /// is the likeliest home for echo's delay, which has not been dragged yet.
    ///
    /// This byte has been read wrong twice. First as a *mix* selector, on the
    /// strength of two blocks differing only there in a device with two mixes.
    /// Then as a mirror of the enable, which fitted every value seen until the
    /// echo capture produced a third one. Both readings were consistent with the
    /// evidence at the time, which is the point: a two-valued field looks like a
    /// flag until it takes a third value.
    DelayEffectParameters = 0x94,

    /// Compressor on or off. Never appears in the captured init sequence at all;
    /// found only by watching the vendor software switch the compressor off.
    CompressorEnable = 0x9b,

    /// Compressor parameters, 17 bytes, byte-identical to init[30].
    ///
    /// **Body byte 9 is the threshold in dB**, and it is the only byte that moves
    /// when the threshold slider is dragged: values of `0xfb`, `0xfc`, `0xe6` and
    /// `0xee` read as -5, -4, -26 and -18 dB. Whether it is a signed byte or the
    /// low half of a Q8 value at bytes 8..9 cannot be told apart yet -- byte 8 has
    /// been `0x00` every time, and every observed threshold was a whole dB.
    ///
    /// **Body byte 11 is the ratio**, the only byte the ratio slider moves:
    /// `0x00`, `0x0c` and `0x10` have been seen. Threshold at 9 and ratio at 11,
    /// both odd offsets with a zero below them, suggest the body is a selector
    /// byte followed by 16-bit little-endian values -- which would make both
    /// fields Q8, integer part in the high byte.
    ///
    /// The repeated 16-bit pair at bytes 4..7, `0x0e83` twice, sits where attack
    /// and release would and has not been confirmed.
    ///
    /// Body byte 0 may select which dynamics processor is meant: it is `0x00`
    /// here and `0x80` in init[29], and the noise gate is the deck's other
    /// dynamics processor.
    CompressorParameters = 0x9f,

    /// One equaliser band: `[band 0..7][0x01][enable]` then five little-endian
    /// signed 32-bit Q30 biquad coefficients, `b0 b1 b2 a1 a2`.
    ///
    /// **Only bands 1..6 are adjustable.** Across every capture taken, bands 0 and
    /// 7 have exactly one value each and their numerators are `(1, -2, 1)*K` and
    /// `(1, 2, 1)*K` -- a fixed high-pass and low-pass band-limiting the chain.
    /// Bands 1 to 6 take several values each.
    ///
    /// The adjustable ones are peaking sections: `b1` is exactly `a1` in every
    /// band seen, both being the same `-2cos w`, and the gain rides in the
    /// coefficients rather than being sent separately. A band whose `b` equals its
    /// `a` throughout is unity, which is a slider at 0 dB.
    ///
    /// Six adjustable bands against the vendor UI's **eight** sliders, and the map
    /// is confirmed by prediction rather than only by counting:
    ///
    /// | band | 1 | 2 | 3 | 4 | 5 | 6 |
    /// |------|---|---|---|---|---|---|
    /// | Hz   | 100 | 250 | 500 | 1000 | 4000 | 8000 |
    ///
    /// 8 kHz was measured first and moves band 6. Counting back predicted that
    /// **16 kHz and 50 Hz have no band at all** and that 100 Hz would move band 1;
    /// both held. Dragging either outer slider makes the vendor software re-send
    /// all eight bands unchanged, which is why those drags looked like failures
    /// for four captures before the cause was understood.
    EqualiserBand = 0xa3,

    /// Carries no body and always follows an `0x81 0x01`. Reads as apply/commit.
    Commit = 0xb2,
};

/// Body offsets within FramedCommand::DelayEffectParameters.
///
/// Established by dragging one slider at a time in the vendor software and
/// watching which byte moved -- the same method that mapped the compressor. Level,
/// damp and diffusion are plain 8-bit controls, each seen spanning `0x00` to
/// `0xff`.
///
/// **Decay is shared.** It moves for both the time slider and the room size
/// slider, while room size *also* writes its own 16-bit field. That is what a
/// reverb's feedback gain does: the decay coefficient needed for a given RT60
/// depends on the delay length, and the delay length is the room size. So the
/// host computes the coefficient and sends the size alongside it.
namespace delayEffect
{
    inline constexpr std::size_t DecayOffset = 1;     ///< 8-bit; depends on time *and* room size.
    inline constexpr std::size_t DampOffset = 2;      ///< 8-bit.
    inline constexpr std::size_t LevelOffset = 3;     ///< 8-bit, the wet mix.
    inline constexpr std::size_t DiffusionOffset = 8; ///< 8-bit.
    inline constexpr std::size_t TypeOffset = 14;     ///< 0 none, 1 reverb, 2 echo.
    inline constexpr std::size_t RoomSizeOffset = 18; ///< 16-bit little-endian.
} // namespace delayEffect

/// Which effect a parameter belongs to, for grouping in an interface.
enum class EffectId : std::uint8_t
{
    NoiseGate = 0,
    Compressor = 1,
    Equaliser = 2,
    Reverb = 3,
    Echo = 4,

    Last = Echo
};

/// Number of effects, derived from the enumeration rather than stated.
inline constexpr std::size_t EffectCount = static_cast<std::size_t>(EffectId::Last) + 1;

/// What each effect is called, indexed by the enumerator.
inline constexpr std::array<std::string_view, EffectCount> EffectNames {
    "Noise gate", "Compressor", "Equaliser", "Reverb", "Echo"
};

/// @param effect The effect to name.
/// @return Its display label.
[[nodiscard]] constexpr std::string_view nameOf(EffectId effect) noexcept
{
    return EffectNames[static_cast<std::size_t>(effect)];
}

/// Every adjustable DSP value whose position on the wire has been established.
///
/// Only fields confirmed by capturing the vendor software moving one slider and
/// watching which byte changed appear here. The gate's four, the echo's three and
/// the compressor's remaining three are known to exist but not yet located, and
/// stay out until they are -- a parameter listed at a guessed offset would write
/// a real byte somewhere else.
enum class Parameter : std::uint8_t
{
    ReverbLevel = 0,
    ReverbDecay = 1,
    ReverbDamp = 2,
    ReverbDiffusion = 3,
    ReverbRoomSize = 4,
    CompressorThreshold = 5,
    CompressorRatio = 6,

    Last = CompressorRatio
};

/// Number of parameters, derived from the enumeration rather than stated.
inline constexpr std::size_t ParameterCount = static_cast<std::size_t>(Parameter::Last) + 1;

/// @param parameter The parameter to index.
/// @return Its zero-based position, for indexing a per-parameter table.
[[nodiscard]] constexpr std::size_t indexOf(Parameter parameter) noexcept
{
    return static_cast<std::size_t>(parameter);
}

/// How a parameter's value sits in the command body.
enum class ParameterEncoding : std::uint8_t
{
    UnsignedByte,   ///< One byte, `0..255`.
    SignedByte,     ///< One byte, two's complement -- a threshold in dB.
    UnsignedWordLE, ///< Two bytes, little-endian.
};

/// One adjustable value: what it is called, and where it lives on the wire.
///
/// The whole DSP surface is this table. A method per control --
/// `setReverbLevel`, `setCompressorThreshold` -- would be a dozen near-identical
/// functions today and thirty once the remaining sliders are found. As a table,
/// each newly located field is **one row**, and every interface built on the
/// table gains the control without new code.
struct ParameterInfo
{
    Parameter id;               ///< Which parameter this row describes.
    std::string_view name;      ///< Display label, as a person would say it.
    EffectId effect;            ///< Which effect it groups under.
    FramedCommand command;      ///< The command whose body carries it.
    std::uint8_t bodyOffset;    ///< Where in that body it sits.
    ParameterEncoding encoding; ///< How it is stored there.
    int minimum;                ///< Lowest accepted value, inclusive.
    int maximum;                ///< Highest accepted value, inclusive.
    std::string_view unit;      ///< Suffix for display; empty when unitless.
};

/// Every parameter we can currently write, in enumerator order.
inline constexpr std::array<ParameterInfo, ParameterCount> Parameters { {
    { .id = Parameter::ReverbLevel,
      .name = "Level",
      .effect = EffectId::Reverb,
      .command = FramedCommand::DelayEffectParameters,
      .bodyOffset = delayEffect::LevelOffset,
      .encoding = ParameterEncoding::UnsignedByte,
      .minimum = 0,
      .maximum = 255,
      .unit = "" },
    { .id = Parameter::ReverbDecay,
      .name = "Decay",
      .effect = EffectId::Reverb,
      .command = FramedCommand::DelayEffectParameters,
      .bodyOffset = delayEffect::DecayOffset,
      .encoding = ParameterEncoding::UnsignedByte,
      .minimum = 0,
      .maximum = 255,
      .unit = "" },
    { .id = Parameter::ReverbDamp,
      .name = "Damp",
      .effect = EffectId::Reverb,
      .command = FramedCommand::DelayEffectParameters,
      .bodyOffset = delayEffect::DampOffset,
      .encoding = ParameterEncoding::UnsignedByte,
      .minimum = 0,
      .maximum = 255,
      .unit = "" },
    { .id = Parameter::ReverbDiffusion,
      .name = "Diffusion",
      .effect = EffectId::Reverb,
      .command = FramedCommand::DelayEffectParameters,
      .bodyOffset = delayEffect::DiffusionOffset,
      .encoding = ParameterEncoding::UnsignedByte,
      .minimum = 0,
      .maximum = 255,
      .unit = "" },
    { .id = Parameter::ReverbRoomSize,
      .name = "Room size",
      .effect = EffectId::Reverb,
      .command = FramedCommand::DelayEffectParameters,
      .bodyOffset = delayEffect::RoomSizeOffset,
      .encoding = ParameterEncoding::UnsignedWordLE,
      .minimum = 0,
      .maximum = 1023,
      .unit = "" },
    { .id = Parameter::CompressorThreshold,
      .name = "Threshold",
      .effect = EffectId::Compressor,
      .command = FramedCommand::CompressorParameters,
      .bodyOffset = 9,
      .encoding = ParameterEncoding::SignedByte,
      .minimum = -60,
      .maximum = 0,
      .unit = " dB" },
    { .id = Parameter::CompressorRatio,
      .name = "Ratio",
      .effect = EffectId::Compressor,
      .command = FramedCommand::CompressorParameters,
      .bodyOffset = 11,
      .encoding = ParameterEncoding::UnsignedByte,
      .minimum = 1,
      .maximum = 20,
      .unit = ":1" },
} };

static_assert(rowsInEnumeratorOrder(Parameters, [](ParameterInfo const& row) { return row.id; }),
              "Parameters must list every Parameter at its own enumerator's index");

/// The longest parameter body among the commands we build.
inline constexpr std::size_t MaxFramedBodySize = 22;

/// A parameterised command's body, as the vendor sends it.
///
/// The framed family has **no read-back**, so editing one field means starting
/// from a known-good body rather than from zeroes. These are the exact bodies the
/// captured init sequence sends, so after connect() a cache seeded from here and
/// the device agree -- which is the only reason a write-only surface can be
/// edited one field at a time at all.
struct FramedDefault
{
    FramedCommand command;                            ///< Which command this is the body for.
    std::uint8_t length;                              ///< Meaningful bytes in @ref body.
    std::array<std::uint8_t, MaxFramedBodySize> body; ///< The body, zero-padded.
};

/// Known-good bodies, one per parameterised command.
inline constexpr auto FramedDefaults = std::to_array<FramedDefault>({
    // init[25]: reverb, with the delay-effect type byte set to reverb.
    { .command = FramedCommand::DelayEffectParameters,
      .length = 21,
      .body = { 0x40, 0x9b, 0xff, 0x19, 0x40, 0x7f, 0x80, 0x66, 0xe5, 0x10, 0x00,
                0x4e, 0x04, 0x40, 0x01, 0x43, 0x05, 0xff, 0xca, 0x00, 0x10 } },
    // init[30]: the compressor, threshold -18 dB and ratio 12.
    { .command = FramedCommand::CompressorParameters,
      .length = 17,
      .body = { 0x00, 0xa0, 0x00, 0x10, 0x83, 0x0e, 0x83, 0x0e, 0x00, 0xee, 0x00,
                0x0c, 0xd6, 0x04, 0x31, 0x00, 0x1f } },
});

/// @param command The command to look up.
/// @return Its index in FramedDefaults, or nothing when it carries no parameters.
[[nodiscard]] constexpr std::optional<std::size_t> framedDefaultIndex(FramedCommand command) noexcept
{
    for (std::size_t index = 0; index < FramedDefaults.size(); ++index)
        if (FramedDefaults[index].command == command)
            return index;

    return std::nullopt;
}

static_assert(framedDefaultIndex(FramedCommand::DelayEffectParameters).has_value()
                  && framedDefaultIndex(FramedCommand::CompressorParameters).has_value(),
              "every command named in Parameters needs a default body to edit from");

/// @param parameter The parameter to describe.
/// @return Its row in Parameters.
[[nodiscard]] constexpr ParameterInfo const& describe(Parameter parameter) noexcept
{
    return Parameters[indexOf(parameter)];
}

/// One address or command and what it is called.
///
/// A pair table rather than an enumerator-indexed array, because both of these
/// enumerations are sparse: they name the handful of values the hardware has
/// confirmed out of a 256-entry space, and most of that space stays unnamed on
/// purpose.
struct WireName
{
    std::uint8_t value;    ///< The address or command byte.
    std::string_view name; ///< What to call it when printing a capture.
};

/// Names for the property addresses that have one.
inline constexpr auto PropertyNames = std::to_array<WireName>({
    { .value = 0x0f, .name = "DisplayPower" },
    { .value = 0x14, .name = "KnobLedModeAt14" },
    { .value = 0x15, .name = "SelectedMix (00 creator, 01 audience)" },
    { .value = 0x1d, .name = "SettingsTransaction (01 begin, 00 end)" },
    { .value = 0x1e, .name = "KnobLedBrightness" },
    { .value = 0x1f, .name = "MicGain" },
    { .value = 0x21, .name = "KnobLedSelect / mixer mode" },
    { .value = 0x27, .name = "creator mix levels (base; +track)" },
    { .value = 0x2a, .name = "creator System level" },
    { .value = 0x2b, .name = "creator Game level" },
    { .value = 0x2c, .name = "creator Chat level" },
    { .value = 0x31, .name = "audience System level" },
    { .value = 0x2e, .name = "audience mix levels (base; +track)" },
    { .value = 0x35, .name = "KnobPropertyAt35" },
});

/// Names for the framed commands that have one.
inline constexpr auto FramedCommandNames = std::to_array<WireName>({
    { .value = 0x85, .name = "DelayEffectEnable (reverb/echo)" },
    { .value = 0x87, .name = "EqualiserEnableA" },
    { .value = 0x88, .name = "EqualiserEnableB" },
    { .value = 0x94, .name = "DelayEffectParameters (byte 14: 1 reverb, 2 echo)" },
    { .value = 0x9c, .name = "EqualiserEnableC" },
    { .value = 0x9e, .name = "NoiseGateParameters" },
    { .value = 0x9b, .name = "CompressorEnable" },
    { .value = 0x9f, .name = "CompressorParameters" },
    { .value = 0xa3, .name = "EqualiserBand" },
    { .value = 0xb2, .name = "Commit" },
});

/// @param table One of the name tables above.
/// @param value The byte to look up.
/// @return Its name, or an empty view when it has none.
[[nodiscard]] constexpr std::string_view nameIn(std::span<WireName const> table,
                                                std::uint8_t value) noexcept
{
    for (auto const& entry: table)
        if (entry.value == value)
            return entry.name;

    return {};
}

/// @param payload A 0xfe-family command.
/// @return The checksum its last byte should carry.
///
/// Verified against all 24 such commands in the captured sequences.
[[nodiscard]] constexpr std::uint8_t framedChecksum(Payload const& payload) noexcept
{
    std::size_t const length = payload[2];
    unsigned sum = 0;
    for (std::size_t index = 2; index + 1 < length; ++index)
        sum += payload[index];

    return static_cast<std::uint8_t>(sum & 0xFF);
}

/// Event type byte identifying a screen touch in InputReport::event.
inline constexpr std::uint8_t ScreenTouchEventType = 0x10;

/// One screen chunk as hid_write() wants it: a report-id byte followed by the
/// 1024-byte report proper.
inline constexpr std::size_t ScreenChunkSize = 1025;

/// Bytes of header before the JPEG data in a screen chunk: report id, sequence
/// number, padding, payload length and checksum.
inline constexpr std::size_t ScreenChunkHeaderSize = 13;

/// Most JPEG bytes one chunk can carry, derived from the two sizes above.
inline constexpr std::size_t ScreenChunkPayloadSize = ScreenChunkSize - ScreenChunkHeaderSize;

/// Offset of the chunk sequence number within a screen chunk.
inline constexpr std::size_t ScreenChunkSequenceOffset = 1;

/// Offset of the two bytes that mark the last chunk of a frame.
///
/// Both carry 0x01 on the final chunk and 0x00 on every other one. Verified
/// across 49 of the 50 complete frames in the vendor capture -- the fiftieth is
/// truncated at the end of the capture, not a counter-example.
///
/// Load-bearing in the most frustrating way: without them the deck accepts every
/// chunk, reports no error, and simply never puts the frame on screen.
inline constexpr std::size_t ScreenChunkFinalOffset = 2;

/// How many bytes carry that marker.
inline constexpr std::size_t ScreenChunkFinalByteCount = 2;

/// What those bytes carry on the last chunk.
inline constexpr std::uint8_t ScreenChunkFinalMarker = 0x01;

/// Offset of the little-endian payload length within a screen chunk.
inline constexpr std::size_t ScreenChunkLengthOffset = 9;

/// Offset of the little-endian payload checksum within a screen chunk. The
/// checksum is the 16-bit unsigned sum of the chunk's JPEG bytes.
inline constexpr std::size_t ScreenChunkChecksumOffset = 11;

/// @param report The report as received.
/// @return The checksum byte 0x39 should carry.
///
/// Verified against 164 distinct reports from two independent captures -- the
/// vendor software's and our own -- with no exceptions.
[[nodiscard]] constexpr std::uint8_t checksumOf(std::span<std::uint8_t const> report) noexcept
{
    unsigned sum = 0;
    for (auto const byte: report.first(ChecksumOffset))
        sum += byte;

    return static_cast<std::uint8_t>(sum & 0xFF);
}

/// @param report The bytes as received, ControlReportSize of them.
/// @return Whether the checksum byte matches the rest.
[[nodiscard]] constexpr bool isChecksumValid(std::span<std::uint8_t const> report) noexcept
{
    return report.size() > ChecksumOffset && report[ChecksumOffset] == checksumOf(report);
}

/// How many meters the deck reports: one per track.
///
/// Each is a **stereo pair** -- left then right, both 16-bit big-endian -- so a
/// track's meter occupies four bytes and the block runs from AudioMetersOffset in
/// the deck's printed knob order.
///
/// Established with the vendor's software showing its per-track peak view, with
/// the microphone live and music on System and nothing else connected: the Mic
/// and System pairs moved and the other four sat at zero. The stereo layout shows
/// in the pairs themselves -- the microphone is mono, so its left and right agree
/// to within ten counts, while System carries stereo music and its two differ by
/// around a thousand.
///
/// Confirmed again from the host side, which is the cheaper check: playing a tone
/// into one of the per-track sinks reads 48% on that track and 0% on the other
/// five. `scripts/setup-audio.sh --identify` does exactly that.
///
/// The meters are **pre-fader**, so they identify a channel and cannot confirm a
/// level register -- a track reads the same at 0% as at 100%.
///
/// This file was wrong about the meters three times before that: one per playback
/// channel, then a single pre-fader stereo mix, then one per mix. Every one of
/// those came from reading only the first pair, which is the microphone -- and a
/// live microphone hears whatever is played into the room, so it appeared to
/// answer every track in turn.
inline constexpr std::size_t AudioMeterCount = KnobCount;

/// Bytes per track: a left and a right, each 16-bit.
inline constexpr std::size_t AudioMeterStride = 4;

/// Full scale for an audio meter, and the scale is linear: a tone at 0.7 of
/// full amplitude reads 0x5999, which is 70% of this.
inline constexpr int AudioLevelFullScale = 0x7fff;

/// @param high The more significant byte.
/// @param low The less significant byte.
/// @return The 16-bit value the deck encodes big-endian, unlike the screen
///         chunk header's little-endian fields.
[[nodiscard]] constexpr int decodeBigEndian16(std::uint8_t high, std::uint8_t low) noexcept
{
    return (int { high } << 8) | int { low };
}

/// @param raw A meter reading against AudioLevelFullScale.
/// @return It as a percentage of full scale.
[[nodiscard]] constexpr int toPercent(int raw) noexcept
{
    return (raw * 100) / AudioLevelFullScale;
}

/// @param low The less significant byte.
/// @param high The more significant byte.
/// @return The 16-bit value the deck encodes little-endian for touch positions.
[[nodiscard]] constexpr int decodeLittleEndian16(std::uint8_t low, std::uint8_t high) noexcept
{
    return (int { high } << 8) | int { low };
}

/// Offset of the six per-knob LED levels within the LED command payload.
inline constexpr std::size_t KnobLedLevelOffset = 4;

/// Highest LED level the hardware tracks (0x14).
inline constexpr int MaxKnobLedLevel = 20;

/// Bit representing each Button in PhysicalButtonEvent::buttons, indexed by Button.
inline constexpr std::array<std::uint8_t, ButtonCount> ButtonBits { 0x08, 0x04, 0x02, 0x01 };

/// Bit representing each knob in InputReport::knobPush and ::knobTouch, indexed by KnobId.
inline constexpr std::array<std::uint8_t, KnobCount> KnobBits { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20 };

/// Mask covering the four physical buttons, derived from the table above rather
/// than written out, so a fifth button is one more row and nothing else.
inline constexpr std::uint8_t PhysicalButtonMask = [] {
    std::uint8_t mask = 0;
    for (auto const bit: ButtonBits)
        mask = static_cast<std::uint8_t>(mask | bit);
    return mask;
}();

/// @param button The button to look up.
/// @param buttons The button bitmask from an input report.
/// @return Whether @p button is down in @p buttons.
[[nodiscard]] constexpr bool isDown(Button button, std::uint8_t buttons) noexcept
{
    return (buttons & ButtonBits[indexOf(button)]) != 0;
}

/// @param knob The knob to look up.
/// @param mask A knob bitmask from an input report (push or touch).
/// @return Whether @p knob's bit is set in @p mask.
[[nodiscard]] constexpr bool isSet(KnobId knob, std::uint8_t mask) noexcept
{
    return (mask & KnobBits[indexOf(knob)]) != 0;
}

// ScreenTouchEvent::flags is deliberately not decoded. It was read as a contact
// flag -- zero meaning "nothing is touching" -- and that was wrong in a way that
// cost the screen most of its usefulness: a finger held still reports zero for
// the whole press, and so does every two-finger gesture, so a stationary tap and
// all two-finger input produced no events at all. Only a single finger being
// dragged sets it non-zero, which is why dragging was the one thing that worked.
//
// Contact is signalled by the report TYPE, not by this byte. The reports start
// when the finger lands and stop when it lifts; the deck sends no release event
// of its own, and it suppresses its ordinary heartbeat for the whole touch --
// idle it reports 10 to 15 times a second, and eight seconds of dragging carried
// nine such reports in one run and none in another. That is why treating the
// first non-ordinary report as the lift works.
//
// **A held finger repeats, at about eleven reports a second.** Measured twice: 45
// consecutive reports across a 4.8-second hold, and later 228 reports from one
// deliberate motionless contact, the coordinate constant in both. A reading that
// said a stationary finger goes unreported was taken from single-report contacts
// assumed to be holds; they were taps, and it has been withdrawn.
//
// Values seen with a finger down: 0x00, 0x10, 0x14, 0x18, 0x1c, 0x48, 0x49.
//
// What it is: a value that is fixed per contact and only ever counts UP. Every
// contact begins at 0x00, and across roughly 1400 measured transitions not one
// went downward and none returned to 0x00. 0x00 is the not-moving state -- it
// carried movement in 3 of 36 reports where the others carried it in 76 to 100
// per cent of theirs.
//
// What it is not, each measured and refuted rather than argued away:
//   * a counter -- a counter cycles, and this latches;
//   * an accumulator of distance or time -- one contact flipped after 28 px and
//     471 ms, another after 17 px and 47 ms, so no threshold in either unit fires
//     at both;
//   * a magnitude -- the slow gesture settled at 0x1c and the fast one at 0x14,
//     and 0x14 carried a mean step of 40 px in one run and 2 px in another;
//   * a gesture class -- five deliberate flicks and five deliberate press-drags
//     produced no 0x14 at all;
//   * finger count -- the panel does not track more than one finger, so 0x48 and
//     0x49 are not that either.
//
// Four readings have now died. The bit that separates 0x14 from 0x1c is 0x08 and
// nobody knows what it means; it is left alone rather than given a fifth guess.

/// Trims what the transport delivered down to a report, or rejects it.
///
/// hidapi prepends the report id when the device numbers its reports and does
/// not when it does not, and this deck sends two lengths. Both cases are handled
/// here rather than at each call site, so a tool reading the deck directly and
/// the driver reading it through Device cannot disagree about where a report
/// starts.
///
/// @param raw Exactly the bytes the transport returned.
/// @return The report, or an empty span if these bytes are not one.
[[nodiscard]] constexpr std::span<std::uint8_t const> reportPayload(
    std::span<std::uint8_t const> raw) noexcept
{
    if (raw.size() == PaddedReportSize + 1)
        raw = raw.subspan(1);

    if (raw.size() != ControlReportSize && raw.size() != PaddedReportSize)
        return {};

    return raw;
}

/// Reads an input report out of the bytes the transport delivered.
///
/// @param bytes ControlReportSize bytes as received.
/// @return What the driver understands of them.
[[nodiscard]] constexpr InputReport decodeReport(std::span<std::uint8_t const> bytes) noexcept
{
    InputReport report {};

    report.isScreenTouch = bytes[EventTypeOffset] == ScreenTouchEventType;
    if (report.isScreenTouch)
    {
        report.touchFlags = bytes[TouchFlagsOffset];
        report.touchX = decodeLittleEndian16(bytes[TouchXOffset], bytes[TouchXOffset + 1]);
        report.touchY = decodeLittleEndian16(bytes[TouchYOffset], bytes[TouchYOffset + 1]);
    }
    else
        report.buttons = static_cast<std::uint8_t>(bytes[ButtonsOffset] & PhysicalButtonMask);

    report.knobPush = bytes[KnobPushOffset];
    report.knobTouch = bytes[KnobTouchOffset];

    for (std::size_t knob = 0; knob < KnobCount; ++knob)
        report.knobValues[knob] = bytes[KnobValuesOffset + knob];

    for (std::size_t channel = 0; channel < AudioMeterCount; ++channel)
    {
        auto const offset = AudioMetersOffset + (channel * AudioMeterStride);

        // The louder side of the pair. A peak meter answers "how close is this to
        // clipping", and that is whichever channel is loudest -- averaging would
        // hide a hard-panned transient, which is exactly what a meter is for.
        report.audioMeters[channel] = std::max(decodeBigEndian16(bytes[offset], bytes[offset + 1]),
                                               decodeBigEndian16(bytes[offset + 2], bytes[offset + 3]));
    }

    return report;
}

} // namespace ax310::protocol
