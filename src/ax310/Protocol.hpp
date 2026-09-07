// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Enumerators.hpp"
#include "Types.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

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

/// Second byte of a command: which group of things it addresses.
///
/// `0x10` is the properties -- the mixer levels, the LED registers, everything
/// with a name in PropertyNames -- and it was long taken for the only group there
/// was. It is not. The captured sequences use six more, and the display's
/// brightness turned out to live in one of them:
///
///   | group  | what it is |
///   | ------ | ---------- |
///   | `0x01` | the firmware version, see IdentityGroup |
///   | `0x03` | written once by init, `01 03 01`; unknown |
///   | `0x04` | written twice by shutdown, `01 04 01`; unknown |
///   | `0x09` | written by both, `01 09 02`; unknown |
///   | `0x0a` | the display: brightness and blanking, see DisplayGroup |
///   | `0x10` | properties |
///   | `0xa0` | the serial number, see SerialGroup |
///
/// The four bytes read the same way in every group -- kind, group, address,
/// length, then that many values -- so all of them go through commandAt(). What
/// an address *means* is the group's own business, and the *replies* do not all
/// share the layout: a property answers with its address and length echoed back,
/// group `0x01` runs straight into data, and group `0xa0` echoes four bytes first.
///
/// A rule that applies to every read: **the deck does not clear its reply buffer.**
/// A short answer is followed by whatever the previous answer left there, so a
/// caller must take exactly the length it asked for and no more. This is why the
/// probe tool reads `0x0f` between interesting registers -- that answers zeroes,
/// which makes a stale tail obvious instead of plausible.
inline constexpr std::uint8_t PropertyGroup = 0x10;

/// The group that answers with the deck's firmware version.
///
/// The first command the vendor software sends, before anything else: a read of
/// group `0x01` with a zero address and zero length. What comes back is a fixed
/// block, decoded by matching it against the version string Creator Central was
/// showing for the same deck at the same time.
inline constexpr std::uint8_t IdentityGroup = 0x01;

/// The group that answers with the deck's serial number, as ASCII digits.
inline constexpr std::uint8_t SerialGroup = 0xa0;

/// Where the values start in a reply from IdentityGroup.
///
/// The reply echoes the kind and the group and then runs straight into data --
/// there is no address or length in between, unlike a property reply.
inline constexpr std::size_t IdentityValuesOffset = 2;

/// Where the ASCII starts in a reply from SerialGroup, which does echo four bytes.
inline constexpr std::size_t SerialValuesOffset = 4;

/// The deck's firmware version, as Creator Central displays it.
///
/// Laid out from one device and one capture, so the offsets are observed rather
/// than specified. Every field was confirmed against the string the vendor showed
/// for that deck: `1.5 10.53 ( 24011113 / 23122210 / 22051216 / a5 / 57 )`.
///
/// The vendor prints the build stamps and both version numbers as **decimal**
/// renderings of each byte -- `0x18` shows as `24` -- and the two trailing codes
/// as **hexadecimal**. That inconsistency is the vendor's; this keeps the bytes.
struct FirmwareVersion
{
    std::array<std::uint8_t, 2> version {};       ///< `01 05`, shown as 1.5.
    std::array<std::uint8_t, 2> secondVersion {}; ///< `0a 35`, shown as 10.53.
    std::array<std::uint8_t, 4> buildA {};        ///< `18 01 0b 0d`, shown as 24011113.
    std::array<std::uint8_t, 4> buildB {};
    std::array<std::uint8_t, 4> buildC {};
    std::uint8_t codeA {};                        ///< `a5`, shown as hex.
    std::uint8_t codeB {};                        ///< `57`, shown as hex.
};

/// @param reply A reply to a read of IdentityGroup.
/// @return What it says, or nothing if it is not such a reply.
[[nodiscard]] constexpr std::optional<FirmwareVersion> parseFirmwareVersion(
    std::span<std::uint8_t const> reply) noexcept
{
    constexpr std::size_t Needed = IdentityValuesOffset + 19;
    if (reply.size() < Needed || reply[0] != std::to_underlying(CommandKind::Get)
        || reply[1] != IdentityGroup)
        return std::nullopt;

    auto const at = [reply](std::size_t index) {
        return reply[IdentityValuesOffset + index];
    };

    return FirmwareVersion {
        .version = { at(12), at(13) },
        .secondVersion = { at(14), at(15) },
        .buildA = { at(0), at(1), at(2), at(3) },
        .buildB = { at(4), at(5), at(6), at(7) },
        .buildC = { at(8), at(9), at(10), at(11) },
        // at(16) is 0x03 on the one deck seen and is not part of what the vendor
        // displays, so it is read past rather than guessed at.
        .codeA = at(17),
        .codeB = at(18),
    };
}

