// SPDX-License-Identifier: Apache-2.0
#include "UsbmonCapture.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>

namespace ax310::tools
{

namespace
{
    /// pcap file header, and the record header in front of every packet.
    constexpr std::size_t FileHeaderSize = 24;
    constexpr std::size_t RecordHeaderSize = 16;
    constexpr std::size_t LinkTypeOffset = 20;

    /// Link types for the two Linux USB monitor encapsulations.
    constexpr std::uint32_t LinkTypeUsbLinux = 189;
    constexpr std::uint32_t LinkTypeUsbLinuxMmapped = 220;

    /// struct usbmon_packet, which prefixes every captured URB.
    ///
    /// The 8-byte setup packet sits at offset 40 **inside** this header rather
    /// than in front of the data, so the payload starts at 64 for control
    /// transfers exactly as it does for interrupt ones. Reading it the other way
    /// silently yields nothing at all, which cost a session here.
    constexpr std::size_t UsbmonHeaderSize = 64;
    constexpr std::size_t UrbTypeOffset = 8;
    constexpr std::size_t TransferTypeOffset = 9;
    constexpr std::size_t EndpointOffset = 10;
    constexpr std::size_t DeviceOffset = 11;
    constexpr std::size_t UrbLengthOffset = 32;

    /// The URB's own timestamp: seconds as a signed 64-bit at 16, microseconds as
    /// a signed 32-bit at 24. The pcap record in front of it carries a timestamp
    /// too, but this is the one the kernel took when the transfer happened.
    constexpr std::size_t TimestampSecondsOffset = 16;
    constexpr std::size_t TimestampMicrosecondsOffset = 24;

    /// 'S' marks a submission -- the host handing the URB over -- and 'C' its
    /// completion. Which one carries the bytes depends on direction: an OUT
    /// transfer has them at submission, an **IN** transfer only at completion,
    /// because at submission there is nothing to send yet.
    ///
    /// Keeping only submissions is why an inbound stream looked empty: every
    /// report the deck sent was in a record that had been thrown away.
    constexpr std::uint8_t SubmitMarker = 'S';
    constexpr std::uint8_t CompleteMarker = 'C';

    [[nodiscard]] std::uint32_t readWord(std::span<std::uint8_t const> bytes, bool isBigEndian) noexcept
    {
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index)
        {
            auto const byte = static_cast<std::uint32_t>(bytes[index]);
            value |= isBigEndian ? byte << (8U * (3U - index)) : byte << (8U * index);
        }
        return value;
    }

    [[nodiscard]] std::uint64_t readDoubleWord(std::span<std::uint8_t const> bytes,
                                               bool isBigEndian) noexcept
    {
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < 8; ++index)
        {
            auto const byte = static_cast<std::uint64_t>(bytes[index]);
            value |= isBigEndian ? byte << (8U * (7U - index)) : byte << (8U * index);
        }
        return value;
    }
} // namespace

std::string_view describe(CaptureError error) noexcept
{
    switch (error)
    {
        case CaptureError::Unreadable: return "the file could not be read";
        case CaptureError::NotPcap: return "not a pcap file";
        case CaptureError::IsPcapNg: return "this is pcapng; capture with -F pcap";
        case CaptureError::NotUsbmon: return "not a capture of a usbmon interface";
    }
    return "unknown error";
}

std::expected<std::vector<CapturedUrb>, CaptureError> readUsbmonCapture(std::string const& path)
{
    std::ifstream file { path, std::ios::binary };
    if (!file)
        return std::unexpected(CaptureError::Unreadable);

    std::vector<std::uint8_t> const blob { std::istreambuf_iterator<char> { file },
                                           std::istreambuf_iterator<char> {} };
    if (blob.size() < FileHeaderSize)
        return std::unexpected(CaptureError::Unreadable);

    auto const magic = std::span { blob }.first(4);
    bool isBigEndian = false;
    if (magic[0] == 0xa1 && magic[1] == 0xb2)
        isBigEndian = true;
    else if (magic[0] == 0xd4 && magic[1] == 0xc3)
        isBigEndian = false;
    else if (magic[0] == 0x4d && magic[1] == 0x3c)
        isBigEndian = false;
    else if (magic[0] == 0x0a && magic[1] == 0x0d)
        return std::unexpected(CaptureError::IsPcapNg);
    else
        return std::unexpected(CaptureError::NotPcap);

    auto const linkType = readWord(std::span { blob }.subspan(LinkTypeOffset), isBigEndian);
    if (linkType != LinkTypeUsbLinux && linkType != LinkTypeUsbLinuxMmapped)
        return std::unexpected(CaptureError::NotUsbmon);

    std::vector<CapturedUrb> urbs;
    std::size_t offset = FileHeaderSize;
    while (offset + RecordHeaderSize <= blob.size())
    {
        auto const captured = readWord(std::span { blob }.subspan(offset + 8), isBigEndian);
        offset += RecordHeaderSize;

        auto const available = std::min(static_cast<std::size_t>(captured), blob.size() - offset);
        auto const record = std::span { blob }.subspan(offset, available);
        offset += available;

        if (record.size() < UsbmonHeaderSize)
            continue;

        auto const urbType = record[UrbTypeOffset];
        if (urbType != SubmitMarker && urbType != CompleteMarker)
            continue;

        auto const seconds =
            static_cast<std::int64_t>(readDoubleWord(record.subspan(TimestampSecondsOffset), isBigEndian));
        auto const microseconds = static_cast<std::int32_t>(
            readWord(record.subspan(TimestampMicrosecondsOffset), isBigEndian));

        auto const body = record.subspan(UsbmonHeaderSize);
        urbs.push_back(CapturedUrb {
            .transferType = record[TransferTypeOffset],
            .endpoint = record[EndpointOffset],
            .device = record[DeviceOffset],
            .urbLength = readWord(record.subspan(UrbLengthOffset), isBigEndian),
            .isCompletion = urbType == CompleteMarker,
            .timestampMicroseconds = (seconds * 1'000'000) + microseconds,
            .payload = { body.begin(), body.end() },
        });
    }

    return urbs;
}

} // namespace ax310::tools
