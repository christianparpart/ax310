// SPDX-License-Identifier: Apache-2.0
#include <ax310/Commands.hpp>
#include <ax310/Device.hpp>
#include <ax310/FakeHidTransport.hpp>

#include <Printers.hpp>
#include <ReportBuilder.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <thread>
#include <vector>

using namespace ax310;
using namespace std::chrono_literals;
using ax310::testing::ReportBuilder;

namespace
{

/// The four collaborators plus the device, constructed together.
///
/// Device is neither copyable nor movable, so it has to be built in place; this
/// is what lets a case say `Harness harness;` and get a wired-up driver.
struct Harness
{
    FakeHidTransport transport;
    ManualClock clock;
    CapturingLogger logger;
    RecordingListener listener;
    Device device { transport, clock, logger, listener };

    /// The transport stamps every send from the same clock the driver sleeps
    /// on, so a case can ask how far apart two writes actually went out.
    Harness() { transport.useClock(clock); }

    /// Gives every track the same level in both mixes before a connect.
    ///
    /// A turn is relative, so it needs a level to be relative *to*, and that
    /// level is the deck's. A case that skips this connects to a deck holding
    /// nothing and every track starts silent.
    ///
    /// @param steps The level, in the hardware's own steps, 0..20.
    void holdLevels(int steps)
    {
        for (auto const mix: AllMixes)
        {
            auto const block = mix == MixId::Creator ? protocol::Property::CreatorMixLevels
                                                     : protocol::Property::AudienceMixLevels;
            for (auto const knob: AllKnobs)
                transport.setRegister(protocol::levelAddressOf(block, knob),
                                      { static_cast<std::uint8_t>(steps) });
        }
    }

    /// Puts a control-mode deck on the fake bus.
    void presentControlDevice()
    {
        transport.presentDevice(protocol::descriptorFor(DeviceMode::Control).productId,
                                { HidInterface { .path = "/dev/control", .interfaceNumber = 0 } });
    }

    /// Puts a base-mode deck on the fake bus, the state it enumerates in.
    void presentBaseDevice()
    {
        transport.presentDevice(protocol::descriptorFor(DeviceMode::Base).productId,
                                { HidInterface { .path = "/dev/base", .interfaceNumber = 4 } });
    }

    /// Brings the device up in control mode and clears what that produced, so a
    /// case asserts on its own traffic rather than on the connect.
    void connectInControlMode()
    {
        presentControlDevice();
        auto const state = device.connect();
        REQUIRE(state.has_value());
        REQUIRE(*state == ConnectionState::Connected);
        // Drops the initialisation traffic, so a case sees only its own.
        transport.clearSent();
        listener.clear();
    }
};

/// What choosing a mix takes, pinned by its own case further down.
constexpr std::size_t MixSwitchWrites = 5;

/// The gaps inside a mix switch: one ahead of the ring-colour record and one
/// behind it, so the deck is not asked to take a record between two crowding
/// writes.
constexpr std::size_t MixSwitchGaps = 2;

/// What adopting the deck's own state costs a connect, in reports sent.
///
/// One read for the mix it came up monitoring, the writes that choosing a mix
/// takes -- made whether or not that read answered, because a mix nobody can
/// read is settled rather than left to disagree with the deck -- and one read
/// per level in both mixes.
constexpr std::size_t StateAdoptionReads = 1 + MixSwitchWrites + (MixCount * KnobCount);

/// @param frame The bytes to sum.
/// @return The 16-bit unsigned sum the chunk header carries.
std::uint16_t checksumOf(std::span<std::uint8_t const> frame)
{
    std::uint16_t sum = 0;
    for (auto const byte: frame)
        sum = static_cast<std::uint16_t>(sum + byte);
    return sum;
}

/// @param size How many bytes.
/// @return A frame whose bytes vary, so a chunking bug shows up as wrong content
///         rather than as an identical-looking block.
std::vector<std::uint8_t> makeFrame(std::size_t size)
{
    std::vector<std::uint8_t> frame(size);
    std::ranges::iota(frame, std::uint8_t { 1 });
    return frame;
}

} // namespace

// --- connect -------------------------------------------------------------

TEST_CASE("connect reports DeviceNotFound when the bus is empty", "[device][connect]")
{
    Harness harness;

    auto const result = harness.device.connect();

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DeviceError::DeviceNotFound);
    CHECK(harness.device.connectionState() == ConnectionState::Disconnected);
    CHECK(harness.listener.empty());
}

TEST_CASE("connect opens the deck and wakes it", "[device][connect]")
{
    Harness harness;
    harness.presentControlDevice();

    auto const result = harness.device.connect();

    REQUIRE(result.has_value());
    CHECK(*result == ConnectionState::Connected);
    CHECK(harness.device.isConnected());
    CHECK(harness.transport.openedPath() == "/dev/control");

    REQUIRE(harness.listener.count<ConnectionChanged>() == 1);
    CHECK(harness.listener.nth<ConnectionChanged>().state == ConnectionState::Connected);

    // The deck comes up asleep every time, so the sequence goes out on every
    // attach rather than only when some "needs init" state is detected. Ahead of
    // it goes one read per register the sequence is about to overwrite; this fake
    // answers none of them, so nothing is written back.
    CHECK(harness.transport.sentCount(FakeHidTransport::Channel::FeatureReport)
          == protocol::PreservedAddresses.size() + commands::InitPayloads.size()
                 + protocol::EffectEnables.size() + StateAdoptionReads + ButtonCount);
    // The mix read and the twelve level reads; the writes beside them are sends,
    // not reads.
    CHECK(harness.transport.featureReadCount()
          == protocol::PreservedAddresses.size() + 1 + (MixCount * KnobCount));

    // One gap per handshake payload, one more per button record, and the two a
    // mix switch takes around its ring-colour record: the deck applies only some
    // of a run of records sent back to back, so every one of them is spaced the
    // way the handshake is.
    CHECK(harness.clock.totalSlept()
          == 10ms * (commands::InitPayloads.size() + ButtonCount + MixSwitchGaps));
}

TEST_CASE("connect goes straight to the deck's own device", "[device][connect]")
{
    Harness harness;
    harness.presentControlDevice();
    harness.presentBaseDevice();

    REQUIRE(harness.device.connect().has_value());

    // Base is a second USB device of the same unit, carrying the audio side's
    // media keys. Both are on the bus at once and only Control is ours, so the
    // other one is never even enumerated.
    REQUIRE(harness.transport.probes().size() == 1);
    CHECK(harness.transport.probes().front().second
          == protocol::descriptorFor(DeviceMode::Control).productId);
}

TEST_CASE("a deck that will not take its initialisation is not reported as connected", "[device][connect]")
{
    Harness harness;
    harness.presentControlDevice();
    // Counted past the snapshot reads, so the failure lands where this case says
    // it does: partway through the initialisation itself.
    harness.transport.failSendAfter(DeviceError::WriteFailed, protocol::PreservedAddresses.size() + 3);

    auto const result = harness.device.connect();

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DeviceError::WriteFailed);

    // Reporting Connected here would leave the app polling a deck nothing woke:
    // dark screen, no reports, and no error to explain either.
    CHECK(harness.device.connectionState() != ConnectionState::Connected);
    CHECK(harness.listener.count<ConnectionChanged>() == 0);
}

TEST_CASE("connect sends the init payloads framed with a report-id byte", "[device][connect]")
{
    Harness harness;
    harness.presentControlDevice();

    REQUIRE(harness.device.connect().has_value());

    // The snapshot reads go out first, so the init payloads start after them.
    auto const first = protocol::PreservedAddresses.size();
    REQUIRE(harness.transport.sent().size()
            == first + commands::InitPayloads.size() + protocol::EffectEnables.size()
                   + StateAdoptionReads + ButtonCount);
    for (std::size_t index = 0; index < commands::InitPayloads.size(); ++index)
    {
        INFO("init payload " << index);
        auto const& sent = harness.transport.sent()[first + index].bytes;

        // 65 bytes, not 64: the interface declares no report IDs, so hidapi's
        // leading byte is 0x00. Sending the bare 64 made hidapi read 0x81 as a
        // report id, and every payload was rejected.
        REQUIRE(sent.size() == protocol::FeatureReportSize);
        CHECK(sent.front() == 0x00);
        CHECK(std::ranges::equal(std::span { sent }.subspan(1), commands::InitPayloads[index]));
    }
}

TEST_CASE("connect surfaces a transport that will not initialise", "[device][connect]")
{
    Harness harness;
    harness.presentControlDevice();
    harness.transport.failInitialize(DeviceError::HidInitFailed);

    auto const result = harness.device.connect();

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DeviceError::HidInitFailed);
}

TEST_CASE("connect reports OpenFailed when the deck is present but will not open", "[device][connect]")
{
    Harness harness;
    harness.presentControlDevice();
    harness.transport.failOpen(DeviceError::OpenFailed);

    auto const result = harness.device.connect();

    REQUIRE_FALSE(result.has_value());

    // Found-but-unopenable is a different problem from absent -- usually a
    // permissions one -- and the caller can only say so if we distinguish them.
    CHECK(result.error() == DeviceError::OpenFailed);
}

TEST_CASE("connect accepts an interface whose number the backend does not report", "[device][connect]")
{
    Harness harness;

    // macOS and Windows backends report -1 rather than a USB interface number.
    harness.transport.presentDevice(protocol::descriptorFor(DeviceMode::Control).productId,
                                    { HidInterface { .path = "/dev/unnumbered", .interfaceNumber = -1 } });

    auto const result = harness.device.connect();

    REQUIRE(result.has_value());
    CHECK(*result == ConnectionState::Connected);
    CHECK(harness.transport.openedPath() == "/dev/unnumbered");
}

TEST_CASE("connect skips interfaces that are not the one carrying the protocol", "[device][connect]")
{
    Harness harness;
    harness.transport.presentDevice(protocol::descriptorFor(DeviceMode::Control).productId,
                                    {
                                        HidInterface { .path = "/dev/audio", .interfaceNumber = 2 },
                                        HidInterface { .path = "/dev/control", .interfaceNumber = 0 },
                                    });

    REQUIRE(harness.device.connect().has_value());

    // The deck is a composite device; opening its audio interface would succeed
    // and then never produce a report.
    CHECK(harness.transport.openedPath() == "/dev/control");
}

