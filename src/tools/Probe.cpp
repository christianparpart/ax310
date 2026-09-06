// SPDX-License-Identifier: Apache-2.0
/// Asks the AX310 what its registers hold, and toggles the effects it is safe to
/// toggle.
///
/// Replaces a pile of one-off Python that re-implemented the framing and the
/// checksum. Everything on the wire here comes from Protocol.hpp, so this tool
/// cannot disagree with the driver about what a command looks like.
///
/// It deliberately does **not** go through Device: connect() replays the captured
/// sequence, which is somebody's saved configuration, and a tool meant for
/// looking at a deck must not reconfigure it on the way in.

#include "UsbmonCapture.hpp"

#include <ax310/HidApiTransport.hpp>
#include <ax310/IConsole.hpp>
#include <ax310/Protocol.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace ax310;

/// Framed commands this tool will send, and nothing else.
///
/// Every one of these is observed on the wire with the value it is given here, so
/// sending one stays inside what the hardware already expects. That matters: a
/// blind write of 0x01 to address 0x16 wedged this deck badly enough to need a
/// power cycle, and an allow-list is the whole safety story.
constexpr std::array<protocol::FramedCommand, 2> ToggleableEffects {
    protocol::FramedCommand::DelayEffectEnable,
    protocol::FramedCommand::CompressorEnable,
};

/// Builds one framed single-byte command: fe 00 06 <command> <value> <checksum>.
[[nodiscard]] protocol::Payload framedEnable(protocol::FramedCommand command, std::uint8_t value) noexcept
{
    protocol::Payload payload {};
    payload[0] = protocol::FramedCommandMarker;
    payload[1] = 0x00;
    payload[2] = 0x06;
    payload[3] = static_cast<std::uint8_t>(command);
    payload[4] = value;
    payload[5] = protocol::framedChecksum(payload);
    return payload;
}

/// Takes std::string rather than string_view deliberately: from_chars needs a
/// pointer pair, and clang-tidy rightly objects to producing one from a view
/// whose backing may not be contiguous with what the callee expects.
[[nodiscard]] std::optional<unsigned> parseHex(std::string const& text)
{
    unsigned value = 0;
    auto const* const first = text.c_str();
    auto const* const last = first + text.size();
    auto const result = std::from_chars(first, last, value, 16);
    if (result.ec != std::errc {} || result.ptr != last)
        return std::nullopt;

    return value;
}

/// Opens the deck's control interface, without initialising anything.
[[nodiscard]] bool openControl(HidApiTransport& transport, IConsole& console)
{
    if (auto const started = transport.initialize(); !started)
    {
        writeErrorLine(console, "hidapi would not start: {}", describe(started.error()));
        return false;
    }

    auto const descriptor = protocol::descriptorFor(DeviceMode::Control);
    auto const interfaces = transport.enumerate(protocol::VendorId, descriptor.productId);
    for (auto const& candidate: interfaces)
    {
        if (candidate.interfaceNumber != descriptor.interfaceNumber && candidate.interfaceNumber != -1)
            continue;

        if (transport.open(candidate.path))
            return true;
    }

    writeErrorLine(console, "no control-mode AX310 could be opened -- a VM holding the device "
                 "takes it away from the host entirely");
    return false;
}

/// Reads one register, checking the reply echoes what was asked for.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> readRegister(HidApiTransport& transport,
                                                                    std::uint8_t address,
                                                                    std::size_t length)
{
    auto const request = protocol::frameFeatureReport(protocol::getPropertyAt(address, length));
    if (!transport.sendFeatureReport(request))
        return std::nullopt;

    protocol::FeatureReport reply {};
    auto const read = transport.getFeatureReport(reply);
    if (!read || *read < protocol::ReplyValuesOffset + length)
        return std::nullopt;

    auto const values = protocol::propertyReplyValues(reply, address);
    if (!values || values->size() < length)
        return std::nullopt;

    return std::vector<std::uint8_t> { values->begin(), values->begin() + static_cast<long>(length) };
}

