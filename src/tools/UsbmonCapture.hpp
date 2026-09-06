// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ax310::tools
{

/// One URB out of a Linux usbmon capture.
struct CapturedUrb
{
    std::uint8_t transferType = 0; ///< 0 isochronous, 1 interrupt, 2 control, 3 bulk.
    std::uint8_t endpoint = 0;     ///< Endpoint address; bit 7 set means IN.
    std::uint8_t device = 0;       ///< usbmon device number.
    std::uint32_t urbLength = 0;   ///< Length the URB declared, which may exceed what was captured.
    bool isCompletion = false;     ///< Whether this is the callback rather than the submission.
    std::vector<std::uint8_t> payload;

    /// @return Whether this URB travelled from the device to the host.
    [[nodiscard]] bool isInbound() const noexcept { return (endpoint & 0x80U) != 0; }
};

/// Why a capture could not be read.
enum class CaptureError : std::uint8_t
{
    Unreadable,   ///< The file could not be opened or is too short to hold a header.
    NotPcap,      ///< No recognised pcap magic number.
    IsPcapNg,     ///< pcapng, which this does not read -- capture with `-F pcap`.
    NotUsbmon,    ///< A pcap, but not of a USB monitor interface.
};

/// @param error What went wrong.
/// @return Text suitable for a diagnostic.
[[nodiscard]] std::string_view describe(CaptureError error) noexcept;

/// Reads every URB out of a classic pcap of a Linux usbmon interface.
///
/// Deliberately not a general pcap reader. It handles the one format this
/// project's own capture script produces, and says so plainly when handed
/// anything else -- a decoder that half-reads an unexpected file produces
/// confident nonsense, which is worse here than a refusal.
///
/// @param path The capture to read.
/// @return Every URB in order, or why it could not be read.
[[nodiscard]] std::expected<std::vector<CapturedUrb>, CaptureError> readUsbmonCapture(
    std::string const& path);

} // namespace ax310::tools
