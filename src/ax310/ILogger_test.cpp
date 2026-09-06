// SPDX-License-Identifier: Apache-2.0
#include <ax310/ILogger.hpp>

#include <Printers.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ax310;

TEST_CASE("logTo formats through one entry point that takes the level", "[logging]")
{
    CapturingLogger logger;

    logTo(logger, LogLevel::Warning, "chunk {} of {} failed", 3, 7);

    REQUIRE(logger.lines().size() == 1);
    CHECK(logger.lines().front().level == LogLevel::Warning);
    CHECK(logger.lines().front().message == "chunk 3 of 7 failed");
}

TEST_CASE("logTo keeps lines in the order they were written", "[logging]")
{
    CapturingLogger logger;

    logTo(logger, LogLevel::Debug, "first");
    logTo(logger, LogLevel::Error, "second");

    REQUIRE(logger.lines().size() == 2);
    CHECK(logger.lines()[0].message == "first");
    CHECK(logger.lines()[1].message == "second");
}

TEST_CASE("contains finds a line by level and substring", "[logging]")
{
    CapturingLogger logger;
    logTo(logger, LogLevel::Warning, "sendScreen failed: the device is not connected");

    CHECK(logger.contains(LogLevel::Warning, "not connected"));
    CHECK_FALSE(logger.contains(LogLevel::Error, "not connected"));
    CHECK_FALSE(logger.contains(LogLevel::Warning, "no such text"));
}

TEST_CASE("NullLogger accepts everything and keeps nothing", "[logging]")
{
    NullLogger logger;

    // The point is that it does not crash and has no state to inspect; a driver
    // handed one must behave exactly as it does with a real logger.
    logTo(logger, LogLevel::Error, "dropped on the floor");
    SUCCEED("NullLogger swallowed the line");
}

TEST_CASE("isLoggable keeps a line at or above the threshold and drops the rest", "[logging]")
{
    // The decision StderrLogger makes, extracted so it can be checked without
    // capturing a stream. Every combination, because an off-by-one on a
    // comparison here silently loses one severity band.
    for (std::size_t levelIndex = 0; levelIndex < LogLevelCount; ++levelIndex)
    {
        for (std::size_t thresholdIndex = 0; thresholdIndex < LogLevelCount; ++thresholdIndex)
        {
            auto const level = static_cast<LogLevel>(levelIndex);
            auto const threshold = static_cast<LogLevel>(thresholdIndex);

            INFO("level " << nameOf(level) << ", threshold " << nameOf(threshold));
            CHECK(isLoggable(level, threshold) == (levelIndex >= thresholdIndex));
        }
    }
}

TEST_CASE("the default threshold keeps everything except debug", "[logging]")
{
    CHECK_FALSE(isLoggable(LogLevel::Debug, LogLevel::Info));
    CHECK(isLoggable(LogLevel::Info, LogLevel::Info));
    CHECK(isLoggable(LogLevel::Error, LogLevel::Info));
}

TEST_CASE("levelFromName is the inverse of nameOf", "[logging]")
{
    for (std::size_t index = 0; index < LogLevelCount; ++index)
    {
        auto const level = static_cast<LogLevel>(index);
        INFO("level " << nameOf(level));
        REQUIRE(levelFromName(nameOf(level)).has_value());
        CHECK(*levelFromName(nameOf(level)) == level);
    }
}

TEST_CASE("levelFromName reports a name it does not know rather than guessing", "[logging]")
{
    CHECK_FALSE(levelFromName("verbose").has_value());
    CHECK_FALSE(levelFromName("").has_value());
    CHECK_FALSE(levelFromName("DEBUG").has_value()); // Case matters; the table is lower-case.
}
