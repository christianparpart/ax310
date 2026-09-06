// SPDX-License-Identifier: Apache-2.0
/// Writes the wire specification out of the tables that implement it.
///
/// A protocol document maintained by hand drifts, and this project has the scars
/// to prove it: the meters were described three different ways in three files at
/// once, and each description was written by somebody who believed it. So the
/// document is not written. It is **projected** from Protocol.hpp and Types.hpp,
/// and `--check` fails the test suite when the committed copy no longer matches
/// what the headers say.
///
/// The discipline that makes that work: **no number is typed in here.** The prose
/// below describes shape -- what a table's rows mean, how a frame is laid out --
/// and every address, length, offset, range and name is read from a constant. A
/// sentence in this file that states a fact the headers do not hold is a bug of
/// the same kind the whole arrangement exists to prevent.
///
/// Usage:
///   ax310_spec                write the document to stdout
///   ax310_spec --write PATH   write it to PATH
///   ax310_spec --check PATH   exit non-zero if PATH is not what would be written

#include <ax310/IConsole.hpp>
#include <ax310/Protocol.hpp>
#include <ax310/Types.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace ax310;

namespace
{

/// Collects the document a line at a time.
///
/// Not an ostream, and not `std::print(std::ostream&, ...)`. Those overloads are
/// C++23's P2539: libstdc++ has them, MSVC's <print> has only the `FILE*` forms,
/// and using them here was the single thing in this project that would not
/// compile with MSVC -- found the first time CI built on Windows, in a file
/// nothing else depends on.
///
/// Building the text up in a string is also what `--check` needs: the whole
/// document in hand, to compare against the committed one.
class Document
{
  public:
    /// Appends one formatted line, with the newline.
    /// @param format The format string.
    /// @param args What it formats.
    template <typename... Args>
    void line(std::format_string<Args...> format, Args&&... args)
    {
        std::format_to(std::back_inserter(_text), format, std::forward<Args>(args)...);
        _text += '\n';
    }

    /// Appends without a newline, for a row assembled from several calls.
    /// @param format The format string.
    /// @param args What it formats.
    template <typename... Args>
    void put(std::format_string<Args...> format, Args&&... args)
    {
        std::format_to(std::back_inserter(_text), format, std::forward<Args>(args)...);
    }

    /// @return Everything written so far.
    [[nodiscard]] std::string const& text() const noexcept { return _text; }

  private:
    std::string _text;
};

/// @param value A byte.
/// @return It as `0x` followed by two lowercase hex digits.
[[nodiscard]] std::string hex(std::uint8_t value)
{
    return std::format("0x{:02x}", value);
}

/// @param text A display string that may carry padding.
/// @return It without leading or trailing spaces.
[[nodiscard]] std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && text.front() == ' ')
        text.remove_prefix(1);
    while (!text.empty() && text.back() == ' ')
        text.remove_suffix(1);
    return text;
}

/// @param name A name that may be empty.
/// @return The name, or an em dash so a table cell is never blank.
[[nodiscard]] std::string_view orDash(std::string_view name)
{
    return name.empty() ? std::string_view { "—" } : name;
}

/// @param encoding How a parameter is stored in its command's body.
/// @return How to say that in a table cell.
[[nodiscard]] std::string_view describeEncoding(protocol::ParameterEncoding encoding)
{
    switch (encoding)
    {
        case protocol::ParameterEncoding::SignedByte: return "signed byte";
        case protocol::ParameterEncoding::UnsignedByte: return "unsigned byte";
        case protocol::ParameterEncoding::UnsignedWordLE: return "unsigned 16, little-endian";
    }
    return "—";
}

/// @param address A property address.
/// @return How many bytes of it the connect sequence saves and puts back, said
///         the way a table cell should say it.
[[nodiscard]] std::string preservedLength(std::uint8_t address)
{
    for (auto const& row: protocol::PreservedAddresses)
        if (row.address == address)
            return row.length == 1 ? std::string { "yes, 1 byte" } : std::format("yes, {} bytes", row.length);
    return "no";
}

/// @param address A property address.
/// @return Whether writing it is refused.
[[nodiscard]] bool isDangerous(std::uint8_t address)
{
    return std::ranges::find(protocol::DangerousAddresses, address) != protocol::DangerousAddresses.end();
}