/// @param reply A reply to a read of SerialGroup.
/// @return The ASCII digits, or an empty span if it is not such a reply.
///
/// The run is delimited by the first byte that is not a digit, because the deck
/// leaves the previous reply behind whatever it writes and the tail is somebody
/// else's answer rather than padding.
[[nodiscard]] constexpr std::span<std::uint8_t const> parseSerialNumber(
    std::span<std::uint8_t const> reply) noexcept
{
    if (reply.size() <= SerialValuesOffset || reply[0] != std::to_underlying(CommandKind::Get)
        || reply[1] != SerialGroup)
        return {};

    auto const digits = reply.subspan(SerialValuesOffset);
    std::size_t length = 0;
    while (length < digits.size() && digits[length] >= '0' && digits[length] <= '9')
        ++length;

    return digits.first(length);
}

/// Builds any command in the four-byte grammar every group shares.
///
/// @param kind Read or write.
/// @param group Which group the address belongs to; see PropertyGroup.
/// @param address What to address within it.
/// @param values The values to write, empty for a read.
/// @param length The length byte, when it is not simply the number of values --
///        a read states how much it expects back and carries no values.
/// @return The full 64-byte payload.
[[nodiscard]] constexpr Payload commandAt(CommandKind kind, std::uint8_t group,
                                          std::uint8_t address,
                                          std::span<std::uint8_t const> values,
                                          std::size_t length = 0) noexcept
{
    Payload payload {};
    payload[0] = std::to_underlying(kind);
    payload[1] = group;
    payload[2] = address;
    payload[3] = static_cast<std::uint8_t>(values.empty() ? length : values.size());
    std::ranges::copy(values, std::next(payload.begin(), 4));
    return payload;
}

/// @return The command that asks the deck for its firmware version.
///
/// Address and length are both zero: this group's reply is a fixed block and the
/// request carries nothing to select within it.
[[nodiscard]] constexpr Payload identityRequest() noexcept
{
    return commandAt(CommandKind::Get, IdentityGroup, 0x00, {}, 0);
}

/// @return The command that asks the deck for its serial number.
///
/// Length one, which is what the vendor asks for and is not the length of the
/// answer -- thirteen digits come back regardless.
[[nodiscard]] constexpr Payload serialRequest() noexcept
{
    return commandAt(CommandKind::Get, SerialGroup, 0x00, {}, 1);
}

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
///   * **bytes 6 and 7 are not understood, and they are not inert.** Two records
///     with the same button and the same colour, captured minutes apart, differ
///     in them, so they are not a checksum of this record and not derived from
///     its contents. They are not passive either: on the hardware, the same
///     selector carrying the same colour `00 37 ff` lights the button with `00
///     1d` in that pair and leaves it dark with `f9 3d`, while `ff 00 00` lights
///     under both. So the pair decides whether a given colour shows at all.
///     `0xf9` there sits beside the `0xf8` a knob record carries in byte 6 and
///     the `0xf8` a surround record carries in byte 4; whether those are one
///     field is untested, and none of the three is explained.
///
/// There is no brightness field. The vendor scales the colour host-side and sends
/// the result: its slider at minimum sent `0x19` on the lit channel and at
/// maximum `0xff`. `0x19` is 25, the same floor its panel-brightness slider uses.
///
/// Which selector is which physical button is settled, by driving all four at once
/// to four colours nobody could confuse and reading the deck: see
/// ButtonColourSelectors. The input direction is settled too, and by this one --
/// lighting a single button is what identifies the press that follows it. See
/// ButtonBits.
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
/// Confirmed against the deck rather than inferred from the vendor's labels: all
/// four were driven in one pass to red, green, blue and yellow, and each colour
/// appeared under the button this table names. One pass rather than four, so the
/// answer cannot be an artefact of writes landing in the wrong order.
///
/// This settles the output direction, and it is what settled the input direction
/// as well: `ax310_probe --inputs` lights one button at a time and reads the byte
/// that arrives, so the press is identified by the light rather than by the table
/// being checked. See ButtonBits.
inline constexpr std::array<std::uint8_t, ButtonCount> ButtonColourSelectors {
    0x3c, // TopLeft
    0x3d, // TopRight
    0x3f, // BottomLeft
    0x3e, // BottomRight
};
// ButtonColourSelectors is indexed by Button and its length is tied to
// ButtonCount, so a missing row will not compile. Nothing can check the order:
// the selectors are opaque bytes, and only the hardware knows which is which.

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

/// One light's colour, at full brightness. Buttons and knob rings alike.
struct LightColour
{
    std::uint8_t red {};
    std::uint8_t green {};
    std::uint8_t blue {};
};

/// How brightly the function buttons come up when nothing has chosen otherwise.
///
/// Picked at the deck rather than derived: every channel at full is glaring on a
/// desk, and the hardware's floor of MinLightChannel is hard to see in a lit
/// room. One number, so there is one thing to change.
inline constexpr int DefaultButtonBrightnessPercent = 75;

