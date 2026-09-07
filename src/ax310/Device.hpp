// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Event.hpp"
#include "IClock.hpp"
#include "IHidTransport.hpp"
#include "ILogger.hpp"
#include "Protocol.hpp"
#include "Types.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <span>

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
/// The effects chain's state, as much of it as this driver can name.
///
/// A value type rather than a series of calls, so it can be held, compared and
/// stored whole -- the interface keeps one of these between runs and the driver
/// puts it back after a connect, which is what stops the deck starting in
/// whatever state the last program to touch it left behind.
struct EffectState
{
    /// Whether each of protocol::EffectEnables is on, in that table's order.
    ///
    /// Default is every effect off. That is the only starting point this project
    /// can define without guessing somebody's taste, and it is the harmless one:
    /// an unprocessed signal, rather than a stranger's reverb.
    std::array<bool, protocol::EffectEnables.size()> enabled {};

    /// One entry per protocol::Parameter, set only where somebody chose a value.
    ///
    /// Per parameter rather than all-or-nothing, and this is the whole point.
    /// Every parameter belongs to a framed command whose body carries far more
    /// than that parameter, so writing one writes the lot -- and the bodies this
    /// driver edits from are protocol::FramedDefaults, which are the captured
    /// vendor payloads for reverb and the compressor. Handing back a full block
    /// of values nobody chose therefore re-imposes exactly the microphone chain
    /// that was taken out of the handshake, through the door marked settings.
    std::array<std::optional<int>, protocol::ParameterCount> parameters {};
};

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
    /// Sets the panel's brightness.
    ///
    /// @param level A percentage. Clamped to the range the vendor's own slider
    ///        sends, 25 to 100: dimmer values have never been observed and the
    ///        vendor offers blankScreen() instead of going below its minimum.
    /// @return Nothing, or why the write failed.
    [[nodiscard]] std::expected<void, DeviceError> setScreenBrightness(int level);

    /// Turns the panel off.
    ///
    /// There is no matching call to turn it on: the deck wakes itself when the
    /// glass is touched, and a capture of that happening contains no
    /// host-to-device traffic to imitate.
    ///
    /// @return Nothing, or why the write failed.
    [[nodiscard]] std::expected<void, DeviceError> blankScreen();

    /// Sets how brightly the knob LED rings glow.
    /// @param percent Brightness, 0 to 100.
    /// @return Nothing, or why the write failed.
    [[nodiscard]] std::expected<void, DeviceError> setKnobLedBrightness(int percent);

    /// Lights one function button in a colour.
    ///
    /// There is no brightness field: the level is baked into the channels before
    /// they are sent, which is what protocol::scaledChannel is for.
    ///
    /// @param button Which button.
    /// @param red Red, 0 to 255, at the brightness wanted.
    /// @param green Green.
    /// @param blue Blue.
    /// @return Nothing, or why the write failed.
    [[nodiscard]] std::expected<void, DeviceError> setButtonColour(Button button, std::uint8_t red,
                                                                   std::uint8_t green,
                                                                   std::uint8_t blue);

    /// What the effects chain is to be put into after a connect.
    ///
    /// Held rather than applied at once: a connect is where it lands, because the
    /// deck may not be attached yet and because that is the moment the state would
    /// otherwise be inherited from whatever ran before.
    ///
    /// @param state What the chain should be.
    void setEffectState(EffectState const& state);

    /// @return The chain as this driver last set it: the enables from what was
    ///         asked for, the parameters from the cache the writes go through.
    [[nodiscard]] EffectState effectState() const;

    /// Switches one effect on or off and remembers that it did.
    /// @param command One of protocol::EffectEnables.
    /// @param enabled Whether it should be on.
    /// @return Nothing, or why the write failed.
    [[nodiscard]] std::expected<void, DeviceError> setEffectEnabled(protocol::FramedCommand command,
                                                                    bool enabled);

    /// Puts the effects chain into the state setEffectState was given.
    ///
    /// Part of a connect. The handshake carries no microphone chain, so the deck
    /// keeps whatever the last program to touch it configured -- which on a deck
    /// the vendor software has run is that software's settings. This is what makes
    /// the starting state a decision rather than an inheritance.
    ///
    /// The noise gate is not included: no enable for it has been captured, only
    /// parameter blocks. Failures are logged rather than returned, for the reason
    /// lightButtonsWithDefaults gives.
    void applyEffectState();

    /// Lights all four buttons with protocol::DefaultButtonColours, at
    /// protocol::DefaultButtonBrightnessPercent.
    ///
    /// Part of a connect, because the captured initialisation leaves two of the
    /// four dark. Failures are logged rather than returned: a button that would
    /// not light is not a reason to refuse the deck.
    void lightButtonsWithDefaults();

    /// Waits until a level write would not crowd the one before it.
    ///
    /// A dial sends one write per detent and the deck applies only some of a run
    /// sent back to back, so a fast turn lands on whichever of them it kept.
    /// Every other run of writes this driver sends is already spaced.
    void spaceLevelWrites();

    /// Reads back the mix and the levels the deck came up holding, and puts the
    /// ring colour where that mix says it should be.
    ///
    /// Failures are logged rather than returned, the way restoreProperties'
    /// are: the deck is up by this point, and a driver that refused the
    /// connection over an unread register would be worse than one that says so.
    void adoptTheDecksOwnState();

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

    /// @param mix Which mix to report.
    /// @param knob Which track.
    /// @return What the deck last said this level was, or what was last written
    ///         to it. A cache of the deck's own register and never a value of
    ///         this driver's own -- readLevels() is what fills it.
    [[nodiscard]] Level level(MixId mix, KnobId knob) const;

    /// Reads every track's level in both mixes back from the deck.
    ///
    /// Twelve round trips, so it belongs to a connect or to a mix change rather
    /// than to whoever asks first. It is also the only thing that makes level()
    /// worth reading: the deck holds these, this driver only remembers them.
    ///
    /// @return Nothing, or why the first failed read failed. The cache keeps
    ///         whatever was read before that.
    [[nodiscard]] std::expected<void, DeviceError> readLevels();

    /// Chooses which mix the deck monitors and displays on its knob rings.
    ///
    /// Fenced with Property::SettingsTransaction, matching the vendor: mode
    /// changes are bracketed, and level writes are not.
    ///
    /// @param mix The mix to monitor.
    /// @return Nothing, or why the write failed.
    [[nodiscard]] std::expected<void, DeviceError> selectMix(MixId mix);

    /// @return Which mix the deck was last read to be monitoring, or last told
    ///         to monitor.
    [[nodiscard]] MixId selectedMix() const;

    /// Reads which mix the deck is monitoring.
    ///
    /// Worth asking rather than assuming: connect() puts back the mix the deck
    /// was on before the handshake, which is not necessarily the one this driver
    /// would have picked.
    ///
    /// @return The mix Property::SelectedMix names, or why the read failed.
    [[nodiscard]] std::expected<MixId, DeviceError> readSelectedMix();

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
    /// What the effects chain is put into on connect. Defaults to everything off.
    EffectState _effectState {};

    /// Which parameters somebody has actually chosen a value for. Nothing else
    /// is stored or restored, because a parameter nobody set has no value worth
    /// putting back -- only a default that writing would impose.
    std::array<bool, protocol::ParameterCount> _parameterChosen {};

    std::array<protocol::FramedDefault, protocol::FramedDefaults.size()> _framedBodies =
        protocol::FramedDefaults;

    /// Guards everything below it that both threads reach.
    ///
    /// The poll thread moves the mix's levels when a knob turns; the thread
    /// driving the interface reads them to draw, and writes them when somebody
    /// drags a track or switches mix. Separate from _writeMutex, which is held
    /// across a transfer -- these are only ever held for an assignment.
    mutable std::mutex _stateMutex;

    /// Which mix the deck is monitoring, as last read from it or last chosen.
    MixId _mix = MixId::Creator;

    /// Every track's level in both mixes, as last read from the deck or last
    /// written to it.
    ///
    /// A cache of the deck's own registers rather than a state of this driver's:
    /// the deck reports a turn as a relative counter, so somebody has to hold the
    /// absolute value, but what that value *is* comes from the hardware. The
    /// rings display the selected mix's row, so this is also what the deck is
    /// showing.
    std::array<std::array<Level, KnobCount>, MixCount> _levels {};

    /// When the last level write went out, so a run of them can be spaced.
    /// Empty until one has.
    std::optional<std::chrono::steady_clock::time_point> _lastLevelWrite;

    /// Serialises writes: the host may push a frame from one thread while
    /// another drives poll().
    std::mutex _writeMutex;
};

} // namespace ax310
