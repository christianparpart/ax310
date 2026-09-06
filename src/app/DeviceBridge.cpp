// SPDX-License-Identifier: Apache-2.0
#include "DeviceBridge.hpp"

#include <QFile>
#include <QMetaType>
#include <QString>
#include <QVariantMap>

#include <chrono>
#include <cstddef>
#include <span>
#include <utility>
#include <variant>

namespace ax310::app
{

namespace
{
    /// How long one poll waits for a report before the loop gets a chance to
    /// notice a stop request.
    constexpr std::chrono::milliseconds PollTimeout { 100 };

    /// The deck's screen, in pixels. A frame of any other size is ignored by the
    /// hardware without complaint.
    constexpr int ScreenWidth = 800;
    constexpr int ScreenHeight = 480;

    /// How long to wait before trying to connect again.
    constexpr std::chrono::milliseconds ReconnectInterval { 2000 };

    /// How often the worker says it is still going round.
    constexpr std::chrono::milliseconds HeartbeatInterval { 5000 };

    /// A visitor made from a set of lambdas -- the standard std::visit idiom.
    template <typename... Ts>
    struct Overloaded: Ts...
    {
        using Ts::operator()...;
    };
} // namespace

void registerDeviceMetaTypes()
{
    qRegisterMetaType<Button>();
    qRegisterMetaType<KnobId>();
    qRegisterMetaType<Touch>();
    qRegisterMetaType<TouchPhase>();
    qRegisterMetaType<ConnectionState>();
}

DeviceBridge::DeviceBridge(LogLevel logLevel, QObject* parent):
    QObject(parent),
    _ownedTransport { std::make_unique<HidApiTransport>() },
    _ownedClock { std::make_unique<SystemClock>() },
    _ownedConsole { std::make_unique<SystemConsole>() },
    _ownedLogger { std::make_unique<ConsoleLogger>(*_ownedConsole, logLevel) },
    _transport { *_ownedTransport },
    _clock { *_ownedClock },
    _logger { *_ownedLogger },
    _device { _transport, _clock, _logger, *this }
{
}

DeviceBridge::DeviceBridge(IHidTransport& transport, IClock& clock, ILogger& logger, QObject* parent):
    QObject(parent),
    _transport { transport },
    _clock { clock },
    _logger { logger },
    _device { _transport, _clock, _logger, *this }
{
}

DeviceBridge::~DeviceBridge()
{
    stop();
}

void DeviceBridge::start()
{
    if (_isRunning.exchange(true))
        return;

    _worker = std::thread(&DeviceBridge::run, this);
}

void DeviceBridge::stop()
{
    if (!_isRunning.exchange(false))
        return;

    if (_worker.joinable())
        _worker.join();

    _device.disconnect();
}

bool DeviceBridge::isConnected() const
{
    return _device.isConnected();
}

void DeviceBridge::run()
{
    // A poll that times out logs nothing, so an idle deck and a wedged worker
    // look identical in the trace. The heartbeat tells them apart.
    auto lastBeat = _clock.now();
    std::size_t polls = 0;

    while (_isRunning.load())
    {
        if (_clock.now() - lastBeat >= HeartbeatInterval)
        {
            logTo(_logger, LogLevel::Debug, "worker alive: {} polls since the last beat", polls);
            polls = 0;
            lastBeat = _clock.now();
        }

        if (!_device.isConnected())
        {
            // A base-mode deck answers Connecting and has to re-enumerate, so
            // that outcome waits exactly like a failure does.
            auto const state = _device.connect();
            if (!state || *state != ConnectionState::Connected)
            {
                _clock.sleepFor(ReconnectInterval);
                continue;
            }

            // Read on this thread, not the GUI's: it is twelve USB round trips,
            // and it belongs to the connect rather than to whoever asks first.
            publishLevels();
            continue;
        }

        ++polls;
        if (auto const polled = _device.poll(PollTimeout); !polled)
        {
            logTo(_logger, LogLevel::Warning, "Read failed: {}", describe(polled.error()));
            _device.disconnect();
        }
    }
}

void DeviceBridge::setKnobLedBrightness(int percent)
{
    if (auto const result = _device.setKnobLedBrightness(percent); !result)
        logTo(_logger, LogLevel::Warning, "setKnobLedBrightness failed: {}", describe(result.error()));
}

void DeviceBridge::publishLevels()
{
    for (auto const mix: AllMixes)
    {
        for (auto const knob: AllKnobs)
        {
            auto const level = _device.level(mix, knob);
            if (!level)
            {
                logTo(_logger,
                      LogLevel::Debug,
                      "could not read the {} level for {}: {}",
                      nameOf(mix),
                      nameOf(knob),
                      describe(level.error()));
                continue;
            }

            _levels[indexOf(mix)][indexOf(knob)] = level->asPercent();
            emit levelChanged(static_cast<int>(indexOf(mix)),
                              static_cast<int>(indexOf(knob)),
                              level->asPercent());
        }
    }
}

QStringList DeviceBridge::trackNames() const
{
    QStringList names;
    names.reserve(static_cast<qsizetype>(KnobCount));
    for (auto const knob: AllKnobs)
    {
        auto const label = nameOf(knob);
        names.append(QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size())));
    }

    return names;
}