/// What the four buttons are lit with when nothing has chosen otherwise, indexed
/// by Button and stated at full brightness -- scaledChannel applies the level.
///
/// One warm family rather than four unrelated hues, so the row reads as part of
/// the same instrument. They are spread as far apart within it as warm colours
/// allow, because they have to be told apart at a glance and a narrower spread
/// was tried on the hardware first: an amber and a gold two steps apart were one
/// light to the eye.
inline constexpr std::array<LightColour, ButtonCount> DefaultButtonColours {
    LightColour { .red = 0xff, .green = 0x7a, .blue = 0x00 }, // TopLeft, amber
    LightColour { .red = 0xff, .green = 0x60, .blue = 0x50 }, // TopRight, coral
    LightColour { .red = 0xff, .green = 0xe0, .blue = 0x1a }, // BottomLeft, gold
    LightColour { .red = 0xc0, .green = 0x18, .blue = 0x20 }, // BottomRight, deep red
};
// Indexed by Button and tied to ButtonCount, so a button without a colour will
// not compile. Which colour suits which button is a matter of taste and nothing
// can check it; that all four differ is checked in the tests.

/// The knob rings take one colour for all six, at the button address.
///
/// The same `0xc0` the buttons use, with byte 0 selecting which bank of lights the
/// record addresses: `0x00` a function button, `0x01` the knob rings. Bytes 1 and 2
/// then read as a first index and a count -- one light at `0x3c` for a button, ten
/// from `0xc0` for the rings -- which is a reading that fits every record captured
/// and has not been tested by writing anything else.
inline constexpr std::uint8_t ButtonBank = 0x00;
inline constexpr std::uint8_t KnobBank = 0x01;
inline constexpr std::uint8_t KnobFirstLight = 0xc0;
inline constexpr std::uint8_t KnobLightCount = 0x0a;

/// Builds the knob-ring colour record.
///
/// **The mix is not in the record.** The vendor offers a separate colour for the
/// creator and audience mixes, and setting either sends this same command: captures
/// of "red on the creator mix" and "red on the audience mix" are byte-identical.
/// The deck applies the colour to whichever mix is selected, so a caller wanting
/// the other mix's colour must select that mix first -- the colour cannot be aimed.
///
/// The vendor also writes `Property::KnobLedBrightness` immediately before this,
/// every time, with the value already in the register. Replaying it is harmless and
/// this does not, because nothing has shown the colour depends on it.
///
/// @param red Red, 0 to 255.
/// @param green Green.
/// @param blue Blue.
/// @return The ten values to write to ButtonColourAddress.
[[nodiscard]] constexpr std::array<std::uint8_t, 10> knobColourRecord(
    std::uint8_t red, std::uint8_t green, std::uint8_t blue) noexcept
{
    return { KnobBank, KnobFirstLight, KnobLightCount, red, green, blue,
             0xf8,     0x00,           ButtonLit,      0x80 };
}

/// What the knob rings are lit with in each mix, indexed by MixId.
///
/// The rings are how the deck itself says which mix is live: blue for the creator
/// mix, orange for the audience mix. Both values are the vendor's, and they are
/// where the interface's own creator and audience colours come from -- so the
/// window and the hardware say the same thing in the same language.
///
/// These have to be written on every switch. The ring record carries no mix at
/// all, so the deck colours whichever mix is selected, and a switch that does not
/// write the new mix's colour leaves the rings showing the old one's.
inline constexpr std::array<LightColour, MixCount> MixRingColours {
    LightColour { .red = 0x00, .green = 0x7d, .blue = 0xff }, // Creator, blue
    LightColour { .red = 0xff, .green = 0x7d, .blue = 0x00 }, // Audience, orange
};

/// What Property::KnobLedSelect carries for each mix, indexed by MixId.
///
/// Captured from three of the vendor's actions, which together read this address
/// as carrying the mixer mode and the monitored mix in one byte: `0x80` a single
/// creator mix, `0x01` a single audience mix, `0x00` Dual Mix. The init sequence
/// writes `0x80`, which is why a freshly attached deck monitors the creator mix.
///
/// Writing SelectedMix alone moves the audio and leaves the rings behind; this is
/// the half that moves them.
inline constexpr std::array<std::uint8_t, MixCount> KnobLedSelectForMix { 0x80, 0x01 };

/// Where the surround light strip is configured.
///
/// The second record address, and the last one that was unaccounted for. Nothing
/// restores it on connect, so whatever the strip was last told survives a
/// reconnect -- including a black Solid, which looks exactly like a strip that
/// does not work.
inline constexpr std::uint8_t SurroundAddress = 0xe0;

