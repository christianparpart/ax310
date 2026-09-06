// SPDX-License-Identifier: Apache-2.0
#include <ax310/Commands.hpp>
#include <ax310/Protocol.hpp>

#include <Printers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ios>
#include <span>

using namespace ax310;

TEST_CASE("the button bit table matches the layout the captures show", "[protocol]")
{
    // These four constants are the whole reason a caller never sees a mask. If
    // they drift, every button in the UI silently becomes a different button.
    CHECK(protocol::ButtonBits[indexOf(Button::TopLeft)] == 0x08);
    CHECK(protocol::ButtonBits[indexOf(Button::TopRight)] == 0x04);
    CHECK(protocol::ButtonBits[indexOf(Button::BottomLeft)] == 0x02);
    CHECK(protocol::ButtonBits[indexOf(Button::BottomRight)] == 0x01);
}

TEST_CASE("PhysicalButtonMask is derived from the bit table rather than written out", "[protocol]")
{
    std::uint8_t expected = 0;
    for (auto const bit: protocol::ButtonBits)
        expected = static_cast<std::uint8_t>(expected | bit);

    CHECK(protocol::PhysicalButtonMask == expected);
    CHECK(protocol::PhysicalButtonMask == 0x0F);
}

TEST_CASE("isDown reads one button out of a bitmask", "[protocol]")
{
    CHECK(protocol::isDown(Button::TopLeft, 0x08));
    CHECK_FALSE(protocol::isDown(Button::TopRight, 0x08));

    // Several at once, which the device does report.
    CHECK(protocol::isDown(Button::TopLeft, 0x0A));
    CHECK(protocol::isDown(Button::BottomLeft, 0x0A));
    CHECK_FALSE(protocol::isDown(Button::BottomRight, 0x0A));

    for (auto const button: AllButtons)
        CHECK_FALSE(protocol::isDown(button, 0x00));
}

TEST_CASE("isSet reads one knob out of a bitmask", "[protocol]")
{
    CHECK(protocol::KnobBits[indexOf(KnobId::Mic)] == 0x01);
    CHECK(protocol::KnobBits[indexOf(KnobId::Chat)] == 0x20);

    CHECK(protocol::isSet(KnobId::Mic, 0x01));
    CHECK(protocol::isSet(KnobId::Chat, 0x20));
    CHECK_FALSE(protocol::isSet(KnobId::LineIn, 0x01));

    for (auto const knob: AllKnobs)
        CHECK(protocol::isSet(knob, 0x3F)); // All six at once.
}

TEST_CASE("the mode descriptors carry the two personalities the deck enumerates as", "[protocol]")
{
    CHECK(protocol::VendorId == 0x07ca);

    CHECK(protocol::descriptorFor(DeviceMode::Base).productId == 0x0310);
    CHECK(protocol::descriptorFor(DeviceMode::Base).interfaceNumber == 4);

    CHECK(protocol::descriptorFor(DeviceMode::Control).productId == 0x1310);
    CHECK(protocol::descriptorFor(DeviceMode::Control).interfaceNumber == 0);
}

TEST_CASE("a command payload is framed with a report-id byte", "[protocol]")
{
    // The interface declares no report IDs, so hidapi's leading byte is always
    // zero -- and getting this wrong is why the whole init sequence was rejected
    // and the deck stayed dark.
    protocol::Payload payload {};
    payload[0] = 0x81;
    payload[1] = 0x10;
    payload.back() = 0xAB;

    auto const framed = protocol::frameFeatureReport(payload);

    REQUIRE(framed.size() == protocol::FeatureReportSize);
    CHECK(framed.size() == payload.size() + 1);
    CHECK(framed[0] == 0x00);
    CHECK(framed[1] == 0x81);
    CHECK(framed[2] == 0x10);
    CHECK(framed.back() == 0xAB);
}