void dumpKnownRegisters(HidApiTransport& transport, IConsole& console)
{
    // Reading 0x0f between the interesting ones scrubs the reply buffer: it
    // answers zeroes, so anything left over from a previous read becomes obvious
    // rather than being mistaken for an answer.
    for (auto const& preserved: protocol::PreservedAddresses)
    {
        auto const values = readRegister(transport, preserved.address, preserved.length);
        auto const name = protocol::nameIn(protocol::PropertyNames, preserved.address);

        if (!values)
        {
            writeLine(console, "  0x{:02x}  {:<38}  <no answer>", preserved.address, name);
            continue;
        }

        std::string text;
        for (auto const byte: *values)
            text += std::format("{:02x} ", byte);

        writeLine(console, "  0x{:02x}  {:<38}  {}",
                     preserved.address,
                     name.empty() ? std::string_view { "" } : name,
                     text);

        static_cast<void>(readRegister(transport, static_cast<std::uint8_t>(protocol::Property::DisplayPower), 1));
    }
}

/// Writes one track's level in one mix.
[[nodiscard]] bool setLevel(HidApiTransport& transport, IConsole& console,
                            protocol::Property block, KnobId knob, std::uint8_t level)
{
    auto const address = protocol::levelAddressOf(block, knob);
    std::array<std::uint8_t, 1> const values { level };
    auto const framed = protocol::frameFeatureReport(protocol::setPropertyAt(address, values));
    if (!transport.sendFeatureReport(framed))
        return false;

    writeLine(console, "{} level 0x{:02x} = {}", nameOf(knob), address, level);
    return true;
}

