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
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
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
    payload[3] = std::to_underlying(command);
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

    // The audience mix's levels are not in that table -- the handshake does not
    // write them, so there is nothing to preserve -- but a mixer question is
    // never about one block. Read here so both are on screen together, next to
    // the two registers that say which of them the deck is monitoring and which
    // the rings are showing.
    writeLine(console, "");

    for (auto const& extra: { protocol::PreservedAddress { .address = std::to_underlying(
                                                               protocol::Property::AudienceMixLevels),
                                                           .length = protocol::KnobPropertyLength } })
    {
        auto const values = readRegister(transport, extra.address, extra.length);
        auto const name = protocol::nameIn(protocol::PropertyNames, extra.address);

        if (!values)
        {
            writeLine(console, "  0x{:02x}  {:<38}  <no answer>", extra.address, name);
            continue;
        }

        std::string text;
        for (auto const byte: *values)
            text += std::format("{:02x} ", byte);

        writeLine(console, "  0x{:02x}  {:<38}  {}", extra.address, name, text);
    }
}

/// Sends one prepared request and collects the answer.
///
/// The identity groups answer in a shape of their own, so this does not go
/// through readRegister(), which checks that a property reply echoes its address.
///
/// @param transport An open control interface.
/// @param request The request to send, from Protocol.hpp.
/// @param reply Where to put the answer.
/// @return Whether an answer came back.
[[nodiscard]] bool ask(HidApiTransport& transport, protocol::Payload const& request,
                       protocol::FeatureReport& reply)
{
    if (!transport.sendFeatureReport(protocol::frameFeatureReport(request)))
        return false;

    return transport.getFeatureReport(reply).has_value();
}

