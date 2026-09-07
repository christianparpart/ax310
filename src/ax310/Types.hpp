// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Enumerators.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>

/// Vocabulary types for the AX310 device API.
///
/// Everything here is Qt-free and free of wire layout: these are the names the
/// rest of the program uses to talk about buttons, knobs and device state. The
/// bits that represent them in a HID report live in Protocol.h, in one table
/// each, so no caller ever handles a raw mask.
namespace ax310
{

/// One of the four physical buttons, identified by position rather than by the
/// bit that reports it.
enum class Button : std::uint8_t
{
    TopLeft = 0,
    TopRight = 1,
    BottomLeft = 2,
    BottomRight = 3
};

/// Number of physical buttons, derived from the enumeration rather than stated.
inline constexpr std::size_t ButtonCount = enumerators::denseEnumeratorCount<Button>();

/// Every Button, in enumerator order, for iterating without an index loop.
inline constexpr auto AllButtons = enumerators::denseEnumeratorsOf<Button>();

/// Where each button sits on the deck, indexed by the enumerator.
///
/// Positions rather than functions, because the deck prints nothing on them and
/// what they do is the interface's business, not the driver's.
inline constexpr std::array<std::string_view, ButtonCount> ButtonNames {
    "top-left", "top-right", "bottom-left", "bottom-right"
};

/// @param button The button to name.
/// @return Where it sits.
[[nodiscard]] constexpr std::string_view nameOf(Button button) noexcept
{
    return ButtonNames[static_cast<std::size_t>(button)];
}

/// How the surround light strip is animated.
///
/// Six modes, and the vendor's seventh -- "off" -- is not one of them: turning the
/// strip off sends Solid with a black colour, byte for byte. The enumeration keeps
/// only what the wire distinguishes, so `Off` cannot drift away from the colour
/// that actually implements it.
///
/// The three RGB modes cycle through hues by themselves and ignore the colour in
/// the record; the vendor still fills it with white rather than leaving it stale.
enum class SurroundMode : std::uint8_t
{
    Solid = 0,
    Pulsing = 1,
    Blinking = 2,
    PulsingRgb = 3,
    BlinkingRgb = 4,
    ScrollingRgb = 5
};

/// Number of surround modes, derived from the enumeration rather than stated.
inline constexpr std::size_t SurroundModeCount = enumerators::denseEnumeratorCount<SurroundMode>();

/// What each mode is called, indexed by the enumerator.
inline constexpr std::array<std::string_view, SurroundModeCount> SurroundModeNames {
    "Solid", "Pulsing", "Blinking", "Pulsing RGB", "Blinking RGB", "Scrolling RGB",
};

/// Every SurroundMode, in enumerator order, for iterating without an index loop.
inline constexpr auto AllSurroundModes = enumerators::denseEnumeratorsOf<SurroundMode>();

/// @param mode The mode to name.
/// @return Its display label.
[[nodiscard]] constexpr std::string_view nameOf(SurroundMode mode) noexcept
{
    return SurroundModeNames[static_cast<std::size_t>(mode)];
}

/// @param mode The mode to ask about.
/// @return Whether the mode cycles hues on its own and ignores the record's colour.
[[nodiscard]] constexpr bool cyclesHues(SurroundMode mode) noexcept
{
    return mode == SurroundMode::PulsingRgb || mode == SurroundMode::BlinkingRgb
           || mode == SurroundMode::ScrollingRgb;
}

/// @param mode The mode to ask about.
/// @return Whether the mode animates, and so whether its frequency byte is read.
[[nodiscard]] constexpr bool isAnimated(SurroundMode mode) noexcept
{
    return mode != SurroundMode::Solid;
}

/// The deck's six rotary knobs, named as they are printed on its face, left to
/// right. The first three are physical inputs and the last three are the host's
/// digital tracks -- which is why the deck presents six playback channels as three
/// stereo pairs and the vendor's manual lists exactly three playback devices.
enum class KnobId : std::uint8_t
{
    Mic = 0,
    LineIn = 1,
    Console = 2,
    System = 3,
    Game = 4,
    Chat = 5
};

/// Number of rotary knobs, derived from the enumeration rather than stated.
inline constexpr std::size_t KnobCount = enumerators::denseEnumeratorCount<KnobId>();

/// What each knob is called, as printed on the deck, indexed by the enumerator.
inline constexpr std::array<std::string_view, KnobCount> KnobNames {
    "Mic", "Line In", "Console", "System", "Game", "Chat"
};

/// @param knob The knob to name.
/// @return Its printed label.
[[nodiscard]] constexpr std::string_view nameOf(KnobId knob) noexcept
{
    return KnobNames[static_cast<std::size_t>(knob)];
}

/// Which of the deck's two independent mixes a level belongs to.
///
/// The deck carries two complete six-track mixes and applies each to a different
/// destination, so a level is meaningless without saying which mix it is in.
enum class MixId : std::uint8_t
{
    /// What the streamer hears in the headphones.
    Creator = 0,

