// SPDX-License-Identifier: Apache-2.0
/// Turns a usbmon capture of the AX310 into a readable list of commands.
///
/// The point of a capture is the handful of bytes the host sent, and a raw
/// capture buries them: the vendor software redraws the deck's panel at about
/// thirty frames a second, so one idle capture held eleven thousand screen chunks
/// and not one command. This prints just the host-to-device commands, in the
/// grammar from AGENT.md, using the names in Protocol.hpp -- so the tool cannot
/// drift from the driver's own idea of what a register is called.

#include "UsbmonCapture.hpp"

#include <ax310/IConsole.hpp>
#include <ax310/Protocol.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace ax310;

/// usbmon transfer types this cares about.
constexpr std::uint8_t InterruptTransfer = 1;
constexpr std::uint8_t ControlTransfer = 2;

/// The deck's screen output report. Anything this size is a frame chunk rather
/// than a command, and there are a great many of them.
constexpr std::uint32_t ScreenChunkUrbSize = 1024;

/// How many body bytes to print before giving up on one line.
constexpr std::size_t MaxPrintedBytes = 24;

[[nodiscard]] std::string hexOf(std::span<std::uint8_t const> bytes)
{
    std::string text;
    for (auto const byte: bytes.first(std::min(bytes.size(), MaxPrintedBytes)))
        text += std::format("{:02x} ", byte);

    if (!text.empty())
        text.pop_back();

    return text;
}

/// @param payload One command payload.
/// @return A line describing it, or nothing when it carries nothing at all.
[[nodiscard]] std::optional<std::string> describeCommand(std::span<std::uint8_t const> payload)
{
    if (payload.empty())
        return std::nullopt;

    // Tolerate a leading report-id byte. Our own writes do not carry one on the
    // wire, and the vendor software's framing is not established.
    if (payload[0] == 0x00 && payload.size() > 1
        && (payload[1] == static_cast<std::uint8_t>(protocol::CommandKind::Set)
            || payload[1] == static_cast<std::uint8_t>(protocol::CommandKind::Get)
            || payload[1] == protocol::FramedCommandMarker))
        payload = payload.subspan(1);

    auto const head = payload[0];
    auto const isProperty = head == static_cast<std::uint8_t>(protocol::CommandKind::Set)
                            || head == static_cast<std::uint8_t>(protocol::CommandKind::Get);

    if (isProperty && payload.size() > 3 && payload[1] == protocol::PropertyGroup)
    {
        auto const* const kind =
            head == static_cast<std::uint8_t>(protocol::CommandKind::Set) ? "SET" : "GET";
        auto const address = payload[2];
        std::size_t const length = payload[3];
        auto const name = protocol::nameIn(protocol::PropertyNames, address);
        auto const values = payload.subspan(4, std::min(length, payload.size() - 4));

        return std::format("{} 0x{:02x} len {:<3} [{}] {}",
                           kind,
                           address,
                           length,
                           name.empty() ? std::string_view { "?" } : name,
                           hexOf(values));
    }

    if (head == protocol::FramedCommandMarker && payload.size() > 3)
    {
        std::size_t const total = payload[2];
        auto const command = payload[3];
        auto const name = protocol::nameIn(protocol::FramedCommandNames, command);

        // The length counts the whole command, so the body is what sits between
        // the command byte and the trailing checksum.
        auto const bodyLength = total > 5 ? std::min(total - 5, payload.size() - 4) : std::size_t { 0 };
        auto const body = payload.subspan(4, bodyLength);

        return std::format("CMD 0x{:02x} len {:<3} [{}] {}",
                           command,
                           total,
                           name.empty() ? std::string_view { "?" } : name,
                           hexOf(body));
    }

    if (std::ranges::any_of(payload, [](std::uint8_t byte) { return byte != 0; }))
        return std::format("raw {}", hexOf(payload));

    return std::nullopt;
}

/// A summary of what the deck sent back.
struct Inbound
{
    std::size_t reports = 0;
    std::size_t shortest = 0;
    std::size_t longest = 0;
    /// Distinct values seen at each byte offset, so a moving field stands out.
    std::array<std::set<std::uint8_t>, protocol::PaddedReportSize> values;
};

/// Summarises device-to-host reports.
///
/// Every capture so far has been read for what the host *sent*, which is what
/// identifies a command. It says nothing about what the deck streams back --
/// and the question of where the meters live is entirely about that. A byte
/// that takes many values while audio plays is a level; one that takes two or
/// three is a flag; one that never moves is structure.
[[nodiscard]] Inbound summariseInbound(std::vector<tools::CapturedUrb> const& urbs)
{
    Inbound summary;
    for (auto const& urb: urbs)
    {
        // Inbound bytes exist only at completion.
        if (!urb.isInbound() || !urb.isCompletion || urb.payload.empty())
            continue;

        if (urb.transferType != InterruptTransfer && urb.transferType != ControlTransfer)
            continue;

        // The deck interleaves an all-zero report between real ones; counting it
        // would make every byte look like it moves.
        if (std::ranges::none_of(urb.payload, [](std::uint8_t byte) { return byte != 0; }))
            continue;

        ++summary.reports;
        auto const size = urb.payload.size();
        summary.shortest = summary.shortest == 0 ? size : std::min(summary.shortest, size);
        summary.longest = std::max(summary.longest, size);

        for (std::size_t index = 0; index < std::min(size, protocol::PaddedReportSize); ++index)
            summary.values[index].insert(urb.payload[index]);
    }

    return summary;
}