/// Prints what the deck says it is: `--identify`.
///
/// The same two reads the vendor software opens with, formatted the way Creator
/// Central shows them, so the two can be compared without translating anything.
///
/// @param transport An open control interface.
/// @param console Where the narration goes.
/// @return Process status.
int runIdentify(HidApiTransport& transport, IConsole& console)
{
    protocol::FeatureReport reply {};
    if (!ask(transport, protocol::identityRequest(), reply))
    {
        writeErrorLine(console, "the deck did not answer the identity read");
        return EXIT_FAILURE;
    }

    auto const payload = protocol::reportPayload(reply);
    auto const version = protocol::parseFirmwareVersion(payload);
    if (!version)
    {
        writeErrorLine(console, "the identity reply was not the shape we know");
        return EXIT_FAILURE;
    }

    auto const stamp = [](std::array<std::uint8_t, 4> const& build) {
        return std::format("{:02}{:02}{:02}{:02}", build[0], build[1], build[2], build[3]);
    };

    writeLine(console,
              "firmware  {}.{} {}.{} ( {} / {} / {} / {:02x} / {:02x} )",
              version->version[0],
              version->version[1],
              version->secondVersion[0],
              version->secondVersion[1],
              stamp(version->buildA),
              stamp(version->buildB),
              stamp(version->buildC),
              version->codeA,
              version->codeB);

    protocol::FeatureReport serialReply {};
    if (!ask(transport, protocol::serialRequest(), serialReply))
    {
        writeErrorLine(console, "the deck did not answer the serial read");
        return EXIT_FAILURE;
    }

    auto const serial = protocol::parseSerialNumber(protocol::reportPayload(serialReply));
    if (serial.empty())
    {
        writeErrorLine(console, "the serial reply carried no digits");
        return EXIT_FAILURE;
    }

    writeLine(console, "serial    {}", std::string_view {
        reinterpret_cast<char const*>(serial.data()), serial.size() });
    return EXIT_SUCCESS;
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

/// What each button is called on the command line, indexed by Button. Short
/// because they are typed; nameOf(Button) is what output uses, since a person
/// reading a result wants the position spelled out.
inline constexpr std::array<std::string_view, ButtonCount> ButtonAbbreviations {
    "tl", "tr", "bl", "br"
};

/// @param name One of tl, tr, bl, br.
/// @return The button it names, or nothing.
[[nodiscard]] std::optional<Button> buttonFromName(std::string_view name)
{
    for (auto const button: AllButtons)
        if (name == ButtonAbbreviations[indexOf(button)])
            return button;

    return std::nullopt;
}

/// Lights one function button, or turns it off.
///
/// @param transport An open control interface.
/// @param console Where the narration goes.
/// @param button Which button.
/// @param red Red, 0 to 255.
/// @param green Green.
/// @param blue Blue.
/// @return Process status.
int setButtonColour(HidApiTransport& transport, IConsole& console, Button button,
                    std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
    auto const lit = red != 0 || green != 0 || blue != 0;
    auto const record =
        protocol::buttonColourRecord(protocol::selectorFor(button), red, green, blue, lit);
    auto const framed =
        protocol::frameFeatureReport(protocol::setPropertyAt(protocol::ButtonColourAddress, record));

    if (!transport.sendFeatureReport(framed))
    {
        writeErrorLine(console, "the write failed");
        return EXIT_FAILURE;
    }

    writeLine(console,
              "{} (selector 0x{:02x}) set to {:02x}{:02x}{:02x}{}",
              ButtonAbbreviations[indexOf(button)],
              protocol::selectorFor(button),
              red,
              green,
              blue,
              lit ? "" : ", which turns it off");
    return EXIT_SUCCESS;
}

/// Parses and performs `--button <tl|tr|bl|br> <rr> <gg> <bb>`.
/// @param transport An open control interface.
/// @param console Where the narration goes.
/// @param arguments The whole command line.
/// @return Process status.
int runButton(HidApiTransport& transport, IConsole& console, std::span<char* const> arguments)
{
    auto const which = buttonFromName(arguments[2]);
    auto const red = parseHex(arguments[3]);
    auto const green = parseHex(arguments[4]);
    auto const blue = parseHex(arguments[5]);

    if (!which || !red || !green || !blue || *red > 0xff || *green > 0xff || *blue > 0xff)
    {
        writeErrorLine(console, "usage: --button <tl|tr|bl|br> <rr> <gg> <bb>, all hex");
        return EXIT_FAILURE;
    }

    return setButtonColour(transport,
                           console,
                           *which,
                           static_cast<std::uint8_t>(*red),
                           static_cast<std::uint8_t>(*green),
                           static_cast<std::uint8_t>(*blue));
}

/// Sends one record to a light address and says so.
/// @param transport An open control interface.
/// @param console Where the narration goes.
/// @param address Which record address.
/// @param record The ten values.
/// @param narration What to print when it lands.
/// @return Process status.
int sendLightRecord(HidApiTransport& transport, IConsole& console, std::uint8_t address,
                    std::span<std::uint8_t const> record, std::string_view narration)
{
    auto const framed = protocol::frameFeatureReport(protocol::setPropertyAt(address, record));
    if (!transport.sendFeatureReport(framed))
    {
        writeErrorLine(console, "the write failed");
        return EXIT_FAILURE;
    }

    writeLine(console, "{}", narration);
    return EXIT_SUCCESS;
}

/// Parses and performs `--knob-colour <rr> <gg> <bb>`.
///
/// Colours the rings of whichever mix is selected, because the record cannot name a
/// mix. Switching mixes afterwards shows the other mix's colour, not this one.
///
/// @param transport An open control interface.
/// @param console Where the narration goes.
/// @param arguments The whole command line.
/// @return Process status.
int runKnobColour(HidApiTransport& transport, IConsole& console, std::span<char* const> arguments)
{
    auto const red = parseHex(arguments[2]);
    auto const green = parseHex(arguments[3]);
    auto const blue = parseHex(arguments[4]);

    if (!red || !green || !blue || *red > 0xff || *green > 0xff || *blue > 0xff)
    {
        writeErrorLine(console, "usage: --knob-colour <rr> <gg> <bb>, all hex");
        return EXIT_FAILURE;
    }

    auto const record = protocol::knobColourRecord(static_cast<std::uint8_t>(*red),
                                                   static_cast<std::uint8_t>(*green),
                                                   static_cast<std::uint8_t>(*blue));
    return sendLightRecord(transport,
                           console,
                           protocol::ButtonColourAddress,
                           record,
                           std::format("the rings of the selected mix set to {:02x}{:02x}{:02x}",
                                       *red,
                                       *green,
                                       *blue));
}

/// @param name What was typed.
/// @return The mode it names, or nothing. "off" is spelt separately by the caller.
[[nodiscard]] std::optional<SurroundMode> surroundModeFromName(std::string_view name)
{
    static constexpr std::array<std::string_view, SurroundModeCount> Spellings {
        "solid", "pulsing", "blinking", "pulsing-rgb", "blinking-rgb", "scrolling-rgb",
    };
    static_assert(rowsInEnumeratorOrder(AllSurroundModes));

    for (auto const mode: AllSurroundModes)
        if (name == Spellings[indexOf(mode)])
            return mode;
    return std::nullopt;
}

/// Parses and performs `--surround off` or `--surround <mode> <rate> <rr> <gg> <bb>`.
/// @param transport An open control interface.
/// @param console Where the narration goes.
/// @param arguments The whole command line.
/// @return Process status.
int runSurround(HidApiTransport& transport, IConsole& console, std::span<char* const> arguments)
{
    if (arguments.size() == 3 && std::string_view { arguments[2] } == "off")
        return sendLightRecord(transport,
                               console,
                               protocol::SurroundAddress,
                               protocol::surroundOffRecord(),
                               "the surround strip turned off");

    auto const mode = arguments.size() == 7 ? surroundModeFromName(arguments[2]) : std::nullopt;
    auto const rate = arguments.size() == 7 ? parseHex(arguments[3]) : std::nullopt;
    auto const red = arguments.size() == 7 ? parseHex(arguments[4]) : std::nullopt;
    auto const green = arguments.size() == 7 ? parseHex(arguments[5]) : std::nullopt;
    auto const blue = arguments.size() == 7 ? parseHex(arguments[6]) : std::nullopt;

    if (!mode || !rate || !red || !green || !blue || *rate > 0xff || *red > 0xff || *green > 0xff
        || *blue > 0xff)
    {
        writeErrorLine(console,
                       "usage: --surround off, or --surround "
                       "<solid|pulsing|blinking|pulsing-rgb|blinking-rgb|scrolling-rgb> "
                       "<freq> <rr> <gg> <bb>, all hex. Solid ignores freq; scale the "
                       "colour to dim, there is no brightness field");
        return EXIT_FAILURE;
    }

    auto const record = protocol::surroundRecord(*mode,
                                                 static_cast<std::uint8_t>(*rate),
                                                 static_cast<std::uint8_t>(*red),
                                                 static_cast<std::uint8_t>(*green),
                                                 static_cast<std::uint8_t>(*blue));
    return sendLightRecord(
        transport,
        console,
        protocol::SurroundAddress,
        record,
        std::format("surround set to {}{}{}",
                    nameOf(*mode),
                    isAnimated(*mode) ? std::format(" at frequency 0x{:02x}", *rate) : "",
                    cyclesHues(*mode) ? ", which cycles hues and ignores the colour" : ""));
}

/// Reads the optional trailing seconds argument the watching modes take.
/// @param arguments The whole command line.
/// @param fallback What to use when it was not given.
/// @return The seconds, or nothing if what was given is not a sane duration.
[[nodiscard]] std::optional<int> parseSeconds(std::span<char* const> arguments, unsigned fallback)
{
    auto const given = arguments.size() == 3 ? parseHex(arguments[2])
                                             : std::optional<unsigned> { fallback };
    if (!given || *given == 0 || *given > 600)
        return std::nullopt;

    return static_cast<int>(*given);
}

/// Sets the panel's brightness, or turns it off.
///
/// The display group is `[0x01][0x0a][level]` and the level is a percentage, with
/// 0xff meaning off. All three of those were captured from the vendor software.
///
/// @param transport An open control interface.
/// @param console Where the narration goes.
/// @param level The percentage, or protocol::PanelOffLevel.
/// @return Process status.
int setPanel(HidApiTransport& transport, IConsole& console, std::uint8_t level)
{
    if (!transport.sendFeatureReport(protocol::frameFeatureReport(protocol::displayCommand(level))))
    {
        writeErrorLine(console, "the write failed");
        return EXIT_FAILURE;
    }

    if (level == protocol::PanelOffLevel)
        writeLine(console, "sent 01 0a ff -- the panel should be dark; touch it to wake it");
    else
        writeLine(console, "sent 01 0a {:02x} -- the panel should be at {}%", level, level);

    return EXIT_SUCCESS;
}

/// Parses and performs `--panel-brightness <percent>`.
/// @param transport An open control interface.
/// @param console Where the narration goes.
/// @param arguments The whole command line.
/// @return Process status.
int runPanelBrightness(HidApiTransport& transport, IConsole& console,
                       std::span<char* const> arguments)
{
    auto const percent = parseHex(arguments[2]);
    if (!percent || *percent < static_cast<unsigned>(protocol::MinPanelBrightness)
        || *percent > static_cast<unsigned>(protocol::MaxPanelBrightness))
    {
        writeErrorLine(console,
                       "--panel-brightness takes {}..{}, in hex like the other commands; "
                       "the vendor's own slider goes no dimmer",
                       protocol::MinPanelBrightness,
                       protocol::MaxPanelBrightness);
        return EXIT_FAILURE;
    }

    return setPanel(transport, console, static_cast<std::uint8_t>(*percent));
}


/// What `--try` was asked to do.
struct TryRequest
{
    std::uint8_t address = 0;
    std::uint8_t value = 0;
    int seconds = 8;
    bool fenced = false;
};

/// Parses `--try <addr> <value> [seconds] [--fenced]`, in either order after the
/// value, because remembering which comes first is not worth a usage error.
///
/// @param arguments The whole command line.
/// @return What to do, or nothing if the arguments do not say.
[[nodiscard]] std::optional<TryRequest> parseTry(std::span<char* const> arguments)
{
    auto const address = parseHex(arguments[2]);
    auto const value = parseHex(arguments[3]);
    auto seconds = std::optional<unsigned> { 8 };
    bool fenced = false;

    for (std::size_t index = 4; index < arguments.size(); ++index)
    {
        if (std::string_view { arguments[index] } == "--fenced")
            fenced = true;
        else
            seconds = parseHex(arguments[index]);
    }

    if (!address || !value || *address > 0xff || *value > 0xff || !seconds || *seconds == 0
        || *seconds > 600)
        return std::nullopt;

    return TryRequest { .address = static_cast<std::uint8_t>(*address),
                        .value = static_cast<std::uint8_t>(*value),
                        .seconds = static_cast<int>(*seconds),
                        .fenced = fenced };
}

/// Writes one byte to one register, waits for somebody to look at the deck, and
/// puts the old value back.
///
/// This is the only generic write this tool has, and it is built the careful way
/// on purpose. Writing `0x01` to `0x16` once wedged a deck badly enough to need a
/// power cycle, which is why DangerousAddresses exists -- and why a tool that can
/// write anywhere would undo the whole point of it. So: the address is refused if
/// it is on that list, the old value is read first and restored afterwards
/// whatever happens, and the restore is verified by reading it back rather than
/// assumed.
///
/// It answers the questions that need eyes rather than a capture. `0x21` is
/// documented as both the mixer mode and which knob rings light, and nobody has
/// established which -- one write with the deck in view settles it.
///
/// @param transport An open control interface.
/// @param console Where the narration goes.
/// @param address The register to poke.
/// @param value The byte to write.
/// @param seconds How long to leave it there.
/// @param fenced Whether to wrap it in the settings transaction the vendor uses
///        for mode changes.
/// @return Process status.
int tryRegister(HidApiTransport& transport, IConsole& console, std::uint8_t address,
                std::uint8_t value, int seconds, bool fenced)
{
    if (std::ranges::find(protocol::DangerousAddresses, address) != protocol::DangerousAddresses.end())
    {
        writeErrorLine(console,
                       "refusing 0x{:02x}: writing there once wedged a deck badly enough to "
                       "need a power cycle",
                       address);
        return EXIT_FAILURE;
    }

    auto const before = readRegister(transport, address, 1);
    if (!before || before->empty())
    {
        writeErrorLine(console, "0x{:02x} did not answer, so there is nothing to put back", address);
        return EXIT_FAILURE;
    }

    auto const original = before->front();
    writeLine(console, "0x{:02x} currently holds 0x{:02x}", address, original);

    auto const write = [&](std::uint8_t byte) {
        std::array<std::uint8_t, 1> const values { byte };
        return transport.sendFeatureReport(protocol::frameFeatureReport(protocol::setPropertyAt(address, values)))
                   .has_value();
    };

    auto const fence = static_cast<std::uint8_t>(protocol::Property::SettingsTransaction);
    auto const writeFence = [&](std::uint8_t byte) {
        std::array<std::uint8_t, 1> const values { byte };
        return transport.sendFeatureReport(protocol::frameFeatureReport(protocol::setPropertyAt(fence, values)))
                   .has_value();
    };

    if (fenced && !writeFence(0x01))
    {
        writeErrorLine(console, "could not open the settings transaction");
        return EXIT_FAILURE;
    }

    if (!write(value))
    {
        writeErrorLine(console, "the write failed");
        if (fenced)
            static_cast<void>(writeFence(0x00));
        return EXIT_FAILURE;
    }

    if (fenced)
        static_cast<void>(writeFence(0x00));

    writeLine(console, "0x{:02x} = 0x{:02x} for {} seconds -- look at the deck now", address, value, seconds);
    std::this_thread::sleep_for(std::chrono::seconds { seconds });

    if (fenced)
        static_cast<void>(writeFence(0x01));
    auto const restored = write(original);
    if (fenced)
        static_cast<void>(writeFence(0x00));

    if (!restored)
    {
        writeErrorLine(console,
                       "COULD NOT PUT 0x{:02x} BACK. It still holds 0x{:02x}; set it with "
                       "--try {:02x} {:02x}",
                       address, value, address, original);
        return EXIT_FAILURE;
    }

    auto const after = readRegister(transport, address, 1);
    if (after && !after->empty() && after->front() == original)
        writeLine(console, "0x{:02x} put back to 0x{:02x}, and it reads back", address, original);
    else
        writeErrorLine(console, "0x{:02x} was written back but does not read as 0x{:02x}", address, original);

    return EXIT_SUCCESS;
}

/// Everything one --touches run measured.
struct TouchSummary
{
    std::size_t touches = 0;
    std::size_t contacts = 0;
    std::size_t othersBetween = 0;
    std::size_t gesturesWithOthers = 0;
    std::map<std::string, std::size_t> midGestureKinds;
    std::string contactLog;
    std::map<std::uint8_t, std::size_t> seen;
    std::map<std::uint8_t, std::size_t> startsAContact;
    std::map<std::uint8_t, std::size_t> movedWith;
    std::map<std::uint8_t, int> travelWith;
    std::map<std::uint8_t, int> furthestWith;
    std::map<std::pair<std::uint8_t, std::uint8_t>, std::size_t> followedBy;
};

/// Prints what a run measured. Separate from the loop that measures it, so
/// neither has to be read while thinking about the other.
/// @param console Where it goes.
/// @param run What was measured.
void reportTouchSummary(IConsole& console, TouchSummary const& run)
{
    writeLine(console, "");
    writeLine(console, "{} touch reports across {} contacts.", run.touches, run.contacts);
    writeLine(console, "");
    writeLine(console, "How often each value appeared, and how often it began a contact:");
    for (auto const& [flags, count]: run.seen)
        writeLine(console,
                  "  0x{:02x}  {:>4} times, {:>4} of them first",
                  flags,
                  count,
                  run.startsAContact.contains(flags) ? run.startsAContact.at(flags) : 0);

    writeLine(console, "");
    writeLine(console, "Each contact, and where along it the value changed:");
    writeLine(console, "{}", run.contactLog);

    writeLine(console, "");
    writeLine(console, "And how much the finger was moving when each value was reported:");
    writeLine(console, "  {:>5}  {:>10}  {:>10}  {:>10}", "flags", "moving", "mean step", "worst step");
    for (auto const& [flags, count]: run.seen)
    {
        auto const moving = run.movedWith.contains(flags) ? run.movedWith.at(flags) : 0;
        auto const travel = run.travelWith.contains(flags) ? run.travelWith.at(flags) : 0;
        writeLine(console,
                  "   0x{:02x}  {:>4} of {:<4}  {:>10}  {:>10}",
                  flags,
                  moving,
                  count,
                  count > 0 ? travel / static_cast<int>(count) : 0,
                  run.furthestWith.contains(flags) ? run.furthestWith.at(flags) : 0);
    }

    writeLine(console, "");
    writeLine(console, "Which value follows which, within one contact:");
    for (auto const& [pair, count]: run.followedBy)
        writeLine(console, "  0x{:02x} -> 0x{:02x}  {:>4}", pair.first, pair.second, count);

    writeLine(console, "");
    writeLine(console,
              "Non-touch reports arriving mid-gesture: {} of them, on {} occasions.",
              run.othersBetween,
              run.gesturesWithOthers);
    writeLine(console,
              "All-zero reports are excluded, because Device drops those before anything "
              "sees them. What is left is what would end a drag: Device treats the first "
              "of these as the finger lifting.");
    if (!run.midGestureKinds.empty())
    {
        writeLine(console, "");
        writeLine(console, "Their first eight bytes, which is where a lift marker would be:");
        for (auto const& [shape, count]: run.midGestureKinds)
            writeLine(console, "  {}  x{}", shape, count);
    }

}

/// Prints every screen touch with the byte nobody has explained, and then the
/// thing a tally cannot show: which value follows which.
///
/// Report offset 0x01 carries seven distinct values across every capture so far
/// and is not speed and not finger count -- both were tested and both refuted.
/// The first run of this printed only a tally, and a tally cannot distinguish a
/// counter from a category: 0x10, 0x14, 0x18 and 0x1c are 0x10 | (n << 2) for
/// n = 0..3, which looks like a two-bit field, but they came back 94, 50, 12 and
/// 166 times, which no counter does. Transitions separate the two readings.
///
/// The deck does not announce a lift: it simply stops sending touch reports, so a
/// gap marks the end of a contact. That is how Device decides Released too.
///
/// @param transport An open control interface.
/// @param console Where the touches are printed.
/// @param seconds How long to watch.
/// @return Process status.
int watchTouches(HidApiTransport& transport, IConsole& console, int seconds)
{
    using Clock = std::chrono::steady_clock;

    std::array<std::uint8_t, protocol::PaddedReportSize + 1> buffer {};
    std::map<std::uint8_t, std::size_t> seen;
    std::map<std::pair<std::uint8_t, std::uint8_t>, std::size_t> followedBy;
    std::map<std::uint8_t, std::size_t> startsAContact;
    std::map<std::uint8_t, std::size_t> movedWith;
    std::map<std::uint8_t, int> travelWith;
    std::map<std::uint8_t, int> furthestWith;

    // One line per contact: what the value did, and how far and how long into the
    // contact it did it. Per-step movement cannot separate a value that tracks
    // instantaneous speed from one that accumulates, and the two runs so far
    // disagree about which this is.
    std::string contactLog;
    std::size_t closingReports = 0;
    int closingTravel = 0;
    std::size_t reportsInContact = 0;
    int travelInContact = 0;
    auto contactBegan = Clock::now();

    // Every report that is not a touch, counted between touches. Device treats
    // the first of these as the finger lifting, so if they arrive mid-gesture the
    // driver is reporting a release while the finger is still down.
    std::size_t othersBetween = 0;
    std::size_t gesturesWithOthers = 0;
    std::size_t othersSinceTouch = 0;

    // What those reports actually are. If the deck marks a lift at all, it is in
    // here -- and if they are all one shape, it does not, and release has to be
    // inferred some other way.
    std::map<std::string, std::size_t> midGestureKinds;

    std::size_t touches = 0;
    std::size_t contacts = 0;
    std::optional<std::uint8_t> previousFlags;
    int previousX = 0;
    int previousY = 0;
    auto previousAt = Clock::now();

    // A held finger repeats at about five reports a second, so a third of a
    // second of silence is a lift rather than a pause.
    constexpr auto ContactGap = std::chrono::milliseconds { 300 };

    writeLine(console, "{:>5}  {:>5}  {:>5}  {:>6}  {:>6}  {}", "flags", "x", "y", "dx", "dy", "");

    auto const deadline = Clock::now() + std::chrono::seconds { seconds };
    while (Clock::now() < deadline)
    {
        auto const bytesRead = transport.read(buffer, std::chrono::milliseconds { 100 });
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

        if (payload[protocol::EventTypeOffset] != protocol::ScreenTouchEventType)
        {
            // Device drops an all-zero report before anything sees it -- the deck
            // interleaves them and decoding one would release every held button.
            // This has to drop them too, or it measures reports the driver never
            // acts on and calls them a defect. It did exactly that once.
            if (std::ranges::all_of(payload, [](std::uint8_t byte) { return byte == 0; }))
                continue;

            // Only interesting once a finger is down and before it has lifted.
            if (previousFlags.has_value() && (Clock::now() - previousAt) <= ContactGap)
            {
                ++othersSinceTouch;
                std::string shape;
                for (std::size_t index = 0; index < 8 && index < payload.size(); ++index)
                    shape += std::format("{:02x} ", payload[index]);
                ++midGestureKinds[shape];
            }
            continue;
        }

        if (othersSinceTouch > 0)
        {
            othersBetween += othersSinceTouch;
            ++gesturesWithOthers;
            othersSinceTouch = 0;
        }

        auto const report = protocol::decodeReport(payload);
        auto const now = Clock::now();
        auto const isNewContact = !previousFlags.has_value() || (now - previousAt) > ContactGap;

        ++touches;
        ++seen[report.touchFlags];
        if (isNewContact)
        {
            if (contacts > 0)
                contactLog += "\n";
            ++contacts;
            ++startsAContact[report.touchFlags];
            reportsInContact = 0;
            travelInContact = 0;
            contactBegan = now;
            if (contacts > 1)
                contactLog += std::format("   [{} reports, {} px]", closingReports, closingTravel);
            contactLog += std::format("\n  contact {:<3} 0x{:02x}", contacts, report.touchFlags);
        }
        else
            ++followedBy[{ *previousFlags, report.touchFlags }];

        // How far the finger moved since the previous report, which is what tells
        // a movement flag from a counter.
        int const step = isNewContact ? 0
                                       : std::abs(report.touchX - previousX)
                                             + std::abs(report.touchY - previousY);
        travelWith[report.touchFlags] += step;
        furthestWith[report.touchFlags] = std::max(furthestWith[report.touchFlags], step);
        if (step > 0)
            ++movedWith[report.touchFlags];

        writeLine(console,
                  " 0x{:02x}  {:>5}  {:>5}  {:>6}  {:>6}  {}",
                  report.touchFlags,
                  report.touchX,
                  report.touchY,
                  isNewContact ? 0 : report.touchX - previousX,
                  isNewContact ? 0 : report.touchY - previousY,
                  isNewContact ? "<- new contact" : "");

        ++reportsInContact;
        travelInContact += step;
        closingReports = reportsInContact;
        closingTravel = travelInContact;
        if (!isNewContact && report.touchFlags != *previousFlags)
        {
            auto const elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - contactBegan);
            contactLog += std::format(" -> 0x{:02x} after {} reports, {} px, {} ms",
                                      report.touchFlags,
                                      reportsInContact,
                                      travelInContact,
                                      elapsed.count());
        }

        previousFlags = report.touchFlags;
        previousX = report.touchX;
        previousY = report.touchY;
        previousAt = now;
    }

    reportTouchSummary(console,
                       { .touches = touches,
                         .contacts = contacts,
                         .othersBetween = othersBetween,
                         .gesturesWithOthers = gesturesWithOthers,
                         .midGestureKinds = midGestureKinds,
                         .contactLog = contactLog + std::format("   [{} reports, {} px]", closingReports, closingTravel),
                         .seen = seen,
                         .startsAContact = startsAContact,
                         .movedWith = movedWith,
                         .travelWith = travelWith,
                         .furthestWith = furthestWith,
                         .followedBy = followedBy });

    if (touches == 0)
        writeErrorLine(console, "nothing arrived -- is another program holding the deck?");

    return touches > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
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

/// One button or one knob, as the walk below saw it.
struct InputObservation
{
    std::uint8_t raw = 0;   ///< The byte as it arrived, before any masking.
    bool sawPress = false;
    bool sawRelease = false;
    std::chrono::milliseconds releaseAfter { 0 };
};

/// Everything the walk measured, so the printing is separate from the measuring.
struct InputWalk
{
    std::array<InputObservation, ButtonCount> buttons {};
    std::array<InputObservation, KnobCount> knobs {};
    std::uint8_t together = 0; ///< The byte seen with two buttons held at once.
    bool sawTogether = false;

    /// Whether every button and every knob was actually pressed.
    ///
    /// A walk that gave up on a round says nothing about the rounds after it. A
    /// press that arrives just after its own round's deadline satisfies the next
    /// one, so one slow start attributes every later press to the wrong control
    /// -- and the summary would report a table that disagrees with hardware it
    /// never measured. Neither table is judged unless its walk was complete.
    bool buttonsComplete = false;
    bool knobsComplete = false;
};

/// How many press-waits' worth of time the knob window gets, since it covers six
/// pushes rather than one.
constexpr int KnobWindowRounds = 3;

/// How many times a button round waits again before giving the walk up.
constexpr int PressAttempts = 3;

/// Reads reports until one's byte at @p offset satisfies @p wanted.
///
/// Two kinds of report are skipped. A screen touch, because byte 0 carries the
/// event type there and a finger on the glass would read as buttons nobody is
/// pressing. And the all-zero filler the deck interleaves between real reports,
/// because a release has to be told apart from a gap -- both show zero in the
/// byte, and only the filler is zero everywhere else.
///
/// @param transport An open control interface.
/// @param offset Which byte of the report to watch.
/// @param wanted What that byte has to satisfy.
/// @param deadline When to give up.
/// @return The byte that satisfied it, or nothing if the deadline passed first.
template <typename Predicate>
[[nodiscard]] std::optional<std::uint8_t> awaitByte(HidApiTransport& transport, std::size_t offset,
                                                    Predicate wanted,
                                                    std::chrono::steady_clock::time_point deadline)
{
    std::array<std::uint8_t, protocol::PaddedReportSize + 1> buffer {};

    while (std::chrono::steady_clock::now() < deadline)
    {
        auto const bytesRead = transport.read(buffer, std::chrono::milliseconds { 100 });
        if (!bytesRead)
            return std::nullopt;
        if (*bytesRead == 0)
            continue;

        auto const payload = protocol::reportPayload(std::span { buffer }.first(*bytesRead));
        if (payload.size() <= offset)
            continue;
        if (payload[protocol::EventTypeOffset] == protocol::ScreenTouchEventType)
            continue;
        if (std::ranges::none_of(payload, [](std::uint8_t byte) { return byte != 0; }))
            continue;

        if (auto const byte = payload[offset]; wanted(byte))
            return byte;
    }

    return std::nullopt;
}

/// Waits for one press and the release after it, at one byte of the report.
///
/// @param transport An open control interface.
/// @param offset Which byte carries the bitmask.
/// @param seconds How long to wait for the press.
/// @return What was seen.
[[nodiscard]] InputObservation awaitPress(HidApiTransport& transport, std::size_t offset,
                                          int seconds)
{
    using Clock = std::chrono::steady_clock;

    InputObservation seen;
    auto const pressed =
        awaitByte(transport, offset, [](std::uint8_t byte) { return byte != 0; },
                  Clock::now() + std::chrono::seconds { seconds });
    if (!pressed)
        return seen;

    seen.sawPress = true;
    seen.raw = *pressed;

    // Whether a release is reported at all is the question, not an assumption:
    // the driver fires on a rising edge and returns early when the byte has not
    // changed, so a deck that never sends the zero would lose every second press.
    auto const started = Clock::now();
    auto const released = awaitByte(transport, offset, [](std::uint8_t byte) { return byte == 0; },
                                    started + std::chrono::seconds { seconds });
    seen.sawRelease = released.has_value();
    if (seen.sawRelease)
        seen.releaseAfter =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started);

    return seen;
}

