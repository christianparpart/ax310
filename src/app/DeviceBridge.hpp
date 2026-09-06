// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <ax310/Device.hpp>
#include <ax310/Event.hpp>
#include <ax310/HidApiTransport.hpp>
#include <ax310/IClock.hpp>
#include <ax310/ILogger.hpp>
#include <ax310/Types.hpp>

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QVariantList>

#include <atomic>
#include <memory>
#include <thread>

namespace ax310::app
{

/// Qt's view of the deck, and the only place the driver meets Qt.
///
/// Implements IDeviceListener, turns each DeviceEvent into a signal QML can bind
/// to, and owns the thread that drives Device::poll() -- because the driver
/// deliberately owns no thread of its own. Everything below this class is
/// Qt-free.
///
/// The collaborators the driver needs are constructed here and injected into it,
/// which is also where a test would substitute fakes for all four.
class DeviceBridge final: public QObject, public IDeviceListener
{
    Q_OBJECT
    Q_PROPERTY(bool isConnected READ isConnected NOTIFY connectionStateChanged)
    Q_PROPERTY(QVariantList parameters READ parameters CONSTANT)
    Q_PROPERTY(QStringList trackNames READ trackNames CONSTANT)
    Q_PROPERTY(int selectedMix READ selectedMix NOTIFY mixChanged)
    Q_PROPERTY(bool panelShowsEffects READ panelShowsEffects WRITE setPanelShowsEffects NOTIFY
                   panelShowsEffectsChanged)

  public:
    /// Builds the seams the driver needs and injects them: a real USB transport,
    /// the system clock, and a logger writing to stderr.
    ///
    /// @param logLevel Lowest level to report; Debug traces every HID report and
    ///        every decoded event, which is what a protocol problem needs.
    /// @param parent Qt parent.
    explicit DeviceBridge(LogLevel logLevel = LogLevel::Info, QObject* parent = nullptr);

    /// Borrows seams somebody else owns, so the interface can be driven with no
    /// hardware present.
    ///
    /// This is what makes the GUI testable. Rendering a mixer requires a device
    /// to render the state of; with a FakeHidTransport supplying scripted
    /// reports, the whole stack from wire bytes to pixels runs in CI.
    ///
    /// @param transport Where reports come from and commands go.
    /// @param clock What paces the handshake.
    /// @param logger Where diagnostics go.
    /// @param parent Qt parent.
    DeviceBridge(IHidTransport& transport, IClock& clock, ILogger& logger,
                 QObject* parent = nullptr);
    ~DeviceBridge() override;

    DeviceBridge(DeviceBridge const&) = delete;
    DeviceBridge& operator=(DeviceBridge const&) = delete;
    DeviceBridge(DeviceBridge&&) = delete;
    DeviceBridge& operator=(DeviceBridge&&) = delete;

    /// Starts the worker thread, which connects and then polls until stopped.
    /// Reconnection is part of that loop, so a deck plugged in later is picked
    /// up without anything else happening.
    void start();

    /// Stops the worker thread and disconnects. Safe to call more than once.
    Q_INVOKABLE void stop();

    /// @return Whether the deck is ready to be talked to.
    [[nodiscard]] bool isConnected() const;

    /// Sets how brightly the knob LED rings glow. Confirmed on the hardware.
    /// @param percent Brightness, 0 to 100.
    Q_INVOKABLE void setKnobLedBrightness(int percent);

    /// @return What the deck prints under each knob, left to right.
    ///
    /// Taken from the driver's KnobNames rather than retyped in QML, so the
    /// labels on screen cannot drift from the labels the protocol uses.
    [[nodiscard]] QStringList trackNames() const;

    /// Describes every adjustable DSP value, for QML to build controls from.
    ///
    /// One map per parameter: `id`, `name`, `unit`, `effect`, `effectName`,
    /// `minimum`, `maximum`. Constant, because the table is -- values travel
    /// separately through parameterChanged, so a slider being dragged never
    /// causes its own delegate to be rebuilt underneath it.
    ///
    /// @return The parameter descriptions, in enumerator order.
    [[nodiscard]] QVariantList parameters() const;

    /// @param parameter A protocol::Parameter, as an int for QML.
    /// @return What the driver last wrote, or the vendor default.
    Q_INVOKABLE [[nodiscard]] int parameterValue(int parameter) const;

    /// Sets one DSP parameter. QML-facing, so a failure is logged rather than
    /// returned: QML cannot consume a std::expected.
    /// @param parameter A protocol::Parameter, as an int.
    /// @param value The new value; the driver clamps it.
    Q_INVOKABLE void setParameter(int parameter, int value);

    /// Sets one track's level in one mix.
    /// @param mix An ax310::MixId, as an int.
    /// @param knob An ax310::KnobId, as an int.
    /// @param percent The level a person asked for; the driver rounds it to the
    ///        nearest step the hardware can hold.
    Q_INVOKABLE void setLevel(int mix, int knob, int percent);

    /// Chooses which mix the deck monitors, and which one the interface edits.
    /// @param mix An ax310::MixId, as an int.
    Q_INVOKABLE void selectMix(int mix);

    /// @param mix An ax310::MixId, as an int.
    /// @param knob An ax310::KnobId, as an int.
    /// @return That track's level in that mix, in percent.
    ///
    /// Reads a cache rather than the deck. An interface must be able to ask for
    /// the current state rather than only react to a change -- QML loaded after a
    /// connect would otherwise show zeroes until somebody moved something.
    Q_INVOKABLE [[nodiscard]] int levelPercent(int mix, int knob) const;

    /// @return The mix currently being edited, as an int.
    [[nodiscard]] int selectedMix() const noexcept;