TEST_CASE("the screen chunk geometry is self-consistent", "[protocol]")
{
    CHECK(protocol::ScreenChunkSize == 1025);
    CHECK(protocol::ScreenChunkHeaderSize == 13);

    // Derived, not asserted twice: 1012 is what is left of the report.
    CHECK(protocol::ScreenChunkPayloadSize == protocol::ScreenChunkSize - protocol::ScreenChunkHeaderSize);
    CHECK(protocol::ScreenChunkPayloadSize == 1012);

    // Every header field has to fit before the payload starts.
    CHECK(protocol::ScreenChunkSequenceOffset < protocol::ScreenChunkHeaderSize);
    CHECK(protocol::ScreenChunkLengthOffset + 1 < protocol::ScreenChunkHeaderSize);
    CHECK(protocol::ScreenChunkChecksumOffset + 1 < protocol::ScreenChunkHeaderSize);
}

TEST_CASE("the wire report is exactly the size the device sends", "[protocol]")
{
    // A size assertion on the wire, not on any struct. InputReport is a decoded
    // value now, and how large it happens to be says nothing about the deck.
    CHECK(protocol::ControlReportSize == 58);
    CHECK(protocol::PaddedReportSize == 64);
    CHECK(protocol::ChecksumOffset == protocol::ControlReportSize - 1);
    CHECK(protocol::Payload {}.size() == protocol::PaddedReportSize);

    // Every field the decoder reads has to fit inside the report.
    CHECK(protocol::KnobValuesOffset + KnobCount <= protocol::ControlReportSize);
    CHECK(protocol::AudioMetersOffset + (protocol::AudioMeterCount * 2) <= protocol::ControlReportSize);
    CHECK(protocol::TouchYOffset + 1 < protocol::ControlReportSize);
}

TEST_CASE("the report checksum is the low byte of every preceding byte summed", "[protocol]")
{
    // Verified against 164 distinct reports from two independent captures: the
    // vendor software's and one taken from this deck.
    std::array<std::uint8_t, protocol::ControlReportSize> report {};
    CHECK(protocol::checksumOf(report) == 0x00);
    CHECK(protocol::isChecksumValid(report));

    report[protocol::KnobTouchOffset] = 0x01;
    report[protocol::KnobValuesOffset] = 0x0e;
    CHECK(protocol::checksumOf(report) == 0x0f);
    CHECK_FALSE(protocol::isChecksumValid(report));

    report[protocol::ChecksumOffset] = 0x0f;
    CHECK(protocol::isChecksumValid(report));

    // Wraps rather than saturates, and the checksum byte is not part of the sum.
    report[protocol::KnobValuesOffset + 1] = 0xff;
    CHECK(protocol::checksumOf(report) == 0x0e);
}

TEST_CASE("a report decodes into a value, not a view onto the buffer", "[protocol]")
{
    std::array<std::uint8_t, protocol::ControlReportSize> bytes {};
    bytes[protocol::ButtonsOffset] = 0x08;
    bytes[protocol::KnobPushOffset] = 0x02;
    bytes[protocol::KnobTouchOffset] = 0x04;
    bytes[protocol::KnobValuesOffset + 2] = 0x2f;

    auto const report = protocol::decodeReport(bytes);

    CHECK_FALSE(report.isScreenTouch);
    CHECK(report.buttons == 0x08);
    CHECK(report.knobPush == 0x02);
    CHECK(report.knobTouch == 0x04);
    CHECK(report.knobValues[2] == 0x2f);
}

TEST_CASE("a touch report decodes little-endian while its meters decode big-endian", "[protocol]")
{
    // The same 58 bytes carry both orders. Overlaying a struct hid that; reading
    // the fields out states it.
    std::array<std::uint8_t, protocol::ControlReportSize> bytes {};
    bytes[protocol::EventTypeOffset] = protocol::ScreenTouchEventType;
    bytes[protocol::TouchFlagsOffset] = 0x1c;
    bytes[protocol::TouchXOffset] = 0xbc; // 700, little-endian
    bytes[protocol::TouchXOffset + 1] = 0x02;
    bytes[protocol::TouchYOffset] = 0x2c; // 300, little-endian
    bytes[protocol::TouchYOffset + 1] = 0x01;
    bytes[protocol::AudioMetersOffset] = 0x7f; // full scale, big-endian
    bytes[protocol::AudioMetersOffset + 1] = 0xff;

    auto const report = protocol::decodeReport(bytes);

    CHECK(report.isScreenTouch);
    CHECK(report.touchFlags == 0x1c);
    CHECK(report.touchX == 700);
    CHECK(report.touchY == 300);
    CHECK(protocol::toPercent(report.audioMeters[0]) == 100);
}