QVariantList DeviceBridge::parameters() const
{
    QVariantList rows;
    rows.reserve(static_cast<qsizetype>(protocol::Parameters.size()));

    for (auto const& info: protocol::Parameters)
    {
        QVariantMap row;
        row["id"] = static_cast<int>(indexOf(info.id));
        row["name"] = QString::fromUtf8(info.name.data(), static_cast<qsizetype>(info.name.size()));
        row["unit"] = QString::fromUtf8(info.unit.data(), static_cast<qsizetype>(info.unit.size()));
        row["effect"] = static_cast<int>(info.effect);
        auto const effectName = nameOf(info.effect);
        row["effectName"] =
            QString::fromUtf8(effectName.data(), static_cast<qsizetype>(effectName.size()));
        row["minimum"] = info.minimum;
        row["maximum"] = info.maximum;
        rows.append(row);
    }

    return rows;
}

int DeviceBridge::parameterValue(int parameter) const
{
    if (parameter < 0 || std::cmp_greater_equal(parameter, protocol::Parameters.size()))
        return 0;

    return _device.parameter(static_cast<protocol::Parameter>(parameter));
}

void DeviceBridge::setParameter(int parameter, int value)
{
    if (parameter < 0 || std::cmp_greater_equal(parameter, protocol::Parameters.size()))
    {
        logTo(_logger, LogLevel::Warning, "qml asked for unknown parameter {}", parameter);
        return;
    }

    auto const id = static_cast<protocol::Parameter>(parameter);
    if (auto const result = _device.setParameter(id, value); !result)
    {
        logTo(_logger, LogLevel::Warning, "setParameter failed: {}", describe(result.error()));
        return;
    }

    // Echoed from what the driver actually holds, not from what was asked for,
    // so a clamped value corrects the control rather than leaving it lying.
    emit parameterChanged(parameter, _device.parameter(id));
}

void DeviceBridge::setLevel(int mix, int knob, int percent)
{
    if (mix < 0 || std::cmp_greater_equal(mix, MixCount) || knob < 0
        || std::cmp_greater_equal(knob, KnobCount))
    {
        logTo(_logger, LogLevel::Warning, "qml asked for mix {} knob {}", mix, knob);
        return;
    }

    auto const level = Level::fromPercent(percent);
    auto const result =
        _device.setLevel(static_cast<MixId>(mix), static_cast<KnobId>(knob), level);
    if (!result)
    {
        logTo(_logger, LogLevel::Warning, "setLevel failed: {}", describe(result.error()));
        return;
    }

    _levels[static_cast<std::size_t>(mix)][static_cast<std::size_t>(knob)] = level.asPercent();
    emit levelChanged(mix, knob, level.asPercent());
}

