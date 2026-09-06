// SPDX-License-Identifier: Apache-2.0
#include <ax310/IConsole.hpp>
#include <ax310/ILogger.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ax310;

TEST_CASE("output and diagnostics are kept apart", "[console]")
{
    // Which stream a message went to is behaviour, not decoration: it is what
    // lets somebody pipe a tool's answer somewhere without the complaints
    // travelling with it.
    CapturingConsole console;

    writeLine(console, "the answer");
    writeErrorLine(console, "the complaint");

    CHECK(console.output() == "the answer\n");
    CHECK(console.errors() == "the complaint\n");
}

TEST_CASE("write does not add a newline and writeLine does", "[console]")
{
    CapturingConsole console;

    write(console, "| {} ", 1);
    write(console, "| {} |", 2);
    writeLine(console, "");

    CHECK(console.output() == "| 1 | 2 |\n");
}

TEST_CASE("the console writes text rather than re-formatting it", "[console]")
{
    // A device path or a register name can contain a brace. Passing already
    // formatted text back through a formatter would treat it as a format string
    // and throw on the first one -- so the interface takes bytes, and only the
    // free helpers format.
    CapturingConsole console;

    console.write("a path with {braces} in it");

    CHECK(console.output() == "a path with {braces} in it");
}

TEST_CASE("a log line carries its level and ends in a newline", "[console][logging]")
{
    // The line ConsoleLogger composes was previously only observable by running
    // something and reading a terminal. This is that line.
    CapturingConsole console;
    ConsoleLogger logger { console, LogLevel::Info };

    logTo(logger, LogLevel::Warning, "cannot open {}", "/dev/hidraw9");

    CHECK(console.errors() == "[warning] cannot open /dev/hidraw9\n");
    CHECK(console.output().empty());
}

TEST_CASE("a log line below the threshold reaches the console at all", "[console][logging]")
{
    CapturingConsole console;
    ConsoleLogger logger { console, LogLevel::Warning };

    logTo(logger, LogLevel::Debug, "chatter");
    logTo(logger, LogLevel::Error, "the real thing");

    // Dropped rather than written and filtered later, which matters because
    // formatting a debug line the caller will never see is work done for nobody.
    CHECK_FALSE(console.errorsContain("chatter"));
    CHECK(console.errorsContain("the real thing"));
}

TEST_CASE("logging goes to the diagnostic stream, never to output", "[console][logging]")
{
    // ax310_spec writes a document to stdout. A log line landing in the middle
    // of it would corrupt the document silently -- the drift check would then
    // fail on a file that was generated correctly.
    CapturingConsole console;
    ConsoleLogger logger { console, LogLevel::Debug };

    logTo(logger, LogLevel::Info, "connected");

    CHECK(console.output().empty());
}

TEST_CASE("NullConsole discards both streams", "[console]")
{
    NullConsole console;
    writeLine(console, "ignored");
    writeErrorLine(console, "also ignored");
    SUCCEED("a test that would crash if the calls were not implemented");
}