/// The wire value for each mode, indexed by SurroundMode.
///
/// Consecutive in steps of four rather than of one, which is why these are a table
/// and not arithmetic on the enumerator. What the low two bits are for is unknown;
/// every captured record has them clear.
inline constexpr std::array<std::uint8_t, SurroundModeCount> SurroundModeSelectors {
    0x34, // Solid
    0x38, // Pulsing
    0x2c, // Blinking
    0x28, // PulsingRgb
    0x30, // BlinkingRgb
    0x24, // ScrollingRgb
};
// As with the button selectors: the length is tied to the enumeration, and the
// order is a claim about hardware that no assertion can settle.

/// @param mode Which mode.
/// @return The selector byte its record carries.
[[nodiscard]] constexpr std::uint8_t selectorFor(SurroundMode mode) noexcept
{
    return SurroundModeSelectors[indexOf(mode)];
}

/// The extremes the vendor's frequency slider reached, in the animated modes.
///
/// Observed, not proven to be the limits: the slider was taken to each end and
/// these are what came out. Nothing has tried a value outside them.
inline constexpr std::uint8_t MinSurroundFrequency = 0x01;
inline constexpr std::uint8_t MaxSurroundFrequency = 0x0a;

/// What the vendor's software leaves in the frequency byte in Solid.
///
/// `0x7d`, `0xcd`, `0xf8` and `0xfb` have all been seen there, changing mid-drag
/// with nothing else written. What the vendor puts in it tracks neither of the two
/// things it plausibly could: both ends of the brightness slider sent `0xfb`, and
/// so did all ten colour presets.
///
/// **That is a fact about the vendor's software, not about the deck.** Driving the
/// byte by hand produces colour effects on the strip that have not been
/// characterised, so the deck does read it -- the vendor simply never varies it in
/// a way a capture could show. A capture can only ever show the first of those.
///
/// This value is one of the resting ones, sent because replaying an observed shape
/// is the rule this project writes under. See docs/todo.md, and
/// .agent/rules/hardware-facts.md for the readings this byte has survived.
inline constexpr std::uint8_t SolidFrequencyAtRest = 0xf8;

/// The range a colour channel spans as the vendor's brightness slider moves.
///
/// The deck has no brightness field anywhere: brightness is applied to the colour
/// before it is sent, on all three kinds of light. `0x19` is the floor for the
/// function buttons and for the strip alike, in solid and in pulsing.
inline constexpr std::uint8_t MinLightChannel = 0x19;
inline constexpr std::uint8_t MaxLightChannel = 0xff;

/// Applies a brightness to one colour channel the way the vendor does.
///
/// Straight-line between the two ends, which fits the three points measured
/// (`0x19` at the bottom, `0xff` at the top, and roughly `0x85` at a slider left
/// near the middle by hand). A hand-placed midpoint cannot distinguish a line from
/// a gentle curve, so this is the simplest reading of the evidence rather than a
/// proven encoding.
///
/// @param channel The channel at full brightness, 0 to 255.
/// @param percent Brightness, 0 to 100.
/// @return The channel to send.
[[nodiscard]] constexpr std::uint8_t scaledChannel(std::uint8_t channel, int percent) noexcept
{
    if (channel == 0)
        return 0; // An unlit channel stays unlit; the floor is not a colour shift.

    auto const clamped = std::clamp(percent, 0, 100);
    auto const span = int { MaxLightChannel } - int { MinLightChannel };
    auto const ceiling = int { MinLightChannel } + ((span * clamped) / 100);
    return static_cast<std::uint8_t>((int { channel } * ceiling) / int { MaxLightChannel });
}

/// Builds one surround-strip record.
///
/// The layout, from thirteen captures covering all seven of the vendor's modes and
/// both ends of its colour, brightness and frequency controls:
///
///     01 <mode> 01 20 <rate> 00 00 <r> <g> <b>
///
/// Byte 4 is the frequency, and **only** in the modes that animate. Holding the
/// frequency slider still while dragging brightness from one end to the other left
/// it at `0x0a` across every record, so nothing else rides in it.
///
/// **There is no brightness field.** Brightness is applied to the colour before it
/// is sent -- `0xff` down to `0x19` as the slider travels -- in solid exactly as in
/// pulsing, and on the buttons and rings the same way. Use scaledChannel().
///
/// In Solid the byte carries something else again -- see SolidFrequencyAtRest --
/// and it is not a brightness: at both ends of the vendor's slider it was `0xfb`
/// while the colour travelled the whole way.
///
/// @param mode Which animation.
/// @param frequency How fast it animates. In Solid the deck does something else
///        with this byte that is not yet characterised; pass SolidFrequencyAtRest
///        there unless deliberately exploring it.
/// The channel order is red, green, blue, confirmed by driving `ff 00 00` at the
/// strip and looking at it. Nothing in the captures could settle it: the vendor's
/// ten presets are a hue wheel, and a hue wheel read backwards is still one.
///
/// @param red Red, 0 to 255. Ignored by the deck in the hue-cycling modes.
/// @param green Green.
/// @param blue Blue.
/// @return The ten values to write to SurroundAddress.
[[nodiscard]] constexpr std::array<std::uint8_t, 10> surroundRecord(
    SurroundMode mode, std::uint8_t frequency, std::uint8_t red, std::uint8_t green,
    std::uint8_t blue) noexcept
{
    return { 0x01, selectorFor(mode), 0x01, 0x20, frequency, 0x00, 0x00, red, green, blue };
}