/// Watches the deck's per-track meters and reports what each one peaked at.
///
/// The routing map in scripts/setup-audio.sh was established by ear, because the
/// meters were misread at the time as a single stereo mix. They are one per
/// track, so a tone played into one sink now names its own channel: run this,
/// play into a sink, and read which track moved.
///
/// It cannot confirm a level register. The meters are pre-fader and read the same
/// at 0% as at 100%, which is a separate question and still needs an ear.
///
/// @param transport An open control interface.
/// @param seconds How long to watch.
/// @return Process status.
int watchMeters(HidApiTransport& transport, IConsole& console, int seconds)
{
    std::array<int, KnobCount> peaks {};
    std::array<std::uint8_t, protocol::PaddedReportSize + 1> buffer {};

    std::string header;
    for (auto const knob: AllKnobs)
        header += std::format("{:>9}", nameOf(knob));
    writeLine(console, "{}", header);

    auto const started = std::chrono::steady_clock::now();
    auto const deadline = started + std::chrono::seconds { seconds };
    auto nextLine = started;
    std::size_t reports = 0;

    while (std::chrono::steady_clock::now() < deadline)
    {
        auto const bytesRead = transport.read(buffer, std::chrono::milliseconds { 200 });
        if (!bytesRead)
        {
            writeErrorLine(console, "the read failed");
            return EXIT_FAILURE;
        }
        if (*bytesRead == 0)
            continue;

        auto const payload = protocol::reportPayload(std::span { buffer }.first(*bytesRead));
        if (payload.empty())
            continue;

        auto const report = protocol::decodeReport(payload);
        ++reports;
        for (auto const knob: AllKnobs)
        {
            auto const index = indexOf(knob);
            peaks[index] = std::max(peaks[index], report.audioMeters[index]);
        }

        // The deck sends far faster than anybody can read, so the live line is
        // rate limited and the peaks -- which are what the measurement is for --
        // are taken from every report.
        auto const now = std::chrono::steady_clock::now();
        if (now < nextLine)
            continue;
        nextLine = now + std::chrono::milliseconds { 200 };

        std::string line;
        for (auto const knob: AllKnobs)
            line += std::format("{:>8}%", protocol::toPercent(report.audioMeters[indexOf(knob)]));
        writeLine(console, "{}", line);
    }

    std::string summary;
    for (auto const knob: AllKnobs)
        summary += std::format("{:>8}%", protocol::toPercent(peaks[indexOf(knob)]));
    writeLine(console, "");
    writeLine(console, "{}", header);
    writeLine(console, "{}   <- peak over {} reports", summary, reports);

    // The same six numbers again, in knob order and nothing else on the line, so
    // a script can read them. scripts/setup-audio.sh identifies each channel this
    // way instead of playing a tone and asking somebody which knob answered.
    std::string machine = "peak";
    for (auto const knob: AllKnobs)
        machine += std::format(" {}", protocol::toPercent(peaks[indexOf(knob)]));
    writeLine(console, "{}", machine);

    if (reports == 0)
        writeErrorLine(console, "the deck sent nothing -- is another program holding it?");

    return reports > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

int usage(IConsole& console, std::string_view program)
{
    writeErrorLine(console, "usage: {} <command>", program);
    writeErrorLine(console, "  --dump                 read every register the handshake overwrites");
    writeErrorLine(console, "  --read <addr> <len>    read one register, both decimal or 0x-free hex");
    writeErrorLine(console, "  --effect <cmd> <0|1>   toggle one known effect enable");
    writeErrorLine(console, "  --level <1|2> <knob> <00..14>  set one track's level in one mix (hex)");
    writeErrorLine(console, "  --meters [seconds]     watch the per-track meters, and report their peaks");
    writeErrorLine(console, "");
    writeErrorLine(console, "  knobs, as printed on the deck:");
    writeErrorLine(console, "    {}", []() {
        std::string names;
        for (auto const knob: AllKnobs)
            names += std::format("{}  ", nameOf(knob));
        return names;
    }());
    writeErrorLine(console, "");
    writeErrorLine(console, "  effects that may be toggled:");
    for (auto const effect: ToggleableEffects)
        writeErrorLine(console, "    {:02x}  {}",
                     static_cast<std::uint8_t>(effect),
                     protocol::nameIn(protocol::FramedCommandNames, static_cast<std::uint8_t>(effect)));
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char* argv[])
{
    SystemConsole console;

    auto const arguments = std::span { argv, static_cast<std::size_t>(argc) };
    if (argc < 2)
        return usage(console, arguments[0]);

    auto const command = std::string_view { arguments[1] };

    HidApiTransport transport;
    if (!openControl(transport, console))
        return EXIT_FAILURE;

    if (command == "--dump")
    {
        dumpKnownRegisters(transport, console);
        return EXIT_SUCCESS;
    }

    if (command == "--meters" && argc <= 3)
    {
        auto const seconds = argc == 3 ? parseHex(arguments[2]) : std::optional<unsigned> { 10 };
        if (!seconds || *seconds == 0 || *seconds > 600)
        {
            writeErrorLine(console, "--meters takes a number of seconds, 1 to 600");
            return EXIT_FAILURE;
        }

        return watchMeters(transport, console, static_cast<int>(*seconds));
    }

    if (command == "--read" && argc == 4)
    {
        auto const address = parseHex(arguments[2]);
        auto const length = parseHex(arguments[3]);
        if (!address || !length || *address > 0xff || *length == 0 || *length > protocol::MaxPreservedLength)
        {
            writeErrorLine(console, "address must be 00..ff and length 1..{}", protocol::MaxPreservedLength);
            return EXIT_FAILURE;
        }

        auto const values = readRegister(transport, static_cast<std::uint8_t>(*address), *length);
        if (!values)
        {
            writeErrorLine(console, "0x{:02x} did not answer", *address);
            return EXIT_FAILURE;
        }

        for (auto const byte: *values)
            write(console, "{:02x} ", byte);
        writeLine(console, "");
        return EXIT_SUCCESS;
    }

    if (command == "--level" && argc == 5)
    {
        auto const mix = std::string_view { arguments[2] };
        auto const knob = knobFromName(arguments[3]);
        auto const level = parseHex(arguments[4]);
        if ((mix != "1" && mix != "2") || !knob || !level || *level > 0x14)
        {
            writeErrorLine(console, "usage: --level <1|2> <knob name> <00..14, hex like the other commands>");
            return EXIT_FAILURE;
        }

        auto const block =
            mix == "1" ? protocol::Property::CreatorMixLevels : protocol::Property::AudienceMixLevels;
        return setLevel(transport, console, block, *knob, static_cast<std::uint8_t>(*level))
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
    }

    if (command == "--effect" && argc == 4)
    {
        auto const which = parseHex(arguments[2]);
        auto const value = parseHex(arguments[3]);
        if (!which || !value || *value > 1)
            return usage(console, arguments[0]);

        auto const wanted = static_cast<protocol::FramedCommand>(*which);
        if (std::ranges::find(ToggleableEffects, wanted) == ToggleableEffects.end())
        {
            writeErrorLine(console, "refusing 0x{:02x}: only confirmed effect enables may be toggled",
                         *which);
            return EXIT_FAILURE;
        }

        auto const framed = protocol::frameFeatureReport(framedEnable(wanted, static_cast<std::uint8_t>(*value)));
        if (!transport.sendFeatureReport(framed))
        {
            writeErrorLine(console, "the write failed");
            return EXIT_FAILURE;
        }

        writeLine(console, "sent {} = 0x{:02x}",
                     protocol::nameIn(protocol::FramedCommandNames, static_cast<std::uint8_t>(wanted)),
                     *value);
        return EXIT_SUCCESS;
    }

    return usage(console, arguments[0]);
}
