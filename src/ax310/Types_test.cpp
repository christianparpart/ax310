// SPDX-License-Identifier: Apache-2.0
#include <ax310/Types.hpp>

#include <Printers.hpp>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string_view>

using namespace ax310;

TEST_CASE("indexOf maps every enumerator to its own position", "[types]")
{
    CHECK(indexOf(Button::TopLeft) == 0);
    CHECK(indexOf(Button::BottomRight) == ButtonCount - 1);
    CHECK(indexOf(KnobId::Mic) == 0);
    CHECK(indexOf(KnobId::Chat) == KnobCount - 1);
    CHECK(indexOf(DeviceMode::Base) == 0);
    CHECK(indexOf(DeviceMode::Control) == 1);
}

TEST_CASE("the enumerator tables list every value at its own index", "[types]")
{
    // rowsInEnumeratorOrder already static_asserts this, so the value here is in
    // the counts: a table shorter than its enum would compile and quietly make
    // every "for (auto x: AllX)" loop skip the tail.
    REQUIRE(AllButtons.size() == ButtonCount);
    REQUIRE(AllKnobs.size() == KnobCount);

    CHECK(rowsInEnumeratorOrder(AllButtons));
    CHECK(rowsInEnumeratorOrder(AllKnobs));

    CHECK(AllButtons.front() == Button::TopLeft);
    CHECK(AllButtons.back() == Button::BottomRight);
    CHECK(AllKnobs.front() == KnobId::Mic);
    CHECK(AllKnobs.back() == KnobId::Chat);
}

TEST_CASE("describe answers for every DeviceError and never repeats itself", "[types]")
{
    std::set<std::string_view> seen;

    for (std::size_t index = 0; index < DeviceErrorCount; ++index)
    {
        auto const error = static_cast<DeviceError>(index);
        auto const text = describe(error);

        INFO("DeviceError index " << index);
        CHECK_FALSE(text.empty());
        CHECK(seen.insert(text).second); // A duplicate would mean a row was copied and not edited.
    }

    CHECK(describe(DeviceError::NotConnected) == "the device is not connected");
}

TEST_CASE("nameOf answers for every LogLevel", "[types][logging]")
{
    REQUIRE(LogLevelNames.size() == LogLevelCount);

    CHECK(nameOf(LogLevel::Debug) == "debug");
    CHECK(nameOf(LogLevel::Info) == "info");
    CHECK(nameOf(LogLevel::Warning) == "warning");
    CHECK(nameOf(LogLevel::Error) == "error");
}

TEST_CASE("LogLevel orders by severity, which is what threshold filtering relies on", "[types][logging]")
{
    CHECK(LogLevel::Debug < LogLevel::Info);
    CHECK(LogLevel::Info < LogLevel::Warning);
    CHECK(LogLevel::Warning < LogLevel::Error);
}

TEST_CASE("a level converts between steps and percent without rounding", "[types][level]")
{
    // The deck has 21 steps and one step is 5%, so both directions are exact.
    // The old setKnobVolume took percent and truncated `percent * 20 / 100`,
    // which snapped 24% to 20% -- this type exists so that cannot come back.
    for (int steps = 0; steps <= Level::MaxSteps; ++steps)
    {
        auto const level = Level::fromSteps(steps);
        int const roundTripped = level.steps();
        CHECK(roundTripped == steps);
        CHECK(level.asPercent() == steps * Level::PercentPerStep);
        CHECK(Level::fromPercent(level.asPercent()) == level);
    }
}

TEST_CASE("a level clamps rather than wrapping", "[types][level]")
{
    int const clampedLow = Level::fromSteps(-4).steps();
    int const clampedHigh = Level::fromSteps(99).steps();
    CHECK(clampedLow == 0);
    CHECK(clampedHigh == Level::MaxSteps);
    CHECK(Level::fromPercent(-10).asPercent() == 0);
    CHECK(Level::fromPercent(400).asPercent() == 100);
}

TEST_CASE("a percent between steps rounds to the nearest reachable one", "[types][level]")
{
    // 23% is not reachable. Rounding to 25% is visible and defensible; the old
    // truncation quietly lost it downwards.
    CHECK(Level::fromPercent(23).asPercent() == 25);
    CHECK(Level::fromPercent(22).asPercent() == 20);
    CHECK(Level::fromPercent(24).asPercent() == 25);
    CHECK(Level::fromPercent(2).asPercent() == 0);
    CHECK(Level::fromPercent(3).asPercent() == 5);
}

TEST_CASE("a default level is silent", "[types][level]")
{
    int const silent = Level {}.steps();
    CHECK(silent == 0);
    CHECK(Level {}.asPercent() == 0);
}

TEST_CASE("the mixes are named and indexed in enumerator order", "[types][mix]")
{
    CHECK(indexOf(MixId::Creator) == 0);
    CHECK(indexOf(MixId::Audience) == 1);
    CHECK(MixCount == std::size_t { 2 });
    CHECK(nameOf(MixId::Creator) == "Creator");
    CHECK(nameOf(MixId::Audience) == "Audience");
    CHECK(AllMixes.front() == MixId::Creator);
    CHECK(AllMixes.back() == MixId::Audience);
}