/// Builds the record that turns the strip off.
///
/// Solid and black, because that is what the vendor's "off" mode sends -- there is
/// no mode byte for darkness.
///
/// @return The ten values to write to SurroundAddress.
[[nodiscard]] constexpr std::array<std::uint8_t, 10> surroundOffRecord() noexcept
{
    return surroundRecord(SurroundMode::Solid, SolidFrequencyAtRest, 0x00, 0x00, 0x00);
}

/// The display command group: `[0x01][0x0a][level]`, and that is the whole of it.
///
/// A second family beside the property one, which is why the screen's brightness
/// is not among the property addresses. `0x1e` is the nearest-looking candidate
/// there and dims the knob rings instead, established on hardware.
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

/// @return The command that asks the display group for its current state.
///
/// The answer puts the panel's brightness back in the *address* slot, which is
/// the same place a write puts it -- captured answering `0x64` while the vendor
/// was showing 100%.
[[nodiscard]] constexpr Payload displayRequest() noexcept
{
    return commandAt(CommandKind::Get, DisplayGroup, 0x00, {}, 0);
}

/// Builds a display command.
/// @param level A percentage, or PanelOffLevel.
/// @return The payload to frame and send.
[[nodiscard]] constexpr Payload displayCommand(std::uint8_t level) noexcept
{
    // The level goes in the address slot, not a value slot, and the length stays
    // zero. Read in the grammar every group shares, this group takes its argument
    // as the thing it addresses -- which is why the command is three bytes with
    // no room for a value. The captured init writes 0xaa here, outside the
    // brightness range and not the off sentinel; see Commands.hpp.
    return commandAt(CommandKind::Set, DisplayGroup, level, {});
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
    /// The knob LED rings display this block. They show whichever mix the deck is
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

    /// Which knobs light at all, and which mix their rings display. Init writes
    /// 0x80; writing 0x10 leaves only the first knob's ring lit, so it selects
    /// rather than scales. The encoding is not worked out -- a six-knob mask would
    /// not be 0x80.
    ///
    /// The name is too narrow, and the second meaning is the load-bearing one. The
    /// vendor writes 0x80 for a single creator mix, 0x01 for a single audience
    /// mix, and 0x00 for Dual Mix, always alongside 0x22. Confirmed on the
    /// hardware: writing 0x80 or 0x01 here is what moves the levels the rings
    /// display, and SelectedMix on its own does not. See KnobLedSelectForMix.
    ///
    /// 0x22 is written by the vendor beside it and its meaning is unrecorded. This
    /// driver does not write it, and the rings follow the mix without it.
    KnobLedSelect = 0x21,

    /// What the Line Out socket carries: see LineOutSource.
    ///
    /// Captured from the vendor's Audio Output pane, cycling the dropdown through
    /// all three entries and back to the first, which reproduced the first value
    /// exactly. Init writes `0x01` -- the audience mix -- which is one more piece
    /// of somebody's settings rather than anything a deck needs to start.
    ///
    /// One hardware observation sits oddly beside this and is kept rather than
    /// explained away: writing `0x00` here was once seen to light every ring.
    /// Nothing has reproduced it since. A line-out source of "creator mix"
    /// plausibly changes what the rings display, and against a dropdown that
    /// writes exactly these three values it is the weaker of the two readings --
    /// but it was observed, so it is written down.
    LineOutSource = 0x14,

    /// Microphone input configuration, as a bitfield.
    ///
    /// Four captures pin two bits and leave two set in all of them:
    ///
    ///   | bit  | meaning |
    ///   | 0    | phantom power: XLR sends `0x0e`, XLR + 48V sends `0x0f` |
    ///   | 3    | the chat mic takes the microphone **without** effects |
    ///   | 1, 2 | set in every capture; unknown |
    ///
    /// XLR and 6.3 mm are indistinguishable here -- both send `0x0e` -- so the
    /// deck senses the connector rather than being told about it.
    ///
    /// Bit 3's polarity reads backwards until it is named for what it does: the
    /// vendor's "Mic with effects" clears it and "Mic without effects" sets it,
    /// so it is a bypass rather than an enable.
    MicConfiguration = 0x20,

    /// Headphone output volume, `0x00` to `0x14`.
    ///
    /// The same 21 steps the mixer levels use. Captured by dragging the vendor's
    /// headphone slider to each end. Init reads this address twice and writes
    /// `0x11` to it, which is 17 of 20.
    HeadphoneVolume = 0x3c,

    /// Line Out volume, `0x00` to `0x14`, laid out exactly like HeadphoneVolume.
    ///
    /// Nothing to do with the button-colour selector that shares this number: a
    /// selector is a byte *inside* a record written to `0xc0`, and this is an
    /// address in the property space.
    LineOutVolume = 0x3d,

    /// Base of the **audience mix's** six per-track levels, laid out exactly like
    /// CreatorMixLevels: `base + track`, same knob order. This is the mix the
    /// stream captures; muting a track here leaves the headphones untouched.
    ///
    /// Confirmed against the hardware: writing `0x31`, which is this base plus
    /// System, silences a System tone in the mix on the deck's second capture pair
    /// and leaves the first pair untouched. So the deck really does carry two
    /// independent six-track mixes.
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
    /// creator mix. Only that one direction was captured; the write for audience
    /// back to creator is inferred, and both directions are confirmed on the
    /// hardware through Device::selectMix.
    ///
    /// **This address alone moves the audio and nothing else.** The knob rings
    /// keep the colour and the levels of the mix that was on before, which looks
    /// exactly like a switch that did not happen. KnobLedSelect and a ring-colour
    /// record are the other two thirds of a switch; see MixRingColours.
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
    return commandAt(CommandKind::Set, PropertyGroup, address, values);
}

