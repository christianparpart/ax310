// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Protocol.hpp"

#include <array>
#include <cstdint>

/// Vendor command sequences replayed from USB captures of the AVerMedia
/// software.
///
/// **This is not an initialisation sequence. It is somebody's settings.**
///
/// Since established beyond doubt: payload 21 writes `0x85 = 00`, payload 23
/// writes `0x85 = 01`, and payload 25 is byte-identical to the parameter block
/// the vendor software sends when reverb is switched on. `0x85` was confirmed as
/// the reverb enable by capturing that toggle. **Replaying this turns the user's
/// reverb on**, which is exactly what was heard on a live call.
///
/// It was captured from one run of the vendor software, on one deck, configured
/// one way -- and replaying it imposes that configuration on whatever deck it is
/// sent to. Two symptoms established that, both observed during a live voice
/// call: every knob level jumped to 50% (`SET 0x27 = 0a x6`), and the deck's
/// room-reverb effect came on.
///
/// The structure says the same thing. Two command families are interleaved here:
///
///   * property writes, `[01|81] 10 <address> <length> <values>`, which carry the
///     mixer and LED state -- knob levels at 0x27, ring brightness at 0x1e;
///   * a framed family, `[fe][00][length][command][body][checksum]`, which is
///     plainly a DSP configuration. `0x85`, `0x87`, `0x88` and `0x9c` take a
///     single byte and read as enables; `0x94`, `0x9e` and `0x9f` carry 17 to 22
///     byte parameter blocks; `0xa3` appears eight times indexed 0..7 with a
///     per-entry enable and twenty bytes of coefficients, which is the shape of
///     an eight-band equaliser. `0xa3` is since decoded: five Q30 biquad
///     coefficients per band, verified by two bands being algebraically flat and
///     one being a unity-gain lowpass. The order is the tell: `0x85 = 00`, then
///     a `0x94` parameter block, then `0x85 = 01` -- disable, configure, enable.
///
/// Which of these actually wakes the hardware, and which merely restores a
/// stranger's taste in reverb, is **not established**. Until it is, sending the
/// whole thing is the only known way to get a working deck and it costs the user
/// their settings. Separating the two is the open problem; see AGENT.md.
///
/// The rows stay verbatim, in one place, so the next capture can be diffed
/// against them.
namespace ax310::commands
{

/// Vendor sequence replayed to turn a base-mode device into a control-mode one.
///
/// Written command by command rather than as 74 blobs, so what it does can be
/// read. Every payload below produces the same bytes the capture did; where a
/// high-level builder would not reproduce them exactly, the record stays raw and
/// says why.
///
/// Reading it through, the sequence has three parts: it interrogates the deck,
/// it configures the DSP, and then it restores one person's settings. Only the
/// first two could plausibly be what wakes the hardware.
///
/// One difference from the capture, deliberate. Eight of these payloads -- the
/// button-colour records -- arrived with 24 to 28 nonzero bytes *after* the ten
/// the length field declares, and those bytes are the vendor application's own
/// memory: Windows heap pointers, and `0x00007ff9b1bfa6f1` appearing in several
/// records, which is a module address rather than anything about a deck. Writing
/// them out again would replay a stranger's process image over USB to no purpose,
/// so the padding here is zero. The deck reads the declared length and no more --
/// every command this driver sends is zero-padded and the hardware takes them,
/// including the colour records that were driven by hand.
inline constexpr std::array<protocol::Payload, 74> InitPayloads { {
    // **Identify yourself.** The very first thing the vendor sends, and what
    // comes back is the firmware version -- which is how the group was decoded,
    // by holding the reply against the string Creator Central was displaying.
    protocol::identityRequest(),
    protocol::framedPayload(protocol::FramedCommand::Commit),

    // Then it reads its way across the deck before writing anything. A capture
    // of the vendor starting up recorded the answers; they are noted here beside
    // each read, from one deck configured one way, as a sanity check on shape
    // rather than as values anybody should expect.
    protocol::getProperty(protocol::Property::KnobLedBrightness, 1), // 0x09
    protocol::getProperty(protocol::Property::MicConfiguration, 1),  // 0x06
    protocol::getProperty(protocol::Property::LineOutVolume, 1),     // 0x14, maximum
    protocol::getProperty(protocol::Property::HeadphoneVolume, 1),   // 0x14, maximum
    protocol::getPropertyAt(0x22, 1),                                // 0x02, meaning unknown
    protocol::getProperty(protocol::Property::KnobLedSelect, 1),     // 0x00
    protocol::getProperty(protocol::Property::CreatorMixLevels, 7),  // six levels, then a zero
    protocol::getProperty(protocol::Property::AudienceMixLevels, 7),
    protocol::getProperty(protocol::Property::KnobPropertyAt35, 7),  // answers one value, not six

    // Read one byte at a time, and 0x11 is later written with three. They are
    // most likely one three-byte quantity that the vendor reads a byte at a time.
    protocol::getPropertyAt(0x11, 1), // 0x01
    protocol::getPropertyAt(0x12, 1), // 0x08
    protocol::getPropertyAt(0x13, 1), // 0x01

    // The display group, which answers with the panel's brightness in the address
    // slot -- `81 0a 64`, while the vendor was showing 100%.
    protocol::displayRequest(),

    // Microphone configuration. Bit 0 is phantom power, so 0x0e leaves it off;
    // bit 3 is set, which routes the chat mic without effects.
    protocol::setProperty(protocol::Property::MicConfiguration, std::uint8_t { 0x0e }),

    // The serial number, thirteen ASCII digits.
    protocol::serialRequest(),

    // The firmware read and its commit again, unchanged.
    protocol::identityRequest(),
    protocol::framedPayload(protocol::FramedCommand::Commit),

    // 0x23, whose two known values have no effect anyone has observed.
    protocol::setPropertyAt(0x23, std::uint8_t { 0x00 }),

    // Microphone gain, 0x1a of a 0x00..0x38 range.
    protocol::setProperty(protocol::Property::MicGain, std::uint8_t { 0x1a }),

    // The delay effect: disable, configure, enable, configure again. The order
    // is the tell -- this is a settings restore, not a hardware bring-up, and
    // payload 25 is byte-identical to what the vendor sends when reverb is
    // switched on. **Replaying this turns the user's reverb on.**
    protocol::framedPayload(protocol::FramedCommand::DelayEffectEnable, 0x00),
    protocol::framedPayload(protocol::FramedCommand::DelayEffectParameters,
                            std::to_array<std::uint8_t>({
                                0x40, 0xff, 0x7f, 0xff, 0x40, 0x7f, 0x80, 0x66,
                                0x7f, 0x10, 0x3f, 0x4e, 0x04, 0x40, 0x00, 0x43,
                                0x05, 0x00, 0x00, 0x00, 0x10 })),
    protocol::framedPayload(protocol::FramedCommand::DelayEffectEnable, 0x01),
    protocol::framedPayload(protocol::FramedCommand::DelayEffectParameters,
                            std::to_array<std::uint8_t>({
                                0x40, 0xff, 0x7f, 0xff, 0x40, 0x7f, 0x80, 0x66,
                                0x7f, 0x10, 0x3f, 0x4e, 0x04, 0x40, 0x01, 0x43,
                                0x05, 0x00, 0x00, 0x00, 0x10 })),
    protocol::framedPayload(protocol::FramedCommand::DelayEffectParameters,
                            std::to_array<std::uint8_t>({
                                0x40, 0x9b, 0xff, 0x19, 0x40, 0x7f, 0x80, 0x66,
                                0xe5, 0x10, 0x00, 0x4e, 0x04, 0x40, 0x01, 0x43,
                                0x05, 0xff, 0xca, 0x00, 0x10 })),

    // The noise gate. Payload 26 is all-zero apart from its tail; 27 and 28 are
    // identical to each other, so one of the three is redundant.
    protocol::framedPayload(protocol::FramedCommand::NoiseGateParameters,
                            std::to_array<std::uint8_t>({
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0xff, 0x7f, 0xff, 0xff, 0xff, 0x7f })),
    protocol::framedPayload(protocol::FramedCommand::NoiseGateParameters,
                            std::to_array<std::uint8_t>({
                                0x9b, 0x03, 0x8d, 0x02, 0x2c, 0x00, 0x00, 0x00,
                                0xb9, 0x01, 0x96, 0x78, 0x00, 0x00, 0xff, 0x7f,
                                0xd8, 0x02, 0xbb, 0x0f, 0x01, 0x00 })),
    protocol::framedPayload(protocol::FramedCommand::NoiseGateParameters,
                            std::to_array<std::uint8_t>({
                                0x9b, 0x03, 0x8d, 0x02, 0x2c, 0x00, 0x00, 0x00,
                                0xb9, 0x01, 0x96, 0x78, 0x00, 0x00, 0xff, 0x7f,
                                0xd8, 0x02, 0xbb, 0x0f, 0x01, 0x00 })),

    // The compressor, two parameter blocks with no enable between them.
    protocol::framedPayload(protocol::FramedCommand::CompressorParameters,
                            std::to_array<std::uint8_t>({
                                0x80, 0xeb, 0x99, 0x19, 0xa3, 0x02, 0x49, 0x00,
                                0x80, 0xf9, 0xcc, 0x04, 0x91, 0x00, 0x07, 0x00,
                                0x10 })),
    protocol::framedPayload(protocol::FramedCommand::CompressorParameters,
                            std::to_array<std::uint8_t>({
                                0x00, 0xa0, 0x00, 0x10, 0x83, 0x0e, 0x83, 0x0e,
                                0x00, 0xee, 0x00, 0x0c, 0xd6, 0x04, 0x31, 0x00,
                                0x1f })),

    // The equaliser: three enables, then eight bands.
    protocol::framedPayload(protocol::FramedCommand::EqualiserEnableA, 0x01),
    protocol::framedPayload(protocol::FramedCommand::EqualiserEnableB, 0x01),
    protocol::framedPayload(protocol::FramedCommand::EqualiserEnableC, 0x01),

    // Eight bands, indexed 0..7 in body byte 0, each with an enable in byte 1 and
    // five Q30 biquad coefficients. Bands 5 and 6 carry 0x00 in byte 2 where the
    // others carry 0x01.
    protocol::framedPayload(protocol::FramedCommand::EqualiserBand,
                            std::to_array<std::uint8_t>({
                                0x00, 0x01, 0x01, 0xe4, 0xd8, 0xac, 0x3f, 0x39,
                                0x4e, 0xa6, 0x80, 0xe4, 0xd8, 0xac, 0x3f, 0x1c,
                                0xb8, 0xa6, 0x80, 0xab, 0x1b, 0x5a, 0x3f })),
    protocol::framedPayload(protocol::FramedCommand::EqualiserBand,
                            std::to_array<std::uint8_t>({
                                0x01, 0x01, 0x01, 0xb0, 0x23, 0x14, 0x40, 0x89,
                                0x65, 0x4d, 0x81, 0x9a, 0xc1, 0xa1, 0x3e, 0x89,
                                0x65, 0x4d, 0x81, 0x4b, 0xe5, 0xb5, 0x3e })),
    protocol::framedPayload(protocol::FramedCommand::EqualiserBand,
                            std::to_array<std::uint8_t>({
                                0x02, 0x01, 0x01, 0x00, 0x00, 0x00, 0x40, 0x9c,
                                0x1a, 0x41, 0x83, 0x33, 0x2a, 0xd3, 0x3c, 0x9c,
                                0x1a, 0x41, 0x83, 0x33, 0x2a, 0xd3, 0x3c })),
    protocol::framedPayload(protocol::FramedCommand::EqualiserBand,
                            std::to_array<std::uint8_t>({
                                0x03, 0x01, 0x01, 0x41, 0x4b, 0x35, 0x3f, 0xa8,
                                0x45, 0x01, 0x88, 0xa3, 0x8b, 0x17, 0x39, 0xa8,
                                0x45, 0x01, 0x88, 0xe4, 0xd6, 0x4c, 0x38 })),
    protocol::framedPayload(protocol::FramedCommand::EqualiserBand,
                            std::to_array<std::uint8_t>({
                                0x04, 0x01, 0x01, 0x00, 0x00, 0x00, 0x40, 0x83,
                                0xe2, 0xf6, 0x8c, 0x47, 0x90, 0x36, 0x34, 0x83,
                                0xe2, 0xf6, 0x8c, 0x47, 0x90, 0x36, 0x34 })),
    protocol::framedPayload(protocol::FramedCommand::EqualiserBand,
                            std::to_array<std::uint8_t>({
                                0x05, 0x01, 0x00, 0x15, 0x32, 0x38, 0x69, 0x77,
                                0xd2, 0x1b, 0x80, 0xf5, 0xca, 0xad, 0x2e, 0x77,
                                0xd2, 0x1b, 0x80, 0x09, 0xfd, 0xe5, 0x17 })),
    protocol::framedPayload(protocol::FramedCommand::EqualiserBand,
                            std::to_array<std::uint8_t>({
                                0x06, 0x01, 0x00, 0x13, 0xf8, 0x1e, 0x6e, 0x7d,
                                0x7c, 0x33, 0xc8, 0xcb, 0x52, 0x71, 0x17, 0x7d,
                                0x7c, 0x33, 0xc8, 0xde, 0x4a, 0x90, 0x05 })),
    protocol::framedPayload(protocol::FramedCommand::EqualiserBand,
                            std::to_array<std::uint8_t>({
                                0x07, 0x01, 0x01, 0x64, 0x62, 0x41, 0x22, 0xc7,
                                0xc4, 0x82, 0x44, 0x64, 0x62, 0x41, 0x22, 0x93,
                                0x7b, 0x06, 0x36, 0xfb, 0x0d, 0xff, 0x12 })),

    // A write in group 0x09, which nobody has identified. Together with 0x03 and
    // 0x04 it is one of the three writes left in either sequence that are neither
    // interrogation nor somebody's settings.
    protocol::commandAt(protocol::CommandKind::Set, 0x09, 0x02, {}, 0),

    // A *write* to the serial group, four zero bytes. Reading it gives the serial
    // number; what writing it does is unknown and this driver does not try.
    protocol::commandAt(protocol::CommandKind::Set, protocol::SerialGroup, 0x00, {}, 4),

    // The display group, which is where the panel's brightness lives. 0xaa is
    // outside the 25..100 the brightness slider produces and is not the 0xff that
    // blanks it, so what this sets is not known.
    protocol::displayCommand(0xaa),

    // **Here the sequence stops configuring and starts imposing.** Everything
    // below is one person's mixer state, captured once. 0x0a is 50%, which is why
    // replaying this was seen to move every knob to half on a live call.
    //
    // The two writes to 0x14 set the Line Out source to the audience mix. They are
    // identical and consecutive, so one of them does nothing.
    protocol::setProperty(protocol::Property::LineOutSource,
                          static_cast<std::uint8_t>(LineOutSource::AudienceMix)),
    protocol::setProperty(protocol::Property::LineOutSource,
                          static_cast<std::uint8_t>(LineOutSource::AudienceMix)),
    protocol::setProperty(protocol::Property::KnobLedSelect, std::uint8_t { 0x80 }),
    protocol::setPropertyAt(0x22, std::uint8_t { 0x12 }),
    // One byte at the block's base address, which is the Mic track and not the
    // block -- written through levelAddressOf() so it cannot read as the latter.
    protocol::setPropertyAt(protocol::levelAddressOf(protocol::Property::CreatorMixLevels, KnobId::Mic),
                            std::uint8_t { 0x0a }),
    protocol::setProperty(protocol::Property::KnobPropertyAt35, std::uint8_t { 0x0a }),
    protocol::setProperty(protocol::Property::CreatorMixLevels,
                          std::to_array<std::uint8_t>({
                              0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x00 })),

    // 0x11 written with three zero bytes, after being read at payload 11.
    protocol::setPropertyAt(0x11,
                            std::to_array<std::uint8_t>({
                                0x00, 0x00, 0x00 })),

    // Select the creator mix.
    protocol::setProperty(protocol::Property::SelectedMix,
                          static_cast<std::uint8_t>(MixId::Creator)),

    // A write in group 0x03, then the probe-and-commit pair a third time.
    protocol::commandAt(protocol::CommandKind::Set, 0x03, 0x01, {}, 0),
    protocol::identityRequest(),
    protocol::framedPayload(protocol::FramedCommand::Commit),

    // The knob rings: brightness, then colour.
    protocol::setProperty(protocol::Property::KnobLedBrightness, std::uint8_t { 0x0d }),

    // Bytes 6 and 7 are 0xf9 0x00 here where the later captures have 0xf8 0x00,
    // which is one more reason to treat that pair as carrying nothing. That is
    // also why this is a raw record and not knobColourRecord(): the builder would
    // produce different bytes in a position whose meaning is unknown.
    protocol::setPropertyAt(protocol::ButtonColourAddress,
                            std::to_array<std::uint8_t>({
                                0x01, 0xc0, 0x0a, 0x00, 0x7d, 0xff, 0xf9, 0x00,
                                0x1f, 0x80 })),

    // The surround strip, set to a scrolling rainbow. Sent twice, identically.
    protocol::setPropertyAt(
        protocol::SurroundAddress,
        protocol::surroundRecord(SurroundMode::ScrollingRgb, 0x06, 0xff, 0xff, 0xff)),
    protocol::setPropertyAt(
        protocol::SurroundAddress,
        protocol::surroundRecord(SurroundMode::ScrollingRgb, 0x06, 0xff, 0xff, 0xff)),

    // Headphone volume, twice, then Line Out volume: 0x11 of 0x14, so 85%. The
    // vendor reads both at payloads 4 and 5 and reads them again at 65 and 66.
    // Nothing to do with the button selectors that share these numbers -- those
    // are bytes inside a record written to 0xc0, these are property addresses.
    protocol::setProperty(protocol::Property::HeadphoneVolume, std::uint8_t { 0x11 }),
    protocol::setProperty(protocol::Property::HeadphoneVolume, std::uint8_t { 0x11 }),
    protocol::setProperty(protocol::Property::LineOutVolume, std::uint8_t { 0x11 }),

    // The four function buttons, first all to a warm white, then to somebody's
    // chosen scheme: red, blue, blue, red. Raw for the same reason as payload 58 --
    // bytes 6 and 7 vary between these records and buttonColourRecord() would not
    // reproduce them.
    protocol::setPropertyAt(protocol::ButtonColourAddress,
                            std::to_array<std::uint8_t>({
                                0x00, 0x3c, 0x01, 0xff, 0xf4, 0xec, 0x00, 0x3d,
                                0x1f, 0x80 })),
    protocol::getProperty(protocol::Property::HeadphoneVolume, 1),
    protocol::getProperty(protocol::Property::LineOutVolume, 1),
    protocol::setPropertyAt(protocol::ButtonColourAddress,
                            std::to_array<std::uint8_t>({
                                0x00, 0x3d, 0x01, 0xff, 0xf4, 0xec, 0x00, 0x3b,
                                0x1f, 0x80 })),
    protocol::setPropertyAt(protocol::ButtonColourAddress,
                            std::to_array<std::uint8_t>({
                                0x00, 0x3f, 0x01, 0xff, 0xf4, 0xec, 0x00, 0x3d,
                                0x1f, 0x80 })),
    protocol::setPropertyAt(protocol::ButtonColourAddress,
                            std::to_array<std::uint8_t>({
                                0x00, 0x3e, 0x01, 0xff, 0xf4, 0xec, 0x00, 0x3d,
                                0x1f, 0x80 })),

    // And then the same four again, in the colours the user had actually picked.
    protocol::setPropertyAt(protocol::ButtonColourAddress,
                            std::to_array<std::uint8_t>({
                                0x00, 0x3c, 0x01, 0xff, 0x00, 0x00, 0xf9, 0x3d,
                                0x1f, 0x80 })),
    protocol::setPropertyAt(protocol::ButtonColourAddress,
                            std::to_array<std::uint8_t>({
                                0x00, 0x3d, 0x01, 0x00, 0x37, 0xff, 0xf9, 0x3d,
                                0x1f, 0x80 })),
    protocol::setPropertyAt(protocol::ButtonColourAddress,
                            std::to_array<std::uint8_t>({
                                0x00, 0x3f, 0x01, 0x00, 0x37, 0xff, 0xf9, 0x3d,
                                0x1f, 0x80 })),
    protocol::setPropertyAt(protocol::ButtonColourAddress,
                            std::to_array<std::uint8_t>({
                                0x00, 0x3e, 0x01, 0xff, 0x00, 0x00, 0xf9, 0x3d,
                                0x1f, 0x80 })),
} };

/// Vendor sequence that blanks the screen and puts the deck back to sleep.
/// Captured from the vendor software; the individual commands are not yet
/// understood, which is the only reason they are still opaque blobs.
inline constexpr std::array<protocol::Payload, 14> ShutdownPayloads { {
    // Two writes in groups nobody has identified, then the display told 0x02.
    // The same three run again at the end, which is the only structure here: the
    // sequence brackets the light-extinguishing with them.
    protocol::commandAt(protocol::CommandKind::Set, 0x04, 0x01, {}, 0),
    protocol::commandAt(protocol::CommandKind::Set, 0x09, 0x02, std::to_array<std::uint8_t>({ 0x00 })),
    protocol::setProperty(protocol::Property::DisplayPower, std::uint8_t { 0x02 }),

    // The four function buttons, unlit. Byte 8 is 0x00 where a lit record has
    // 0x1f, and the colour left behind is blue -- so this darkens them rather
    // than blanking them, exactly as turning one off in the vendor UI does.
    // Raw, because bytes 6 and 7 are 0xfe 0x00 here and buttonColourRecord()
    // writes something else into a pair whose meaning is unknown.
    protocol::setPropertyAt(protocol::ButtonColourAddress,
                            std::to_array<std::uint8_t>({
                                0x00, 0x3c, 0x01, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x80 })),
    protocol::setPropertyAt(protocol::ButtonColourAddress,
                            std::to_array<std::uint8_t>({
                                0x00, 0x3d, 0x01, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x80 })),
    protocol::setPropertyAt(protocol::ButtonColourAddress,
                            std::to_array<std::uint8_t>({
                                0x00, 0x3f, 0x01, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x80 })),
    protocol::setPropertyAt(protocol::ButtonColourAddress,
                            std::to_array<std::uint8_t>({
                                0x00, 0x3e, 0x01, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x80 })),

    // The knob rings: dimmed, then darkened the same way.
    protocol::setProperty(protocol::Property::KnobLedBrightness, std::uint8_t { 0x09 }),
    protocol::setPropertyAt(protocol::ButtonColourAddress,
                            std::to_array<std::uint8_t>({
                                0x01, 0xc0, 0x0a, 0x00, 0x00, 0xff, 0xfe, 0x00, 0x00, 0x80 })),

    // The surround strip, turned off -- and this is the vendor doing it, which is
    // independent confirmation that "off" is a black Solid and not a mode of its
    // own. Sent twice, identically, as it was when switched off in the UI.
    protocol::setPropertyAt(protocol::SurroundAddress,
                            protocol::surroundRecord(SurroundMode::Solid, 0xfe, 0x00, 0x00, 0x00)),
    protocol::setPropertyAt(protocol::SurroundAddress,
                            protocol::surroundRecord(SurroundMode::Solid, 0xfe, 0x00, 0x00, 0x00)),

    // And the opening three again, unchanged.
    protocol::commandAt(protocol::CommandKind::Set, 0x04, 0x01, {}, 0),
    protocol::commandAt(protocol::CommandKind::Set, 0x09, 0x02, std::to_array<std::uint8_t>({ 0x00 })),
    protocol::setProperty(protocol::Property::DisplayPower, std::uint8_t { 0x02 }),
} };

} // namespace ax310::commands