TEST_CASE("audio meters decode big-endian", "[protocol]")
{
    CHECK(protocol::decodeBigEndian16(0x01, 0x91) == 401);
    CHECK(protocol::decodeLittleEndian16(0x91, 0x01) == 401);
    CHECK(protocol::AudioMeterCount == KnobCount);

    std::array<std::uint8_t, protocol::ControlReportSize> bytes {};
    // Mic's pair: left at full scale, right quieter. The decoder reports the
    // louder side, so a hard-panned peak is not averaged away.
    bytes[protocol::AudioMetersOffset] = 0x7f;
    bytes[protocol::AudioMetersOffset + 1] = 0xff;
    bytes[protocol::AudioMetersOffset + 2] = 0x10;
    bytes[protocol::AudioMetersOffset + 3] = 0x00;

    // System sits three tracks along, four bytes each.
    auto const system = protocol::AudioMetersOffset
                        + (indexOf(KnobId::System) * protocol::AudioMeterStride);
    bytes[system] = 0x40;
    bytes[system + 1] = 0x00;
    bytes[system + 2] = 0x40;
    bytes[system + 3] = 0x00;

    auto const report = protocol::decodeReport(bytes);
    CHECK(protocol::toPercent(report.audioMeters[indexOf(KnobId::Mic)]) == 100);
    CHECK(protocol::toPercent(report.audioMeters[indexOf(KnobId::LineIn)]) == 0);
    CHECK(protocol::toPercent(report.audioMeters[indexOf(KnobId::System)]) == 50);
    CHECK(protocol::toPercent(report.audioMeters[indexOf(KnobId::Chat)]) == 0);
}

TEST_CASE("a property write is built the way the vendor builds it", "[protocol][commands]")
{
    // The strongest check available without the hardware: the command model must
    // reproduce, byte for byte, what the captured init sequence actually sends.
    // InitPayloads[51] is the LED write -- six knobs at 0x0a.
    std::array<std::uint8_t, protocol::KnobPropertyLength> const levels { 0x0a, 0x0a, 0x0a, 0x0a,
                                                                          0x0a, 0x0a, 0x00 };
    auto const built = protocol::setProperty(protocol::Property::CreatorMixLevels, levels);

    auto const& captured = commands::InitPayloads[51];
    INFO("built    " << std::hex << int { built[0] } << " " << int { built[1] } << " " << int { built[2] }
                     << " " << int { built[3] });
    INFO("captured " << std::hex << int { captured[0] } << " " << int { captured[1] } << " "
                     << int { captured[2] } << " " << int { captured[3] });
    CHECK(std::ranges::equal(built, captured));
}

TEST_CASE("a property read is a write with no value", "[protocol][commands]")
{
    // InitPayloads[8] reads the same property the test above writes.
    auto const built = protocol::getProperty(protocol::Property::CreatorMixLevels, protocol::KnobPropertyLength);

    CHECK(std::ranges::equal(built, commands::InitPayloads[8]));
    CHECK(built[0] == static_cast<std::uint8_t>(protocol::CommandKind::Get));
    CHECK(built[1] == protocol::PropertyGroup);
    CHECK(built[2] == static_cast<std::uint8_t>(protocol::Property::CreatorMixLevels));
    CHECK(built[3] == protocol::KnobPropertyLength);
}

TEST_CASE("a property write carries its own length", "[protocol][commands]")
{
    std::array<std::uint8_t, 1> const one { 0x0d };
    auto const brightness = protocol::setProperty(protocol::Property::KnobLedBrightness, one);

    CHECK(brightness[0] == static_cast<std::uint8_t>(protocol::CommandKind::Set));
    CHECK(brightness[2] == 0x1e);
    CHECK(brightness[3] == 1);
    CHECK(brightness[4] == 0x0d);

    // Nothing beyond the value: 39 of the 51 captured property commands are
    // clean this way, and the twelve that are not are the 0x0a-record writes,
    // which this builder does not claim to produce.
    for (std::size_t index = 5; index < brightness.size(); ++index)
        CHECK(brightness[index] == 0x00);
}

