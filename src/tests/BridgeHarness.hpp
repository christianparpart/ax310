// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <app/DeviceBridge.hpp>
#include <ax310/FakeHidTransport.hpp>
#include <ax310/IClock.hpp>
#include <ax310/ILogger.hpp>
#include <ax310/Protocol.hpp>

#include <QEventLoop>
#include <QTimer>

#include <array>

namespace ax310::tests
{

/// A DeviceBridge with a scripted deck behind it and no interface in front.
///
/// Shared, because both the rendering cases and the bridge's own cases need a
/// connected fake and they had drifted into two copies of it -- one of which had
/// to be edited every time the connect sequence changed.
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

    /// Lets the event loop run, which the worker thread's connect, the meter
    /// ballistics and any queued signal all need before anything is worth
    /// looking at.
    ///
    /// @param milliseconds How long to let it run.
    static void settle(int milliseconds = 350)
    {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    }
};

} // namespace ax310::tests
