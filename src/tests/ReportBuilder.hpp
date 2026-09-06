// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <ax310/Protocol.hpp>
#include <ax310/Types.hpp>

namespace ax310::testing
{

/// Builds a raw input report the way the device sends one: as bytes at offsets.
///
/// Deliberately NOT by filling in a protocol::InputReport and copying it over.
/// The struct is the thing under test -- building a fixture out of it would
/// assert that the struct agrees with itself, and would keep agreeing after a
/// field moved. Writing the offsets out by hand is what pins the layout the
/// captures actually show, and is why this lives here rather than beside
/// Protocol.hpp.
///
/// Layout, from the capture analysis in AGENT.md:
///   0      event byte: a button bitmask, or 0x10 marking a screen touch
///   1      touch state, when byte 0 says screen touch
///   2..3   touch x, little-endian
///   4..5   touch y, little-endian
///   6      knob push bitmask
///   7      knob touch bitmask
///   8..13  knob positions, one byte each
///   14..57 padding
class ReportBuilder
{
  public:
    /// Every real report the deck sends carries a set byte at offset 16, which
    /// the decoder ignores but which is what distinguishes a real report from the
    /// all-zero filler the deck interleaves between them. Setting it by default
    /// keeps fixtures on the real side of that line.
    ReportBuilder() noexcept { _bytes[LiveMarkerOffset] = 0x01; }

    /// @return The all-zero filler report, which carries no event at all.
    [[nodiscard]] static std::vector<std::uint8_t> filler()
    {
        return std::vector<std::uint8_t>(protocol::ControlReportSize, 0);
    }

    /// @param buttons The button bitmask to place at offset 0.
    /// @return This builder, for chaining.
    ReportBuilder& buttons(std::uint8_t buttons) noexcept
    {
        _bytes[0] = buttons;
        return *this;
    }

    /// Marks the report a screen touch and fills in its fields.
    ///
    /// The report being a screen touch is itself what says a finger is down; the
    /// flags byte does not, and reading it as contact is what made stationary
    /// and two-finger touches disappear.
    ///
    /// @param x Horizontal coordinate.
    /// @param y Vertical coordinate.
    /// @param flags The unexplained byte at offset 1, as observed on the wire.
    /// @return This builder, for chaining.
    ReportBuilder& screenTouch(std::uint16_t x, std::uint16_t y, std::uint8_t flags) noexcept
    {
        _bytes[0] = protocol::ScreenTouchEventType;
        _bytes[1] = flags;
        _bytes[2] = static_cast<std::uint8_t>(x & 0xFF);
        _bytes[3] = static_cast<std::uint8_t>(x >> 8);
        _bytes[4] = static_cast<std::uint8_t>(y & 0xFF);
        _bytes[5] = static_cast<std::uint8_t>(y >> 8);
        return *this;
    }

    /// @param mask The knob-push bitmask to place at offset 6.
    /// @return This builder, for chaining.
    ReportBuilder& knobPush(std::uint8_t mask) noexcept
    {
        _bytes[6] = mask;
        return *this;
    }

    /// @param mask The knob-touch bitmask to place at offset 7.
    /// @return This builder, for chaining.
    ReportBuilder& knobTouch(std::uint8_t mask) noexcept
    {
        _bytes[7] = mask;
        return *this;
    }

    /// @param channel Which meter, below protocol::AudioMeterCount.
    /// @param raw Raw meter value, 0..0x7fff.
    /// @return This builder, for chaining.
    /// Sets one track's meter. Writes both sides of the stereo pair, because the
    /// decoder takes the louder of the two and a builder that filled only one
    /// would be testing half the field.
    ReportBuilder& audioMeter(std::size_t channel, std::uint16_t raw) noexcept
    {
        // Big-endian, unlike the screen chunk header's little-endian fields.
        auto const base = AudioMetersOffset + (channel * protocol::AudioMeterStride);
        _bytes[base] = static_cast<std::uint8_t>(raw >> 8);
        _bytes[base + 1] = static_cast<std::uint8_t>(raw & 0xFF);
        _bytes[base + 2] = static_cast<std::uint8_t>(raw >> 8);
        _bytes[base + 3] = static_cast<std::uint8_t>(raw & 0xFF);
        return *this;
    }

    /// @param knob Which knob's position to set.
    /// @param value Its raw position, 0..20.
    /// @return This builder, for chaining.
    ReportBuilder& knobValue(KnobId knob, std::uint8_t value) noexcept
    {
        _bytes[KnobValuesOffset + indexOf(knob)] = value;
        return *this;
    }

    /// @return The report as the transport would hand it over: 58 bytes, with a
    ///         valid checksum, because the decoder checks it before trusting it.
    [[nodiscard]] std::vector<std::uint8_t> build() const
    {
        std::vector<std::uint8_t> report { _bytes.begin(), _bytes.end() };
        report.back() = checksumFor(report);
        return report;
    }

    /// @param report A report body whose last byte is the checksum.
    /// @return The checksum that byte must carry.
    [[nodiscard]] static std::uint8_t checksumFor(std::vector<std::uint8_t> const& report)
    {
        unsigned sum = 0;
        for (std::size_t index = 0; index + 1 < report.size(); ++index)
            sum += report[index];
        return static_cast<std::uint8_t>(sum & 0xFF);
    }

    /// @return The report with a leading report-id byte, as some backends
    ///         deliver it: 65 bytes.
    [[nodiscard]] std::vector<std::uint8_t> buildWithReportId() const
    {
        auto const body = build();
        std::vector<std::uint8_t> padded { 0x00 };
        padded.insert(padded.end(), body.begin(), body.end());
        padded.resize(protocol::PaddedReportSize + 1, 0x00);
        return padded;
    }

  private:
    // Taken from Protocol.hpp rather than restated. These were copies, and the
    // meter offset had already drifted: the protocol moved to 0x12 and this stayed
    // at 0x1e, so the builder wrote meters into a dead region and the test failed
    // for a reason that had nothing to do with the code under test.
    static constexpr std::size_t KnobValuesOffset = protocol::KnobValuesOffset;
    static constexpr std::size_t AudioMetersOffset = protocol::AudioMetersOffset;

    /// A byte every real report sets and the filler leaves clear.
    static constexpr std::size_t LiveMarkerOffset = 16;

    std::array<std::uint8_t, protocol::ControlReportSize> _bytes {};
};

} // namespace ax310::testing