TEST_CASE("the framed command family checksums every captured command", "[protocol][commands]")
{
    // [0xfe][0x00][length][body...][checksum], length counting the whole command.
    // All 24 such commands in the captured sequences agree with this.
    std::size_t checked = 0;
    for (auto const& payload: commands::InitPayloads)
    {
        if (payload[0] != protocol::FramedCommandMarker)
            continue;

        auto const length = std::size_t { payload[2] };
        REQUIRE(length >= 4);
        REQUIRE(length <= payload.size());

        INFO("framed command of length " << length);
        CHECK(payload[length - 1] == protocol::framedChecksum(payload));

        // Whatever follows the stated length is padding and must be zero.
        for (std::size_t index = length; index < payload.size(); ++index)
            CHECK(payload[index] == 0x00);

        ++checked;
    }
    // Every one of them is in the init sequence; the shutdown sequence uses only
    // property commands.
    CHECK(checked == 24);
}

TEST_CASE("every captured property command agrees with the model", "[protocol][commands]")
{
    // A census rather than a spot check: if the grammar is right, every property
    // command in both sequences decomposes under it.
    auto const init = std::span { commands::InitPayloads };
    auto const shutdown = std::span { commands::ShutdownPayloads };

    std::size_t properties = 0;
    for (auto const& payload: std::array<std::span<protocol::Payload const>, 2> { init, shutdown })
    {
        for (auto const& command: payload)
        {
            if (command[1] != protocol::PropertyGroup)
                continue;
            if (command[0] != static_cast<std::uint8_t>(protocol::CommandKind::Set)
                && command[0] != static_cast<std::uint8_t>(protocol::CommandKind::Get))
                continue;

            INFO("property 0x" << std::hex << int { command[2] } << " length " << std::dec
                               << int { command[3] });
            CHECK(command[3] > 0); // Every one states a length.
            ++properties;
        }
    }
    CHECK(properties == 51);
}

TEST_CASE("a track's level address is its mix block plus its knob index", "[protocol][levels]")
{
    using namespace ax310::protocol;

    // Established on the hardware in both blocks: dragging Mic writes 0x27 and
    // dragging System writes 0x2a, which is the same block three tracks along.
    CHECK(levelAddressOf(Property::CreatorMixLevels, KnobId::Mic) == 0x27);
    CHECK(levelAddressOf(Property::CreatorMixLevels, KnobId::System) == 0x2a);
    CHECK(levelAddressOf(Property::CreatorMixLevels, KnobId::Game) == 0x2b);
    CHECK(levelAddressOf(Property::CreatorMixLevels, KnobId::Chat) == 0x2c);

    // 0x31 was confirmed directly: it silences a System tone in the second mix
    // and leaves the first alone.
    CHECK(levelAddressOf(Property::AudienceMixLevels, KnobId::Mic) == 0x2e);
    CHECK(levelAddressOf(Property::AudienceMixLevels, KnobId::System) == 0x31);

    // The two blocks must not overlap, or one mix would write into the other.
    auto const firstEnd = levelAddressOf(Property::CreatorMixLevels, KnobId::Chat);
    auto const secondStart = levelAddressOf(Property::AudienceMixLevels, KnobId::Mic);
    CHECK(firstEnd < secondStart);
}

TEST_CASE("a report is found whether or not hidapi prepended the report id", "[protocol]")
{
    using namespace ax310::protocol;

    // hidapi prepends the report id for a device that numbers its reports and
    // does not for one that does not, and this deck sends two lengths. Getting
    // this wrong shifts every field by one byte, which reads as a deck that has
    // gone mad rather than as an off-by-one.
    std::array<std::uint8_t, PaddedReportSize + 1> withId {};
    withId[0] = 0x01;
    withId[1] = 0xab;
    auto const trimmed = reportPayload(withId);
    REQUIRE(trimmed.size() == PaddedReportSize);
    CHECK(trimmed[0] == 0xab);

    std::array<std::uint8_t, PaddedReportSize> padded {};
    padded[0] = 0xcd;
    CHECK(reportPayload(padded).size() == PaddedReportSize);
    CHECK(reportPayload(padded)[0] == 0xcd);

    std::array<std::uint8_t, ControlReportSize> control {};
    CHECK(reportPayload(control).size() == ControlReportSize);

    // Anything else is not a report, and saying so beats decoding rubbish.
    std::array<std::uint8_t, 7> tooShort {};
    CHECK(reportPayload(tooShort).empty());
    std::array<std::uint8_t, PaddedReportSize + 2> tooLong {};
    CHECK(reportPayload(tooLong).empty());
}