/// @param seen What one round observed.
/// @return The raw byte for the summary's column, or a dash if nothing arrived.
[[nodiscard]] std::string rawColumn(InputObservation const& seen)
{
    return seen.sawPress ? std::format("{:02x}", seen.raw) : std::string { "--" };
}

/// @param seen What one round observed.
/// @return How many bits the byte carried, or a dash.
[[nodiscard]] std::string bitsColumn(InputObservation const& seen)
{
    return seen.sawPress ? std::format("{}", std::popcount(seen.raw)) : std::string { "-" };
}

/// @param seen What one round observed.
/// @param expected The bit the table claims that button or knob produces.
/// @param table Which table, for the message.
/// @return The summary's verdict column.
[[nodiscard]] std::string verdictFor(InputObservation const& seen, std::uint8_t expected,
                                     std::string_view table)
{
    if (!seen.sawPress)
        return "nothing arrived";
    if (seen.raw == expected)
        return "agrees";

    return std::format("DISAGREES with {}", table);
}

/// @param seen What one round observed.
/// @return What to say about the release that followed the press.
[[nodiscard]] std::string releaseNote(InputObservation const& seen)
{
    if (!seen.sawRelease)
        return ", NO RELEASE REPORTED";

    return std::format(", released after {} ms", seen.releaseAfter.count());
}

