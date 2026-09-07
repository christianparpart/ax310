// SPDX-License-Identifier: Apache-2.0
#include <ax310/Enumerators.hpp>
#include <ax310/ILogger.hpp>
#include <ax310/Protocol.hpp>
#include <ax310/Types.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ax310;
using namespace ax310::enumerators;

namespace
{

/// Every enumeration whose count is derived by walking values from zero.
///
/// `denseEnumeratorCount()` stops at the first gap, which is what makes it cheap
/// enough to sit in a header included everywhere -- and what makes it wrong for
/// an enumeration that grew a hole, silently losing everything past it.
///
/// So the exhaustive search runs here instead, once, and the two are compared.
/// The full search instantiates a template for all 256 candidate values, which
/// costs about a second and a half per translation unit; that is affordable in
/// one test and was not affordable in Types.hpp.
template <typename Enum>
consteval bool denseWalkFindsEverything()
{
    return denseEnumeratorCount<Enum>() == enumeratorCount<Enum>();
}

static_assert(denseWalkFindsEverything<Button>(), "Button is no longer dense from zero");
static_assert(denseWalkFindsEverything<KnobId>(), "KnobId is no longer dense from zero");
static_assert(denseWalkFindsEverything<MixId>(), "MixId is no longer dense from zero");
static_assert(denseWalkFindsEverything<SurroundMode>(), "SurroundMode is no longer dense from zero");
static_assert(denseWalkFindsEverything<LineOutSource>(),
              "LineOutSource is no longer dense from zero");
static_assert(denseWalkFindsEverything<DeviceMode>(), "DeviceMode is no longer dense from zero");
static_assert(denseWalkFindsEverything<DeviceError>(), "DeviceError is no longer dense from zero");
static_assert(denseWalkFindsEverything<LogLevel>(), "LogLevel is no longer dense from zero");
static_assert(denseWalkFindsEverything<protocol::EffectId>(),
              "EffectId is no longer dense from zero");
static_assert(denseWalkFindsEverything<protocol::Parameter>(),
              "Parameter is no longer dense from zero");

} // namespace

TEST_CASE("the counts are what the enumerations declare", "[enumerators]")
{
    // Against the numbers the old `Last = Something` idiom produced, so the
    // change that retired it cannot have moved any of them.
    CHECK(ButtonCount == 4);
    CHECK(KnobCount == 6);
    CHECK(MixCount == 2);
    CHECK(SurroundModeCount == 6);
    CHECK(LineOutSourceCount == 3);
    CHECK(DeviceModeCount == 2);
    CHECK(DeviceErrorCount == 6);
    CHECK(LogLevelCount == 4);
    CHECK(protocol::EffectCount == 5);
    CHECK(protocol::ParameterCount == 7);
}

TEST_CASE("the derived arrays hold every enumerator, in order", "[enumerators]")
{
    REQUIRE(AllButtons.size() == ButtonCount);
    CHECK(AllButtons.front() == Button::TopLeft);
    CHECK(AllButtons.back() == Button::BottomRight);

    REQUIRE(AllKnobs.size() == KnobCount);
    CHECK(AllKnobs.front() == KnobId::Mic);
    CHECK(AllKnobs.back() == KnobId::Chat);

    // The ends are checked rather than the whole list because the middle is
    // exactly what the enumeration says; naming all six here would be a second
    // hand-written copy of the thing this change removed.
    CHECK(AllMixes.back() == MixId::Audience);
    CHECK(AllSurroundModes.back() == SurroundMode::ScrollingRgb);
    CHECK(AllLineOutSources.back() == LineOutSource::ChatMic);
}

TEST_CASE("an enumeration with a hole is caught, not truncated", "[enumerators]")
{
    // The failure the static assertions above exist for, demonstrated on an
    // enumeration shaped to have it: the cheap walk stops at the gap and the
    // exhaustive search does not, so the two disagree.
    enum class Holed : std::uint8_t
    {
        A = 0,
        B = 1,
        Far = 9,
    };

    STATIC_CHECK(denseEnumeratorCount<Holed>() == 2);
    STATIC_CHECK(enumeratorCount<Holed>() == 3);
    STATIC_CHECK(!denseWalkFindsEverything<Holed>());
}