// The wire specification in docs/wire-protocol.md is generated from the tables
// below it, so its numbers cannot drift. Its *sentences* can: an endianness, an
// ordering, a claim that two blocks are contiguous. Each of these pins one of
// them, so the prose fails the suite rather than merely becoming untrue.

TEST_CASE("the spec's tables have no phantom rows", "[protocol][spec]")
{
    using namespace ax310::protocol;

    // A hand-written std::array length that exceeds the initialisers leaves
    // value-initialised rows behind. FramedCommandNames was declared 11 with ten
    // entries, so a command 0x00 with no name sat in it until something iterated
    // the table -- which nothing did until the spec generator.
    for (auto const& row: PropertyNames)
    {
        UNSCOPED_INFO("property " << static_cast<int>(row.value));
        CHECK_FALSE(row.name.empty());
    }
    for (auto const& row: FramedCommandNames)
    {
        UNSCOPED_INFO("command " << static_cast<int>(row.value));
        CHECK(row.value != 0x00);
        CHECK_FALSE(row.name.empty());
    }
    for (auto const& row: PreservedAddresses)
        CHECK(row.length > 0);
    for (auto const& row: Parameters)
        CHECK_FALSE(row.name.empty());
}

TEST_CASE("the meters are big-endian and the touch coordinates are not", "[protocol][spec]")
{
    using namespace ax310::protocol;

    // The one claim in the document that no table holds, and the one this
    // project got wrong most often. A report is mixed-endian in the same frame.
    std::array<std::uint8_t, ControlReportSize> bytes {};

    // 0x1234 big-endian in the microphone's left channel, and louder than its
    // right, so decodeReport reports it.
    bytes[AudioMetersOffset] = 0x12;
    bytes[AudioMetersOffset + 1] = 0x34;

    bytes[EventTypeOffset] = ScreenTouchEventType;
    bytes[TouchXOffset] = 0x34;     // 0x1234 little-endian
    bytes[TouchXOffset + 1] = 0x12;

    auto const report = decodeReport(bytes);
    CHECK(report.audioMeters[0] == 0x1234);
    CHECK(report.touchX == 0x1234);
}

TEST_CASE("the two level blocks are contiguous and in knob order", "[protocol][spec]")
{
    using namespace ax310::protocol;

    // The document prints an address per (mix, track) and calls the blocks
    // contiguous. That is a property of levelAddressOf(), not of a table.
    for (auto const block: { Property::CreatorMixLevels, Property::AudienceMixLevels })
    {
        auto const base = static_cast<std::uint8_t>(block);
        for (auto const knob: AllKnobs)
            CHECK(levelAddressOf(block, knob) == base + indexOf(knob));
    }

    // And they do not overlap, which is what makes two independent mixes possible.
    auto const creatorEnd = static_cast<std::uint8_t>(Property::CreatorMixLevels) + KnobCount;
    CHECK(creatorEnd <= static_cast<std::uint8_t>(Property::AudienceMixLevels));
}

TEST_CASE("every parameter fits inside the body it is written into", "[protocol][spec]")
{
    using namespace ax310::protocol;

    // The document prints a body offset per parameter. An offset past the end of
    // the known-good body would write outside it -- and the driver edits that
    // body in place, because the DSP chain has no read-back to correct it.
    for (auto const& row: Parameters)
    {
        auto const slot = framedDefaultIndex(row.command);
        UNSCOPED_INFO(row.name << " in command " << static_cast<int>(row.command));
        REQUIRE(slot.has_value());

        auto const& body = FramedDefaults[*slot];
        std::size_t const width =
            row.encoding == ParameterEncoding::UnsignedWordLE ? 2 : 1;
        CHECK(row.bodyOffset + width <= body.length);
    }
}