    /// What the stream captures. Adjusting it is inaudible to the streamer,
    /// which is exactly why it needs to be visible in the interface.
    Audience = 1
};

/// Whether the deck keeps the two mixes apart.
///
/// Not a second name for MixId. `MixId` says which mix is being monitored;
/// this says whether there are two of them to monitor. The deck carries both in
/// one register, and confusing them is what made a switch flatten somebody's
/// levels: in Single the deck copies the monitored mix's playback tracks over
/// the other block, so only Dual gives each mix a profile of its own.
enum class MixerMode : std::uint8_t
{
    /// One mix of the host's audio, shared. The physical inputs stay per-mix.
    Single = 0,

    /// Two independent mixes, which is what the interface draws.
    Dual = 1
};

/// Number of mixer modes, derived from the enumeration rather than stated.
inline constexpr std::size_t MixerModeCount = enumerators::denseEnumeratorCount<MixerMode>();

/// What each mixer mode is called, indexed by the enumerator.
inline constexpr std::array<std::string_view, MixerModeCount> MixerModeNames { "Single", "Dual" };

/// @param mode The mode to name.
/// @return Its name.
[[nodiscard]] constexpr std::string_view nameOf(MixerMode mode) noexcept
{
    return MixerModeNames[static_cast<std::size_t>(mode)];
}

/// @param mode The mode to index.
/// @return Its position, for indexing a table.
[[nodiscard]] constexpr std::size_t indexOf(MixerMode mode) noexcept
{
    return static_cast<std::size_t>(mode);
}

/// Number of mixes, derived from the enumeration rather than stated.
inline constexpr std::size_t MixCount = enumerators::denseEnumeratorCount<MixId>();

/// What each mix is called, indexed by the enumerator.
inline constexpr std::array<std::string_view, MixCount> MixNames { "Creator", "Audience" };

/// Every MixId, in enumerator order, for iterating without an index loop.
inline constexpr auto AllMixes = enumerators::denseEnumeratorsOf<MixId>();

/// What the deck's Line Out socket carries.
///
/// A third option beyond the two mixes, which is why this is its own enumeration
/// rather than a MixId: the socket can be fed the chat microphone directly,
/// bypassing both mixes.
enum class LineOutSource : std::uint8_t
{
    CreatorMix = 0,
    AudienceMix = 1,
    ChatMic = 2
};

/// Number of line-out sources, derived from the enumeration rather than stated.
inline constexpr std::size_t LineOutSourceCount = enumerators::denseEnumeratorCount<LineOutSource>();

/// What each source is called, indexed by the enumerator.
inline constexpr std::array<std::string_view, LineOutSourceCount> LineOutSourceNames {
    "Creator Mix",
    "Audience Mix",
    "Chat Mic",
};

/// Every LineOutSource, in enumerator order.
inline constexpr auto AllLineOutSources = enumerators::denseEnumeratorsOf<LineOutSource>();

/// @param source The source to name.
/// @return Its display label.
[[nodiscard]] constexpr std::string_view nameOf(LineOutSource source) noexcept
{
    return LineOutSourceNames[static_cast<std::size_t>(source)];
}

/// @param mix The mix to name.
/// @return Its display label.
[[nodiscard]] constexpr std::string_view nameOf(MixId mix) noexcept
{
    return MixNames[static_cast<std::size_t>(mix)];
}

/// One track's level in one mix.
///
/// A named type rather than a bare `int` because the two scales in play do not
/// interchange safely. The deck stores a level in **steps**, `0..20`; a person
/// thinks in **percent**. Those happen to convert exactly -- 21 steps means one
/// step is 5% -- so neither direction rounds, and that is worth locking in rather
/// than rediscovering: the older `setKnobVolume` took an `int percent` and
/// truncated it with `percent * 20 / 100`, quietly snapping 24% to 20%.
///
/// The wire always carries steps. Percent belongs to the interface and is
/// converted at the boundary, never carried inward.
class Level
{
  public:
    /// Highest step the hardware accepts.
    static constexpr int MaxSteps = 20;