/// @param seen One round each, in the controls' own order.
/// @return Whether every round produced a different single bit.
///
/// Two controls cannot share a bit, so a repeat is not a finding about the
/// hardware: it means a round was answered with the wrong control. Telling those
/// two apart is the difference between a measurement and a mis-press, and without
/// this check the summary reports the second as the first.
[[nodiscard]] bool everyRoundIsADistinctBit(std::span<InputObservation const> seen)
{
    std::vector<std::uint8_t> bits;
    for (auto const& observation: seen)
    {
        if (!observation.sawPress || std::popcount(observation.raw) != 1)
            return false;
        if (std::ranges::find(bits, observation.raw) != bits.end())
            return false;

        bits.push_back(observation.raw);
    }

    return true;
}

/// @param agrees Whether every observed bit matched the table.
/// @param usable Whether the walk that produced it can be read at all.
/// @return What to say about the table.
[[nodiscard]] std::string_view tableVerdict(bool agrees, bool usable)
{
    if (!usable)
        return "was not measured -- the walk is not usable";

    return agrees ? "agrees with the hardware" : "DISAGREES with the hardware";
}

/// Prints what the walk found, and whether the tables agree with it.
///
/// Separate from the walk that measured it, so neither has to be read while
/// thinking about the other.
///
/// @param console Where it goes.
/// @param walk What was measured.
/// @return Whether both walks were usable, so a caller's status can say so too.
[[nodiscard]] bool reportInputWalk(IConsole& console, InputWalk const& walk)
{
    writeLine(console, "");
    writeLine(console, "{:>14}  {:>5}  {:>5}  {:>8}  {}", "button", "raw", "bit", "expected", "");

    auto const buttonsUsable = walk.buttonsComplete && everyRoundIsADistinctBit(walk.buttons);
    bool buttonsAgree = buttonsUsable;
    for (auto const button: AllButtons)
    {
        auto const& seen = walk.buttons[indexOf(button)];
        auto const expected = protocol::ButtonBits[indexOf(button)];
        auto const agrees = seen.sawPress && seen.raw == expected;
        buttonsAgree = buttonsAgree && agrees;

        writeLine(console,
                  "{:>14}  {:>5}  {:>5}  {:>8}  {}",
                  nameOf(button),
                  rawColumn(seen),
                  bitsColumn(seen),
                  std::format("{:02x}", expected),
                  verdictFor(seen, expected, "ButtonBits"));
    }

    // The release half, which is about the driver rather than about the table.
    for (auto const button: AllButtons)
    {
        auto const& seen = walk.buttons[indexOf(button)];
        if (seen.sawPress && !seen.sawRelease)
            writeLine(console,
                      "  the {} button reported no release -- the driver's rising edge never "
                      "re-arms, so a second press of it would be lost",
                      nameOf(button));
    }

    if (walk.sawTogether)
    {
        auto const bits = std::popcount(walk.together);
        writeLine(console, "");
        writeLine(console,
                  "two at once: raw {:02x}, {} bit(s) set -- {}",
                  walk.together,
                  bits,
                  bits >= 2 ? "they OR into one report, which is what the bitmask reading assumes"
                            : "ONLY ONE BIT: the report does not combine them, and the bitmask "
                              "reading is wrong");
    }

    writeLine(console, "");
    writeLine(console, "{:>14}  {:>5}  {:>8}  {}", "knob", "raw", "expected", "");

    auto const knobsUsable = walk.knobsComplete && everyRoundIsADistinctBit(walk.knobs);
    bool knobsAgree = knobsUsable;
    for (auto const knob: AllKnobs)
    {
        auto const& seen = walk.knobs[indexOf(knob)];
        auto const expected = protocol::KnobBits[indexOf(knob)];
        auto const agrees = seen.sawPress && seen.raw == expected;
        knobsAgree = knobsAgree && agrees;

        writeLine(console,
                  "{:>14}  {:>5}  {:>8}  {}",
                  nameOf(knob),
                  rawColumn(seen),
                  std::format("{:02x}", expected),
                  verdictFor(seen, expected, "KnobBits"));
    }

    writeLine(console, "");
    writeLine(console,
              "ButtonBits {}, KnobBits {}",
              tableVerdict(buttonsAgree, buttonsUsable),
              tableVerdict(knobsAgree, knobsUsable));

    // The corrected tables, ready to paste, so nobody has to transcribe a byte
    // out of the rows above.
    if (!buttonsUsable)
        writeLine(console,
                  "  the button walk is not usable: a round went unanswered, or two rounds "
                  "reported the same bit -- which two buttons cannot do, so one was answered "
                  "with the wrong button. Run it again rather than reading the rows above.");
    else if (!buttonsAgree)
    {
        std::string row;
        for (auto const button: AllButtons)
            row += std::format("0x{:02x}, ", walk.buttons[indexOf(button)].raw);
        writeLine(console, "  ButtonBits should read: {{ {}}}", row);
    }
    if (!knobsUsable)
        writeLine(console,
                  "  the knob walk is not usable: not every knob was pushed, or two pushes "
                  "carried the same bit -- run it again rather than reading the rows above.");
    else if (!knobsAgree)
    {
        std::string row;
        for (auto const knob: AllKnobs)
            row += std::format("0x{:02x}, ", walk.knobs[indexOf(knob)].raw);
        writeLine(console, "  KnobBits should read: {{ {}}}", row);
    }

    return buttonsUsable && knobsUsable;
}