void DeviceBridge::selectMix(int mix)
{
    if (mix < 0 || std::cmp_greater_equal(mix, MixCount))
    {
        logTo(_logger, LogLevel::Warning, "qml asked for unknown mix {}", mix);
        return;
    }

    auto const chosen = static_cast<MixId>(mix);
    if (auto const result = _device.selectMix(chosen); !result)
    {
        logTo(_logger, LogLevel::Warning, "selectMix failed: {}", describe(result.error()));
        return;
    }

    _mix = chosen;
    emit mixChanged(mix);
}

int DeviceBridge::levelPercent(int mix, int knob) const
{
    if (mix < 0 || std::cmp_greater_equal(mix, MixCount) || knob < 0
        || std::cmp_greater_equal(knob, KnobCount))
        return 0;

    return _levels[static_cast<std::size_t>(mix)][static_cast<std::size_t>(knob)];
}

int DeviceBridge::selectedMix() const noexcept
{
    return static_cast<int>(indexOf(_mix));
}

bool DeviceBridge::micMonitor() const
{
    return levelPercent(static_cast<int>(indexOf(MixId::Creator)),
                        static_cast<int>(indexOf(KnobId::Mic)))
           > 0;
}

void DeviceBridge::setMicMonitor(bool enabled)
{
    auto const creatorMix = static_cast<int>(indexOf(MixId::Creator));
    auto const micTrack = static_cast<int>(indexOf(KnobId::Mic));
    auto const current = levelPercent(creatorMix, micTrack);

    if (!enabled)
    {
        // Remember what is being silenced. Restoring to full instead would be
        // louder than what the person had, which is the wrong way to be wrong.
        if (current > 0)
            _monitorRestoreLevel = current;
        setLevel(creatorMix, micTrack, 0);
        return;
    }

    setLevel(creatorMix, micTrack, _monitorRestoreLevel > 0 ? _monitorRestoreLevel : 100);
}

bool DeviceBridge::panelShowsEffects() const noexcept
{
    return _panelShowsEffects;
}

void DeviceBridge::setPanelShowsEffects(bool showing)
{
    if (_panelShowsEffects == showing)
        return;

    _panelShowsEffects = showing;
    emit panelShowsEffectsChanged(showing);
}

void DeviceBridge::sendScreen(QByteArray const& jpeg)
{
    auto const frame = std::span { reinterpret_cast<std::uint8_t const*>(jpeg.constData()),
                                   static_cast<std::size_t>(jpeg.size()) };

    if (auto const result = _device.sendScreen(frame); !result)
        logTo(_logger, LogLevel::Warning, "sendScreen failed: {}", describe(result.error()));
}

void DeviceBridge::sendFrame(int width, int height, QByteArray const& jpeg)
{
    if (width != ScreenWidth || height != ScreenHeight)
    {
        // Worth an error rather than a note: the deck accepts the transfer and
        // then shows nothing, so a wrong size looks exactly like a dead screen.
        // Said once per size, though -- at thirty frames a second the repeat
        // buried everything else in the log, which is its own kind of unhelpful.
        if (width != _reportedBadWidth || height != _reportedBadHeight)
        {
            _reportedBadWidth = width;
            _reportedBadHeight = height;
            logTo(_logger,
                  LogLevel::Error,
                  "frame is {}x{}, the deck needs {}x{} -- it will be ignored",
                  width,
                  height,
                  ScreenWidth,
                  ScreenHeight);
        }
        return;
    }

    _reportedBadWidth = 0;
    _reportedBadHeight = 0;

    logTo(_logger, LogLevel::Debug, "frame: {}x{}, {} bytes", width, height, jpeg.size());

    // Writes the next frame to a file and stops. The panel is the one surface
    // whose contents cannot be inspected any other way -- it lives on the device,
    // and asking somebody to read numbers off a 5" screen across a desk is not a
    // diagnosis.
    // Overwrites, so the file always holds what the deck is showing now. Dumping
    // only the first frame caught the panel mid-connect, before the levels had
    // been read back, and made a half-drawn moment look like a bug.
    if (auto const path = qEnvironmentVariable("AX310_DUMP_FRAME"); !path.isEmpty())
    {
        QFile file { path };
        if (file.open(QIODevice::WriteOnly))
            file.write(jpeg);
    }

    sendScreen(jpeg);
}

