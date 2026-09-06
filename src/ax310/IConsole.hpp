// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace ax310
{

/// Where a program's output goes.
///
/// The fourth injected seam, beside IHidTransport, IClock and ILogger, and it
/// exists for the same two reasons they do.
///
/// The first is testable behaviour. A tool that decides what to print is making a
/// decision worth checking, and until this existed those decisions could only be
/// checked by running the binary and reading a terminal -- so mostly they were
/// not checked at all.
///
/// The second is that writing to a console is not as portable as it looks. The
/// ostream overloads of `std::print` are C++23's P2539: libstdc++ has them and
/// MSVC's `<print>` does not. That cost one build the first time this project was
/// compiled on Windows, in a file that had been written the day before. One
/// implementation of this interface is now the only place in the tree that names
/// stdout or stderr, so there is one place to keep portable rather than seven.
///
/// Neither method appends a newline. `writeLine` and `writeErrorLine` below do,
/// and are what callers normally want.
class IConsole
{
  public:
    IConsole() = default;
    virtual ~IConsole() = default;

    // Injected by reference and owned as a concrete type, never copied and never
    // sliced -- the same reasoning as the other three seams.
    IConsole(IConsole const&) = delete;
    IConsole& operator=(IConsole const&) = delete;
    IConsole(IConsole&&) = delete;
    IConsole& operator=(IConsole&&) = delete;

    /// Ordinary output: what the program was asked to produce.
    /// @param text Written verbatim, with no newline added.
    virtual void write(std::string_view text) = 0;

    /// Diagnostics: what went wrong, and usage. Kept separate so a caller can
    /// pipe the answer somewhere without the complaints going with it.
    /// @param text Written verbatim, with no newline added.
    virtual void writeError(std::string_view text) = 0;
};

/// Formats and writes one line to ordinary output.
///
/// A free function rather than a member, so IConsole stays two methods wide and
/// a fake has two methods to implement. The same shape as logTo() next door.
///
/// @param console Where it goes.
/// @param format The format string.
/// @param args What it formats.
template <typename... Args>
void writeLine(IConsole& console, std::format_string<Args...> format, Args&&... args)
{
    console.write(std::format(format, std::forward<Args>(args)...) + "\n");
}

/// Formats and writes to ordinary output without a trailing newline, for a line
/// assembled from several calls.
/// @param console Where it goes.
/// @param format The format string.
/// @param args What it formats.
template <typename... Args>
void write(IConsole& console, std::format_string<Args...> format, Args&&... args)
{
    console.write(std::format(format, std::forward<Args>(args)...));
}

/// Formats and writes one line to the diagnostic stream.
/// @param console Where it goes.
/// @param format The format string.
/// @param args What it formats.
template <typename... Args>
void writeErrorLine(IConsole& console, std::format_string<Args...> format, Args&&... args)
{
    console.writeError(std::format(format, std::forward<Args>(args)...) + "\n");
}

/// The real one: standard output and standard error.
///
/// The only type in this project that names either. It writes bytes rather than
/// formatting, so the portability question is confined to one file.
class SystemConsole final: public IConsole
{
  public:
    void write(std::string_view text) override;
    void writeError(std::string_view text) override;
};

/// Discards everything. For a test that does not care what was printed.
class NullConsole final: public IConsole
{
  public:
    void write(std::string_view /*text*/) override {}
    void writeError(std::string_view /*text*/) override {}
};

/// Keeps everything, so a test can assert on what a program decided to say.
///
/// The two streams are kept apart because which one a message went to is part of
/// the behaviour: an answer on stdout and a complaint on stderr is what lets a
/// caller pipe one without the other.
class CapturingConsole final: public IConsole
{
  public:
    void write(std::string_view text) override { _output += text; }
    void writeError(std::string_view text) override { _errors += text; }

    /// @return Everything written to ordinary output, concatenated.
    [[nodiscard]] std::string const& output() const noexcept { return _output; }

    /// @return Everything written to the diagnostic stream, concatenated.
    [[nodiscard]] std::string const& errors() const noexcept { return _errors; }

    /// @param needle What to look for.
    /// @return Whether ordinary output contains it.
    [[nodiscard]] bool outputContains(std::string_view needle) const
    {
        return _output.contains(needle);
    }

    /// @param needle What to look for.
    /// @return Whether the diagnostic stream contains it.
    [[nodiscard]] bool errorsContain(std::string_view needle) const
    {
        return _errors.contains(needle);
    }

  private:
    std::string _output;
    std::string _errors;
};

} // namespace ax310