TEST_CASE("no address is both preserved and refused", "[protocol][spec]")
{
    using namespace ax310::protocol;

    // Connect saves the preserved addresses and writes them back. If one were
    // also refused, the restore would silently drop it and the deck would keep
    // whatever the handshake left there.
    for (auto const& row: PreservedAddresses)
    {
        UNSCOPED_INFO("preserved address " << static_cast<int>(row.address));
        CHECK(std::ranges::find(DangerousAddresses, row.address) == DangerousAddresses.end());
    }
}

TEST_CASE("the button colour selectors are not in the enum's order", "[protocol][buttons]")
{
    using namespace ax310::protocol;

    // Captured one button at a time, all four. The selectors are consecutive but
    // clockwise, where Button is row-major -- so FirstButtonSelector + index
    // would light the wrong two, which is the whole reason this is a table.
    CHECK(selectorFor(Button::TopLeft) == 0x3c);
    CHECK(selectorFor(Button::TopRight) == 0x3d);
    CHECK(selectorFor(Button::BottomRight) == 0x3e);
    CHECK(selectorFor(Button::BottomLeft) == 0x3f);

    // Every button has its own, and between them they cover the four the vendor
    // writes when it turns all of them off.
    auto seen = ButtonColourSelectors;
    std::ranges::sort(seen);
    CHECK(std::ranges::adjacent_find(seen) == seen.end());
    CHECK(seen.front() == 0x3c);
    CHECK(seen.back() == 0x3f);
}

TEST_CASE("a button colour record is the shape the vendor sends", "[protocol][buttons]")
{
    using namespace ax310::protocol;

    // Byte for byte against a captured record: the vendor lighting the top-left
    // button red sent 00 3c 01 ff 00 00 ?? ?? 1f 80, where the two unexplained
    // bytes are not derived from the record and vary between captures.
    auto const red = buttonColourRecord(selectorFor(Button::TopLeft), 0xff, 0x00, 0x00, true);
    CHECK(red[1] == 0x3c);
    CHECK(red[3] == 0xff);
    CHECK(red[4] == 0x00);
    CHECK(red[5] == 0x00);
    CHECK(red[8] == ButtonLit);

    // Off clears the enable as well as the colour: the vendor writes both, so a
    // colour of zero on its own is not what it sends.
    auto const dark = buttonColourRecord(selectorFor(Button::TopLeft), 0, 0, 0, false);
    CHECK(dark[8] == ButtonDark);
    CHECK(dark[3] == 0);
}

TEST_CASE("the knob rings take one colour, at the button address", "[protocol][knobs]")
{
    using namespace ax310::protocol;

    // Byte for byte against the capture of the vendor setting the rings red:
    // 01 c0 0a ff 00 00 f8 00 1f 80. Bank 1 rather than 0 is what separates this
    // from a button record; the two share an address.
    auto const red = knobColourRecord(0xff, 0x00, 0x00);
    CHECK(red[0] == KnobBank);
    CHECK(red[1] == KnobFirstLight);
    CHECK(red[2] == KnobLightCount);
    CHECK(red[3] == 0xff);
    CHECK(red[4] == 0x00);
    CHECK(red[5] == 0x00);
    CHECK(red[8] == ButtonLit);

    // The record carries no mix. Captures of the same colour set on the creator
    // and on the audience mix are identical, so nothing here may vary by mix --
    // a caller selects the mix first and the deck colours that one.
    CHECK(red[0] != ButtonBank);
    CHECK(knobColourRecord(0x00, 0xff, 0x00)[4] == 0xff);
}

TEST_CASE("every surround mode has its own selector", "[protocol][surround]")
{
    using namespace ax310::protocol;

    // The captured values, one per mode, kept here so a reordering of the
    // enumeration cannot quietly move them.
    CHECK(selectorFor(SurroundMode::ScrollingRgb) == 0x24);
    CHECK(selectorFor(SurroundMode::PulsingRgb) == 0x28);
    CHECK(selectorFor(SurroundMode::Blinking) == 0x2c);
    CHECK(selectorFor(SurroundMode::BlinkingRgb) == 0x30);
    CHECK(selectorFor(SurroundMode::Solid) == 0x34);
    CHECK(selectorFor(SurroundMode::Pulsing) == 0x38);

    // Distinct, and four apart. The gap is unexplained and the point of the table.
    auto seen = SurroundModeSelectors;
    std::ranges::sort(seen);
    CHECK(std::ranges::adjacent_find(seen) == seen.end());
    for (std::size_t index = 1; index < seen.size(); ++index)
        CHECK(seen[index] - seen[index - 1] == 4);
}

