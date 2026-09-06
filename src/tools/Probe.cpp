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

/// What each button is called on the command line, indexed by Button. There is no
/// nameOf(Button) in the driver -- the buttons carry no printed labels the way the
/// knobs do, so a name for one is this tool's own convenience rather than a fact
/// about the hardware.
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
                       "<rate> <rr> <gg> <bb>, all hex");
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
        std::format("surround set to {} at rate 0x{:02x}{}",
                    nameOf(*mode),
                    *rate,
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

int usage(IConsole& console, std::string_view program)
{
    writeErrorLine(console, "usage: {} <command>", program);
    writeErrorLine(console, "  --dump                 read every register the handshake overwrites");
    writeErrorLine(console, "  --read <addr> <len>    read one register, both decimal or 0x-free hex");
    writeErrorLine(console, "  --effect <cmd> <0|1>   toggle one known effect enable");
    writeErrorLine(console, "  --level <1|2> <knob> <00..14>  set one track's level in one mix (hex)");
    writeErrorLine(console, "  --meters [seconds]     watch the per-track meters, and report their peaks");
    writeErrorLine(console, "  --touches [seconds]    watch screen touches, including the unexplained flags byte");
    writeErrorLine(console, "  --panel-off            blank the panel; touch it to wake it again");
    writeErrorLine(console, "  --panel-brightness <19..64>  set panel brightness, percent in hex");
    writeErrorLine(console, "  --button <tl|tr|bl|br> <rr> <gg> <bb>   light one function button");
    writeErrorLine(console, "  --knob-colour <rr> <gg> <bb>            colour the selected mix's rings");
    writeErrorLine(console, "  --surround off                          turn the surround strip off");
    writeErrorLine(console, "  --surround <mode> <rate> <rr> <gg> <bb> drive the surround strip");
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

    if (command == "--dump")
    {
        dumpKnownRegisters(transport, console);
        return EXIT_SUCCESS;
    }

    if (auto const status = runLightCommand(transport, console, arguments))
        return *status;

    if (command == "--panel-off" && argc == 2)
        return setPanel(transport, console, protocol::PanelOffLevel);

    if (command == "--panel-brightness" && argc == 3)
        return runPanelBrightness(transport, console, arguments);

    if (command == "--touches" && argc <= 3)
    {
        auto const seconds = parseSeconds(arguments, 20);
        if (!seconds)
        {
            writeErrorLine(console, "--touches takes a number of seconds, 1 to 600");
            return EXIT_FAILURE;
        }

        return watchTouches(transport, console, *seconds);
    }

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

    if (command == "--meters" && argc <= 3)
    {
        auto const seconds = parseSeconds(arguments, 10);
        if (!seconds)
        {
            writeErrorLine(console, "--meters takes a number of seconds, 1 to 600");
            return EXIT_FAILURE;
        }

        return watchMeters(transport, console, *seconds);
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