/// Lights every button but one, so the one that is lit is the one to press.
/// @param transport An open control interface.
/// @param console Where a failed write is reported.
/// @param only Which button to light, or nothing to light none of them.
void lightOnly(HidApiTransport& transport, IConsole& console, std::optional<Button> only)
{
    auto const apply = [&transport, &console](Button button, bool wanted) {
        // Off is literal black, not scaledChannel(..., 0): that returns
        // MinLightChannel, because the vendor's brightness slider bottoms out at
        // the floor rather than at nothing. Dimming to zero percent leaves four
        // buttons faintly glowing, and the two with the highest channels still
        // read as lit -- which is not a cue anybody can act on.
        auto const& colour = protocol::DefaultButtonColours[indexOf(button)];
        auto const level = protocol::DefaultButtonBrightnessPercent;
        auto const record = protocol::buttonColourRecord(
            protocol::selectorFor(button),
            wanted ? protocol::scaledChannel(colour.red, level) : 0,
            wanted ? protocol::scaledChannel(colour.green, level) : 0,
            wanted ? protocol::scaledChannel(colour.blue, level) : 0,
            wanted);

        auto const framed =
            protocol::frameFeatureReport(protocol::setPropertyAt(protocol::ButtonColourAddress, record));
        if (!transport.sendFeatureReport(framed))
            writeErrorLine(console, "could not light the {} button", nameOf(button));
    };

    // The lit one goes first and the dark ones after, so moving from one round to
    // the next passes through two buttons lit rather than through none. Darkening
    // first passes through all-four-dark instead, which is exactly the cue this
    // walk uses for its next phase -- somebody watching the deck sees that flash
    // between rounds and answers the wrong question. It cost three walks.
    if (only)
        apply(*only, true);

    for (auto const button: AllButtons)
        if (only != button)
            apply(button, false);
}