TEST_CASE("a surround record is the shape the vendor sends", "[protocol][surround]")
{
    using namespace ax310::protocol;

    // Byte for byte against the capture of solid blue: 01 34 01 20 f8 00 00 00 00 ff.
    auto const solid = surroundRecord(SurroundMode::Solid, 0xf8, 0x00, 0x00, 0xff);
    CHECK(solid == std::array<std::uint8_t, 10> { 0x01, 0x34, 0x01, 0x20, 0xf8,
                                                  0x00, 0x00, 0x00, 0x00, 0xff });

    // And against pulsing at each end of the frequency slider.
    CHECK(surroundRecord(SurroundMode::Pulsing, MinSurroundFrequency, 0, 0, 0x19)[4] == 0x01);
    CHECK(surroundRecord(SurroundMode::Pulsing, MaxSurroundFrequency, 0, 0, 0x19)[4] == 0x0a);

    // The hue-cycling modes were captured with white in the colour they ignore.
    auto const scrolling = surroundRecord(SurroundMode::ScrollingRgb, 0x03, 0xff, 0xff, 0xff);
    CHECK(scrolling == std::array<std::uint8_t, 10> { 0x01, 0x24, 0x01, 0x20, 0x03,
                                                      0x00, 0x00, 0xff, 0xff, 0xff });

    // Off is not a mode: it is Solid with nothing lit, which is the whole of the
    // difference between the vendor's "off" capture and its "solid" one.
    auto const off = surroundOffRecord();
    CHECK(off[1] == selectorFor(SurroundMode::Solid));
    CHECK(off[7] == 0x00);
    CHECK(off[8] == 0x00);
    CHECK(off[9] == 0x00);
}

TEST_CASE("surround modes say what their rate byte means", "[protocol][surround]")
{
    // Solid is the only mode whose rate is a brightness; the rest are frequencies.
    CHECK(!isAnimated(SurroundMode::Solid));
    for (auto const mode: AllSurroundModes)
        if (mode != SurroundMode::Solid)
            CHECK(isAnimated(mode));

    // And exactly three cycle hues on their own.
    CHECK(std::ranges::count_if(AllSurroundModes, [](auto mode) { return cyclesHues(mode); }) == 3);
    CHECK(cyclesHues(SurroundMode::PulsingRgb));
    CHECK(!cyclesHues(SurroundMode::Pulsing));
}

TEST_CASE("brightness is carried by the colour, not by a field", "[protocol][surround]")
{
    using namespace ax310::protocol;

    // The two ends of the vendor's slider, captured in pulsing with the frequency
    // held still: the channel travels the whole way and byte 4 does not move.
    CHECK(scaledChannel(0xff, 100) == MaxLightChannel);
    CHECK(scaledChannel(0xff, 0) == MinLightChannel);

    auto const bright = surroundRecord(SurroundMode::Pulsing, 0x0a, 0, 0, scaledChannel(0xff, 100));
    auto const dim = surroundRecord(SurroundMode::Pulsing, 0x0a, 0, 0, scaledChannel(0xff, 0));
    CHECK(bright[4] == dim[4]);
    CHECK(bright[9] == 0xff);
    CHECK(dim[9] == 0x19);

    // The line puts the midpoint at 0x8c. The hand-placed slider gave 0x85, which
    // on this line is 47% -- close enough for a dragged slider, and the reason the
    // curve is called consistent with the evidence rather than measured from it.
    CHECK(scaledChannel(0xff, 50) == 0x8c);
    CHECK(scaledChannel(0xff, 47) == 0x85);

    // An unlit channel stays unlit. Lifting it to the floor would turn a pure red
    // into a washed-out pink at the bottom of the slider, which is not what the
    // vendor sends: its dim captures keep the two dark channels at zero.
    CHECK(scaledChannel(0x00, 0) == 0x00);
    CHECK(scaledChannel(0x00, 100) == 0x00);

    // Out of range clamps rather than wrapping through the conversion.
    CHECK(scaledChannel(0xff, -10) == MinLightChannel);
    CHECK(scaledChannel(0xff, 500) == MaxLightChannel);
}