TEST_CASE("connect on an already connected device is a no-op", "[device][connect]")
{
    Harness harness;
    harness.connectInControlMode();

    auto const result = harness.device.connect();

    REQUIRE(result.has_value());
    CHECK(*result == ConnectionState::Connected);
    CHECK(harness.listener.empty()); // No second announcement.
}

// --- disconnect ----------------------------------------------------------

TEST_CASE("disconnect sends the shutdown sequence and announces the change", "[device][disconnect]")
{
    Harness harness;
    harness.connectInControlMode();

    harness.device.disconnect();

    CHECK(harness.transport.sent().size() == commands::ShutdownPayloads.size());
    CHECK(harness.device.connectionState() == ConnectionState::Disconnected);
    CHECK_FALSE(harness.transport.isOpen());

    REQUIRE(harness.listener.count<ConnectionChanged>() == 1);
    CHECK(harness.listener.nth<ConnectionChanged>().state == ConnectionState::Disconnected);
}

TEST_CASE("disconnect without a connection sends nothing", "[device][disconnect]")
{
    Harness harness;

    harness.device.disconnect();

    CHECK(harness.transport.sent().empty());
    CHECK(harness.listener.empty());
}

// --- poll ----------------------------------------------------------------

TEST_CASE("poll refuses when the device is not connected", "[device][poll]")
{
    Harness harness;

    auto const result = harness.device.poll(100ms);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DeviceError::NotConnected);
}

TEST_CASE("poll passes its timeout through to the transport", "[device][poll]")
{
    Harness harness;
    harness.connectInControlMode();
    harness.transport.queueTimeout();

    REQUIRE(harness.device.poll(250ms).has_value());

    CHECK(harness.transport.lastReadTimeout() == 250ms);
}

TEST_CASE("poll treats a timeout as an idle deck, not an error", "[device][poll]")
{
    Harness harness;
    harness.connectInControlMode();
    harness.transport.queueTimeout();

    auto const result = harness.device.poll(100ms);

    REQUIRE(result.has_value());
    CHECK(harness.listener.empty());
}

TEST_CASE("poll surfaces a read failure", "[device][poll]")
{
    Harness harness;
    harness.connectInControlMode();
    harness.transport.failRead(DeviceError::ReadFailed);

    auto const result = harness.device.poll(100ms);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DeviceError::ReadFailed);
}

TEST_CASE("poll ignores a report of a size it cannot interpret", "[device][poll]")
{
    Harness harness;
    harness.connectInControlMode();
    harness.transport.queueRead(std::vector<std::uint8_t>(17, 0xAB));

    auto const result = harness.device.poll(100ms);

    // Not an error -- a stray report is not a broken device -- but nothing may be
    // decoded out of bytes we do not understand.
    REQUIRE(result.has_value());
    CHECK(harness.listener.empty());
    CHECK(harness.logger.contains(LogLevel::Warning, "Unexpected HID report size"));
}

TEST_CASE("poll strips the report-id byte some backends prepend", "[device][poll]")
{
    Harness harness;
    harness.connectInControlMode();

    // Seed, then press a button, both delivered in the 65-byte form.
    harness.transport.queueRead(ReportBuilder {}.buildWithReportId());
    harness.transport.queueRead(ReportBuilder {}.buttons(0x08).buildWithReportId());

    REQUIRE(harness.device.poll(100ms).has_value());
    REQUIRE(harness.device.poll(100ms).has_value());

    // If the leading byte were not stripped, offset 0 would read as 0x00 and no
    // button would ever be seen.
    REQUIRE(harness.listener.count<ButtonPressed>() == 1);
    CHECK(harness.listener.nth<ButtonPressed>().button == Button::TopLeft);
}

TEST_CASE("the first report is adopted as the starting position, not reported as a change", "[device][poll]")
{
    Harness harness;
    harness.connectInControlMode();

    // A deck whose knobs are already turned and touched when we arrive.
    harness.transport.queueRead(
        ReportBuilder {}.knobTouch(0x3F).knobValue(KnobId::Mic, 10).knobValue(KnobId::System, 20).build());

    REQUIRE(harness.device.poll(100ms).has_value());

    // Without seeding, every knob would fire a volume change on the first report
    // and the UI would jump on connect.
    CHECK(harness.listener.count<KnobVolumeChanged>() == 0);
    CHECK(harness.listener.count<KnobTouched>() == 0);
}

// --- decoding ------------------------------------------------------------

TEST_CASE("a button bit decodes to the button's identity, not to its mask", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();
    harness.transport.queueRead(ReportBuilder {}.build()); // Seed (a real report, not filler).
    REQUIRE(harness.device.poll(100ms).has_value());

    // Written out rather than read from protocol::ButtonBits, so this checks the
    // decode against the mapping confirmed on the deck instead of checking the
    // table against itself. `ax310_probe --inputs` is what measured these: it
    // lights one button and reads the byte that arrives, so the press is
    // identified by the light rather than by the table.
    auto const [bit, expected] = GENERATE(table<std::uint8_t, Button>({
        { 0x08, Button::TopLeft },
        { 0x04, Button::TopRight },
        { 0x02, Button::BottomLeft },
        { 0x01, Button::BottomRight },
    }));

    harness.transport.queueRead(ReportBuilder {}.buttons(bit).build());
    REQUIRE(harness.device.poll(100ms).has_value());

    REQUIRE(harness.listener.count<ButtonPressed>() == 1);
    CHECK(harness.listener.nth<ButtonPressed>().button == expected);
}

TEST_CASE("a button release produces nothing", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();

    harness.transport.queueRead(ReportBuilder {}.build());
    harness.transport.queueRead(ReportBuilder {}.buttons(0x08).build());
    harness.transport.queueRead(ReportBuilder {}.buttons(0x00).build());

    for (int step = 0; step < 3; ++step)
        REQUIRE(harness.device.poll(100ms).has_value());

    // Only the press: the deck has no release event and the driver does not
    // invent one.
    CHECK(harness.listener.count<ButtonPressed>() == 1);
}

TEST_CASE("all four buttons at once report all four", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();
    harness.transport.queueRead(ReportBuilder {}.build());
    REQUIRE(harness.device.poll(100ms).has_value());

    // The deck really does combine them: two held together arrive as one report
    // carrying both bits, measured as 0x09 for top-left and bottom-right. Four is
    // the same thing taken to its end, and it is what PhysicalButtonMask covers.
    harness.transport.queueRead(ReportBuilder {}.buttons(protocol::PhysicalButtonMask).build());
    REQUIRE(harness.device.poll(100ms).has_value());

    REQUIRE(harness.listener.count<ButtonPressed>() == ButtonCount);
    for (auto const button: AllButtons)
    {
        INFO("the " << nameOf(button) << " button");
        CHECK(harness.listener.nth<ButtonPressed>(indexOf(button)).button == button);
    }
}

TEST_CASE("a button already down in the first report is reported as a press", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();

    // poll() seeds the knob fields from the first report so a deck that is
    // already being touched does not announce everything at once, and it
    // deliberately does not seed the buttons: a button cannot be held across a
    // connect the way a knob can be rested on, so the first report showing one
    // down is somebody pressing it.
    harness.transport.queueRead(ReportBuilder {}.buttons(0x08).build());
    REQUIRE(harness.device.poll(100ms).has_value());

    REQUIRE(harness.listener.count<ButtonPressed>() == 1);
    CHECK(harness.listener.nth<ButtonPressed>().button == Button::TopLeft);
}

TEST_CASE("holding one button and adding another reports only the new one", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();

    harness.transport.queueRead(ReportBuilder {}.build());
    harness.transport.queueRead(ReportBuilder {}.buttons(0x08).build());
    harness.transport.queueRead(ReportBuilder {}.buttons(0x0C).build()); // TopLeft held, TopRight added.

    for (int step = 0; step < 3; ++step)
        REQUIRE(harness.device.poll(100ms).has_value());

    REQUIRE(harness.listener.count<ButtonPressed>() == 2);
    CHECK(harness.listener.nth<ButtonPressed>(0).button == Button::TopLeft);
    CHECK(harness.listener.nth<ButtonPressed>(1).button == Button::TopRight);
}

TEST_CASE("a screen touch decodes its coordinates little-endian", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();
    harness.transport.queueRead(ReportBuilder {}.build());
    REQUIRE(harness.device.poll(100ms).has_value());

    harness.transport.queueRead(ReportBuilder {}.screenTouch(700, 300, 0x1c).build());
    REQUIRE(harness.device.poll(100ms).has_value());

    // Little-endian here, unlike the big-endian audio meters in the same report.
    // Verified against the deck: 1367 touches all landed inside 800x480 read this
    // way, and outside it read the other.
    REQUIRE(harness.listener.count<ScreenTouched>() == 1);
    auto const touch = harness.listener.nth<ScreenTouched>();
    CHECK(touch.x == 700);
    CHECK(touch.y == 300);
    CHECK(touch.phase == TouchPhase::Pressed);
}

TEST_CASE("a finger held still is one press, not a press per report", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();
    harness.transport.queueRead(ReportBuilder {}.build());
    REQUIRE(harness.device.poll(100ms).has_value());

    // Measured on the deck: a 4.8-second hold sends 45 screen-touch reports, all
    // carrying the same coordinate. The flags byte stays 0x00 throughout, which
    // is why reading it as a contact flag made a stationary press invisible.
    for (int step = 0; step < 5; ++step)
        harness.transport.queueRead(ReportBuilder {}.screenTouch(549, 228, 0x00).build());

    for (int step = 0; step < 5; ++step)
        REQUIRE(harness.device.poll(100ms).has_value());

    REQUIRE(harness.listener.count<ScreenTouched>() == 1);
    CHECK(harness.listener.nth<ScreenTouched>().phase == TouchPhase::Pressed);
    CHECK(harness.listener.nth<ScreenTouched>().x == 549);
}