/// @param address The address to write.
/// @param value Its new value, when that is a single byte -- which most are.
/// @return The full 64-byte payload.
[[nodiscard]] constexpr Payload setPropertyAt(std::uint8_t address, std::uint8_t value) noexcept
{
    return setPropertyAt(address, std::span { &value, 1 });
}

/// @param address The address to read, named or not.
/// @param length How many bytes the answer is expected to carry.
/// @return The full 64-byte payload.
[[nodiscard]] constexpr Payload getPropertyAt(std::uint8_t address, std::size_t length) noexcept
{
    return commandAt(CommandKind::Get, PropertyGroup, address, {}, length);
}

/// @param property The property to write.
/// @param values Its new value, one byte per element.
/// @return The full 64-byte payload, ready to be framed and sent.
[[nodiscard]] constexpr Payload setProperty(Property property, std::span<std::uint8_t const> values) noexcept
{
    return setPropertyAt(std::to_underlying(property), values);
}

/// @param property The property to write.
/// @param value Its new value, when that is a single byte -- which most are.
/// @return The full 64-byte payload.
[[nodiscard]] constexpr Payload setProperty(Property property, std::uint8_t value) noexcept
{
    return setPropertyAt(std::to_underlying(property), value);
}

/// @param property The property to read.
/// @param length How many bytes the answer is expected to carry.
/// @return The full 64-byte payload.
[[nodiscard]] constexpr Payload getProperty(Property property, std::size_t length) noexcept
{
    return getPropertyAt(std::to_underlying(property), length);
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
    return static_cast<std::uint8_t>(std::to_underlying(block) + indexOf(knob));
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

    if (reply[ReplyKindOffset] != std::to_underlying(CommandKind::Get))
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
    Echo = 4
};

/// Number of effects, derived from the enumeration rather than stated.
inline constexpr std::size_t EffectCount = enumerators::denseEnumeratorCount<EffectId>();

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
    CompressorRatio = 6
};

/// Number of parameters, derived from the enumeration rather than stated.
inline constexpr std::size_t ParameterCount = enumerators::denseEnumeratorCount<Parameter>();

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
template <typename Enum>
struct WireName
{
    Enum value;            ///< The address or command, as its enumerator.
    std::string_view name; ///< What to call it when printing a capture.
};

/// Templated on the enumeration rather than on the value.
///
/// Holding the enumerator instead of a byte is what keeps the wire number in one
/// place: a row can only name something the enumeration already defines, so a
/// table and an enum cannot disagree about an address. Templating on the *value*
/// instead would make every row its own type, which would take these out of an
/// array and out of reach of nameIn().
///
/// A few addresses have no enumerator -- the per-track level slots are a base
/// plus an offset, not names -- and those rows cast the computed address rather
/// than writing a literal, so the arithmetic stays the single source.