/// One host-to-device command, and when it went out.
struct TimedCommand
{
    std::string text;

    /// Microseconds since the command before it, or 0 for the first.
    ///
    /// The gap rather than the absolute stamp, because the gap is the question:
    /// this deck applies only some of a run of records sent too close together,
    /// so what the vendor's software waited between two writes is a fact of the
    /// protocol and not an artefact of the capture.
    std::int64_t sincePreviousMicroseconds = 0;
};

/// @param microseconds A gap between two commands, or 0 for the first.
/// @return It as a column: blank for the first command, milliseconds otherwise.
///
/// Milliseconds because that is the scale the deck cares about -- its handshake
/// wants ten between payloads -- and a microsecond column would be six digits of
/// noise around the one number worth reading.
[[nodiscard]] std::string gapText(std::int64_t microseconds)
{
    if (microseconds == 0)
        return {};

    return std::format("+{}.{:03d}ms", microseconds / 1000, microseconds % 1000);
}

/// What one capture contained.
struct Decoded
{
    std::vector<TimedCommand> commands;
    std::size_t screenChunks = 0;
};

[[nodiscard]] Decoded decode(std::vector<tools::CapturedUrb> const& urbs)
{
    Decoded decoded;
    std::int64_t previous = 0;
    for (auto const& urb: urbs)
    {
        // Completions carry no new outbound payload; counting them would report
        // every command twice.
        if (urb.isInbound() || urb.isCompletion)
            continue;

        if (urb.transferType != ControlTransfer && urb.transferType != InterruptTransfer)
            continue;

        if (urb.urbLength >= ScreenChunkUrbSize)
        {
            ++decoded.screenChunks;
            continue;
        }

        auto const payload =
            std::span { urb.payload }.first(std::min(urb.payload.size(), protocol::PaddedReportSize));
        if (auto line = describeCommand(payload); line)
        {
            decoded.commands.push_back(TimedCommand {
                .text = std::move(*line),
                .sincePreviousMicroseconds =
                    previous == 0 ? 0 : urb.timestampMicroseconds - previous,
            });
            previous = urb.timestampMicroseconds;
        }
    }

    return decoded;
}

[[nodiscard]] std::optional<Decoded> load(std::string const& path, IConsole& console)
{
    auto const capture = tools::readUsbmonCapture(path);
    if (!capture)
    {
        writeErrorLine(console, "{}: {}", path, tools::describe(capture.error()));
        return std::nullopt;
    }

    return decode(*capture);
}

} // namespace

int main(int argc, char* argv[])
{
    SystemConsole console;
    auto const arguments = std::span { argv, static_cast<std::size_t>(argc) };
    if (argc < 2 || argc > 3)
    {
        writeErrorLine(console, "usage: {} [--inbound] <capture.pcap> [after.pcap]", arguments[0]);
        writeErrorLine(console, "  one file   : every host-to-device command in it");
        writeErrorLine(console, "  two files  : what the second one added");
        writeErrorLine(console, "  --inbound  : what the deck sent back, and which bytes move");
        return EXIT_FAILURE;
    }

    if (std::string_view { arguments[1] } == "--inbound" && argc == 3)
    {
        auto const capture = tools::readUsbmonCapture(arguments[2]);
        if (!capture)
        {
            writeErrorLine(console, "{}: {}", arguments[2], tools::describe(capture.error()));
            return EXIT_FAILURE;
        }

        auto const summary = summariseInbound(*capture);
        writeLine(console, "{} reports from the deck, {}..{} bytes",
                     summary.reports,
                     summary.shortest,
                     summary.longest);
        writeLine(console, "");
        writeLine(console, "byte  distinct  what it looks like");
        for (std::size_t index = 0; index < summary.values.size(); ++index)
        {
            auto const count = summary.values[index].size();
            if (count < 2)
                continue;

            std::string_view kind = "a flag";
            if (count > 24)
                kind = "a level";
            else if (count > 4)
                kind = "changes a lot";

            writeLine(console, "0x{:02x}  {:>8}  {}", index, count, kind);
        }

        return EXIT_SUCCESS;
    }

    auto const first = load(arguments[1], console);
    if (!first)
        return EXIT_FAILURE;

    if (argc == 2)
    {
        for (std::size_t index = 0; index < first->commands.size(); ++index)
            writeLine(console,
                      "{:3d}  {:>10}  {}",
                      index,
                      gapText(first->commands[index].sincePreviousMicroseconds),
                      first->commands[index].text);

        writeErrorLine(console, "\n{} host-to-device commands ({} screen chunks skipped)",
                     first->commands.size(),
                     first->screenChunks);
        return EXIT_SUCCESS;
    }

    auto const second = load(arguments[2], console);
    if (!second)
        return EXIT_FAILURE;

    std::size_t shown = 0;
    for (auto const& command: second->commands)
    {
        // Matched on the text alone. A gap is a property of when a command went
        // out rather than of what it is, so two runs of one action differ in it
        // every time and comparing it would call every command new.
        if (std::ranges::any_of(first->commands, [&](TimedCommand const& known) {
                return known.text == command.text;
            }))
            continue;

        writeLine(console,
                  "{:3d}  {:>10}  {}",
                  shown++,
                  gapText(command.sincePreviousMicroseconds),
                  command.text);
    }

    writeErrorLine(console, "\n{} new of {} (baseline had {}, {} screen chunks skipped)",
                 shown,
                 second->commands.size(),
                 first->commands.size(),
                 second->screenChunks);
    return EXIT_SUCCESS;
}