TEST_CASE("the lift is the moment touch reports stop", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();
    harness.transport.queueRead(ReportBuilder {}.build());
    REQUIRE(harness.device.poll(100ms).has_value());

    harness.transport.queueRead(ReportBuilder {}.screenTouch(100, 100, 0x00).build());
    harness.transport.queueRead(ReportBuilder {}.screenTouch(120, 110, 0x48).build());
    // An ordinary report: the deck has no release event, it simply stops sending
    // touch reports.
    harness.transport.queueRead(ReportBuilder {}.build());

    for (int step = 0; step < 3; ++step)
        REQUIRE(harness.device.poll(100ms).has_value());

    REQUIRE(harness.listener.count<ScreenTouched>() == 3);
    CHECK(harness.listener.nth<ScreenTouched>(0).phase == TouchPhase::Pressed);
    CHECK(harness.listener.nth<ScreenTouched>(1).phase == TouchPhase::Moved);

    auto const release = harness.listener.nth<ScreenTouched>(2);
    CHECK(release.phase == TouchPhase::Released);
    // Reported where the finger last was, not at the origin.
    CHECK(release.x == 120);
    CHECK(release.y == 110);
}

TEST_CASE("a two-finger gesture is still tracked", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();
    harness.transport.queueRead(ReportBuilder {}.build());
    REQUIRE(harness.device.poll(100ms).has_value());

    // Two fingers report one coordinate and leave the flags byte at zero for the
    // whole gesture, so reading that byte as contact dropped two-finger input
    // entirely. The report type is what says a finger is down.
    harness.transport.queueRead(ReportBuilder {}.screenTouch(217, 305, 0x00).build());
    harness.transport.queueRead(ReportBuilder {}.screenTouch(229, 309, 0x00).build());
    harness.transport.queueRead(ReportBuilder {}.screenTouch(571, 384, 0x00).build());

    for (int step = 0; step < 3; ++step)
        REQUIRE(harness.device.poll(100ms).has_value());

    REQUIRE(harness.listener.count<ScreenTouched>() == 3);
    CHECK(harness.listener.nth<ScreenTouched>(0).phase == TouchPhase::Pressed);
    CHECK(harness.listener.nth<ScreenTouched>(1).phase == TouchPhase::Moved);
    CHECK(harness.listener.nth<ScreenTouched>(2).phase == TouchPhase::Moved);
    CHECK(harness.listener.nth<ScreenTouched>(2).x == 571);
}

TEST_CASE("no release is reported when nothing was touching", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();

    harness.transport.queueRead(ReportBuilder {}.build());
    harness.transport.queueRead(ReportBuilder {}.buttons(0x08).build());

    REQUIRE(harness.device.poll(100ms).has_value());
    REQUIRE(harness.device.poll(100ms).has_value());

    CHECK(harness.listener.count<ScreenTouched>() == 0);
}

TEST_CASE("a screen touch is not also read as a button press", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();
    harness.transport.queueRead(ReportBuilder {}.build());
    REQUIRE(harness.device.poll(100ms).has_value());

    // Byte 0 is 0x10 for a touch, and the two events share those six bytes. A
    // decoder that checked the buttons first would see bit 0x10 set -- outside
    // the physical-button mask, but only because that mask is applied.
    harness.transport.queueRead(ReportBuilder {}.screenTouch(400, 240, 0x1c).build());
    REQUIRE(harness.device.poll(100ms).has_value());

    CHECK(harness.listener.count<ScreenTouched>() == 1);
    CHECK(harness.listener.count<ButtonPressed>() == 0);
}

TEST_CASE("a knob push fires on the press only", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();

    harness.transport.queueRead(ReportBuilder {}.build());
    harness.transport.queueRead(ReportBuilder {}.knobPush(0x04).build()); // Knob3 down.
    harness.transport.queueRead(ReportBuilder {}.knobPush(0x00).build()); // Released.

    for (int step = 0; step < 3; ++step)
        REQUIRE(harness.device.poll(100ms).has_value());

    REQUIRE(harness.listener.count<KnobPushed>() == 1);
    CHECK(harness.listener.nth<KnobPushed>().knob == KnobId::Console);
}

TEST_CASE("a knob touch fires on both edges", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();

    harness.transport.queueRead(ReportBuilder {}.build());
    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x02).build()); // Knob2 touched.
    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x00).build()); // Let go.

    for (int step = 0; step < 3; ++step)
        REQUIRE(harness.device.poll(100ms).has_value());

    // Unlike a push, a touch has a meaningful "no longer" -- the UI dims the ring.
    REQUIRE(harness.listener.count<KnobTouched>() == 2);
    CHECK(harness.listener.nth<KnobTouched>(0).knob == KnobId::LineIn);
    CHECK(harness.listener.nth<KnobTouched>(0).touch == Touch::Touched);
    CHECK(harness.listener.nth<KnobTouched>(1).touch == Touch::Released);
}

TEST_CASE("a knob turn moves the volume by the number of steps turned", "[device][decode]")
{
    Harness harness;
    harness.holdLevels(10);
    harness.connectInControlMode();

    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x01).knobValue(KnobId::Mic, 10).build());
    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x01).knobValue(KnobId::Mic, 12).build());

    REQUIRE(harness.device.poll(100ms).has_value());
    REQUIRE(harness.device.poll(100ms).has_value());

    // The reported byte is a counter, not a position, so two steps up is two
    // steps from wherever the deck said the volume already was -- ten steps.
    REQUIRE(harness.listener.count<KnobVolumeChanged>() == 1);
    CHECK(harness.listener.nth<KnobVolumeChanged>().knob == KnobId::Mic);
    CHECK(harness.listener.nth<KnobVolumeChanged>().volume == 60);
}

TEST_CASE("a knob counter that wraps past zero is one step, not a fall of 255", "[device][decode]")
{
    Harness harness;
    harness.holdLevels(10);
    harness.connectInControlMode();

    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x01).knobValue(KnobId::Mic, 0xff).build());
    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x01).knobValue(KnobId::Mic, 0x00).build());

    REQUIRE(harness.device.poll(100ms).has_value());
    REQUIRE(harness.device.poll(100ms).has_value());

    REQUIRE(harness.listener.count<KnobVolumeChanged>() == 1);
    CHECK(harness.listener.nth<KnobVolumeChanged>().volume == 55);
}

TEST_CASE("turning a knob down lowers the volume", "[device][decode]")
{
    Harness harness;
    harness.holdLevels(10);
    harness.connectInControlMode();

    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x01).knobValue(KnobId::Mic, 10).build());
    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x01).knobValue(KnobId::Mic, 7).build());

    REQUIRE(harness.device.poll(100ms).has_value());
    REQUIRE(harness.device.poll(100ms).has_value());

    REQUIRE(harness.listener.count<KnobVolumeChanged>() == 1);
    CHECK(harness.listener.nth<KnobVolumeChanged>().volume == 35);
}

TEST_CASE("the volume stops at the ends of its range", "[device][decode]")
{
    Harness harness;
    harness.holdLevels(10);
    harness.connectInControlMode();

    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x01).knobValue(KnobId::Mic, 0).build());
    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x01).knobValue(KnobId::Mic, 40).build());
    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x01).knobValue(KnobId::Mic, 80).build());

    for (int step = 0; step < 3; ++step)
        REQUIRE(harness.device.poll(100ms).has_value());

    // Two big turns up: the first saturates at 100, the second has nowhere left
    // to go and must not be reported as a change at all.
    REQUIRE(harness.listener.count<KnobVolumeChanged>() == 1);
    CHECK(harness.listener.nth<KnobVolumeChanged>().volume == 100);
}

TEST_CASE("a knob that is not touched does not report a turn", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();

    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x00).knobValue(KnobId::Mic, 5).build());
    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x00).knobValue(KnobId::Mic, 15).build());

    REQUIRE(harness.device.poll(100ms).has_value());
    REQUIRE(harness.device.poll(100ms).has_value());

    CHECK(harness.listener.count<KnobVolumeChanged>() == 0);
}

TEST_CASE("the all-zero filler report is not decoded as anything", "[device][decode]")
{
    Harness harness;
    harness.holdLevels(10);
    harness.connectInControlMode();

    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x01).knobValue(KnobId::Mic, 10).build());
    harness.transport.queueRead(ReportBuilder::filler());
    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x01).knobValue(KnobId::Mic, 11).build());

    for (int step = 0; step < 3; ++step)
        REQUIRE(harness.device.poll(100ms).has_value());

    // The deck interleaves an all-zero report between real ones. Decoding it
    // would let the touched knob go and then take it up again on the next
    // report, so the touch state must not move at all across the three.
    CHECK(harness.listener.count<KnobTouched>() == 0);
    REQUIRE(harness.listener.count<KnobVolumeChanged>() == 1);
    CHECK(harness.listener.nth<KnobVolumeChanged>().volume == 55);
}

TEST_CASE("a knob counter that moves while untouched is absorbed, not reported", "[device][decode]")
{
    Harness harness;
    harness.holdLevels(10);
    harness.connectInControlMode();

    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x01).knobValue(KnobId::Mic, 15).build());
    // Untouched, and the counter has jumped -- as it does across a knob push or a
    // button press, where it was seen going 0x0f -> 0x2f -> 0x4f.
    harness.transport.queueRead(ReportBuilder {}.knobPush(0x01).knobValue(KnobId::Mic, 0x4f).build());
    // Touched again: the jump must not now arrive as one enormous turn.
    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x01).knobValue(KnobId::Mic, 0x50).build());

    for (int step = 0; step < 3; ++step)
        REQUIRE(harness.device.poll(100ms).has_value());

    REQUIRE(harness.listener.count<KnobVolumeChanged>() == 1);
    CHECK(harness.listener.nth<KnobVolumeChanged>().volume == 55);
}