/// Watches one window and takes the knob pushes in the order they arrive.
///
/// A round per knob would need the person at the deck to keep pace with rounds
/// they cannot see, and a round that times out shifts every later one: read that
/// way, four knobs look silent and the two bits that did arrive get attributed to
/// the wrong knobs entirely. One window removes the pacing -- six pushes, left to
/// right, at whatever speed suits, and the order they arrive in is the answer.
///
/// The knobs cannot be lit one at a time the way the buttons can, because the ring
/// record colours all six at once. Their identity comes from the legend printed on
/// the deck instead, which is why the order matters and the pacing must not.
///
/// @param transport An open control interface.
/// @param seconds How long the whole window lasts.
/// @return The distinct push bits, in the order they were first seen.
[[nodiscard]] std::vector<std::uint8_t> collectKnobPushes(HidApiTransport& transport, int seconds)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds { seconds };
    std::vector<std::uint8_t> order;

    while (order.size() < KnobCount)
    {
        // Waiting for a value not seen before is what lets the releases between
        // pushes go by without being counted, and what stops a knob held a moment
        // too long from filling two slots.
        auto const pushed = awaitByte(
            transport,
            protocol::KnobPushOffset,
            [&order](std::uint8_t value) {
                return value != 0 && std::ranges::find(order, value) == order.end();
            },
            deadline);
        if (!pushed)
            break;

        order.push_back(*pushed);
    }

    return order;
}