    /// Percent per step. Exact, and asserted below.
    static constexpr int PercentPerStep = 5;

    /// A silent track.
    constexpr Level() noexcept = default;

    /// @param steps Steps, clamped into `0..MaxSteps`.
    /// @return The level.
    [[nodiscard]] static constexpr Level fromSteps(int steps) noexcept
    {
        return Level { static_cast<std::uint8_t>(std::clamp(steps, 0, MaxSteps)) };
    }

    /// @param percent Percent, clamped into `0..100` and rounded to the nearest
    ///        reachable step -- the hardware has no finer resolution, so a
    ///        caller asking for 23% gets 25% rather than silently losing it.
    /// @return The level.
    [[nodiscard]] static constexpr Level fromPercent(int percent) noexcept
    {
        auto const clamped = std::clamp(percent, 0, 100);
        return fromSteps((clamped + (PercentPerStep / 2)) / PercentPerStep);
    }

    /// @return The level in the hardware's own steps, for the wire.
    [[nodiscard]] constexpr std::uint8_t steps() const noexcept { return _steps; }

    /// @return The level in percent, for a person.
    [[nodiscard]] constexpr int asPercent() const noexcept { return _steps * PercentPerStep; }

    [[nodiscard]] constexpr bool operator==(Level const&) const noexcept = default;

  private:
    constexpr explicit Level(std::uint8_t steps) noexcept: _steps { steps } {}