TEST_CASE("several knobs turning at once are each reported", "[device][decode]")
{
    Harness harness;
    harness.holdLevels(10);
    harness.connectInControlMode();

    harness.transport.queueRead(
        ReportBuilder {}.knobTouch(0x03).knobValue(KnobId::Mic, 5).knobValue(KnobId::LineIn, 5).build());
    harness.transport.queueRead(
        ReportBuilder {}.knobTouch(0x03).knobValue(KnobId::Mic, 6).knobValue(KnobId::LineIn, 3).build());

    REQUIRE(harness.device.poll(100ms).has_value());
    REQUIRE(harness.device.poll(100ms).has_value());

    REQUIRE(harness.listener.count<KnobVolumeChanged>() == 2);
    CHECK(harness.listener.nth<KnobVolumeChanged>(0).knob == KnobId::Mic);
    CHECK(harness.listener.nth<KnobVolumeChanged>(0).volume == 55);
    CHECK(harness.listener.nth<KnobVolumeChanged>(1).knob == KnobId::LineIn);
    CHECK(harness.listener.nth<KnobVolumeChanged>(1).volume == 40);
}

TEST_CASE("a report whose checksum does not match is dropped", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();

    auto corrupt = ReportBuilder {}.buttons(0x08).build();
    corrupt.back() = static_cast<std::uint8_t>(corrupt.back() + 1); // One bit out.
    harness.transport.queueRead(corrupt);

    auto const result = harness.device.poll(100ms);

    // Not an error -- one bad report is not a broken device -- but nothing may be
    // decoded out of bytes that did not survive the wire intact.
    REQUIRE(result.has_value());
    CHECK(harness.listener.empty());
    CHECK(harness.logger.contains(LogLevel::Warning, "checksum"));
}

TEST_CASE("a connect publishes a meter reading of its own", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();

    harness.transport.queueRead(ReportBuilder {}.audioMeter(indexOf(KnobId::Mic), 0x7fff).build());
    REQUIRE(harness.device.poll(100ms).has_value());
    REQUIRE(harness.listener.count<AudioMetersChanged>() == 1);

    // The same six percentages a second time are not forwarded, which is the
    // point of the cache. But it has to be re-armed by a connect: without that a
    // session inherits the reading before it, and a deck sitting at a steady
    // value -- a microphone against a maxed preamp, say -- never produces the
    // event that would correct the interface.
    harness.device.disconnect();
    harness.listener.clear();
    harness.connectInControlMode();

    harness.transport.queueRead(ReportBuilder {}.audioMeter(indexOf(KnobId::Mic), 0x7fff).build());
    REQUIRE(harness.device.poll(100ms).has_value());

    REQUIRE(harness.listener.count<AudioMetersChanged>() == 1);
    CHECK(harness.listener.nth<AudioMetersChanged>().levels[indexOf(KnobId::Mic)] == 100);
}

TEST_CASE("the audio meters are decoded and reported", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();

    // One per track, a stereo pair each. Confirmed with the vendor's per-track
    // peak view enabled, a live microphone and music on System and nothing else
    // connected: those two pairs moved and the other four sat at zero.
    harness.transport.queueRead(ReportBuilder {}
                                    .audioMeter(indexOf(KnobId::Mic), 0x7fff)
                                    .audioMeter(indexOf(KnobId::System), 0x4000)
                                    .build());
    REQUIRE(harness.device.poll(100ms).has_value());

    REQUIRE(harness.listener.count<AudioMetersChanged>() == 1);
    auto const meters = harness.listener.nth<AudioMetersChanged>().levels;
    CHECK(meters[indexOf(KnobId::Mic)] == 100);
    CHECK(meters[indexOf(KnobId::System)] == 50);
    CHECK(meters[indexOf(KnobId::Console)] == 0);
    CHECK(meters[indexOf(KnobId::Chat)] == 0);
}

TEST_CASE("unchanged meters are not re-sent at the report rate", "[device][decode]")
{
    Harness harness;
    harness.connectInControlMode();

    harness.transport.queueRead(ReportBuilder {}.audioMeter(0, 0x4000).build());
    harness.transport.queueRead(ReportBuilder {}.audioMeter(0, 0x4000).build());
    harness.transport.queueRead(ReportBuilder {}.audioMeter(0, 0x2000).build());

    for (int step = 0; step < 3; ++step)
        REQUIRE(harness.device.poll(100ms).has_value());

    // The deck streams meters continuously; forwarding an identical set would
    // wake the UI five times a second for nothing.
    CHECK(harness.listener.count<AudioMetersChanged>() == 2);
}

// --- sendScreen ----------------------------------------------------------

TEST_CASE("sendScreen refuses when the device is not connected", "[device][screen]")
{
    Harness harness;
    auto const frame = makeFrame(100);

    auto const result = harness.device.sendScreen(frame);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DeviceError::NotConnected);
}

TEST_CASE("sendScreen packs a short frame into one chunk", "[device][screen]")
{
    Harness harness;
    harness.connectInControlMode();
    auto const frame = makeFrame(100);

    REQUIRE(harness.device.sendScreen(frame).has_value());

    REQUIRE(harness.transport.sent().size() == 1);
    auto const& packet = harness.transport.sent().front().bytes;

    // Always a full-size packet, whatever the payload: the deck reads a fixed
    // report length and a short write is simply not delivered.
    REQUIRE(packet.size() == protocol::ScreenChunkSize);
    CHECK(packet[0] == 0x00); // Report id.
    CHECK(packet[protocol::ScreenChunkSequenceOffset] == 0);

    auto const length = static_cast<std::uint16_t>(packet[protocol::ScreenChunkLengthOffset]
                                                   | (packet[protocol::ScreenChunkLengthOffset + 1] << 8));
    CHECK(length == frame.size());

    auto const checksum = static_cast<std::uint16_t>(
        packet[protocol::ScreenChunkChecksumOffset] | (packet[protocol::ScreenChunkChecksumOffset + 1] << 8));
    CHECK(checksum == checksumOf(frame));

    auto const payload = std::span { packet }.subspan(protocol::ScreenChunkHeaderSize, frame.size());
    CHECK(std::ranges::equal(frame, payload));
}

TEST_CASE("sendScreen splits a long frame and numbers the chunks", "[device][screen]")
{
    Harness harness;
    harness.connectInControlMode();

    auto const frame = makeFrame((protocol::ScreenChunkPayloadSize * 2) + 500);
    REQUIRE(harness.device.sendScreen(frame).has_value());

    REQUIRE(harness.transport.sent().size() == 3);

    std::size_t offset = 0;
    for (std::size_t index = 0; index < harness.transport.sent().size(); ++index)
    {
        INFO("chunk " << index);
        auto const& packet = harness.transport.sent()[index].bytes;

        REQUIRE(packet.size() == protocol::ScreenChunkSize);
        CHECK(packet[protocol::ScreenChunkSequenceOffset] == index);

        auto const expected = std::min(protocol::ScreenChunkPayloadSize, frame.size() - offset);
        auto const length = static_cast<std::uint16_t>(
            packet[protocol::ScreenChunkLengthOffset] | (packet[protocol::ScreenChunkLengthOffset + 1] << 8));
        CHECK(length == expected);

        auto const chunk = std::span { frame }.subspan(offset, expected);
        auto const checksum =
            static_cast<std::uint16_t>(packet[protocol::ScreenChunkChecksumOffset]
                                       | (packet[protocol::ScreenChunkChecksumOffset + 1] << 8));
        CHECK(checksum == checksumOf(chunk));
        auto const payload = std::span { packet }.subspan(protocol::ScreenChunkHeaderSize, expected);
        CHECK(std::ranges::equal(chunk, payload));

        offset += expected;
    }

    CHECK(offset == frame.size()); // Every byte accounted for.
}

TEST_CASE("only the last chunk of a frame carries the end-of-frame marker", "[device][screen]")
{
    Harness harness;
    harness.connectInControlMode();

    auto const frame = makeFrame((protocol::ScreenChunkPayloadSize * 2) + 100);
    REQUIRE(harness.device.sendScreen(frame).has_value());

    REQUIRE(harness.transport.sent().size() == 3);
    for (std::size_t index = 0; index < harness.transport.sent().size(); ++index)
    {
        INFO("chunk " << index);
        auto const& packet = harness.transport.sent()[index].bytes;
        auto const expected = index + 1 == harness.transport.sent().size() ? protocol::ScreenChunkFinalMarker
                                                                           : std::uint8_t { 0x00 };

        // Without this the deck accepts every chunk and never shows the frame:
        // no error, no rendering, nothing to go on.
        for (std::size_t byte = 0; byte < protocol::ScreenChunkFinalByteCount; ++byte)
            CHECK(packet[protocol::ScreenChunkFinalOffset + byte] == expected);
    }
}

TEST_CASE("a single-chunk frame is marked final on that one chunk", "[device][screen]")
{
    Harness harness;
    harness.connectInControlMode();

    REQUIRE(harness.device.sendScreen(makeFrame(200)).has_value());

    REQUIRE(harness.transport.sent().size() == 1);
    auto const& packet = harness.transport.sent().front().bytes;
    CHECK(packet[protocol::ScreenChunkFinalOffset] == protocol::ScreenChunkFinalMarker);
    CHECK(packet[protocol::ScreenChunkFinalOffset + 1] == protocol::ScreenChunkFinalMarker);
}

TEST_CASE("a frame that is an exact multiple of the chunk size sends no empty tail", "[device][screen]")
{
    Harness harness;
    harness.connectInControlMode();

    auto const frame = makeFrame(protocol::ScreenChunkPayloadSize * 2);
    REQUIRE(harness.device.sendScreen(frame).has_value());

    // An off-by-one here would send a third chunk of zero bytes, which the deck
    // answers by blanking the screen.
    CHECK(harness.transport.sent().size() == 2);
}

TEST_CASE("sendScreen stops at the first failed chunk", "[device][screen]")
{
    Harness harness;
    harness.connectInControlMode();
    harness.transport.failSendAfter(DeviceError::WriteFailed, 1);

    auto const frame = makeFrame(protocol::ScreenChunkPayloadSize * 3);
    auto const result = harness.device.sendScreen(frame);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DeviceError::WriteFailed);

    // Pushing the rest of a frame the deck is not reading stalls the queue behind
    // it; one chunk out, then stop.
    CHECK(harness.transport.sent().size() == 1);
    CHECK(harness.logger.contains(LogLevel::Warning, "Screen chunk"));
}