/// Lights all four buttons the way the driver leaves them.
/// @param transport An open control interface.
/// @param console Where a failed write is reported.
void lightDefaults(HidApiTransport& transport, IConsole& console)
{
    for (auto const button: AllButtons)
    {
        auto const& colour = protocol::DefaultButtonColours[indexOf(button)];
        auto const level = protocol::DefaultButtonBrightnessPercent;
        static_cast<void>(setButtonColour(transport,
                                          console,
                                          button,
                                          protocol::scaledChannel(colour.red, level),
                                          protocol::scaledChannel(colour.green, level),
                                          protocol::scaledChannel(colour.blue, level)));
    }
}

/// Walks the inputs, asking for one press at a time and reporting what arrived.
///
/// The point of the walk is that **identity comes from which button is lit, not
/// from ButtonBits**. Reading the arriving byte through isDown() would only show
/// the table agreeing with itself, which is exactly what the existing decode test
/// does and why the mapping is still open. So one button is lit, its raw byte is
/// recorded, and the table is compared against that afterwards.
///
/// The knobs have no light of their own -- the ring record colours all six at
/// once -- so their identity comes from the legend printed on the deck instead.
///
/// @param transport An open control interface.
/// @param console Where the narration goes.
/// @param seconds How long to wait for each press.
/// @return Process status.
int walkInputs(HidApiTransport& transport, IConsole& console, int seconds)
{
    using Clock = std::chrono::steady_clock;

    // A deck that is asleep looks exactly like a deck nobody is touching, so the
    // difference gets said out loud rather than blamed on the person at the desk.
    writeLine(console, "waiting for the deck to send anything at all...");
    if (!awaitByte(transport, protocol::EventTypeOffset, [](std::uint8_t) { return true; },
                   Clock::now() + std::chrono::seconds { seconds }))
    {
        writeErrorLine(console,
                       "nothing arrived. The deck sends no reports until something initialises "
                       "it -- run `ax310_app --start-minimised` alongside this and try again.");
        return EXIT_FAILURE;
    }

    InputWalk walk;

    // The lights carry the script, so somebody at the deck can do this without
    // the terminal in view -- which matters, because every prompt below competes
    // with looking at the hardware.
    writeLine(console, "");
    writeLine(console, "The deck's own lights say what to do:");
    writeLine(console, "  one button lit   -> press that button");
    writeLine(console, "  all four dark    -> hold any two buttons together");
    writeLine(console, "  all four lit     -> push each knob, left to right");
    writeLine(console, "");
    walk.buttonsComplete = true;
    for (auto const button: AllButtons)
    {
        writeLine(console, "  press the lit button ({})...", nameOf(button));

        // Retried rather than skipped. Advancing past a round nobody answered is
        // what turns a slow start into a wrong table: the press lands during the
        // next round and is credited to the next button.
        //
        // The lighting is inside the retry, not before it, because this tool is
        // not the only thing writing to the deck -- a driver finishing its connect
        // lights all four, and a round that opened before that would be asking for
        // a button nobody can pick out.
        InputObservation seen;
        for (int attempt = 0; attempt < PressAttempts && !seen.sawPress; ++attempt)
        {
            lightOnly(transport, console, button);
            seen = awaitPress(transport, protocol::ButtonsOffset, seconds);
            if (!seen.sawPress)
                writeLine(console, "    still waiting for the lit button...");
        }

        walk.buttons[indexOf(button)] = seen;
        if (!seen.sawPress)
        {
            writeLine(console, "    nothing arrived, so the rest of the buttons are not asked for");
            walk.buttonsComplete = false;
            break;
        }

        writeLine(console, "    raw {:02x}{}", seen.raw, releaseNote(seen));
    }

    lightOnly(transport, console, std::nullopt);
    writeLine(console, "");
    writeLine(console, "Now hold any two buttons together...");
    if (auto const both = awaitByte(transport, protocol::ButtonsOffset,
                                    [](std::uint8_t byte) { return std::popcount(byte) >= 2; },
                                    Clock::now() + std::chrono::seconds { seconds }))
    {
        walk.together = *both;
        walk.sawTogether = true;
        writeLine(console, "    raw {:02x}", *both);
    }
    else
        writeLine(console, "    no report carried two bits");

    // Drained, so the knob rounds do not open with the buttons still held.
    auto const settled = awaitByte(transport, protocol::ButtonsOffset,
                                   [](std::uint8_t byte) { return byte == 0; },
                                   Clock::now() + std::chrono::seconds { seconds });
    static_cast<void>(settled);

    writeLine(console, "");
    writeLine(console, "Now push each knob once, left to right, at whatever speed suits.");
    lightDefaults(transport, console);

    auto const pushes = collectKnobPushes(transport, seconds * KnobWindowRounds);
    for (std::size_t index = 0; index < pushes.size(); ++index)
        walk.knobs[index] = InputObservation { .raw = pushes[index], .sawPress = true };

    walk.knobsComplete = pushes.size() == KnobCount;
    writeLine(console, "    {} of {} knobs pushed", pushes.size(), KnobCount);

    auto const usable = reportInputWalk(console, walk);

    // Left as the driver leaves it, so the deck does not keep whatever the last
    // round of the walk happened to light.
    lightDefaults(transport, console);

    // A walk nobody answered is a failed measurement rather than a result, and
    // saying so in the status as well as in the summary is what keeps it from
    // being read as one.
    return usable ? EXIT_SUCCESS : EXIT_FAILURE;
}