    std::uint8_t _steps = 0;
};

static_assert(Level::MaxSteps * Level::PercentPerStep == 100,
              "steps and percent must convert exactly, or one of the two scales lies");

/// @param name A knob label, as spelled in KnobNames, case-sensitive.
/// @return The matching knob, or nothing when @p name is not one of them.
[[nodiscard]] constexpr std::optional<KnobId> knobFromName(std::string_view name) noexcept
{
    for (std::size_t index = 0; index < KnobCount; ++index)
        if (KnobNames[index] == name)
            return static_cast<KnobId>(index);

    return std::nullopt;
}

/// Every KnobId, in enumerator order, for iterating without an index loop.
inline constexpr auto AllKnobs = enumerators::denseEnumeratorsOf<KnobId>();

/// Whether a knob's capacitive surface is currently being touched.
enum class Touch : std::uint8_t
{
    Released,
    Touched
};

/// Where a contact on the 800x480 touch screen is in its life.
///
/// The deck reports position and a contact flag on every report while a finger
/// is down; it does not say "this is a new touch". The driver derives that, so
/// a host can post a press, then moves, then a release -- the sequence a UI
/// toolkit expects. Repeating a press for every report instead makes a tap
/// register for a single frame and a drag hold the button down, which is
/// exactly how it behaved.
enum class TouchPhase : std::uint8_t
{
    Released = 0, ///< The finger has just come off.
    Pressed = 1,  ///< The finger has just gone down.
    Moved = 2     ///< The finger is still down and has a new position.
};

/// Which of the AX310's two USB personalities is currently on the bus.
///
/// The deck enumerates in Base mode, is handed its initialisation sequence, and
/// re-appears in Control mode. Same hardware, different product id and a
/// different HID interface; Protocol.h holds which is which.
enum class DeviceMode : std::uint8_t
{
    Base = 0,
    Control = 1
};

/// Number of device modes, derived from the enumeration rather than stated.
inline constexpr std::size_t DeviceModeCount = enumerators::denseEnumeratorCount<DeviceMode>();

/// Where the device is in its connect / re-enumerate / run cycle.
///
/// Connecting is not a failure: the device enumerates in base mode (0310), is
/// handed its initialisation sequence, and only then re-appears in control mode
/// (1310). Between those two moments there is nothing to talk to.
enum class ConnectionState : std::uint8_t
{
    Disconnected,
    Connecting,
    Connected
};

/// Why a device operation failed.
enum class DeviceError : std::uint8_t
{
    HidInitFailed = 0,
    DeviceNotFound = 1,
    OpenFailed = 2,
    NotConnected = 3,
    WriteFailed = 4,
    ReadFailed = 5
};

/// Number of DeviceError enumerators, derived from the enumeration.
inline constexpr std::size_t DeviceErrorCount = enumerators::denseEnumeratorCount<DeviceError>();

/// Human-readable text for each DeviceError, indexed by the enumerator.
inline constexpr std::array<std::string_view, DeviceErrorCount> DeviceErrorTexts {
    "hidapi initialisation failed",
    "no AX310 found on the USB bus",
    "the AX310 HID interface could not be opened",
    "the device is not connected",
    "writing to the device failed",
    "reading from the device failed",
};

/// @param error The error to describe.
/// @return Human-readable text for @p error.
[[nodiscard]] constexpr std::string_view describe(DeviceError error) noexcept
{
    return DeviceErrorTexts[static_cast<std::size_t>(error)];
}

/// @param knob The knob to index.
/// @return The knob's zero-based position, for indexing a per-knob table.
[[nodiscard]] constexpr std::size_t indexOf(KnobId knob) noexcept
{
    return static_cast<std::size_t>(knob);
}

/// @param mix The mix to index.
/// @return The mix's zero-based position, for indexing a per-mix table.
[[nodiscard]] constexpr std::size_t indexOf(MixId mix) noexcept
{
    return static_cast<std::size_t>(mix);
}

/// @param source The line-out source to index.
/// @return Its zero-based position, for indexing a per-source table.
[[nodiscard]] constexpr std::size_t indexOf(LineOutSource source) noexcept
{
    return static_cast<std::size_t>(source);
}

/// @param mode The mode to index.
/// @return The mode's zero-based position, for indexing a per-mode table.
[[nodiscard]] constexpr std::size_t indexOf(DeviceMode mode) noexcept
{
    return static_cast<std::size_t>(mode);
}

/// @param button The button to index.
/// @return The button's zero-based position, for indexing a per-button table.
[[nodiscard]] constexpr std::size_t indexOf(Button button) noexcept
{
    return static_cast<std::size_t>(button);
}

/// @param mode The surround mode to index.
/// @return Its zero-based position, for indexing a per-mode table.
[[nodiscard]] constexpr std::size_t indexOf(SurroundMode mode) noexcept
{
    return static_cast<std::size_t>(mode);
}

/// @param rows A table meant to be indexed by its enumerator.
/// @return Whether every row sits at the index of the enumerator it names.
template <typename Enum, std::size_t N>
[[nodiscard]] constexpr bool rowsInEnumeratorOrder(std::array<Enum, N> const& rows) noexcept
{
    return std::ranges::all_of(std::views::iota(std::size_t { 0 }, N),
                               [&rows](std::size_t index) { return indexOf(rows[index]) == index; });
}

/// The same guard for a table of rows rather than of enumerators.
///
/// @param rows The table to check.
/// @param project How to get a row's enumerator.
/// @return Whether every row sits at the index of the enumerator it names.
///
/// Anchoring a table's length on a named enumerator only fires when nothing is
/// wrong; this fires when a row is inserted in the wrong place, which is the
/// mistake that actually happens.
template <typename Row, std::size_t N, typename Project>
[[nodiscard]] constexpr bool rowsInEnumeratorOrder(std::array<Row, N> const& rows,
                                                   Project project) noexcept
{
    return std::ranges::all_of(std::views::iota(std::size_t { 0 }, N), [&](std::size_t index) {
        return indexOf(project(rows[index])) == index;
    });
}

// Every enumeration above is dense from zero, and the counts and AllX arrays are
// built on that: denseEnumeratorsOf() walks values from zero and stops at the
// first gap, so a hole would lose everything past it. Enumerators_test.cpp
// compares that walk against an exhaustive search, which is too slow for a header
// this widely included and cheap enough to pay once in a test.

} // namespace ax310