TEST_CASE("a write after the deck is closed is refused rather than attempted", "[device][screen]")
{
    Harness harness;
    harness.connectInControlMode();
    harness.device.disconnect();
    harness.transport.clearSent();

    // The render timer hands frames to a thread pool, so a frame can arrive
    // after the decision to disconnect. Writing to a closed descriptor produced
    // a burst of "Bad file descriptor" at every shutdown.
    auto const screen = harness.device.sendScreen(makeFrame(2000));
    REQUIRE_FALSE(screen.has_value());
    CHECK(screen.error() == DeviceError::NotConnected);

    auto const level = harness.device.setLevel(MixId::Creator, KnobId::Mic, Level::fromPercent(50));
    REQUIRE_FALSE(level.has_value());
    CHECK(level.error() == DeviceError::NotConnected);

    auto const brightness = harness.device.setKnobLedBrightness(50);
    REQUIRE_FALSE(brightness.has_value());
    CHECK(brightness.error() == DeviceError::NotConnected);

    CHECK(harness.transport.sent().empty());
}

TEST_CASE("a frame stops at the chunk where the deck closes", "[device][screen]")
{
    Harness harness;
    harness.connectInControlMode();

    // Closing part-way through a multi-chunk frame is exactly what disconnect()
    // does to a frame already in flight.
    harness.transport.closeAfterWrites(2);

    auto const result = harness.device.sendScreen(makeFrame(protocol::ScreenChunkPayloadSize * 5));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DeviceError::NotConnected);
    CHECK(harness.transport.sentCount(FakeHidTransport::Channel::Write) == 2);
}

// --- levels --------------------------------------------------------------

TEST_CASE("setKnobLedBrightness writes the LED brightness property", "[device][led]")
{
    Harness harness;
    harness.connectInControlMode();

    REQUIRE(harness.device.setKnobLedBrightness(100).has_value());

    REQUIRE(harness.transport.sent().size() == 1);
    auto const& sent = harness.transport.sent().front().bytes;

    // Framed for the wire, so the command starts after the report-id byte.
    REQUIRE(sent.size() == protocol::FeatureReportSize);
    CHECK(sent[0] == 0x00);
    CHECK(sent[1] == static_cast<std::uint8_t>(protocol::CommandKind::Set));
    CHECK(sent[2] == protocol::PropertyGroup);
    CHECK(sent[3] == static_cast<std::uint8_t>(protocol::Property::KnobLedBrightness));
    CHECK(sent[4] == 1);
    CHECK(sent[5] == protocol::KnobLedBrightnessAtStartup);
}

TEST_CASE("setKnobLedBrightness refuses when the device is not connected", "[device][led]")
{
    Harness harness;

    auto const result = harness.device.setKnobLedBrightness(50);

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DeviceError::NotConnected);
}

TEST_CASE("connect puts back the settings the handshake overwrites", "[device][connect][preserve]")
{
    Harness harness;
    harness.presentControlDevice();

    // Answer every snapshot read with a value that is deliberately not what the
    // init sequence writes, so a restore is distinguishable from doing nothing.
    constexpr std::uint8_t Marker = 0x13;
    for (auto const& preserved: protocol::PreservedAddresses)
    {
        std::vector<std::uint8_t> reply { 0x00,
                                          static_cast<std::uint8_t>(protocol::CommandKind::Get),
                                          protocol::PropertyGroup,
                                          preserved.address,
                                          preserved.length };
        reply.insert(reply.end(), preserved.length, Marker);
        harness.transport.queueFeatureReport(std::move(reply));
    }

    REQUIRE(harness.device.connect().has_value());

    // One read, then the sequence, then one write back per register.
    auto const preserved = protocol::PreservedAddresses.size();
    REQUIRE(harness.transport.sent().size()
            == preserved + commands::InitPayloads.size() + preserved + protocol::EffectEnables.size()
                   + StateAdoptionReads + ButtonCount);

    for (std::size_t index = 0; index < preserved; ++index)
    {
        INFO("restored register " << index);
        auto const& entry = protocol::PreservedAddresses[index];
        auto const& sent = harness.transport.sent()[preserved + commands::InitPayloads.size() + index].bytes;

        REQUIRE(sent.size() == protocol::FeatureReportSize);
        CHECK(sent[1] == static_cast<std::uint8_t>(protocol::CommandKind::Set));
        CHECK(sent[2] == protocol::PropertyGroup);
        CHECK(sent[3] == entry.address);
        CHECK(sent[4] == entry.length);
        for (std::size_t byte = 0; byte < entry.length; ++byte)
            CHECK(sent[5 + byte] == Marker);
    }
}

TEST_CASE("connect puts the effects chain into the state it was given", "[device][connect][effects]")
{
    Harness harness;
    harness.presentControlDevice();

    // The handshake carries no microphone chain, so the deck keeps
    // whatever the last program to touch it left behind. This is what makes the
    // starting state a decision: the interface remembers it and hands it back.
    EffectState wanted;
    wanted.enabled.front() = true;
    harness.device.setEffectState(wanted);

    REQUIRE(harness.device.connect().has_value());

    // The enables go out after the handshake and the restored registers, and
    // before the button colours, which are the last thing a connect sends.
    auto const& sent = harness.transport.sent();
    // The tail of a connect, in order: the effect enables, the reads that adopt
    // the deck's own mix and levels, then the button colours.
    auto const first =
        sent.size() - ButtonCount - StateAdoptionReads - protocol::EffectEnables.size();

    for (std::size_t index = 0; index < protocol::EffectEnables.size(); ++index)
    {
        INFO("effect enable " << index);
        auto const& bytes = sent[first + index].bytes;

        // A framed command: fe 00 <length> <command> <value> <checksum>, after
        // the report-id byte the backend prepends.
        CHECK(bytes[1] == protocol::FramedCommandMarker);
        CHECK(bytes[4] == std::to_underlying(protocol::EffectEnables[index]));
        CHECK(bytes[5] == (wanted.enabled[index] ? 0x01 : 0x00));
    }
}

TEST_CASE("the shutdown sequence is paced the way the handshake is", "[device][connect]")
{
    Harness harness;
    harness.presentControlDevice();
    REQUIRE(harness.device.connect().has_value());

    auto const beforeShutdown = harness.clock.totalSlept();
    harness.device.disconnect();

    // Sent back to back, not all of these take. The sequence darkens the four
    // buttons with four records to the same address one after another, and a deck
    // shut down without a gap keeps one of them lit -- a different one on
    // different runs. The handshake carries the same gap for the same reason.
    CHECK(harness.clock.totalSlept() - beforeShutdown == 10ms * commands::ShutdownPayloads.size());
}

TEST_CASE("a connect nobody configured writes no effect parameters at all",
          "[device][connect][effects]")
{
    Harness harness;
    harness.presentControlDevice();

    REQUIRE(harness.device.connect().has_value());

    // The defect this guards is not a wrong value, it is any value. A parameter
    // write sends its whole framed body, and the bodies this driver edits from
    // are protocol::FramedDefaults -- the captured vendor payloads for reverb and
    // the compressor. So a single parameter written on a connect nobody
    // configured puts that effect's entire configuration back on the deck, which
    // is the microphone chain the handshake had removed arriving by another door.
    for (auto const& sent: harness.transport.sent())
    {
        if (sent.bytes[1] != protocol::FramedCommandMarker)
            continue;

        INFO("framed command 0x" << std::hex << int { sent.bytes[4] });
        CHECK(sent.bytes[4] != std::to_underlying(protocol::FramedCommand::DelayEffectParameters));
        CHECK(sent.bytes[4] != std::to_underlying(protocol::FramedCommand::CompressorParameters));
    }
}

TEST_CASE("a chosen parameter is written before the enables, not after",
          "[device][connect][effects]")
{
    Harness harness;
    harness.presentControlDevice();

    EffectState wanted;
    wanted.parameters[protocol::indexOf(protocol::Parameter::ReverbDecay)] = 90;
    harness.device.setEffectState(wanted);

    REQUIRE(harness.device.connect().has_value());

    // The order carries the meaning. A parameter write sends its whole body, and
    // the delay effect's body carries the byte naming which effect runs -- so a
    // parameter written after an enable can switch back on what the enable just
    // switched off. Enables last means the enable is what the deck is left with.
    std::optional<std::size_t> lastParameter;
    std::optional<std::size_t> firstEnable;
    auto const& sent = harness.transport.sent();

    for (std::size_t index = 0; index < sent.size(); ++index)
    {
        if (sent[index].bytes[1] != protocol::FramedCommandMarker)
            continue;

        auto const command = sent[index].bytes[4];
        if (command == std::to_underlying(protocol::FramedCommand::DelayEffectParameters))
            lastParameter = index;
        else if (command == std::to_underlying(protocol::FramedCommand::DelayEffectEnable)
                 && !firstEnable)
            firstEnable = index;
    }

    REQUIRE(lastParameter.has_value());
    REQUIRE(firstEnable.has_value());
    CHECK(*lastParameter < *firstEnable);
}

TEST_CASE("a deck nobody has configured reports nothing worth storing",
          "[device][connect][effects]")
{
    Harness harness;
    harness.presentControlDevice();
    REQUIRE(harness.device.connect().has_value());

    // What is reported here is what gets written to the settings file and handed
    // back on the next connect, so a parameter reported without anybody having
    // chosen it is how a stored file grows an effect configuration of its own.
    auto const state = harness.device.effectState();
    CHECK(std::ranges::none_of(state.parameters,
                               [](auto const& chosen) { return chosen.has_value(); }));

    // And one somebody did choose is reported, or nothing could ever be kept.
    REQUIRE(harness.device.setParameter(protocol::Parameter::ReverbDecay, 90).has_value());
    CHECK(harness.device.effectState().parameters[protocol::indexOf(
              protocol::Parameter::ReverbDecay)]
          == 90);
}

TEST_CASE("an effects chain nobody configured is switched off", "[device][connect][effects]")
{
    Harness harness;
    harness.presentControlDevice();

    REQUIRE(harness.device.connect().has_value());

    auto const& sent = harness.transport.sent();
    // The tail of a connect, in order: the effect enables, the reads that adopt
    // the deck's own mix and levels, then the button colours.
    auto const first =
        sent.size() - ButtonCount - StateAdoptionReads - protocol::EffectEnables.size();

    for (std::size_t index = 0; index < protocol::EffectEnables.size(); ++index)
    {
        INFO("effect enable " << index);
        CHECK(sent[first + index].bytes[5] == 0x00);
    }
}