/// Names for the property addresses that have one.
inline constexpr auto PropertyNames = std::to_array<WireName<Property>>({
    { .value = Property::DisplayPower, .name = "DisplayPower" },
    { .value = Property::LineOutSource, .name = "LineOutSource (00 creator, 01 audience, 02 chat mic)" },
    { .value = Property::SelectedMix, .name = "SelectedMix (00 creator, 01 audience)" },
    { .value = Property::SettingsTransaction, .name = "SettingsTransaction (01 begin, 00 end)" },
    { .value = Property::KnobLedBrightness, .name = "KnobLedBrightness" },
    { .value = Property::MicGain, .name = "MicGain" },
    { .value = Property::MicConfiguration, .name = "MicConfiguration (bit 0 phantom, bit 3 effects bypass)" },
    { .value = Property::KnobLedSelect, .name = "KnobLedSelect / mixer mode" },
    { .value = Property::CreatorMixLevels, .name = "creator mix levels (base; +track)" },
    { .value = Property::AudienceMixLevels, .name = "audience mix levels (base; +track)" },
    { .value = Property::KnobPropertyAt35, .name = "KnobPropertyAt35" },
    { .value = Property::HeadphoneVolume, .name = "HeadphoneVolume" },
    { .value = Property::LineOutVolume, .name = "LineOutVolume" },

    // Slots inside the two level blocks, which are a base plus a track index and
    // so have no enumerators of their own. Computed rather than written out, so
    // the arithmetic in levelAddressOf() stays the only place the sum is made.
    // Only the four the captures happened to exercise are named; the other eight
    // are equally addressable and equally nameable if anyone wants them.
    { .value = static_cast<Property>(levelAddressOf(Property::CreatorMixLevels, KnobId::System)),
      .name = "creator System level" },
    { .value = static_cast<Property>(levelAddressOf(Property::CreatorMixLevels, KnobId::Game)),
      .name = "creator Game level" },
    { .value = static_cast<Property>(levelAddressOf(Property::CreatorMixLevels, KnobId::Chat)),
      .name = "creator Chat level" },
    { .value = static_cast<Property>(levelAddressOf(Property::AudienceMixLevels, KnobId::System)),
      .name = "audience System level" },
});

// A duplicated row would shadow the later one and never be noticed, because the
// lookup stops at the first match and both would name something plausible.


/// How often the deck's panel may be redrawn, in frames per second.
///
/// Capped rather than merely defaulted. Every frame is an 800x480 image encoded
/// to JPEG on the host and pushed over USB in chunks, so a number typed by
/// somebody who does not know that costs a core and a share of the bus for no
/// visible gain -- the panel cannot show more than the ceiling here. The floor is
/// where the interface stops being usable rather than where it stops being cheap.
///
/// Configuration passes through clampPanelFps, so no stored value can exceed it.
inline constexpr int MinPanelFps = 1;
inline constexpr int MaxPanelFps = 60;
inline constexpr int DefaultPanelFps = 30;

/// @param fps A frame rate somebody asked for.
/// @return That rate, brought inside what this driver will send.
[[nodiscard]] constexpr int clampPanelFps(int fps) noexcept
{
    return std::clamp(fps, MinPanelFps, MaxPanelFps);
}

/// Every framed command that switches an effect on or off.
///
/// A table rather than five calls at the one call site, so switching the chain
/// off is a loop and a newly identified enable is one row. The noise gate is
/// absent because no enable has been found for it -- its parameter blocks are all
/// that has ever been captured.
inline constexpr std::array<FramedCommand, 5> EffectEnables {
    FramedCommand::DelayEffectEnable, FramedCommand::CompressorEnable,
    FramedCommand::EqualiserEnableA,  FramedCommand::EqualiserEnableB,
    FramedCommand::EqualiserEnableC,
};

/// Names for the framed commands that have one.
inline constexpr auto FramedCommandNames = std::to_array<WireName<FramedCommand>>({
    { .value = FramedCommand::DelayEffectEnable, .name = "DelayEffectEnable (reverb/echo)" },
    { .value = FramedCommand::EqualiserEnableA, .name = "EqualiserEnableA" },
    { .value = FramedCommand::EqualiserEnableB, .name = "EqualiserEnableB" },
    { .value = FramedCommand::DelayEffectParameters, .name = "DelayEffectParameters (byte 14: 1 reverb, 2 echo)" },
    { .value = FramedCommand::EqualiserEnableC, .name = "EqualiserEnableC" },
    { .value = FramedCommand::NoiseGateParameters, .name = "NoiseGateParameters" },
    { .value = FramedCommand::CompressorEnable, .name = "CompressorEnable" },
    { .value = FramedCommand::CompressorParameters, .name = "CompressorParameters" },
    { .value = FramedCommand::EqualiserBand, .name = "EqualiserBand" },
    { .value = FramedCommand::Commit, .name = "Commit" },
});



/// @param table One of the name tables above.
/// @param value The byte to look up.
/// @return Its name, or an empty view when it has none.
template <typename Enum, std::size_t N>
[[nodiscard]] constexpr std::string_view nameIn(std::array<WireName<Enum>, N> const& table,
                                                std::uint8_t value) noexcept
{
    for (auto const& entry: table)
        if (std::to_underlying(entry.value) == value)
            return entry.name;

    return {};
}