    /// Which page the deck's panel is on.
    ///
    /// Interface state rather than anything the deck reports, and it lives here
    /// because this is the only object the two QML engines share: the panel is
    /// rendered in a QQuickView of its own and previewed again inside the desktop
    /// window, so without one place to keep this the preview would sit under a
    /// heading that says "on the deck" while showing a different page from the
    /// one the deck is showing.
    ///
    /// @return Whether the panel is showing the effects rather than the tracks.
    [[nodiscard]] bool panelShowsEffects() const noexcept;

    /// @param showing Whether the panel should show the effects.
    void setPanelShowsEffects(bool showing);

    /// Pushes one encoded frame to the deck's screen.
    /// @param jpeg The encoded frame.
    void sendScreen(QByteArray const& jpeg);

    /// Pushes one frame, reporting its dimensions so a wrongly sized grab is
    /// visible in the log rather than silently ignored by the deck.
    /// @param width Pixel width of the image the JPEG was encoded from.
    /// @param height Pixel height of that image.
    /// @param jpeg The encoded frame.
    void sendFrame(int width, int height, QByteArray const& jpeg);

    /// Records that the window grab came back at a size the deck cannot take, so
    /// the resize that follows is visible rather than silent. Said once per size.
    /// @param width The grab's width.
    /// @param height The grab's height.
    void noteGrabResized(int width, int height);

    /// Writes a line to the driver's log from QML.
    ///
    /// QML's own console.log goes to Qt's categorised logging, which on this
    /// machine prints nothing at all -- so QML diagnostics were being swallowed
    /// silently. This routes them through the same ILogger seam as everything
    /// else, which means one place decides where output goes and one threshold
    /// decides what survives.
    ///
    /// The level is a string because QML cannot name a C++ enum declared in a
    /// Qt-free header; it is matched against LogLevelNames, so there is still
    /// only one list of level names in the project. An unrecognised name is
    /// reported rather than guessed at.
    ///
    /// @param level One of "debug", "info", "warning", "error".
    /// @param message The line to write.
    Q_INVOKABLE void log(QString const& level, QString const& message);

    /// Called by the driver from the worker thread. Fans the event out as a
    /// signal, which Qt queues to whichever thread the receiver lives in.
    /// @param event What the deck reported.
    void onDeviceEvent(DeviceEvent const& event) override;

  signals:
    /// A physical button went down.
    void buttonPressed(ax310::Button button);

    /// A knob's tracked volume changed, in percent.
    void knobVolumeChanged(ax310::KnobId knob, int volume);

    /// One track's level in one mix, as the hardware holds it. Emitted for all
    /// twelve after a connect, so the interface starts from the device's state
    /// rather than from a default nobody chose.
    void levelChanged(int mix, int knob, int percent);

    /// A DSP parameter's value changed.
    void parameterChanged(int parameter, int value);

    /// The mix being monitored and edited changed.
    void mixChanged(int mix);

    /// The deck's panel moved between its pages.
    void panelShowsEffectsChanged(bool showing);

    /// A knob's capacitive surface was touched or released.
    void knobTouched(ax310::KnobId knob, ax310::Touch touch);

    /// A knob was pushed in.
    void knobPushed(ax310::KnobId knob);

    /// The touch screen was contacted or released at @p x, @p y.
    void screenTouched(int x, int y, ax310::TouchPhase phase);

    /// The audio meters moved: one per track, in the deck's printed knob order,
    /// each a percentage of full scale. Sent as a list because they arrive
    /// together.
    void audioMetersChanged(QList<int> levels);

    /// The connection moved to @p state.
    void connectionStateChanged(ax310::ConnectionState state);

  private:
    void run();

    /// Reads all twelve levels from the deck and announces them. Runs on the
    /// worker thread as part of a connect.
    void publishLevels();

    // Held only when the production constructor made them; the injecting
    // constructor leaves all three empty and borrows instead.
    std::unique_ptr<HidApiTransport> _ownedTransport;
    std::unique_ptr<SystemClock> _ownedClock;
    std::unique_ptr<StderrLogger> _ownedLogger;

    IHidTransport& _transport;
    IClock& _clock;
    ILogger& _logger;
    Device _device;

    /// The last wrongly-sized frame reported, so a mis-sized grab is said once
    /// rather than thirty times a second.
    int _reportedBadWidth = 0;
    int _reportedBadHeight = 0;
    int _reportedGrabWidth = 0;
    int _reportedGrabHeight = 0;

    /// The levels as last written or last read from the deck, in percent.
    std::array<std::array<int, ax310::KnobCount>, ax310::MixCount> _levels {};

    /// Which mix the interface is editing. Held here rather than read back
    /// because the deck offers no way to ask.
    ax310::MixId _mix = ax310::MixId::Creator;

    /// Which page the panel is on, shared by every view of it.
    bool _panelShowsEffects = false;

    std::atomic<bool> _isRunning { false };
    std::thread _worker;
};

/// Registers the driver's enums with Qt's meta-object system.
///
/// They are declared in Qt-free headers, so moc never sees the enumerators and
/// Q_ENUM_NS is not available. Without this a queued signal emission from the
/// worker thread has no metatype to marshal through. Call once, before the first
/// connection is made.
void registerDeviceMetaTypes();

} // namespace ax310::app

Q_DECLARE_METATYPE(ax310::Button)
Q_DECLARE_METATYPE(ax310::KnobId)
Q_DECLARE_METATYPE(ax310::Touch)
Q_DECLARE_METATYPE(ax310::TouchPhase)
Q_DECLARE_METATYPE(ax310::ConnectionState)