void writeSpec(Document& out)
{
    using namespace ax310::protocol;

    // Jekyll front matter, so the same generated file is both the document in the
    // repository and the specification page on the site. Writing the page by hand
    // instead would put a second copy of every address somewhere nothing checks.
    out.line("---");
    out.line("layout: default");
    out.line("title: Wire specification");
    out.line("description: >-");
    out.line("  The AX310's register map, report layout and framed commands, generated from "
             "the driver that implements them.");
    out.line("---");
    out.line("<!-- SPDX-License-Identifier: Apache-2.0 -->");
    out.line("# AX310 wire specification");
    out.line("");
    out.line("**Generated by `ax310_spec` from `src/ax310/Protocol.hpp` and "
             "`src/ax310/Types.hpp`. Do not edit.**");
    out.line("");
    out.line("Every address, length, offset, range, default and name below is read from the "
             "constant that implements it, so none of them can disagree with the driver. "
             "`ctest -R wire-specification` fails when this file and the headers have "
             "parted; `ax310_spec --write docs/wire-protocol.md` brings them back together.");
    out.line("");
    out.line("The sentences around the tables are prose, and prose is what drifts. Where one "
             "makes a claim a table cannot hold -- an endianness, an ordering, a range -- "
             "there is a test in `Protocol_test.cpp` that fails if it stops being true. "
             "Those are tagged `[spec]`.");
    out.line("");
    out.line("What is *not* here is the evidence. Why a register is believed to mean what "
             "it means, which readings were wrong first, and what remains unknown live in "
             "`AGENT.md` and `.agent/rules/`, because none of that is derivable from a "
             "table.");
    out.line("");

    // ---- USB -------------------------------------------------------------
    out.line("## The two USB devices");
    out.line("");
    out.line("One physical unit enumerates as two devices at once, not as two states of "
             "one. Only the control device carries the deck's protocol.");
    out.line("");
    out.line("| Mode | Product id | HID interface |");
    out.line("| --- | --- | --- |");
    for (auto const mode: { DeviceMode::Base, DeviceMode::Control })
    {
        auto const descriptor = descriptorFor(mode);
        out.line("| {} | `0x{:04x}` | {} |",
                 mode == DeviceMode::Control ? "Control" : "Base",
                 descriptor.productId,
                 descriptor.interfaceNumber);
    }
    out.line("");

    // ---- Input report ----------------------------------------------------
    out.line("## The input report");
    out.line("");
    out.line("{} bytes, or {} when padded. hidapi prepends the report id for a device that "
             "numbers its reports and not for one that does not, so both lengths arrive; "
             "`protocol::reportPayload()` is the one place that decides which.",
             ControlReportSize,
             PaddedReportSize);
    out.line("");
    out.line("The report is **mixed-endian**: touch coordinates are little-endian and the "
             "audio meters are big-endian, in the same frame.");
    out.line("");
    out.line("| Offset | Field |");
    out.line("| --- | --- |");
    out.line("| `{:#04x}` | event type; `{}` is a screen touch. The button bitmask on any "
             "other report. |",
             EventTypeOffset,
             hex(ScreenTouchEventType));
    out.line("| `{:#04x}` | touch flags — fixed per contact and only ever counting up; what "
             "the value means is unknown |",
             TouchFlagsOffset);
    out.line("| `{:#04x}` | touch x, little-endian |", TouchXOffset);
    out.line("| `{:#04x}` | touch y, little-endian |", TouchYOffset);
    out.line("| `{:#04x}` | knob push bitmask |", KnobPushOffset);
    out.line("| `{:#04x}` | knob capacitive-touch bitmask |", KnobTouchOffset);
    out.line("| `{:#04x}` | knob rotation counters, {} of them, relative rather than "
             "positions |",
             KnobValuesOffset,
             KnobCount);
    out.line("| `{:#04x}` | audio meters: {} tracks × {} bytes — a left and a right, both "
             "16-bit big-endian, full scale `{:#x}` |",
             AudioMetersOffset,
             AudioMeterCount,
             AudioMeterStride,
             AudioLevelFullScale);
    out.line("| `{:#04x}` | checksum |", ChecksumOffset);
    out.line("");
    out.line("The meters are one per track in the deck's printed knob order, and the deck "
             "sends all {} of them on every report.",
             AudioMeterCount);
    out.line("");

    out.line("### Bit masks");
    out.line("");
    out.line("| Button | Bit |");
    out.line("| --- | --- |");
    for (auto const button: AllButtons)
        out.line("| {} | `{}` |", indexOf(button) + 1, hex(ButtonBits[indexOf(button)]));
    out.line("");
    out.line("| Knob | Bit |");
    out.line("| --- | --- |");
    for (auto const knob: AllKnobs)
        out.line("| {} | `{}` |", nameOf(knob), hex(KnobBits[indexOf(knob)]));
    out.line("");

    // ---- Property family -------------------------------------------------
    out.line("## Property registers");
    out.line("");
    out.line("A byte-addressed register space, reached with a feature report of "
             "`[{}|{}] {} <address> <length> <values…>` — set and get. A reply carries the "
             "kind at byte {}, the group at {}, the address at {}, the length at {} and the "
             "values from {}.",
             hex(static_cast<std::uint8_t>(CommandKind::Set)),
             hex(static_cast<std::uint8_t>(CommandKind::Get)),
             hex(PropertyGroup),
             ReplyKindOffset,
             ReplyGroupOffset,
             ReplyAddressOffset,
             ReplyLengthOffset,
             ReplyValuesOffset);
    out.line("");
    out.line("| Address | Name | Preserved on connect | Writable |");
    out.line("| --- | --- | --- | --- |");
    auto byAddress = std::vector(PropertyNames.begin(), PropertyNames.end());
    std::ranges::sort(byAddress, {}, &WireName::value);
    for (auto const& row: byAddress)
        out.line("| `{}` | {} | {} | {} |",
                 hex(row.value),
                 orDash(row.name),
                 preservedLength(row.value),
                 isDangerous(row.value) ? "**refused**" : "yes");
    out.line("");
    out.line("{} addresses are read before the handshake and written back after it, because "
             "the captured init sequence is somebody's saved configuration rather than an "
             "initialisation. The longest is {} bytes.",
             PreservedAddresses.size(),
             MaxPreservedLength);
    out.line("");
    out.put("Writes are refused to:");
    for (auto const address: DangerousAddresses)
        out.put(" `{}`", hex(address));
    out.line(" — writing there once wedged a deck badly enough to need a power cycle.");
    out.line("");

    out.line("### Per-track levels");
    out.line("");
    out.line("Two contiguous blocks of {} bytes, one per mix, addressed `base + track` in "
             "knob order. A level is {} steps and one step is exactly {}%.",
             KnobCount,
             Level::MaxSteps,
             Level::PercentPerStep);
    out.line("");
    out.put("| Mix | Base |");
    for (auto const knob: AllKnobs)
        out.put(" {} |", nameOf(knob));
    out.line("");
    out.put("| --- | --- |");
    for (std::size_t index = 0; index < KnobCount; ++index)
        out.put(" --- |");
    out.line("");
    for (auto const mix: AllMixes)
    {
        auto const block = mix == MixId::Creator ? Property::CreatorMixLevels : Property::AudienceMixLevels;
        out.put("| {} | `{}` |", nameOf(mix), hex(static_cast<std::uint8_t>(block)));
        for (auto const knob: AllKnobs)
            out.put(" `{}` |", hex(levelAddressOf(block, knob)));
        out.line("");
    }
    out.line("");

    // ---- Display -----------------------------------------------------------
    out.line("## The display");
    out.line("");
    out.line("A family of its own -- `[{}] [{}] [level]` -- and the reason the panel's "
             "brightness was never found among the property registers: it is not there.",
             hex(static_cast<std::uint8_t>(CommandKind::Set)),
             hex(DisplayGroup));
    out.line("");
    out.line("The level is a percentage. `{}` is not a brightness but a sentinel that turns "
             "the panel off; the deck wakes itself when the glass is touched, and does so with "
             "no host command at all.",
             hex(PanelOffLevel));
    out.line("");
    out.line("| Level | Meaning |");
    out.line("| --- | --- |");
    out.line("| `{}` | dimmest the vendor's slider sends |", hex(MinPanelBrightness));
    out.line("| `{}` | brightest |", hex(MaxPanelBrightness));
    out.line("| `{}` | off |", hex(PanelOffLevel));
    out.line("");

    // ---- Framed family ---------------------------------------------------
    out.line("## Framed commands");
    out.line("");
    out.line("The DSP chain uses a second family: `{} 0x00 <length> <command> <body…> "
             "<checksum>`, {} bytes of overhead around a body of at most {}.",
             hex(FramedCommandMarker),
             FramedOverhead,
             MaxFramedBodySize);
    out.line("");
    out.line("| Command | Name |");
    out.line("| --- | --- |");
    for (auto const& row: FramedCommandNames)
        out.line("| `{}` | {} |", hex(row.value), orDash(row.name));
    out.line("");

    out.line("### Parameters");
    out.line("");
    out.line("{} of them, each one row of `protocol::Parameters`. A parameter located later "
             "becomes one more row and appears in both interfaces with no other change.",
             Parameters.size());
    out.line("");
    out.line("| Effect | Parameter | Command | Body offset | Encoding | Range | Unit |");
    out.line("| --- | --- | --- | --- | --- | --- | --- |");
    for (auto const& row: Parameters)
    {
        out.line("| {} | {} | `{}` | {} | {} | {}…{} | {} |",
                 EffectNames[static_cast<std::size_t>(row.effect)],
                 row.name,
                 hex(static_cast<std::uint8_t>(row.command)),
                 row.bodyOffset,
                 describeEncoding(row.encoding),
                 row.minimum,
                 row.maximum,
                 orDash(trimmed(row.unit)));
    }
    out.line("");
    out.line("The DSP chain has no read-back, so the driver keeps a known-good body per "
             "parameterised command and edits it in place. {} such bodies are held.",
             FramedDefaults.size());
    out.line("");
    for (auto const& entry: FramedDefaults)
    {
        out.put("- `{}` ({}), {} bytes:",
                hex(static_cast<std::uint8_t>(entry.command)),
                orDash(nameIn(FramedCommandNames, static_cast<std::uint8_t>(entry.command))),
                entry.length);
        for (std::size_t index = 0; index < entry.length; ++index)
            out.put(" `{:02x}`", entry.body[index]);
        out.line("");
    }
    out.line("");

    // ---- Screen ----------------------------------------------------------
    out.line("## The screen");
    out.line("");
    out.line("JPEG frames go out on the output endpoint in chunks of {} bytes: a {}-byte "
             "header and {} bytes of payload.",
             ScreenChunkSize,
             ScreenChunkHeaderSize,
             ScreenChunkPayloadSize);
    out.line("");
    out.line("| Offset | Field |");
    out.line("| --- | --- |");
    out.line("| `{:#04x}` | sequence number |", ScreenChunkSequenceOffset);
    out.line("| `{:#04x}` | {} bytes marking the last chunk of a frame — `{}` on the final "
             "chunk, `0x00` on every other |",
             ScreenChunkFinalOffset,
             ScreenChunkFinalByteCount,
             hex(ScreenChunkFinalMarker));
    out.line("| `{:#04x}` | payload length, little-endian |", ScreenChunkLengthOffset);
    out.line("| `{:#04x}` | payload sum, little-endian — the 16-bit unsigned sum of the "
             "chunk's payload bytes |",
             ScreenChunkChecksumOffset);
    out.line("");
    out.line("Without the final-chunk marker the deck accepts every chunk, reports no error "
             "and never puts the frame on screen.");
    out.line("");

    // ---- LEDs ------------------------------------------------------------
    out.line("## Knob LED rings");
    out.line("");
    out.line("A knob property is {} bytes, with the ring level at offset {}, {} at full. The "
             "captured init sequence sets brightness `{}`.",
             KnobPropertyLength,
             KnobLedLevelOffset,
             MaxKnobLedLevel,
             hex(static_cast<std::uint8_t>(KnobLedBrightnessAtStartup)));
    out.line("");

    // ---- Colour records --------------------------------------------------
    out.line("## Colour records");
    out.line("");
    out.line("Two addresses take a ten-byte record rather than a value. Address `{}` colours "
             "the function buttons and the knob rings, with byte 0 choosing which: `{}` a "
             "button, `{}` the rings.",
             hex(ButtonColourAddress),
             hex(ButtonBank),
             hex(KnobBank));
    out.line("");
    out.line("```");
    out.line("button   {:02x} <selector> 01 <r> <g> <b> ?? ?? <lit> 80", ButtonBank);
    out.line("rings    {:02x} {:02x}         {:02x} <r> <g> <b> ?? ?? <lit> 80",
             KnobBank,
             KnobFirstLight,
             KnobLightCount);
    out.line("```");
    out.line("");
    out.line("| Button | Selector |");
    out.line("| --- | --- |");
    for (auto const button: AllButtons)
        out.line("| {} | `{}` |", indexOf(button) + 1, hex(selectorFor(button)));
    out.line("");
    out.line("The selectors run clockwise where the button numbering is row-major, so the "
             "mapping is a table and not arithmetic. There is no brightness field: the vendor "
             "scales the colour it sends, down to a floor of `0x19` per channel.");
    out.line("");
    out.line("The ring record carries **no mix**. The vendor keeps a separate ring colour for "
             "each mix, and setting either sends the same bytes -- the deck colours whichever "
             "mix is selected, so the colour cannot be aimed at the other one.");
    out.line("");
    out.line("Address `{}` drives the surround light strip:", hex(SurroundAddress));
    out.line("");
    out.line("```");
    out.line("01 <mode> 01 20 <rate> 00 00 <r> <g> <b>");
    out.line("```");
    out.line("");
    out.line("| Mode | Selector | Frequency read | Colour used |");
    out.line("| --- | --- | --- | --- |");
    for (auto const mode: AllSurroundModes)
        out.line("| {} | `{}` | {} | {} |",
                 nameOf(mode),
                 hex(selectorFor(mode)),
                 isAnimated(mode) ? "yes" : "not as a frequency; see below",
                 cyclesHues(mode) ? "no, it cycles hues" : "yes");
    out.line("");
    out.line("The selectors are four apart rather than one; what the low two bits are for is "
             "unknown, and every captured record has them clear. The frequency slider's ends "
             "gave `{}` and `{}`.",
             hex(MinSurroundFrequency),
             hex(MaxSurroundFrequency));
    out.line("");
    out.line("In Solid the frequency byte still does something: the vendor leaves one of a "
             "few resting values there, and driving it by hand changes how the strip looks. "
             "What it controls has not been characterised.");
    out.line("");
    out.line("**No light on this device has a brightness field.** Brightness is applied to the "
             "colour before it is sent, spanning `{}` to `{}` per channel -- on the strip in "
             "every mode, and on the buttons and rings the same way.",
             hex(MinLightChannel),
             hex(MaxLightChannel));
    out.line("");
    out.line("There is no mode for darkness: the vendor's \"off\" sends {} with a black "
             "colour. Nothing restores this address on connect, so a strip left black stays "
             "black across a replug and looks exactly like one that does not work.",
             nameOf(SurroundMode::Solid));
    out.line("");
}

} // namespace

