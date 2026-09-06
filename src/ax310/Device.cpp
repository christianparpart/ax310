// SPDX-License-Identifier: Apache-2.0
#include "Device.hpp"

#include "Commands.hpp"

#include <algorithm>
#include <ranges>
#include <string>
#include <utility>

namespace ax310
{

namespace
{

    /// Which transitions of a bitmask bit are worth an event.
    enum class EdgeTrigger : std::uint8_t
    {
        RisingOnly, ///< Only when the bit goes from clear to set.
        BothEdges   ///< Whenever the bit changes either way.
    };

    /// One per-knob bitmask in the input report, and what a change to it means.
    ///
    /// Push and touch differ only in which byte they read, which edge they care
    /// about, and which event they produce -- so they are two rows of a table
    /// rather than two near-identical functions. A third such bitmask would be a
    /// third row and no new code.
    struct KnobBitmaskField
    {
        std::uint8_t protocol::InputReport::* reportField; ///< Where to read it now.
        std::uint8_t Device::ReportState::* stateField;    ///< Where the previous value is kept.
        EdgeTrigger trigger;                               ///< Which transitions count.
        DeviceEvent (*makeEvent)(KnobId knob, protocol::BitState state); ///< What to emit.
    };

    constexpr std::array<KnobBitmaskField, 2> KnobBitmaskFields { {
        {
            .reportField = &protocol::InputReport::knobPush,
            .stateField = &Device::ReportState::knobPush,
            .trigger = EdgeTrigger::RisingOnly,
            .makeEvent = [](KnobId knob, protocol::BitState) -> DeviceEvent {
                return KnobPushed { .knob = knob };
            },
        },
        {
            .reportField = &protocol::InputReport::knobTouch,
            .stateField = &Device::ReportState::knobTouch,
            .trigger = EdgeTrigger::BothEdges,
            .makeEvent = [](KnobId knob, protocol::BitState state) -> DeviceEvent {
                return KnobTouched { .knob = knob,
                                     .touch = state == protocol::BitState::Set ? Touch::Touched
                                                                               : Touch::Released };
            },
        },
    } };

    /// Gap between the payloads of the initialisation handshake. The vendor
    /// software pauses between them and the deck does not come back without it.
    constexpr std::chrono::milliseconds CommandGap { 10 };

    /// Scale from the hardware's 0-20 knob range to the 0-100 the UI speaks.
    constexpr int VolumePercentPerStep = 5;

    /// Leading bytes of a report to log.
    constexpr std::size_t LoggedPrefixBytes = 16;

    /// Strips the report-id byte some backends prepend and rejects sizes we
    /// cannot interpret, rather than decoding whatever arrived.
    ///
    /// @param raw The bytes the transport produced.
    /// @return The report body, or an empty span if the size is unrecognised.
    /// @param report The report just read.
    /// @param last What the previous report carried.
    /// @return Whether anything the driver decodes has changed.
    ///
    /// The deck streams its audio meters continuously -- six stereo pairs from
    /// byte 0x12 -- so the great majority of reports carry nothing else this
    /// driver reads. Logging them all buries the handful that matter, and they
    /// look identical in a dump of the first sixteen bytes, which is the
    /// confusing part.
    [[nodiscard]] bool carriesInput(protocol::InputReport const& report, Device::ReportState const& last)
    {
        if (report.isScreenTouch)
            return true;

        auto const buttons = report.buttons;
        if (buttons != (last.buttons & protocol::PhysicalButtonMask))
            return true;

        if (report.knobPush != last.knobPush || report.knobTouch != last.knobTouch)
            return true;

        return !std::ranges::equal(report.knobValues, last.knobValues);
    }

