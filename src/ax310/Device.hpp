// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <span>

#include "Event.hpp"
#include "IClock.hpp"
#include "IHidTransport.hpp"
#include "ILogger.hpp"
#include "Protocol.hpp"
#include "Types.hpp"

namespace ax310
{

/// The AX310 control deck.
///
/// Qt-free by construction, and free of ambient state: the USB bus, the clock,
/// the log and the event sink all arrive through the constructor, so the whole
/// driver -- enumeration order, the initialisation handshake, report decoding,
/// frame chunking -- runs against fakes with no hardware attached.
///
/// It owns no thread either. poll() performs exactly one read cycle, and
/// whoever wants a loop provides one; that keeps the threading policy with the
/// host, which is the only place that knows what else is running.
class Device
{
  public:
    /// The previous report's fields, kept so a change can be recognised.
    ///
    /// Public because the decode tables in the implementation address its
    /// members by pointer-to-member; it is otherwise an internal detail.
    struct ReportState
    {
        std::uint8_t buttons = 0;
        std::uint8_t knobPush = 0;
        std::uint8_t knobTouch = 0;

        /// 255 rather than 0: the device reports 0 as a real position, so a
        /// value no knob can hold is needed to mean "not seen yet".
        std::array<std::uint8_t, KnobCount> knobValues { 255, 255, 255, 255, 255, 255 };
    };

    /// @param transport The USB HID stack to talk through.
    /// @param clock The clock to pace the initialisation handshake by.
    /// @param logger Where diagnostics go.
    /// @param listener Where decoded events go.
    Device(IHidTransport& transport, IClock& clock, ILogger& logger, IDeviceListener& listener) noexcept;

    ~Device();

