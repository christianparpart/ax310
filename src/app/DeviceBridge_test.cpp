// SPDX-License-Identifier: Apache-2.0
/// What the bridge does with the events the driver hands it.
///
/// The driver's own tests stop at the wire: they assert which bytes a turn or a
/// push produces. What happens *next* -- which mix a turn is applied to, whether
/// a push reaches the thing it is supposed to operate -- lives here, because it
/// is the bridge that decides it and nothing was checking.

#include <ax310/Types.hpp>

#include <BridgeHarness.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace ax310;

namespace
{

constexpr int CreatorMix = 0;
constexpr int MicTrack = 0;

} // namespace

TEST_CASE("pushing the microphone's knob toggles monitoring", "[bridge][knob]")
{
    tests::BridgeHarness harness;
    harness.connectWithLevels({ 15, 10, 10, 10, 10, 10 }, { 12, 10, 10, 10, 10, 10 });

    REQUIRE(harness.bridge.micMonitor());
    REQUIRE(harness.bridge.levelPercent(CreatorMix, MicTrack) == 75);

    // Monitoring is the microphone's level in the mix the streamer hears, so the
    // push silences that track and the push after it puts back what was silenced
    // rather than something louder.
    harness.bridge.onDeviceEvent(KnobPushed { .knob = KnobId::Mic });
    CHECK_FALSE(harness.bridge.micMonitor());
    CHECK(harness.bridge.levelPercent(CreatorMix, MicTrack) == 0);

    harness.bridge.onDeviceEvent(KnobPushed { .knob = KnobId::Mic });
    CHECK(harness.bridge.micMonitor());
    CHECK(harness.bridge.levelPercent(CreatorMix, MicTrack) == 75);
}

TEST_CASE("pushing any other knob leaves monitoring alone", "[bridge][knob]")
{
    tests::BridgeHarness harness;
    harness.connectWithLevels({ 15, 10, 10, 10, 10, 10 }, { 12, 10, 10, 10, 10, 10 });

    REQUIRE(harness.bridge.micMonitor());

    // The microphone is the one knob with a second meaning. Every other push has
    // to stay inert until somebody decides what it should do.
    for (auto const knob: AllKnobs)
    {
        if (knob == KnobId::Mic)
            continue;

        INFO("pushing the " << nameOf(knob) << " knob");
        harness.bridge.onDeviceEvent(KnobPushed { .knob = knob });
        CHECK(harness.bridge.micMonitor());
        CHECK(harness.bridge.levelPercent(CreatorMix, MicTrack) == 75);
    }
}