TEST_CASE("connect lights every button, each in a colour of its own", "[device][connect][light]")
{
    Harness harness;
    harness.presentControlDevice();

    REQUIRE(harness.device.connect().has_value());

    // The four colour records are the last thing a connect sends, after the
    // handshake and after the registers it overwrote are put back.
    auto const& sent = harness.transport.sent();
    REQUIRE(sent.size() > ButtonCount);
    auto const first = sent.size() - ButtonCount;

    std::vector<std::array<std::uint8_t, 3>> colours;
    for (auto const button: AllButtons)
    {
        INFO("the " << nameOf(button) << " button");
        auto const& bytes = sent[first + indexOf(button)].bytes;
        REQUIRE(bytes.size() == protocol::FeatureReportSize);

        CHECK(bytes[1] == static_cast<std::uint8_t>(protocol::CommandKind::Set));
        CHECK(bytes[2] == protocol::PropertyGroup);
        CHECK(bytes[3] == protocol::ButtonColourAddress);
        CHECK(bytes[4] == 10);

        // The record: 00 <selector> 01 <r> <g> <b> ?? ?? <lit> 80, from byte 5.
        CHECK(bytes[5] == protocol::ButtonBank);
        CHECK(bytes[6] == protocol::selectorFor(button));
        CHECK(bytes[13] == protocol::ButtonLit);

        // The colour is the default one with the default brightness already in
        // it, because the deck has no brightness field to carry it separately.
        auto const& wanted = protocol::DefaultButtonColours[indexOf(button)];
        auto const level = protocol::DefaultButtonBrightnessPercent;
        CHECK(bytes[8] == protocol::scaledChannel(wanted.red, level));
        CHECK(bytes[9] == protocol::scaledChannel(wanted.green, level));
        CHECK(bytes[10] == protocol::scaledChannel(wanted.blue, level));

        colours.push_back({ bytes[8], bytes[9], bytes[10] });
    }

    // Four buttons a person can tell apart is the point of the scheme, so no two
    // may go out the same. Checked on what was sent rather than on the table, so
    // brightness scaling collapsing two colours into one would be caught too.
    std::ranges::sort(colours);
    CHECK(std::ranges::adjacent_find(colours) == colours.end());
}

TEST_CASE("the button records are spaced, so the deck applies all four", "[device][connect][light]")
{
    Harness harness;
    harness.presentControlDevice();

    REQUIRE(harness.device.connect().has_value());

    // The deck applies only some of a run of records sent back to back, and the
    // four button colours are one such run. Sent with no gap the last one wins
    // and the other three stay dark, which is what the deck was doing.
    //
    // Asserted as strictly increasing rather than against a duration: the clock
    // only moves when something sleeps, so a later instant *is* a gap, and the
    // case does not have to know how long the driver chose to wait.
    auto const& sent = harness.transport.sent();
    REQUIRE(sent.size() > ButtonCount);
    auto const first = sent.size() - ButtonCount;

    for (std::size_t index = 1; index < ButtonCount; ++index)
    {
        INFO("between button record " << index - 1 << " and " << index);
        CHECK(sent[first + index].at > sent[first + index - 1].at);
    }
}

TEST_CASE("a button is refused when the deck is not connected", "[device][light]")
{
    Harness harness;

    auto const lit = harness.device.setButtonColour(Button::TopLeft, 0xff, 0x00, 0x00);
    REQUIRE_FALSE(lit.has_value());
    CHECK(lit.error() == DeviceError::NotConnected);
    CHECK(harness.transport.sent().empty());
}

TEST_CASE("a register that answers nothing is not written back", "[device][connect][preserve]")
{
    Harness harness;
    harness.presentControlDevice();

    // A cold deck answers no reads at all. Writing a register back from a reply
    // that never arrived would put zeroes into it, which is worse than leaving
    // the handshake's own value there.
    REQUIRE(harness.device.connect().has_value());

    CHECK(harness.transport.sent().size()
          == protocol::PreservedAddresses.size() + commands::InitPayloads.size()
                 + protocol::EffectEnables.size() + StateAdoptionReads + ButtonCount);
}

TEST_CASE("a reply for the wrong address is refused", "[device][preserve]")
{
    Harness harness;
    harness.presentControlDevice();
    REQUIRE(harness.device.connect().has_value());
    harness.transport.clearSent();

    // The deck leaves the previous answer in its reply buffer, so a reply that
    // does not echo the address asked for is the remains of an earlier read.
    harness.transport.queueFeatureReport({ 0x00,
                                           static_cast<std::uint8_t>(protocol::CommandKind::Get),
                                           protocol::PropertyGroup,
                                           0x27,
                                           0x01,
                                           0x42 });

    std::array<std::uint8_t, 1> values {};
    auto const read = harness.device.readProperty(0x1e, values);

    REQUIRE_FALSE(read.has_value());
    CHECK(read.error() == DeviceError::ReadFailed);
}

TEST_CASE("a property read returns what the deck echoed back", "[device][preserve]")
{
    Harness harness;
    harness.presentControlDevice();
    REQUIRE(harness.device.connect().has_value());

    harness.transport.queueFeatureReport({ 0x00,
                                           static_cast<std::uint8_t>(protocol::CommandKind::Get),
                                           protocol::PropertyGroup,
                                           0x27,
                                           0x06,
                                           0x14,
                                           0x0a,
                                           0x0a,
                                           0x0a,
                                           0x0a,
                                           0x14 });

    std::array<std::uint8_t, 6> values {};
    REQUIRE(harness.device.readProperty(0x27, values).has_value());

    CHECK(values == std::array<std::uint8_t, 6> { 0x14, 0x0a, 0x0a, 0x0a, 0x0a, 0x14 });
}

TEST_CASE("a level is written to its own mix's block", "[device][level]")
{
    Harness harness;
    harness.connectInControlMode();

    REQUIRE(harness.device.setLevel(MixId::Creator, KnobId::System, Level::fromPercent(50)).has_value());
    REQUIRE(harness.device.setLevel(MixId::Audience, KnobId::System, Level::fromPercent(50)).has_value());

    REQUIRE(harness.transport.sent().size() == 2);
    auto const& creator = harness.transport.sent()[0].bytes;
    auto const& audience = harness.transport.sent()[1].bytes;

    // Same track, same level, two different registers -- the mixes are separate
    // and a level without a mix would have to guess which.
    CHECK(creator[3] == 0x2a);
    CHECK(audience[3] == 0x31);
    CHECK(creator[4] == 0x01);

    // Steps reach the wire, not percent: 50% is step 10.
    CHECK(creator[5] == 10);
    CHECK(audience[5] == 10);
}

TEST_CASE("every track resolves to its own register in both mixes", "[device][level]")
{
    Harness harness;
    harness.connectInControlMode();

    for (auto const mix: AllMixes)
    {
        auto const base = mix == MixId::Creator
                              ? std::to_underlying(protocol::Property::CreatorMixLevels)
                              : std::to_underlying(protocol::Property::AudienceMixLevels);

        for (auto const knob: AllKnobs)
        {
            INFO("the " << nameOf(mix) << " mix's " << nameOf(knob) << " track");
            harness.transport.clearSent();
            REQUIRE(harness.device.setLevel(mix, knob, Level::fromSteps(3)).has_value());

            // One write per track, and the address is base + track. The Mic in
            // the creator mix is the single exception the deck asks for, and it
            // is named here rather than left as slack in the count -- a stray
            // extra write on any other track is a defect this must catch.
            auto const expected =
                (mix == MixId::Creator && knob == KnobId::Mic) ? std::size_t { 2 } : std::size_t { 1 };
            REQUIRE(harness.transport.sent().size() == expected);
            CHECK(harness.transport.sent()[0].bytes[3] == base + indexOf(knob));
        }
    }
}

TEST_CASE("the microphone's level goes to both registers the vendor writes", "[device][level]")
{
    Harness harness;
    harness.connectInControlMode();

    // The Mic is the one track that takes two writes. Dragging the vendor's own
    // Mic slider writes 0x35 carrying the same value alongside the level, every
    // time, and no other track does anything of the kind.
    REQUIRE(harness.device.setLevel(MixId::Creator, KnobId::Mic, Level::fromSteps(6)).has_value());

    auto const& sent = harness.transport.sent();
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].bytes[3] == static_cast<std::uint8_t>(protocol::Property::CreatorMixLevels));
    CHECK(sent[0].bytes[5] == 6);
    CHECK(sent[1].bytes[3] == static_cast<std::uint8_t>(protocol::Property::KnobPropertyAt35));
    CHECK(sent[1].bytes[5] == 6);
}

TEST_CASE("the audience mix's microphone takes one write", "[device][level]")
{
    Harness harness;
    harness.connectInControlMode();

    // 0x35 is a single register and the pairing has only ever been captured
    // against 0x27, so the audience block does not get it. Writing it there
    // would be imposing a shape nothing has shown the deck to want.
    REQUIRE(harness.device.setLevel(MixId::Audience, KnobId::Mic, Level::fromSteps(6)).has_value());

    auto const& sent = harness.transport.sent();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].bytes[3] == static_cast<std::uint8_t>(protocol::Property::AudienceMixLevels));
}

TEST_CASE("a run of level writes is spaced", "[device][level]")
{
    Harness harness;
    harness.holdLevels(10);
    harness.connectInControlMode();

    // A dial sends one of these per detent. The deck applies only some of a run
    // sent back to back, so a fast turn lands on whichever it kept -- from the
    // desk, a level that jumps up and settles below where the knob was turned.
    for (int step = 0; step < 4; ++step)
        REQUIRE(harness.device.setLevel(MixId::Creator, KnobId::System, Level::fromSteps(10 + step))
                    .has_value());

    auto const& sent = harness.transport.sent();
    REQUIRE(sent.size() == 4);
    for (std::size_t index = 1; index < sent.size(); ++index)
    {
        INFO("between level write " << index - 1 << " and " << index);
        CHECK(sent[index].at > sent[index - 1].at);
    }
}