    Device(Device const&) = delete;
    Device& operator=(Device const&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    /// Finds the deck, opens it, and initialises it if it is in base mode.
    ///
    /// @return Connected when the deck is ready; Connecting when a base-mode
    ///         device was handed its initialisation sequence and must now
    ///         re-enumerate in control mode, which is a wait rather than a
    ///         failure. An error otherwise.
    [[nodiscard]] std::expected<ConnectionState, DeviceError> connect();

    /// Sends the shutdown sequence and closes the device. Safe when unconnected.
    void disconnect();

    /// @return Where the device is in its connect / re-enumerate / run cycle.
    [[nodiscard]] ConnectionState connectionState() const noexcept;

    /// @return Whether the deck is ready to be talked to.
    [[nodiscard]] bool isConnected() const noexcept;

    /// Reads at most one input report and emits what it carries.
    ///
    /// Returns without emitting anything when the read times out, which is the
    /// normal case for an idle deck.
    ///
    /// @param timeout How long to wait for a report.
    /// @return Nothing, or why the read failed.
    [[nodiscard]] std::expected<void, DeviceError> poll(std::chrono::milliseconds timeout);

    /// Sets the screen backlight.
    /// @param level Brightness in percent.
    /// @return Nothing, or why the write failed.
    [[nodiscard]] std::expected<void, DeviceError> setScreenBrightness(int level);

    /// Sets how brightly the knob LED rings glow.
    /// @param percent Brightness, 0 to 100.
    /// @return Nothing, or why the write failed.
    [[nodiscard]] std::expected<void, DeviceError> setKnobLedBrightness(int percent);

    /// Pushes one JPEG frame to the device's screen, chunked as the deck wants.
    /// @param frame The encoded frame.
    /// @return Nothing, or why the transfer failed.
    [[nodiscard]] std::expected<void, DeviceError> sendScreen(std::span<std::uint8_t const> frame);

    /// Sets one track's level in one mix.
    ///
    /// The two mixes are independent, so the mix is not optional -- setting a
    /// level without saying which mix it belongs to is the bug this signature
    /// exists to prevent. Replaces setKnobVolume, which always wrote the creator
    /// block and took a percentage it then truncated.
    ///
    /// @param mix Which mix to adjust.
    /// @param knob Which track.
    /// @param level The new level.
    /// @return Nothing, or why the write failed.
    [[nodiscard]] std::expected<void, DeviceError> setLevel(MixId mix, KnobId knob, Level level);

    /// Reads one track's level back from the deck.
    ///
    /// @param mix Which mix to read.
    /// @param knob Which track.
    /// @return The level the hardware holds, or why the read failed.
    [[nodiscard]] std::expected<Level, DeviceError> level(MixId mix, KnobId knob);

    /// Chooses which mix the deck monitors and displays on its knob rings.
    ///
    /// Fenced with Property::SettingsTransaction, matching the vendor: mode
    /// changes are bracketed, and level writes are not.
    ///
    /// @param mix The mix to monitor.
    /// @return Nothing, or why the write failed.
    [[nodiscard]] std::expected<void, DeviceError> selectMix(MixId mix);

    /// Sets one DSP parameter.
    ///
    /// The framed command family has no read-back, so this edits a cached body
    /// seeded from the same values connect() sends and rewrites the whole
    /// command. That is why the cache is only trustworthy after a connect.
    ///
    /// @param parameter Which value to change.
    /// @param value The new value, clamped into the parameter's documented range.
    /// @return Nothing, or why the write failed.
    [[nodiscard]] std::expected<void, DeviceError> setParameter(protocol::Parameter parameter,
                                                                int value);

    /// @param parameter Which value to report.
    /// @return What this driver last wrote, or the vendor default if it has not
    ///         been written. Not read from the deck, because the deck does not
    ///         offer it.
    [[nodiscard]] int parameter(protocol::Parameter parameter) const noexcept;

    /// Reads one register out of the deck's property space.
    ///
    /// Sends the read command and then asks for the answer, checking that the
    /// reply echoes the address and length that were asked for -- without that
    /// check a caller reads the remains of the previous reply and cannot tell.
    ///
    /// @param address Where to read, named or not.
    /// @param values Where to put the answer; its size is how much to ask for.
    /// @return Nothing, or why the read failed.
    [[nodiscard]] std::expected<void, DeviceError> readProperty(std::uint8_t address,
                                                                std::span<std::uint8_t> values);

    /// Writes one register, refusing addresses known to harm the device.
    ///
    /// The refusal is the point. protocol::DangerousAddresses has been carried
    /// since a blind write to `0x16` wedged this deck badly enough to need a
    /// power cycle, and nothing had ever consulted it -- safely, because there
    /// was no way to write an arbitrary address. This is that way, so it is also
    /// the gate. A refused write is reported as WriteFailed and logged, and
    /// nothing reaches the bus.
    ///
    /// @param address Where to write, named or not.
    /// @param values The bytes to put there.
    /// @return Nothing, or why the write failed.
    [[nodiscard]] std::expected<void, DeviceError> writeProperty(std::uint8_t address,
                                                                 std::span<std::uint8_t const> values);

  private:
    /// What the registers held before the init sequence overwrote them.
    ///
    /// Fixed storage rather than a container: there are thirteen of them and the
    /// longest is seven bytes, so this is small enough to sit in the device and
    /// costs no allocation on a path that runs during connect.
    struct PropertySnapshot
    {
        std::array<std::array<std::uint8_t, protocol::MaxPreservedLength>,
                   protocol::PreservedAddresses.size()>
            values {};

        /// Which entries were actually read. A cold deck may answer nothing at
        /// all, and a register that was not read must not be written back.
        std::array<bool, protocol::PreservedAddresses.size()> isValid {};
    };

    /// Sends one command of the framed family, checksummed.
    /// @param command Which command.
    /// @param body Its body.
    /// @return Nothing, or why the write failed.
    [[nodiscard]] std::expected<void, DeviceError> sendFramed(protocol::FramedCommand command,
                                                              std::span<std::uint8_t const> body);

    /// Reads every register the init sequence is about to overwrite.
    [[nodiscard]] PropertySnapshot snapshotProperties();

    /// Puts back whatever @p snapshot captured. Failures are logged, not
    /// returned: the deck is up by this point, and a restore that did not take is
    /// worth saying but not worth refusing the connection over.
    void restoreProperties(PropertySnapshot const& snapshot);

    /// Sends one command table, feature report first and write as the fallback.
    [[nodiscard]] std::expected<void, DeviceError> sendSequence(std::span<protocol::Payload const> sequence,
                                                                std::chrono::milliseconds gap);

    /// Opens the first interface of @p mode the transport offers.
    [[nodiscard]] std::expected<void, DeviceError> openInterface(DeviceMode mode);

    /// Moves to @p state and tells the listener, if it is a change.
    void publishState(ConnectionState state);

    void dispatchEvent(protocol::InputReport const& report);
    void dispatchAudioMeters(protocol::InputReport const& report);
    void dispatchKnobBitmasks(protocol::InputReport const& report);
    void dispatchKnobValues(protocol::InputReport const& report);

    IHidTransport& _transport;
    IClock& _clock;
    ILogger& _logger;
    IDeviceListener& _listener;

    ConnectionState _state = ConnectionState::Disconnected;
    bool _isSeeded = false;

    /// Whether a finger is currently on the glass, and where it last was.
    ///
    /// The deck signals contact by sending screen-touch reports and signals the
    /// lift by going back to ordinary ones -- it has no release event of its own,
    /// so the release, and the position to report it at, are ours to remember.
    bool _isTouchDown = false;
    int _touchX = 0;
    int _touchY = 0;
    ReportState _last;

    /// The meters as last published, so an unchanged set is not re-sent at the
    /// report rate.
    std::array<int, protocol::AudioMeterCount> _audioMeters { -1, -1, -1, -1, -1, -1 };

    /// The DSP command bodies as this driver last sent them.
    ///
    /// Seeded from the vendor defaults, which are also what connect() sends, so
    /// the cache starts in agreement with the device. It has to be a cache: the
    /// framed family cannot be read back, so editing one field of a body means
    /// remembering the other twenty.
    std::array<protocol::FramedDefault, protocol::FramedDefaults.size()> _framedBodies =
        protocol::FramedDefaults;

    /// Volume per knob, in percent. Held here because the deck reports turns as
    /// a relative counter, so the absolute level is ours to keep, not its.
    std::array<int, KnobCount> _volumes { 50, 50, 50, 50, 50, 50 };

    /// Serialises writes: the host may push a frame from one thread while
    /// another drives poll().
    std::mutex _writeMutex;
};

} // namespace ax310