    /// @param report The report to render.
    /// @return The leading bytes of @p report as hex, for a log line.
    [[nodiscard]] std::string toHex(std::span<std::uint8_t const> report)
    {
        std::string text;
        for (auto const byte: report | std::views::take(LoggedPrefixBytes))
            text += std::format("{:02x} ", byte);
        return text;
    }

} // namespace

Device::Device(IHidTransport& transport, IClock& clock, ILogger& logger, IDeviceListener& listener) noexcept:
    _transport { transport }, _clock { clock }, _logger { logger }, _listener { listener }
{
}

Device::~Device()
{
    disconnect();
}

ConnectionState Device::connectionState() const noexcept
{
    return _state;
}

bool Device::isConnected() const noexcept
{
    return _state == ConnectionState::Connected;
}

void Device::publishState(ConnectionState state)
{
    if (_state == state)
        return;

    _state = state;
    _listener.onDeviceEvent(ConnectionChanged { .state = state });
}

std::expected<void, DeviceError> Device::openInterface(DeviceMode mode)
{
    auto const descriptor = protocol::descriptorFor(mode);
    auto const interfaces = _transport.enumerate(protocol::VendorId, descriptor.productId);

    if (interfaces.empty())
        return std::unexpected(DeviceError::DeviceNotFound);

    for (auto const& candidate: interfaces)
    {
        logTo(_logger,
              LogLevel::Debug,
              "AX310 interface {} (usage page 0x{:x}, usage 0x{:x}) at {}",
              candidate.interfaceNumber,
              candidate.usagePage,
              candidate.usage,
              candidate.path);

        // -1 means the backend does not report interface numbers (macOS,
        // Windows); take the first one it offers in that case.
        if (candidate.interfaceNumber != descriptor.interfaceNumber && candidate.interfaceNumber != -1)
            continue;

        if (auto const opened = _transport.open(candidate.path); opened)
        {
            logTo(_logger, LogLevel::Info, "Opened AX310 at {}", candidate.path);
            return {};
        }
    }

    return std::unexpected(DeviceError::OpenFailed);
}

std::expected<ConnectionState, DeviceError> Device::connect()
{
    if (_state == ConnectionState::Connected)
        return ConnectionState::Connected;

    if (auto const started = _transport.initialize(); !started)
        return std::unexpected(started.error());

    // The deck is always the Control device. Base is a second USB device of the
    // same physical unit carrying the audio side's media keys, and there is no
    // re-enumeration between them -- both are on the bus at once.
    if (auto const opened = openInterface(DeviceMode::Control); !opened)
        return std::unexpected(opened.error());

    // Read first, because what comes next overwrites it.
    //
    // The captured sequence is somebody's saved configuration rather than an
    // initialisation, and replaying it sets every knob level to 50% and rewrites
    // a dozen registers besides. That was observed mid-call, where it moved the
    // microphone level the far end was hearing. The vendor's software replays the
    // same bytes and then puts the user's settings back; this is that second
    // half.
    //
    // It is a partial fix and the limits are worth stating. It covers the
    // property registers only: the DSP chain the sequence also configures --
    // equaliser, and whichever of the four enables is the reverb -- has no
    // read-back we know of, so an effect the sequence switches on stays on. And a
    // deck that answers nothing yet simply leaves the snapshot empty.
    auto const snapshot = snapshotProperties();

    // The deck comes up asleep: dark screen, no report stream at all. The
    // initialisation sequence is what wakes it, and it has to be sent on every
    // attach -- there is no mode to detect, because an uninitialised deck looks
    // exactly like an initialised one until you wait for a report that never
    // arrives.
    logTo(_logger, LogLevel::Info, "Initialising the AX310");
    if (auto const sent = sendSequence(commands::InitPayloads, CommandGap); !sent)
    {
        _transport.close();
        return std::unexpected(sent.error());
    }

    restoreProperties(snapshot);

    _isSeeded = false;
    publishState(ConnectionState::Connected);
    return ConnectionState::Connected;
}

void Device::disconnect()
{
    if (_state != ConnectionState::Connected)
    {
        _transport.close();
        return;
    }

    logTo(_logger, LogLevel::Info, "Shutting the AX310 down");
    if (auto const sent = sendSequence(commands::ShutdownPayloads, std::chrono::milliseconds { 0 }); !sent)
        logTo(_logger, LogLevel::Warning, "Shutdown sequence failed: {}", describe(sent.error()));

    {
        // Closing under the write lock is what stops a frame already inside
        // sendScreen from writing to a descriptor this thread just closed. It
        // showed as a burst of "Bad file descriptor" at every shutdown: the
        // render timer hands frames to a thread pool, and those threads outlive
        // the decision to disconnect.
        std::lock_guard<std::mutex> const lock { _writeMutex };
        _state = ConnectionState::Disconnected;
        _transport.close();
    }

    _listener.onDeviceEvent(ConnectionChanged { .state = ConnectionState::Disconnected });
}

std::expected<void, DeviceError> Device::sendSequence(std::span<protocol::Payload const> sequence,
                                                      std::chrono::milliseconds gap)
{
    for (auto const& payload: sequence)
    {
        auto const framed = protocol::frameFeatureReport(payload);
        {
            std::lock_guard<std::mutex> const lock { _writeMutex };
            if (auto const sent = _transport.sendFeatureReport(framed); !sent)
                return std::unexpected(sent.error());
        }

        if (gap.count() > 0)
            _clock.sleepFor(gap);
    }

    return {};
}

std::expected<void, DeviceError> Device::readProperty(std::uint8_t address, std::span<std::uint8_t> values)
{
    auto const request = protocol::frameFeatureReport(protocol::getPropertyAt(address, values.size()));

    protocol::FeatureReport reply {};
    {
        std::lock_guard<std::mutex> const lock { _writeMutex };
        if (!_transport.isOpen())
            return std::unexpected(DeviceError::NotConnected);

        if (auto const sent = _transport.sendFeatureReport(request); !sent)
            return std::unexpected(sent.error());

        auto const read = _transport.getFeatureReport(reply);
        if (!read)
            return std::unexpected(read.error());

        // The answer is whatever the deck left in its reply buffer, so a short
        // read is not an answer at all.
        if (*read < protocol::ReplyValuesOffset + values.size())
            return std::unexpected(DeviceError::ReadFailed);
    }

    auto const answered = protocol::propertyReplyValues(reply, address);
    if (!answered || answered->size() < values.size())
        return std::unexpected(DeviceError::ReadFailed);

    std::ranges::copy_n(answered->begin(), static_cast<std::ptrdiff_t>(values.size()), values.begin());
    return {};
}

Device::PropertySnapshot Device::snapshotProperties()
{
    PropertySnapshot snapshot {};

    for (std::size_t index = 0; index < protocol::PreservedAddresses.size(); ++index)
    {
        auto const& preserved = protocol::PreservedAddresses[index];
        auto const values = std::span { snapshot.values[index] }.first(preserved.length);

        if (auto const read = readProperty(preserved.address, values); read)
            snapshot.isValid[index] = true;
        else
            logTo(_logger,
                  LogLevel::Debug,
                  "could not read 0x{:02x} before initialising: {}",
                  preserved.address,
                  describe(read.error()));
    }

    return snapshot;
}

void Device::restoreProperties(PropertySnapshot const& snapshot)
{
    std::size_t restored = 0;

    for (std::size_t index = 0; index < protocol::PreservedAddresses.size(); ++index)
    {
        if (!snapshot.isValid[index])
            continue;

        auto const& preserved = protocol::PreservedAddresses[index];
        auto const values = std::span { snapshot.values[index] }.first(preserved.length);
        auto const framed =
            protocol::frameFeatureReport(protocol::setPropertyAt(preserved.address, values));

        {
            std::lock_guard<std::mutex> const lock { _writeMutex };
            if (!_transport.isOpen())
                return;

            if (auto const sent = _transport.sendFeatureReport(framed); !sent)
            {
                logTo(_logger,
                      LogLevel::Warning,
                      "could not restore 0x{:02x}: {}",
                      preserved.address,
                      describe(sent.error()));
                continue;
            }
        }

        ++restored;
        _clock.sleepFor(CommandGap);
    }

    logTo(_logger, LogLevel::Info, "restored {} of the settings the handshake overwrote", restored);
}

std::expected<void, DeviceError> Device::writeProperty(std::uint8_t address,
                                                         std::span<std::uint8_t const> values)
{
    // Refused before anything is built. Writing 0x01 to 0x16 once wedged this
    // deck into needing a power cycle, and the table has been carried since
    // without anything able to consult it -- because until now nothing could
    // write an arbitrary address. This is that path.
    if (std::ranges::contains(protocol::DangerousAddresses, address))
    {
        logTo(_logger,
              LogLevel::Error,
              "refusing to write 0x{:02x}: known to leave the deck needing a power cycle",
              address);
        return std::unexpected(DeviceError::WriteFailed);
    }

    auto const framed = protocol::frameFeatureReport(protocol::setPropertyAt(address, values));

    std::lock_guard<std::mutex> const lock { _writeMutex };
    if (!_transport.isOpen())
        return std::unexpected(DeviceError::NotConnected);

    return _transport.sendFeatureReport(framed);
}

std::expected<void, DeviceError> Device::sendFramed(protocol::FramedCommand command,
                                                    std::span<std::uint8_t const> body)
{
    protocol::Payload payload {};
    payload[0] = protocol::FramedCommandMarker;
    payload[1] = 0x00;
    payload[2] = static_cast<std::uint8_t>(body.size() + protocol::FramedOverhead);
    payload[3] = static_cast<std::uint8_t>(command);
    std::ranges::copy(body, std::next(payload.begin(), 4));
    payload[payload[2] - 1] = protocol::framedChecksum(payload);

    auto const framed = protocol::frameFeatureReport(payload);

    std::lock_guard<std::mutex> const lock { _writeMutex };
    if (!_transport.isOpen())
        return std::unexpected(DeviceError::NotConnected);

    return _transport.sendFeatureReport(framed);
}

std::expected<void, DeviceError> Device::setLevel(MixId mix, KnobId knob, Level level)
{
    auto const block =
        mix == MixId::Creator ? protocol::Property::CreatorMixLevels : protocol::Property::AudienceMixLevels;
    std::array<std::uint8_t, 1> const values { level.steps() };

    // Not fenced with SettingsTransaction, matching the vendor: it brackets mode
    // changes and leaves a level drag unbracketed.
    return writeProperty(protocol::levelAddressOf(block, knob), values);
}

std::expected<Level, DeviceError> Device::level(MixId mix, KnobId knob)
{
    auto const block =
        mix == MixId::Creator ? protocol::Property::CreatorMixLevels : protocol::Property::AudienceMixLevels;

    std::array<std::uint8_t, 1> values {};
    return readProperty(protocol::levelAddressOf(block, knob), values).transform([&values] {
        return Level::fromSteps(values[0]);
    });
}

std::expected<void, DeviceError> Device::selectMix(MixId mix)
{
    std::array<std::uint8_t, 1> const open { 0x01 };
    std::array<std::uint8_t, 1> const chosen { static_cast<std::uint8_t>(indexOf(mix)) };
    std::array<std::uint8_t, 1> const close { 0x00 };

    auto const fence = static_cast<std::uint8_t>(protocol::Property::SettingsTransaction);
    auto const selector = static_cast<std::uint8_t>(protocol::Property::SelectedMix);

    return writeProperty(fence, open)
        .and_then([&] { return writeProperty(selector, chosen); })
        .and_then([&] { return writeProperty(fence, close); });
}

std::expected<void, DeviceError> Device::setParameter(protocol::Parameter parameter, int value)
{
    auto const& info = protocol::describe(parameter);
    auto const slot = protocol::framedDefaultIndex(info.command);
    if (!slot)
        return std::unexpected(DeviceError::WriteFailed);

    auto& entry = _framedBodies[*slot];
    auto const clamped = std::clamp(value, info.minimum, info.maximum);

    switch (info.encoding)
    {
        case protocol::ParameterEncoding::UnsignedByte:
            entry.body[info.bodyOffset] = static_cast<std::uint8_t>(clamped);
            break;
        case protocol::ParameterEncoding::SignedByte:
            entry.body[info.bodyOffset] = static_cast<std::uint8_t>(static_cast<std::int8_t>(clamped));
            break;
        case protocol::ParameterEncoding::UnsignedWordLE:
            entry.body[info.bodyOffset] = static_cast<std::uint8_t>(clamped & 0xFF);
            entry.body[info.bodyOffset + 1] = static_cast<std::uint8_t>((clamped >> 8) & 0xFF);
            break;
    }

    return sendFramed(info.command, std::span { entry.body }.first(entry.length));
}

int Device::parameter(protocol::Parameter parameter) const noexcept
{
    auto const& info = protocol::describe(parameter);
    auto const slot = protocol::framedDefaultIndex(info.command);
    if (!slot)
        return info.minimum;

    auto const& body = _framedBodies[*slot].body;
    switch (info.encoding)
    {
        case protocol::ParameterEncoding::UnsignedByte: return body[info.bodyOffset];
        case protocol::ParameterEncoding::SignedByte:
            return static_cast<std::int8_t>(body[info.bodyOffset]);
        case protocol::ParameterEncoding::UnsignedWordLE:
            return body[info.bodyOffset] | (body[info.bodyOffset + 1] << 8);
    }

    return info.minimum;
}

std::expected<void, DeviceError> Device::setScreenBrightness(int level)
{
    if (!_transport.isOpen())
        return std::unexpected(DeviceError::NotConnected);

    // Clamped to what the vendor's own slider will send. Values below the
    // minimum have never been observed -- the vendor stops at 25 and offers a
    // separate off widget -- and finding out what 1% does belongs on a deck
    // somebody is willing to lose, not here.
    auto const percent = std::clamp(level, protocol::MinPanelBrightness, protocol::MaxPanelBrightness);
    if (percent != level)
        logTo(_logger, LogLevel::Debug, "screen brightness {} clamped to {}", level, percent);

    // Not a property write. The display group is its own family, which is why
    // this went unfound among the property addresses for so long.
    auto const framed = protocol::frameFeatureReport(protocol::displayCommand(static_cast<std::uint8_t>(percent)));

    std::lock_guard<std::mutex> const lock { _writeMutex };

    // Checked inside the lock, for the reason setKnobLedBrightness gives.
    if (!_transport.isOpen())
        return std::unexpected(DeviceError::NotConnected);

    return _transport.sendFeatureReport(framed);
}

std::expected<void, DeviceError> Device::blankScreen()
{
    if (!_transport.isOpen())
        return std::unexpected(DeviceError::NotConnected);

    // Off is a sentinel in the same byte, not a brightness of zero. There is no
    // matching wake: the deck comes back on its own when the glass is touched,
    // and the capture of it doing so carries no host-to-device traffic at all.
    auto const framed = protocol::frameFeatureReport(protocol::displayCommand(protocol::PanelOffLevel));

    std::lock_guard<std::mutex> const lock { _writeMutex };

    // Checked inside the lock, for the reason setKnobLedBrightness gives.
    if (!_transport.isOpen())
        return std::unexpected(DeviceError::NotConnected);

    return _transport.sendFeatureReport(framed);
}

std::expected<void, DeviceError> Device::setKnobLedBrightness(int percent)
{
    if (!_transport.isOpen())
        return std::unexpected(DeviceError::NotConnected);

    // Confirmed on the hardware: 0x01 extinguishes the rings and the startup
    // value lights them. The top of the range is the vendor's startup value
    // because the true ceiling has not been established -- the deck also took
    // 0x14 without complaint.
    auto const clamped = std::clamp(percent, 0, 100);
    auto const scaled = static_cast<std::uint8_t>(clamped * protocol::KnobLedBrightnessAtStartup / 100);
    std::array<std::uint8_t, 1> const values { scaled };

    auto const framed =
        protocol::frameFeatureReport(protocol::setProperty(protocol::Property::KnobLedBrightness, values));

    std::lock_guard<std::mutex> const lock { _writeMutex };

    // Checked inside the lock: disconnect() closes the transport while holding
    // it, so testing beforehand only narrows the window rather than closing it.
    if (!_transport.isOpen())
        return std::unexpected(DeviceError::NotConnected);

    return _transport.sendFeatureReport(framed);
}

std::expected<void, DeviceError> Device::sendScreen(std::span<std::uint8_t const> frame)
{
    std::uint8_t sequence = 0;

    // The deck needs to be told which chunk ends the frame, so the count has to
    // be known before the loop rather than discovered when it runs out.
    auto const chunkCount =
        (frame.size() + protocol::ScreenChunkPayloadSize - 1) / protocol::ScreenChunkPayloadSize;
    std::size_t chunkIndex = 0;

    for (auto const chunk: frame | std::views::chunk(protocol::ScreenChunkPayloadSize))
    {
        ++chunkIndex;
        // Byte 0 is the Report ID, 0x00 for a device without report ids; the
        // transport strips it before the packet reaches the USB bus.
        std::array<std::uint8_t, protocol::ScreenChunkSize> packet {};
        auto const length = static_cast<std::uint16_t>(chunk.size());

        packet[protocol::ScreenChunkSequenceOffset] = sequence;

        // Without this the deck takes every chunk, complains about nothing, and
        // never displays the frame.
        if (chunkIndex == chunkCount)
            for (std::size_t byte = 0; byte < protocol::ScreenChunkFinalByteCount; ++byte)
                packet[protocol::ScreenChunkFinalOffset + byte] = protocol::ScreenChunkFinalMarker;

        packet[protocol::ScreenChunkLengthOffset] = static_cast<std::uint8_t>(length & 0xFF);
        packet[protocol::ScreenChunkLengthOffset + 1] = static_cast<std::uint8_t>(length >> 8);

        std::uint16_t checksum = 0;
        for (auto const byte: chunk)
            checksum = static_cast<std::uint16_t>(checksum + byte);

        packet[protocol::ScreenChunkChecksumOffset] = static_cast<std::uint8_t>(checksum & 0xFF);
        packet[protocol::ScreenChunkChecksumOffset + 1] = static_cast<std::uint8_t>(checksum >> 8);
        std::ranges::copy(chunk, std::next(packet.begin(), protocol::ScreenChunkHeaderSize));

        std::lock_guard<std::mutex> const lock { _writeMutex };

        // Checked inside the lock, on every chunk: disconnect() closes the
        // transport while holding this same lock, and it can land between two
        // chunks of one frame. Testing before taking the lock would only narrow
        // the window rather than close it.
        if (!_transport.isOpen())
            return std::unexpected(DeviceError::NotConnected);

        if (auto const written = _transport.write(packet); !written)
        {
            // Stop rather than stalling the queue with the rest of the frame.
            logTo(_logger, LogLevel::Warning, "Screen chunk {} failed: {}", sequence, _transport.lastError());
            return std::unexpected(written.error());
        }

        ++sequence;
    }

    return {};
}

std::expected<void, DeviceError> Device::poll(std::chrono::milliseconds timeout)
{
    if (_state != ConnectionState::Connected)
        return std::unexpected(DeviceError::NotConnected);

    std::array<std::uint8_t, protocol::PaddedReportSize + 1> buffer {};
    auto const bytesRead = _transport.read(buffer, timeout);
    if (!bytesRead)
        return std::unexpected(bytesRead.error());

    if (*bytesRead == 0)
        return {}; // Timeout; an idle deck says nothing.

    auto const payload = protocol::reportPayload(std::span { buffer }.first(*bytesRead));
    if (payload.empty())
    {
        logTo(_logger, LogLevel::Warning, "Unexpected HID report size: {} bytes", *bytesRead);
        return {};
    }

    // The deck interleaves an all-zero report between real ones. Decoding it
    // would report every held button released and every touched knob let go,
    // and then report them all again on the next real report.
    if (std::ranges::all_of(payload, [](std::uint8_t byte) { return byte == 0; }))
        return {};

    // Validate before trusting any of it: the deck checksums every report, so a
    // corrupt one is recognisable rather than decodable into plausible nonsense.
    if (!protocol::isChecksumValid(payload))
    {
        logTo(_logger,
              LogLevel::Warning,
              "report checksum {:#04x}, expected {:#04x} -- dropped",
              payload[protocol::ChecksumOffset],
              protocol::checksumOf(payload));
        return {};
    }

    // Decoded into a value type: nothing aliases the buffer, so there is no
    // layout to be wrong about and no lifetime to get wrong.
    auto const report = protocol::decodeReport(payload);

    // Logged before the dispatch below moves _last on, and only when something
    // the driver reads has actually changed.
    if (carriesInput(report, _last))
        logTo(_logger, LogLevel::Debug, "HID report: {}", toHex(payload));

    if (!_isSeeded)
    {
        // Adopt the current positions so the first report does not read as a
        // jump from zero.
        _last.knobPush = report.knobPush;
        _last.knobTouch = report.knobTouch;
        std::ranges::copy(report.knobValues, _last.knobValues.begin());
        _isSeeded = true;
    }

    dispatchEvent(report);
    dispatchKnobBitmasks(report);
    dispatchKnobValues(report);
    dispatchAudioMeters(report);

    return {};
}

void Device::dispatchEvent(protocol::InputReport const& report)
{
    if (report.isScreenTouch)
    {
        // A screen-touch report means a finger is on the glass. That is the whole
        // contact signal -- the flags byte is not it.
        auto const x = report.touchX;
        auto const y = report.touchY;

        if (_isTouchDown && x == _touchX && y == _touchY)
            // A held finger repeats about eleven times a second -- measured
            // twice, most recently as 228 reports from one motionless contact --
            // so without this a hold would be delivered as hundreds of identical
            // Moved events.
            return;

        auto const phase = _isTouchDown ? TouchPhase::Moved : TouchPhase::Pressed;
        _isTouchDown = true;
        _touchX = x;
        _touchY = y;

        // The flags byte goes in the log because it is still unexplained and this
        // is where anyone chasing it will look.
        logTo(_logger, LogLevel::Debug, "touch {},{} flags={:#04x}", x, y, report.touchFlags);
        _listener.onDeviceEvent(ScreenTouched { .x = x, .y = y, .phase = phase });
        return;
    }

    // Not a screen-touch report. If a finger was down it has just come off: the
    // deck stops sending touch reports rather than announcing the lift.
    if (_isTouchDown)
    {
        _isTouchDown = false;
        _listener.onDeviceEvent(ScreenTouched { .x = _touchX, .y = _touchY, .phase = TouchPhase::Released });
    }

    // Which bit belongs to which button is in protocol::ButtonBits; nothing here
    // handles a mask by hand.
    auto const buttons = report.buttons;
    if (buttons == _last.buttons)
        return;

    for (auto const button: AllButtons)
    {
        if (protocol::isDown(button, buttons) && !protocol::isDown(button, _last.buttons))
            _listener.onDeviceEvent(ButtonPressed { .button = button });
    }
    _last.buttons = buttons;
}

void Device::dispatchAudioMeters(protocol::InputReport const& report)
{
    std::array<int, protocol::AudioMeterCount> levels {};
    for (std::size_t channel = 0; channel < protocol::AudioMeterCount; ++channel)
        levels[channel] = protocol::toPercent(report.audioMeters[channel]);

    if (levels == _audioMeters)
        return;

    _audioMeters = levels;
    _listener.onDeviceEvent(AudioMetersChanged { .levels = levels });
}

void Device::dispatchKnobBitmasks(protocol::InputReport const& report)
{
    for (auto const& field: KnobBitmaskFields)
    {
        auto const current = report.*(field.reportField);
        auto& previous = _last.*(field.stateField);
        if (current == previous)
            continue;

        for (auto const knob: AllKnobs)
        {
            auto const now =
                protocol::isSet(knob, current) ? protocol::BitState::Set : protocol::BitState::Clear;
            auto const before =
                protocol::isSet(knob, previous) ? protocol::BitState::Set : protocol::BitState::Clear;

            if (now == before)
                continue;
            if (field.trigger == EdgeTrigger::RisingOnly && now != protocol::BitState::Set)
                continue;

            _listener.onDeviceEvent(field.makeEvent(knob, now));
        }
        previous = current;
    }
}

void Device::dispatchKnobValues(protocol::InputReport const& report)
{
    for (auto const knob: AllKnobs)
    {
        auto const index = indexOf(knob);
        auto const current = report.knobValues[index];
        auto const previous = std::exchange(_last.knobValues[index], current);

        if (current == previous)
            continue;

        // Only a touched knob is being turned. The counter also moves on knob
        // pushes and button presses -- observed going 0x0f -> 0x2f -> 0x4f across
        // a push and a button -- so an untouched change is absorbed into _last
        // above and reported to nobody. Absorbing it is what keeps the next real
        // turn from arriving as one enormous delta.
        if (!protocol::isSet(knob, report.knobTouch))
            continue;

        // A wrapping 8-bit counter, not an absolute position: knob 1 has been
        // seen reporting 0x4d while the ring has only 21 levels. Subtracting in
        // eight-bit width and reading the result as signed makes 0xff -> 0x00 one
        // step up rather than a fall of 255.
        auto const difference = static_cast<std::uint8_t>(current - previous);
        auto const delta = difference < 128 ? int { difference } : int { difference } - 256;
        if (delta == 0)
            continue;

        auto const volume = std::clamp(_volumes[index] + (delta * VolumePercentPerStep), 0, 100);
        if (volume == _volumes[index])
            continue;

        _volumes[index] = volume;
        _listener.onDeviceEvent(KnobVolumeChanged { .knob = knob, .volume = volume });
    }
}

} // namespace ax310