/// @param table A name table.
/// @return Whether every enumerator of its enumeration has a row.
///
/// The direction the typed rows cannot cover on their own. A row can only name
/// something the enumeration defines, but nothing stops an enumerator being added
/// with no row, and it would then decode as a bare number for ever.
template <typename Enum, std::size_t N>
[[nodiscard]] consteval bool everyEnumeratorIsNamed(std::array<WireName<Enum>, N> const& table)
{
    return std::ranges::all_of(enumerators::enumeratorsOf<Enum>(), [&table](Enum value) {
        return !nameIn(table, std::to_underlying(value)).empty();
    });
}

/// @param table A name table.
/// @return Whether no two rows name the same wire value.
///
/// A duplicate shadows the later row, and since the lookup stops at the first
/// match both would name something plausible.
template <typename Enum, std::size_t N>
[[nodiscard]] consteval bool everyRowIsDistinct(std::array<WireName<Enum>, N> const& table)
{
    for (std::size_t i = 0; i < N; ++i)
        for (std::size_t j = i + 1; j < N; ++j)
            if (table[i].value == table[j].value)
                return false;

    return true;
}

// The names stay hand-written, because they say more than an identifier would --
// "MicConfiguration (bit 0 phantom, bit 3 effects bypass)" is the whole point of
// the table. These make sure every enumerator has one.
static_assert(everyRowIsDistinct(PropertyNames), "two rows of PropertyNames name the same address");
static_assert(everyEnumeratorIsNamed(PropertyNames),
              "a Property enumerator has no row in PropertyNames");
static_assert(everyRowIsDistinct(FramedCommandNames),
              "two rows of FramedCommandNames name the same command");
static_assert(everyEnumeratorIsNamed(FramedCommandNames),
              "a FramedCommand enumerator has no row in FramedCommandNames");

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

/// Builds one command in the framed family, checksum included.
///
/// @param command Which command.
/// @param body Its parameter bytes, which may be empty.
/// @return The full 64-byte payload, ready to be framed and sent.
[[nodiscard]] constexpr Payload framedPayload(FramedCommand command,
                                              std::span<std::uint8_t const> body) noexcept
{
    Payload payload {};
    payload[0] = FramedCommandMarker;
    payload[1] = 0x00;
    payload[2] = static_cast<std::uint8_t>(body.size() + FramedOverhead);
    payload[3] = std::to_underlying(command);
    std::ranges::copy(body, std::next(payload.begin(), 4));
    payload[payload[2] - 1] = framedChecksum(payload);
    return payload;
}

/// @param command Which command.
/// @param value Its parameter, when the command takes a single byte.
/// @return The full 64-byte payload.
[[nodiscard]] constexpr Payload framedPayload(FramedCommand command, std::uint8_t value) noexcept
{
    return framedPayload(command, std::span { &value, 1 });
}

/// @param command Which command, when it takes no parameters at all.
/// @return The full 64-byte payload.
[[nodiscard]] constexpr Payload framedPayload(FramedCommand command) noexcept
{
    return framedPayload(command, std::span<std::uint8_t const> {});
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
///
/// Confirmed against the deck with `ax310_probe --inputs`, which lights one button
/// at a time and reads the byte that arrives -- so the identity comes from the
/// light rather than from this table. The order runs backwards against the enum's,
/// and that is simply what the hardware does.
///
/// Two buttons held together arrive as one report with both bits set: `0x09` for
/// top-left and bottom-right. And a release is reported, roughly 300 ms after the
/// press, which is what lets Device::dispatchEvent's rising edge re-arm.
inline constexpr std::array<std::uint8_t, ButtonCount> ButtonBits { 0x08, 0x04, 0x02, 0x01 };

/// Bit representing each knob in InputReport::knobPush and ::knobTouch, indexed by KnobId.
///
/// Confirmed by the same walk. The knobs cannot be lit one at a time -- the ring
/// record colours all six at once -- so their identity comes from the legend
/// printed on the deck, and the walk takes the pushes in the order they arrive
/// rather than asking for one per round.
inline constexpr std::array<std::uint8_t, KnobCount> KnobBits { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20 };

/// Every control's bit is its own, and every one is a single bit.
///
/// The masks are how a report is read apart, so a duplicate or a two-bit row would
/// make one control answer for another silently. It is also what the hardware says:
/// four buttons and six knobs each produced a different single bit.
/// @param bits A table of bitmasks.
/// @return Whether every row is a distinct single bit.
template <std::size_t N>
[[nodiscard]] consteval bool everyRowIsOneOwnBit(std::array<std::uint8_t, N> const& bits)
{
    for (std::size_t row = 0; row < N; ++row)
    {
        if (std::popcount(bits[row]) != 1)
            return false;
        for (std::size_t other = row + 1; other < N; ++other)
            if (bits[row] == bits[other])
                return false;
    }

    return true;
}

static_assert(everyRowIsOneOwnBit(ButtonBits), "two buttons cannot share a bit");
static_assert(everyRowIsOneOwnBit(KnobBits), "two knobs cannot share a bit");

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