void DeviceBridge::noteGrabResized(int width, int height)
{
    if (width == _reportedGrabWidth && height == _reportedGrabHeight)
        return;

    _reportedGrabWidth = width;
    _reportedGrabHeight = height;

    // Not a fault: a desktop at 125% scaling grabs 1000x600 for an 800x480 item,
    // and the frame is resized before it goes out. Logged because a silent
    // resize is a silent quality loss, and because it names the cause if the
    // panel ever looks soft.
    logTo(_logger,
          LogLevel::Info,
          "window grab is {}x{}, resizing to {}x{} for the deck",
          width,
          height,
          ScreenWidth,
          ScreenHeight);
}

void DeviceBridge::log(QString const& level, QString const& message)
{
    auto const named = levelFromName(level.toStdString());
    if (!named)
    {
        logTo(_logger,
              LogLevel::Warning,
              "qml asked for unknown log level '{}': {}",
              level.toStdString(),
              message.toStdString());
        return;
    }

    logTo(_logger, *named, "[qml] {}", message.toStdString());
}

void DeviceBridge::onDeviceEvent(DeviceEvent const& event)
{
    // Traced before the fan-out, so a missing signal can be told apart from an
    // event that was never decoded in the first place.
    std::visit(Overloaded {
                   [this](ButtonPressed const& e) {
                       logTo(_logger, LogLevel::Debug, "event: button {}", indexOf(e.button));
                   },
                   [this](KnobPushed const& e) {
                       logTo(_logger, LogLevel::Debug, "event: knob {} pushed", indexOf(e.knob));
                   },
                   [this](KnobTouched const& e) {
                       logTo(_logger,
                             LogLevel::Debug,
                             "event: knob {} touch={}",
                             indexOf(e.knob),
                             e.touch == Touch::Touched);
                   },
                   [this](KnobVolumeChanged const& e) {
                       logTo(_logger, LogLevel::Debug, "event: knob {} volume={}", indexOf(e.knob), e.volume);
                   },
                   [this](ScreenTouched const& e) {
                       logTo(_logger, LogLevel::Debug, "event: screen {},{}", e.x, e.y);
                   },
                   [this](AudioMetersChanged const& e) {
                       logTo(_logger,
                             LogLevel::Debug,
                             // Named by position, because what they measure is
                             // not settled. An earlier version named six elements
                             // of an array that had become two and read four ints
                             // past the end of it.
                             "event: meters {} {}",
                             e.levels[0],
                             e.levels[1]);
                   },
                   [this](ConnectionChanged const& e) {
                       logTo(_logger, LogLevel::Info, "connection state {}", static_cast<int>(e.state));
                   },
               },
               event);

    // One arm per alternative, and the compiler names any alternative that grows
    // into DeviceEvent without being handled here.
    std::visit(Overloaded {
                   [this](ButtonPressed const& e) { emit buttonPressed(e.button); },
                   [this](KnobPushed const& e) { emit knobPushed(e.knob); },
                   [this](KnobTouched const& e) { emit knobTouched(e.knob, e.touch); },
                   [this](KnobVolumeChanged const& e) {
                       // Turning a knob has to be applied by the host: the deck
                       // reports the turn as a relative counter and changes
                       // nothing itself. Without this the knobs move, the numbers
                       // move, and the audio does not.
                       //
                       // It belongs here rather than in QML because three
                       // MixerViews exist at once -- the panel, the desktop
                       // mixer, and the desktop's preview of the panel -- and
                       // each would issue its own write.
                       setLevel(selectedMix(), static_cast<int>(indexOf(e.knob)), e.volume);
                       emit knobVolumeChanged(e.knob, e.volume);
                   },
                   [this](ScreenTouched const& e) { emit screenTouched(e.x, e.y, e.phase); },
                   [this](AudioMetersChanged const& e) {
                       QList<int> levels;
                       levels.reserve(static_cast<qsizetype>(e.levels.size()));
                       for (auto const level: e.levels)
                           levels.append(level);
                       emit audioMetersChanged(levels);
                   },
                   [this](ConnectionChanged const& e) { emit connectionStateChanged(e.state); },
               },
               event);
}

} // namespace ax310::app
