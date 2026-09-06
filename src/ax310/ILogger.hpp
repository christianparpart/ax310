// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ax310
{

/// How much the reader of a log line should care.
enum class LogLevel : std::uint8_t
{
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,

    Last = Error
};

/// Number of log levels, derived from the enumeration rather than stated.
inline constexpr std::size_t LogLevelCount = static_cast<std::size_t>(LogLevel::Last) + 1;

/// Display text for each LogLevel, indexed by the enumerator.
inline constexpr std::array<std::string_view, LogLevelCount> LogLevelNames {
    "debug", "info", "warning", "error"
};

/// @param level The level to name.
/// @return Display text for @p level.
[[nodiscard]] constexpr std::string_view nameOf(LogLevel level) noexcept
{
    return LogLevelNames[static_cast<std::size_t>(level)];
}

/// @param level The level of the line being written.
/// @param threshold The lowest level a sink keeps.
/// @return Whether a line at @p level survives @p threshold.
///
/// Extracted from StderrLogger so the decision can be tested without capturing a
/// stream: what a sink does with a line is its business, but *which* lines reach
/// it is a rule worth pinning.
[[nodiscard]] constexpr bool isLoggable(LogLevel level, LogLevel threshold) noexcept
{
    return level >= threshold;
}

/// @param name A level name, as spelled in LogLevelNames.
/// @return The matching level, or nothing when @p name is not one of them.
///
/// The inverse of nameOf(), driven by the same table so the two cannot drift.
[[nodiscard]] constexpr std::optional<LogLevel> levelFromName(std::string_view name) noexcept
{
    for (std::size_t index = 0; index < LogLevelCount; ++index)
        if (LogLevelNames[index] == name)
            return static_cast<LogLevel>(index);

    return std::nullopt;
}

/// Where the driver's diagnostics go.
///
/// Injected rather than written to stderr directly: a library that prints has
/// decided for its host where output belongs, and a test that wants to assert
/// on what was logged has nowhere to look.
class ILogger
{
  public:
    ILogger() = default;
    virtual ~ILogger() = default;

    // Injected by reference and owned as a concrete type, never copied and never
    // sliced. Saying so is the Rule of Five: a class that declares a destructor
    // and nothing else gets copy and move implicitly, which for a polymorphic
    // base is how slicing happens quietly.
    ILogger(ILogger const&) = delete;
    ILogger& operator=(ILogger const&) = delete;
    ILogger(ILogger&&) = delete;
    ILogger& operator=(ILogger&&) = delete;

    /// @param level How much the reader should care.
    /// @param message The already-formatted line, without a trailing newline.
    virtual void log(LogLevel level, std::string_view message) = 0;
};

/// Formats and logs one line.
///
/// A single entry point taking the level, rather than one function per level:
/// a new level is a new enumerator and a new row in LogLevelNames.
///
/// @param logger Where the line goes.
/// @param level How much the reader should care.
/// @param format The format string.
/// @param args Its arguments.
template <typename... Args>
void logTo(ILogger& logger, LogLevel level, std::format_string<Args...> format, Args&&... args)
{
    logger.log(level, std::format(format, std::forward<Args>(args)...));
}

/// A logger that discards everything. The default where a caller supplies none.
class NullLogger final: public ILogger
{
  public:
    void log(LogLevel /*level*/, std::string_view /*message*/) override {}
};

/// A logger that writes to stderr, one line per call, prefixed with the level.
class StderrLogger final: public ILogger
{
  public:
    /// @param threshold Lines below this level are dropped.
    explicit StderrLogger(LogLevel threshold = LogLevel::Info): _threshold { threshold } {}

    void log(LogLevel level, std::string_view message) override;

  private:
    LogLevel _threshold;
};

/// A logger that keeps what it was told, so a test can assert that a failure was
/// reported rather than swallowed.
class CapturingLogger final: public ILogger
{
  public:
    /// One captured line.
    struct Line
    {
        LogLevel level;
        std::string message;
    };

    void log(LogLevel level, std::string_view message) override
    {
        _lines.push_back(Line { .level = level, .message = std::string { message } });
    }

    /// @return Every line captured, in order.
    [[nodiscard]] std::vector<Line> const& lines() const noexcept { return _lines; }

    /// @param level The level to look for.
    /// @param needle Text the message must contain.
    /// @return Whether any captured line at @p level contains @p needle.
    [[nodiscard]] bool contains(LogLevel level, std::string_view needle) const
    {
        return std::ranges::any_of(
            _lines, [&](Line const& line) { return line.level == level && line.message.contains(needle); });
    }

  private:
    std::vector<Line> _lines;
};

} // namespace ax310