TEST_CASE("the microphone's two writes are spaced from each other", "[device][level]")
{
    Harness harness;
    harness.holdLevels(10);
    harness.connectInControlMode();

    // The Mic is the one track that sends two, which makes it the densest run a
    // dial can produce and the first place a missing gap would show.
    REQUIRE(harness.device.setLevel(MixId::Creator, KnobId::Mic, Level::fromSteps(6)).has_value());

    auto const& sent = harness.transport.sent();
    REQUIRE(sent.size() == 2);
    CHECK(sent[1].at > sent[0].at);
}

TEST_CASE("the levels read back from the deck", "[device][level]")
{
    Harness harness;
    harness.presentControlDevice();

    // Every track in both mixes, so the read covers the whole map rather than
    // the one address a case happens to name. The staircase differs per mix, so
    // a read that took the same block twice would be visible.
    for (auto const knob: AllKnobs)
    {
        auto const step = static_cast<std::uint8_t>(indexOf(knob) + 1);
        harness.transport.setRegister(
            protocol::levelAddressOf(protocol::Property::CreatorMixLevels, knob), { step });
        harness.transport.setRegister(
            protocol::levelAddressOf(protocol::Property::AudienceMixLevels, knob),
            { static_cast<std::uint8_t>(step + 10) });
    }

    REQUIRE(harness.device.connect().has_value());

    for (auto const knob: AllKnobs)
    {
        INFO("the " << nameOf(knob) << " track");
        auto const step = static_cast<int>(indexOf(knob)) + 1;
        CHECK(harness.device.level(MixId::Creator, knob) == Level::fromSteps(step));
        CHECK(harness.device.level(MixId::Audience, knob) == Level::fromSteps(step + 10));
        
    }
}

TEST_CASE("a level written to the deck is what the driver then holds", "[device][level]")
{
    Harness harness;
    harness.connectInControlMode();

    // The cache is the deck's register as this driver last left it, so a write
    // that succeeds moves it and a knob turn afterwards starts from there.
    REQUIRE(harness.device.setLevel(MixId::Audience, KnobId::System, Level::fromPercent(70))
                .has_value());
    REQUIRE(harness.device.level(MixId::Audience, KnobId::System).has_value());
    CHECK(harness.device.level(MixId::Audience, KnobId::System)->asPercent() == 70);

    // And the other mix is untouched, because they are separate registers.
    CHECK(harness.device.level(MixId::Creator, KnobId::System) != Level::fromPercent(70));
}

TEST_CASE("choosing a mix is fenced with a settings transaction", "[device][mix]")
{
    Harness harness;
    harness.connectInControlMode();

    REQUIRE(harness.device.selectMix(MixId::Audience).has_value());

    // The vendor brackets a mode change and leaves a level drag unbracketed;
    // this follows that, so the deck sees the shape it expects. Inside the fence
    // go the three writes a switch is made of.
    auto const& sent = harness.transport.sent();
    REQUIRE(sent.size() == 5);

    CHECK(sent[0].bytes[3] == 0x1d);
    CHECK(sent[0].bytes[5] == 0x01);

    // KnobLedSelect, which is what moves the levels the rings display.
    CHECK(sent[1].bytes[3] == static_cast<std::uint8_t>(protocol::Property::KnobLedSelect));
    CHECK(sent[1].bytes[5] == protocol::KnobLedSelectForMix[indexOf(MixId::Audience)]);

    CHECK(sent[2].bytes[3] == 0x15);
    CHECK(sent[2].bytes[5] == 0x01);

    // The ring colour last, and after the switch. A record cannot be aimed at a
    // mix -- the deck applies it to the one selected when it arrives -- so a
    // colour sent ahead of the switch lands on the mix being left, and the deck
    // ends up an orange panel above blue rings.
    CHECK(sent[3].bytes[3] == protocol::ButtonColourAddress);
    CHECK(sent[3].bytes[5] == protocol::KnobBank);
    CHECK(sent[3].bytes[8] == protocol::MixRingColours[indexOf(MixId::Audience)].red);
    CHECK(sent[3].bytes[9] == protocol::MixRingColours[indexOf(MixId::Audience)].green);
    CHECK(sent[3].bytes[10] == protocol::MixRingColours[indexOf(MixId::Audience)].blue);

    CHECK(sent[4].bytes[3] == 0x1d);
    CHECK(sent[4].bytes[5] == 0x00);
}

TEST_CASE("the ring colour record is spaced from what surrounds it", "[device][mix]")
{
    Harness harness;
    harness.connectInControlMode();

    REQUIRE(harness.device.selectMix(MixId::Audience).has_value());

    // The deck applies only some of a run of records, and this one has a property
    // write ahead of it and the fence close behind. Sent crowded it was dropped
    // often enough that a cold deck came up wearing the handshake's creator blue
    // under an orange panel.
    auto const& sent = harness.transport.sent();
    auto const record = std::ranges::find_if(sent, [](FakeHidTransport::Sent const& one) {
        return one.bytes.size() > 5 && one.bytes[3] == protocol::ButtonColourAddress
               && one.bytes[5] == protocol::KnobBank;
    });

    REQUIRE(record != sent.end());
    REQUIRE(record != sent.begin());
    REQUIRE(std::next(record) != sent.end());

    CHECK(record->at > std::prev(record)->at);
    CHECK(std::next(record)->at > record->at);
}

TEST_CASE("the ring colour is written after the mix it belongs to", "[device][mix]")
{
    Harness harness;
    harness.connectInControlMode();

    REQUIRE(harness.device.selectMix(MixId::Audience).has_value());

    // Stated on its own, because the order is the whole point and a positional
    // case above it can be repaired into agreeing with whatever the code does.
    // The record carries no mix, so what it lands on is decided by what was
    // selected when it arrived.
    auto const& sent = harness.transport.sent();

    auto const selected = std::ranges::find_if(sent, [](FakeHidTransport::Sent const& one) {
        return one.bytes.size() > 5 && one.bytes[3] == static_cast<std::uint8_t>(protocol::Property::SelectedMix);
    });
    auto const coloured = std::ranges::find_if(sent, [](FakeHidTransport::Sent const& one) {
        return one.bytes.size() > 5 && one.bytes[3] == protocol::ButtonColourAddress
               && one.bytes[5] == protocol::KnobBank;
    });

    REQUIRE(selected != sent.end());
    REQUIRE(coloured != sent.end());
    CHECK(std::distance(sent.begin(), selected) < std::distance(sent.begin(), coloured));
}

TEST_CASE("a turn after a mix switch starts from the new mix's level", "[device][mix][decode]")
{
    Harness harness;

    // Deliberately different per mix, so a turn taken from the wrong one lands
    // somewhere the case can name rather than somewhere plausible.
    for (auto const knob: AllKnobs)
    {
        harness.transport.setRegister(
            protocol::levelAddressOf(protocol::Property::CreatorMixLevels, knob), { 10 });
        harness.transport.setRegister(
            protocol::levelAddressOf(protocol::Property::AudienceMixLevels, knob), { 4 });
    }
    harness.connectInControlMode();

    REQUIRE(harness.device.selectMix(MixId::Audience).has_value());
    harness.transport.clearSent();
    harness.listener.clear();

    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x01).knobValue(KnobId::Mic, 10).build());
    harness.transport.queueRead(ReportBuilder {}.knobTouch(0x01).knobValue(KnobId::Mic, 11).build());
    REQUIRE(harness.device.poll(100ms).has_value());
    REQUIRE(harness.device.poll(100ms).has_value());

    // One step up from the audience mix's four, which is 25%. One step up from
    // the creator mix's ten would be 55%, and that is what a knob turn must not
    // produce straight after a switch to the audience mix.
    REQUIRE(harness.listener.count<KnobVolumeChanged>() == 1);
    CHECK(harness.listener.nth<KnobVolumeChanged>().mix == MixId::Audience);
    CHECK(harness.listener.nth<KnobVolumeChanged>().volume == 25);

    // And the write goes to the register that mix keeps its levels in.
    REQUIRE(!harness.transport.sent().empty());
    CHECK(harness.transport.sent()[0].bytes[3]
          == static_cast<std::uint8_t>(protocol::Property::AudienceMixLevels));
    CHECK(harness.transport.sent()[0].bytes[5] == 5);
}

TEST_CASE("connect adopts the mix the deck came up monitoring", "[device][connect][mix]")
{
    Harness harness;
    harness.presentControlDevice();

    // The handshake is bracketed by a snapshot and a restore, so a deck put down
    // on the audience mix comes back up on it. What it cannot come back up with
    // is the ring colour: that lives in a record rather than a property, and the
    // handshake has just written creator blue over it.
    harness.transport.setRegister(static_cast<std::uint8_t>(protocol::Property::SelectedMix), { 0x01 });

    REQUIRE(harness.device.connect().has_value());

    CHECK(harness.device.selectedMix() == MixId::Audience);

    // The last ring record, not the first: the handshake replays one of its own
    // in creator blue, and what matters is the colour the deck is left holding.
    auto const& sent = harness.transport.sent();
    auto const ring = std::ranges::find_last_if(sent, [](FakeHidTransport::Sent const& one) {
        return one.bytes.size() > 10 && one.bytes[3] == protocol::ButtonColourAddress
               && one.bytes[5] == protocol::KnobBank;
    });

    REQUIRE(!ring.empty());
    CHECK(ring.front().bytes[8] == protocol::MixRingColours[indexOf(MixId::Audience)].red);
    CHECK(ring.front().bytes[9] == protocol::MixRingColours[indexOf(MixId::Audience)].green);
    CHECK(ring.front().bytes[10] == protocol::MixRingColours[indexOf(MixId::Audience)].blue);
}

TEST_CASE("connect keeps the creator mix when that is where the deck is", "[device][connect][mix]")
{
    Harness harness;
    harness.presentControlDevice();
    harness.transport.setRegister(static_cast<std::uint8_t>(protocol::Property::SelectedMix), { 0x00 });

    REQUIRE(harness.device.connect().has_value());
    CHECK(harness.device.selectedMix() == MixId::Creator);
}