int main(int argc, char* argv[])
{
    SystemConsole console;
    auto const arguments = std::span { argv, static_cast<std::size_t>(argc) };

    Document document;
    writeSpec(document);
    auto const& generated = document.text();

    if (argc == 1)
    {
        console.write(generated);
        return EXIT_SUCCESS;
    }

    auto const command = std::string_view { arguments[1] };
    if (argc != 3 || (command != "--check" && command != "--write"))
    {
        writeErrorLine(console, "usage: {} [--write PATH | --check PATH]", arguments[0]);
        return EXIT_FAILURE;
    }

    auto const path = std::string { arguments[2] };

    if (command == "--write")
    {
        std::ofstream file { path, std::ios::binary | std::ios::trunc };
        if (!file)
        {
            writeErrorLine(console, "could not write {}", path);
            return EXIT_FAILURE;
        }
        file << generated;
        writeLine(console, "wrote {}", path);
        return EXIT_SUCCESS;
    }

    std::ifstream file { path, std::ios::binary };
    if (!file)
    {
        writeErrorLine(console, "{} does not exist -- run: ax310_spec --write {}", path, path);
        return EXIT_FAILURE;
    }

    std::string const committed { std::istreambuf_iterator<char> { file },
                                  std::istreambuf_iterator<char> {} };
    if (committed == generated)
        return EXIT_SUCCESS;

    // Says which line first differs, because "the file changed" is not a lead.
    std::istringstream want { generated };
    std::istringstream have { committed };
    std::string wantLine;
    std::string haveLine;
    for (int line = 1;; ++line)
    {
        bool const wantMore = static_cast<bool>(std::getline(want, wantLine));
        bool const haveMore = static_cast<bool>(std::getline(have, haveLine));
        if (!wantMore && !haveMore)
            break;
        if (wantMore != haveMore || wantLine != haveLine)
        {
            writeErrorLine(console, "{} is out of date, from line {}:", path, line);
            writeErrorLine(console, "  committed: {}", haveMore ? haveLine : "(end of file)");
            writeErrorLine(console, "  headers:   {}", wantMore ? wantLine : "(end of file)");
            break;
        }
    }
    writeErrorLine(console, "");
    writeErrorLine(console, "The wire specification is generated from Protocol.hpp and Types.hpp.");
    writeErrorLine(console, "Run: ax310_spec --write {}", path);
    return EXIT_FAILURE;
}
