// SPDX-License-Identifier: Apache-2.0
/// What the bridge does with the events the driver hands it.
///
/// The driver's own tests stop at the wire: they assert which bytes a turn or a
/// push produces. What happens *next* -- which mix a turn is applied to, whether
/// a push reaches the thing it is supposed to operate -- lives here, because it
/// is the bridge that decides it and nothing was checking.

#include <app/DeviceBridge.hpp>
#include <ax310/FakeHidTransport.hpp>
#include <ax310/IClock.hpp>
#include <ax310/ILogger.hpp>
#include <ax310/Protocol.hpp>

#include <QEventLoop>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace ax310;

namespace
{

/// A bridge with a scripted deck behind it and no interface in front.
struct BridgeHarness
{
    FakeHidTransport transport;
    ManualClock clock;
    NullLogger logger;
    app::DeviceBridge bridge { transport, clock, logger };

    BridgeHarness()
    {
        transport.presentDevice(protocol::descriptorFor(DeviceMode::Control).productId,
                                { HidInterface { .path = "/dev/control", .interfaceNumber = 0 } });
    }

    ~BridgeHarness() { bridge.stop(); }

    BridgeHarness(BridgeHarness const&) = delete;
    BridgeHarness& operator=(BridgeHarness const&) = delete;
    BridgeHarness(BridgeHarness&&) = delete;
    BridgeHarness& operator=(BridgeHarness&&) = delete;

    /// Brings the fake deck up with a level in every register, so writes succeed
    /// and the bridge starts out agreeing with the deck.
    ///
    /// @param creatorSteps Level per track in the creator mix, 0..20.
    /// @param audienceSteps The same for the audience mix.
    void connectWithLevels(std::array<int, KnobCount> const& creatorSteps,
                           std::array<int, KnobCount> const& audienceSteps)
    {
        for (auto const knob: AllKnobs)
        {
            transport.setRegister(protocol::levelAddressOf(protocol::Property::CreatorMixLevels, knob),
                                  { static_cast<std::uint8_t>(creatorSteps[indexOf(knob)]) });
            transport.setRegister(protocol::levelAddressOf(protocol::Property::AudienceMixLevels, knob),
                                  { static_cast<std::uint8_t>(audienceSteps[indexOf(knob)]) });
        }

        bridge.start();
        settle();
    }

    /// Lets the worker thread get through its connect before a case looks.
    static void settle(int milliseconds = 350)
    {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    }
};

constexpr int CreatorMix = 0;
constexpr int MicTrack = 0;

} // namespace

TEST_CASE("pushing the microphone's knob toggles monitoring", "[bridge][knob]")
{
    BridgeHarness harness;
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
    BridgeHarness harness;
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