TEST_CASE("a mix the deck cannot name is not adopted", "[device][connect][mix]")
{
    Harness harness;
    harness.presentControlDevice();

    // Two values are documented and this is neither. Guessing would put the
    // rings and the audio somewhere nobody asked for, so the driver keeps what
    // it had and says so.
    harness.transport.setRegister(static_cast<std::uint8_t>(protocol::Property::SelectedMix), { 0x7f });

    REQUIRE(harness.device.connect().has_value());
    CHECK(harness.device.selectedMix() == MixId::Creator);
    CHECK(harness.logger.contains(LogLevel::Warning, "could not read the selected mix"));
}

TEST_CASE("each mix gets its own ring colour and ring selector", "[device][mix]")
{
    // The two mixes must not send the same bytes for the parts that say which
    // mix it is, or a switch would be invisible on the deck however well the
    // audio followed -- which is exactly the shape of the defect this fixes.
    auto const creator = protocol::MixRingColours[indexOf(MixId::Creator)];
    auto const audience = protocol::MixRingColours[indexOf(MixId::Audience)];
    CHECK((creator.red != audience.red || creator.green != audience.green
           || creator.blue != audience.blue));

    CHECK(protocol::KnobLedSelectForMix[indexOf(MixId::Creator)]
          != protocol::KnobLedSelectForMix[indexOf(MixId::Audience)]);
}

TEST_CASE("two frames sent at once do not interleave on the wire", "[device][screen]")
{
    Harness harness;
    harness.connectInControlMode();

    // The deck reassembles a frame from its chunks, so a chunk of one frame
    // arriving between two of another gives it a single image made of both --
    // and the panel then shows a blend of two moments, or keeps the older one
    // because the newer never completed its sequence. The application pushes
    // frames from a thread pool, so two of them meeting here is not exotic.
    constexpr std::size_t FrameBytes = protocol::ScreenChunkPayloadSize * 4;
    std::vector<std::uint8_t> const first(FrameBytes, 0xa1);
    std::vector<std::uint8_t> const second(FrameBytes, 0xb2);

    std::vector<std::thread> senders;
    senders.reserve(2);
    for (auto const* frame: { &first, &second })
        senders.emplace_back([&harness, frame] {
            for (int round = 0; round < 20; ++round)
                if (auto const sent = harness.device.sendScreen(*frame); !sent)
                    return;
        });

    for (auto& sender: senders)
        sender.join();

    // Every frame is four chunks of one filler byte. Walking what was sent, the
    // filler may only change where a sequence starts over.
    auto const& sent = harness.transport.sent();
    REQUIRE(sent.size() == std::size_t { 40 } * 4);

    std::uint8_t current = 0;
    for (std::size_t index = 0; index < sent.size(); ++index)
    {
        auto const& bytes = sent[index].bytes;
        REQUIRE(bytes.size() == protocol::ScreenChunkSize);

        auto const sequence = bytes[protocol::ScreenChunkSequenceOffset];
        auto const filler = bytes[protocol::ScreenChunkHeaderSize];

        INFO("chunk " << index << " of the run, sequence " << int { sequence });
        if (sequence == 0)
            current = filler;
        else
            CHECK(filler == current);
    }
}

TEST_CASE("a dangerous address is refused before anything is sent", "[device][safety]")
{
    Harness harness;
    harness.connectInControlMode();

    std::array<std::uint8_t, 1> const values { 0x01 };
    auto const written = harness.device.writeProperty(protocol::DangerousAddresses.front(), values);

    // 0x01 to 0x16 once wedged the deck into needing a power cycle. Nothing
    // should reach the bus, and the refusal should say so rather than fail mutely.
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error() == DeviceError::WriteFailed);
    CHECK(harness.transport.sent().empty());
    CHECK(harness.logger.contains(LogLevel::Error, "power cycle"));
}

TEST_CASE("a parameter is written into its command's body at its own offset", "[device][parameter]")
{
    Harness harness;
    harness.connectInControlMode();

    REQUIRE(harness.device.setParameter(protocol::Parameter::CompressorThreshold, -26).has_value());

    REQUIRE(harness.transport.sent().size() == 1);
    auto const& sent = harness.transport.sent().front().bytes;
    CHECK(sent[1] == protocol::FramedCommandMarker);
    CHECK(sent[4] == static_cast<std::uint8_t>(protocol::FramedCommand::CompressorParameters));

    // Body byte 9, captured from the vendor moving this exact slider. -26 dB is
    // 0xe6 as a signed byte, which is what the deck was seen to receive.
    CHECK(sent[5 + 9] == 0xe6);
    CHECK(harness.device.parameter(protocol::Parameter::CompressorThreshold) == -26);
}

TEST_CASE("a parameter leaves the rest of its command's body alone", "[device][parameter]")
{
    Harness harness;
    harness.connectInControlMode();

    REQUIRE(harness.device.setParameter(protocol::Parameter::CompressorRatio, 16).has_value());

    auto const& sent = harness.transport.sent().front().bytes;
    auto const& reference = protocol::FramedDefaults[1].body;

    // Only the ratio moves. The rest of the body must survive intact, because
    // the deck cannot be read back and a zeroed field would be silent damage.
    for (std::size_t index = 0; index < protocol::FramedDefaults[1].length; ++index)
    {
        INFO("body byte " << index);
        CHECK(sent[5 + index] == (index == 11 ? 16 : reference[index]));
    }
}

TEST_CASE("a sixteen-bit parameter is written little-endian", "[device][parameter]")
{
    Harness harness;
    harness.connectInControlMode();

    REQUIRE(harness.device.setParameter(protocol::Parameter::ReverbRoomSize, 625).has_value());

    auto const& sent = harness.transport.sent().front().bytes;
    CHECK(sent[5 + 18] == 0x71);
    CHECK(sent[5 + 19] == 0x02);
    CHECK(harness.device.parameter(protocol::Parameter::ReverbRoomSize) == 625);
}

TEST_CASE("a parameter clamps to its documented range", "[device][parameter]")
{
    Harness harness;
    harness.connectInControlMode();

    REQUIRE(harness.device.setParameter(protocol::Parameter::CompressorThreshold, 40).has_value());
    CHECK(harness.device.parameter(protocol::Parameter::CompressorThreshold) == 0);

    REQUIRE(harness.device.setParameter(protocol::Parameter::CompressorThreshold, -900).has_value());
    CHECK(harness.device.parameter(protocol::Parameter::CompressorThreshold) == -60);
}

TEST_CASE("an unwritten parameter reports the vendor default", "[device][parameter]")
{
    Harness harness;
    harness.connectInControlMode();

    // The cache is seeded from the same bodies connect() sends, so before any
    // edit it already agrees with the device. init[30] carries -18 dB and 12:1.
    CHECK(harness.device.parameter(protocol::Parameter::CompressorThreshold) == -18);
    CHECK(harness.device.parameter(protocol::Parameter::CompressorRatio) == 12);
}

TEST_CASE("a framed command carries the checksum the deck verifies", "[device][parameter]")
{
    Harness harness;
    harness.connectInControlMode();

    REQUIRE(harness.device.setParameter(protocol::Parameter::ReverbLevel, 200).has_value());

    auto const& sent = harness.transport.sent().front().bytes;
    auto const length = sent[3];

    // Same rule as every captured command: the low byte of everything from the
    // length up to, but not including, the checksum itself.
    unsigned sum = 0;
    for (std::size_t index = 3; index + 1 < std::size_t { length } + 1; ++index)
        sum += sent[index];

    CHECK(sent[length] == static_cast<std::uint8_t>(sum & 0xFF));
}

TEST_CASE("setting a level refuses when the deck is not connected", "[device][level]")
{
    Harness harness;

    auto const result = harness.device.setLevel(MixId::Creator, KnobId::Mic, Level::fromPercent(50));

    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == DeviceError::NotConnected);
    CHECK(harness.transport.sent().empty());
}

TEST_CASE("a level asked for between steps lands on the nearest one", "[device][level]")
{
    Harness harness;
    harness.connectInControlMode();

    // 23% is not reachable on a 21-step control. It rounds to 25%, which is step
    // 5 -- where the retired setKnobVolume truncated it down to step 4.
    REQUIRE(harness.device.setLevel(MixId::Creator, KnobId::Game, Level::fromPercent(23)).has_value());

    CHECK(harness.transport.sent().front().bytes[5] == 5);
}

TEST_CASE("the panel's brightness goes out as a display command", "[device][display]")
{
    Harness harness;
    harness.connectInControlMode();

    REQUIRE(harness.device.setScreenBrightness(64).has_value());

    auto const& sent = harness.transport.sent().back().bytes;
    // [report id][0x01][0x0a][level] -- the display group, not a property write.
    CHECK(sent[1] == static_cast<std::uint8_t>(protocol::CommandKind::Set));
    CHECK(sent[2] == protocol::DisplayGroup);
    CHECK(sent[3] == 64);
}

TEST_CASE("a brightness the vendor would not send is clamped, not passed on",
          "[device][display]")
{
    // The vendor's slider stops at 25 and offers an off widget instead, so
    // nothing below that has ever been observed on the wire. Sending 1% would be
    // finding out what it does on somebody's deck.
    Harness harness;
    harness.connectInControlMode();

    REQUIRE(harness.device.setScreenBrightness(1).has_value());
    CHECK(harness.transport.sent().back().bytes[3] == protocol::MinPanelBrightness);

    REQUIRE(harness.device.setScreenBrightness(400).has_value());
    CHECK(harness.transport.sent().back().bytes[3] == protocol::MaxPanelBrightness);
}

TEST_CASE("blanking the panel sends the off sentinel, which is not a brightness",
          "[device][display]")
{
    Harness harness;
    harness.connectInControlMode();

    REQUIRE(harness.device.blankScreen().has_value());

    auto const& sent = harness.transport.sent().back().bytes;
    CHECK(sent[2] == protocol::DisplayGroup);
    CHECK(sent[3] == protocol::PanelOffLevel);
    CHECK(protocol::PanelOffLevel > protocol::MaxPanelBrightness);
}
