// SPDX-License-Identifier: Apache-2.0
// The test doubles are library code, and a fake nobody exercises does not report
// its own bugs -- it reports the subject's, wrongly. These cases pin the parts of
// the doubles that a case could otherwise misread as a finding about the driver.

#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include <Printers.hpp>
#include <ax310/Event.hpp>
#include <ax310/FakeHidTransport.hpp>
#include <ax310/IClock.hpp>

using namespace ax310;
using namespace std::chrono_literals;

TEST_CASE("ManualClock advances only when something asks it to", "[fakes][clock]")
{
    ManualClock clock;
    auto const start = clock.now();

    CHECK(clock.now() == start); // No time passes on its own.

    clock.sleepFor(250ms);
    CHECK(clock.now() == start + 250ms);

    clock.advance(100ms);
    CHECK(clock.now() == start + 350ms);
}

TEST_CASE("ManualClock records what it was asked to sleep for", "[fakes][clock]")
{
    ManualClock clock;

    clock.sleepFor(10ms);
    clock.sleepFor(20ms);
    clock.advance(500ms); // Advancing is not sleeping and must not be recorded.

    REQUIRE(clock.sleeps().size() == 2);
    CHECK(clock.sleeps()[0] == 10ms);
    CHECK(clock.sleeps()[1] == 20ms);
    CHECK(clock.totalSlept() == 30ms);
}

TEST_CASE("SystemClock does not go backwards", "[fakes][clock]")
{
    SystemClock const clock;

    auto const first = clock.now();
    auto const second = clock.now();

    // No sleep: AGENT.md forbids wall-clock waits in tests. Monotonicity is the
    // only property worth asserting here and it needs no elapsed time.
    CHECK(second >= first);
}

TEST_CASE("RecordingListener counts each alternative separately", "[fakes][listener]")
{
    RecordingListener listener;

    listener.onDeviceEvent(ButtonPressed { .button = Button::TopLeft });
    listener.onDeviceEvent(KnobPushed { .knob = KnobId::LineIn });
    listener.onDeviceEvent(ButtonPressed { .button = Button::BottomRight });

    CHECK(listener.size() == 3);
    CHECK(listener.count<ButtonPressed>() == 2);
    CHECK(listener.count<KnobPushed>() == 1);
    CHECK(listener.count<ScreenTouched>() == 0);
}

TEST_CASE("RecordingListener returns the nth of a kind, skipping the others", "[fakes][listener]")
{
    RecordingListener listener;

    listener.onDeviceEvent(ButtonPressed { .button = Button::TopLeft });
    listener.onDeviceEvent(KnobPushed { .knob = KnobId::LineIn });
    listener.onDeviceEvent(ButtonPressed { .button = Button::BottomRight });

    CHECK(listener.nth<ButtonPressed>(0).button == Button::TopLeft);
    CHECK(listener.nth<ButtonPressed>(1).button == Button::BottomRight);
    CHECK(listener.nth<KnobPushed>(0).knob == KnobId::LineIn);
}

TEST_CASE("RecordingListener hands back a default rather than throwing when asked for one that is not there",
          "[fakes][listener]")
{
    RecordingListener listener;
    listener.onDeviceEvent(ButtonPressed { .button = Button::BottomRight });

    // The point of the default: a case that gets its count wrong should fail on
    // the value it asserts, not terminate on a bad variant access with nothing
    // said about which expectation was wrong.
    CHECK(listener.nth<ButtonPressed>(5).button == Button {});
    CHECK(listener.nth<KnobPushed>().knob == KnobId {});
}

TEST_CASE("RecordingListener forgets on request", "[fakes][listener]")
{
    RecordingListener listener;
    listener.onDeviceEvent(ButtonPressed { .button = Button::TopLeft });

    listener.clear();

    CHECK(listener.empty());
    CHECK(listener.count<ButtonPressed>() == 0);
}

TEST_CASE("FakeHidTransport answers only for the product id it was given", "[fakes][transport]")
{
    FakeHidTransport transport;
    transport.presentDevice(0x1310, { HidInterface { .path = "/dev/one", .interfaceNumber = 0 } });

    CHECK(transport.enumerate(0x07ca, 0x1310).size() == 1);
    CHECK(transport.enumerate(0x07ca, 0x0310).empty());

    REQUIRE(transport.probes().size() == 2);
    CHECK(transport.probes()[0].second == 0x1310);
    CHECK(transport.probes()[1].second == 0x0310);
}

TEST_CASE("FakeHidTransport hands out queued reads once each, then reads as idle", "[fakes][transport]")
{
    FakeHidTransport transport;
    transport.queueRead({ 0xAA, 0xBB });

    std::array<std::uint8_t, 8> buffer {};

    auto const first = transport.read(buffer, 100ms);
    REQUIRE(first.has_value());
    CHECK(*first == 2);
    CHECK(buffer[0] == 0xAA);

    // An empty queue must read as a timeout, not as an error: that is what an
    // idle deck does, and a fake that errored would make every quiet-path case
    // look like a broken device.
    auto const second = transport.read(buffer, 100ms);
    REQUIRE(second.has_value());
    CHECK(*second == 0);
}

TEST_CASE("FakeHidTransport separates the two send channels", "[fakes][transport]")
{
    FakeHidTransport transport;
    REQUIRE(transport.open("/dev/one").has_value());

    std::array<std::uint8_t, 2> const report { 0x01, 0x02 };
    REQUIRE(transport.sendFeatureReport(report).has_value());
    REQUIRE(transport.write(report).has_value());

    CHECK(transport.sentCount(FakeHidTransport::Channel::FeatureReport) == 1);
    CHECK(transport.sentCount(FakeHidTransport::Channel::Write) == 1);
    CHECK(transport.sent().size() == 2);
}

TEST_CASE("FakeHidTransport fails sends only from the point it was told to", "[fakes][transport]")
{
    FakeHidTransport transport;
    transport.failSendAfter(DeviceError::WriteFailed, 2);

    std::array<std::uint8_t, 1> const report { 0x01 };

    CHECK(transport.write(report).has_value());
    CHECK(transport.write(report).has_value());

    auto const third = transport.write(report);
    REQUIRE_FALSE(third.has_value());
    CHECK(third.error() == DeviceError::WriteFailed);
}