int usage(IConsole& console, std::string_view program)
{
    writeErrorLine(console, "usage: {} <command>", program);
    writeErrorLine(console, "  --dump                 read every register the handshake overwrites");
    writeErrorLine(console, "  --read <addr> <len>    read one register, both decimal or 0x-free hex");
    writeErrorLine(console, "  --effect <cmd> <0|1>   toggle one known effect enable");
    writeErrorLine(console, "  --level <1|2> <knob> <00..14>  set one track's level in one mix (hex)");
    writeErrorLine(console, "  --mixer-mode <dual|creator|audience>  two independent mixes, or one shared");
    writeErrorLine(console, "  --meters [seconds]     watch the per-track meters, and report their peaks");
    writeErrorLine(console, "  --touches [seconds]    watch screen touches, including the unexplained flags byte");
    writeErrorLine(console, "  --inputs [seconds]     which bit each button and knob push produces, one at a time");
    writeErrorLine(console, "  --panel-off            blank the panel; touch it to wake it again");
    writeErrorLine(console, "  --panel-brightness <19..64>  set panel brightness, percent in hex");
    writeErrorLine(console, "  --identify                              firmware version and serial number");
    writeErrorLine(console, "  --button <tl|tr|bl|br> <rr> <gg> <bb>   light one function button");
    writeErrorLine(console, "  --knob-colour <rr> <gg> <bb>            colour the selected mix's rings");
    writeErrorLine(console, "  --surround off                          turn the surround strip off");
    writeErrorLine(console, "  --surround <mode> <freq> <rr> <gg> <bb> drive the surround strip");
    writeErrorLine(console, "  --try <addr> <value> [seconds] [--fenced]");
    writeErrorLine(console, "                         write one byte, wait while you look at the deck, put it back");
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

/// Dispatches the commands that watch the deck instead of writing to it.
///
/// Grouped for the same reason runLightCommand below is: main is held under a
/// cognitive-complexity cap, and three modes that differ only in a default
/// duration do not each need their own block up there.
///
/// @param transport An open control interface.
/// @param console Where the narration goes.
/// @param arguments The whole command line.
/// @return The process status, or nothing when this is not one of these commands.
[[nodiscard]] std::optional<int> runWatchCommand(HidApiTransport& transport, IConsole& console,
                                                 std::span<char* const> arguments)
{
    struct Watch
    {
        std::string_view command;
        unsigned defaultSeconds;
        int (*run)(HidApiTransport&, IConsole&, int);
    };

    constexpr std::array<Watch, 3> Watches { {
        { .command = "--meters", .defaultSeconds = 10, .run = watchMeters },
        { .command = "--touches", .defaultSeconds = 20, .run = watchTouches },
        { .command = "--inputs", .defaultSeconds = 30, .run = walkInputs },
    } };

    if (arguments.size() > 3)
        return std::nullopt;

    for (auto const& watch: Watches)
    {
        if (std::string_view { arguments[1] } != watch.command)
            continue;

        auto const seconds = parseSeconds(arguments, watch.defaultSeconds);
        if (!seconds)
        {
            writeErrorLine(console, "{} takes a number of seconds, 1 to 600", watch.command);
            return EXIT_FAILURE;
        }

        return watch.run(transport, console, *seconds);
    }

    return std::nullopt;
}

/// Sets whether the deck keeps its two mixes apart: `--mixer-mode`.
///
/// One byte carries the mode and the monitored mix together, so this takes the
/// pair rather than pretending they are two settings. Dual is what gives each
/// mix a profile of its own -- in single the deck copies the monitored mix's
/// playback levels over the other block, so per-mix volumes do not survive.
///
/// @param transport An open control interface.
/// @param console Where the narration goes.
/// @param wanted One of dual, creator, audience.
/// @return The process status.
[[nodiscard]] int runMixerMode(HidApiTransport& transport, IConsole& console, std::string_view wanted)
{
    std::optional<std::uint8_t> value;
    if (wanted == "dual")
        value = protocol::KnobLedSelectForDualMix;
    else if (wanted == "creator")
        value = protocol::KnobLedSelectForMix[indexOf(MixId::Creator)];
    else if (wanted == "audience")
        value = protocol::KnobLedSelectForMix[indexOf(MixId::Audience)];

    if (!value)
    {
        writeErrorLine(console, "usage: --mixer-mode <dual|creator|audience>");
        writeErrorLine(console, "  dual      two independent mixes");
        writeErrorLine(console, "  creator   one shared mix, monitoring the creator mix");
        writeErrorLine(console, "  audience  one shared mix, monitoring the audience mix");
        return EXIT_FAILURE;
    }

    std::array<std::uint8_t, 1> const values { *value };
    auto const framed = protocol::frameFeatureReport(
        protocol::setPropertyAt(std::to_underlying(protocol::Property::KnobLedSelect), values));

    if (!transport.sendFeatureReport(framed))
    {
        writeErrorLine(console, "the write failed");
        return EXIT_FAILURE;
    }

    writeLine(console, "mixer mode set to {} (0x21 = {:02x})", wanted, *value);
    return EXIT_SUCCESS;
}

/// Dispatches the three commands that drive lights.
///
/// Grouped out of main partly because they belong together -- one address family,
/// one kind of effect -- and partly because main is held under a complexity cap
/// that a third lighting command would have broken.
///
/// @param transport An open control interface.
/// @param console Where the narration goes.
/// @param arguments The whole command line.
/// @return The process status, or nothing if this is not a lighting command.
[[nodiscard]] std::optional<int> runLightCommand(HidApiTransport& transport, IConsole& console,
                                                std::span<char* const> arguments)
{
    auto const command = std::string_view { arguments[1] };
    auto const count = arguments.size();

    if (command == "--button" && count == 6)
        return runButton(transport, console, arguments);
    if (command == "--knob-colour" && count == 5)
        return runKnobColour(transport, console, arguments);
    if (command == "--surround" && (count == 3 || count == 7))
        return runSurround(transport, console, arguments);
    return std::nullopt;
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

    if (command == "--identify" && argc == 2)
        return runIdentify(transport, console);

    if (command == "--dump")
    {
        dumpKnownRegisters(transport, console);
        return EXIT_SUCCESS;
    }

    if (auto const status = runLightCommand(transport, console, arguments))
        return *status;

    if (auto const status = runWatchCommand(transport, console, arguments))
        return *status;

    if (command == "--panel-off" && argc == 2)
        return setPanel(transport, console, protocol::PanelOffLevel);

    if (command == "--panel-brightness" && argc == 3)
        return runPanelBrightness(transport, console, arguments);

    if (command == "--try" && argc >= 4 && argc <= 6)
    {
        auto const asked = parseTry(arguments);
        if (!asked)
        {
            writeErrorLine(console,
                           "usage: --try <addr 00..ff> <value 00..ff> [seconds 1..600] [--fenced]");
            return EXIT_FAILURE;
        }

        return tryRegister(
            transport, console, asked->address, asked->value, asked->seconds, asked->fenced);
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

    if (command == "--mixer-mode" && argc == 3)
        return runMixerMode(transport, console, arguments[2]);

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
