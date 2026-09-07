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

    /// Leading bytes of a report to log.
    ///
    /// Far enough to carry the audio meters, which run from byte 0x12 to 0x29 --
    /// sixteen stopped two bytes short of them, so a hex dump of a report could
    /// never answer a question about a meter and a hardware probe was needed to
    /// see what the deck was actually sending.
    constexpr std::size_t LoggedPrefixBytes = protocol::AudioMetersOffset
                                              + (protocol::AudioMeterCount * protocol::AudioMeterStride);

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

    // The handshake carries no microphone chain, so the deck keeps whatever the
    // last program to touch it configured -- which on a deck the vendor software
    // has run is that software's settings. Applying a state of our own is what
    // makes the chain start where this project puts it.
    applyEffectState();

    adoptTheDecksOwnState();

    // Last, and after the level reads that precede it rather than before them:
    // choosing a mix writes a knob-ring record to the same address these use, and
    // a run of records to one address is what the deck applies only some of.
    //
    // The captured sequence lights two of the four buttons and leaves the other
    // two dark: it writes those two a blue that the byte pair it carries does not
    // show. It also imposes whichever colours the person whose session was
    // captured had chosen, which is not a default this project should ship. Four
    // of our own go on afterwards, so the deck comes up with every button lit and
    // each one tellable from its neighbours.
    lightButtonsWithDefaults();

    _isSeeded = false;

    // Re-armed, so this session publishes a meter reading of its own. The
    // comparison below only forwards a set that differs from the last one, and
    // nothing else resets it -- so a driver that had already seen these six
    // percentages, in this process or before a replug, would show the reading it
    // inherited and never correct it.
    _audioMeters.fill(NoAudioMeterYet);

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

    // Paced the same as the handshake. Sent back to back, some of these do not
    // take: the sequence darkens the four buttons with four records to the same
    // address one after another, and a deck shut down that way is left with one
    // of them still lit -- a different one on different runs. The handshake
    // already carries a gap for the same reason, and a shutdown is not a place
    // where a tenth of a second matters.
    if (auto const sent = sendSequence(commands::ShutdownPayloads, CommandGap); !sent)
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
    payload[3] = std::to_underlying(command);
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
    // changes and leaves a level drag unbracketed. Spaced all the same, because
    // a dial is a run of writes and the deck does not take those back to back.
    //
    // The whole sequence is under one lock: two of these arriving together --
    // a hand on a knob and a pointer on a fader -- would otherwise measure the
    // same gap, wait the same time, and then write together anyway.
    std::lock_guard<std::mutex> const pacing { _paceMutex };

    if (auto const written = writeLevelSpaced(protocol::levelAddressOf(block, knob), values);
        !written)
        return written;

    // Recorded as soon as the level register has taken it, and before the write
    // below that may not: the deck is holding this level now, and a cache saying
    // otherwise is what the next relative turn would compute from.
    {
        std::lock_guard<std::mutex> const lock { _stateMutex };
        _levels[indexOf(mix)][indexOf(knob)] = level;
    }

    // The microphone takes two writes where every other track takes one. The
    // vendor's captures show 0x35 carrying the same value alongside the level
    // whenever the Mic slider moves, and nothing else in the block behaves that
    // way. Only the creator mix: 0x35 is a single register and the pairing has
    // only ever been seen against 0x27.
    if (knob == KnobId::Mic && mix == MixId::Creator)
        return writeLevelSpaced(std::to_underlying(protocol::Property::KnobPropertyAt35), values);

    return {};
}

std::optional<Level> Device::level(MixId mix, KnobId knob) const
{
    std::lock_guard<std::mutex> const lock { _stateMutex };
    return _levels[indexOf(mix)][indexOf(knob)];
}

std::expected<void, DeviceError> Device::writeLevelSpaced(std::uint8_t address,
                                                          std::span<std::uint8_t const> values)
{
    // The wait falls on whichever thread asked, and that includes the one
    // drawing: a fader drag reaches here too. It is bounded by CommandGap and
    // only happens above a hundred writes a second, which a pointer moving at
    // display rate does not reach -- so in practice a drag waits for nothing and
    // a hand spinning a knob is what pays. Taking the write off this thread
    // altogether wants a queue between the interface and the deck, which is a
    // larger change than the defect being fixed here.
    if (_lastLevelWrite)
    {
        auto const since =
            std::chrono::duration_cast<std::chrono::milliseconds>(_clock.now() - *_lastLevelWrite);
        if (since < CommandGap)
            _clock.sleepFor(CommandGap - since);
    }

    auto const written = writeProperty(address, values);

    // Stamped after the transfer rather than before it, so the gap the deck sees
    // is the one between writes landing, not between them being started.
    _lastLevelWrite = _clock.now();
    return written;
}

std::expected<void, DeviceError> Device::readLevels()
{
    std::expected<void, DeviceError> outcome {};

    for (auto const mix: AllMixes)
    {
        auto const block = mix == MixId::Creator ? protocol::Property::CreatorMixLevels
                                                 : protocol::Property::AudienceMixLevels;

        for (auto const knob: AllKnobs)
        {
            std::array<std::uint8_t, 1> values {};
            if (auto const read = readProperty(protocol::levelAddressOf(block, knob), values); !read)
            {
                // Reported once, and the rest are still attempted: a deck that
                // answers eleven of twelve is better cached from eleven than
                // abandoned at the first refusal.
                if (outcome)
                    outcome = std::unexpected(read.error());
                continue;
            }

            std::lock_guard<std::mutex> const lock { _stateMutex };
            _levels[indexOf(mix)][indexOf(knob)] = Level::fromSteps(values[0]);
        }
    }

    return outcome;
}

std::expected<void, DeviceError> Device::selectMix(MixId mix)
{
    std::array<std::uint8_t, 1> const open { 0x01 };
    std::array<std::uint8_t, 1> const chosen { static_cast<std::uint8_t>(indexOf(mix)) };
    std::array<std::uint8_t, 1> const close { 0x00 };
    std::array<std::uint8_t, 1> const rings { protocol::KnobLedSelectForMix[indexOf(mix)] };

    auto const fence = static_cast<std::uint8_t>(protocol::Property::SettingsTransaction);
    auto const selector = static_cast<std::uint8_t>(protocol::Property::SelectedMix);
    auto const ringSelect = static_cast<std::uint8_t>(protocol::Property::KnobLedSelect);

    // SelectedMix alone moves the audio and leaves the knob rings behind, showing
    // the mix that was on before with the colour it had. Two other writes are what
    // move them, and all three go inside one fence:
    //
    //   * KnobLedSelect, which decides the levels the rings display;
    //   * the ring colour, because the record carries no mix -- the deck applies
    //     it to whichever mix is selected, so the new mix's colour has to be said.
    //
    // The colour goes after the switch, which is not the order the vendor's own
    // capture has -- it writes the colour first and its rings follow. The reason
    // for differing is that a record carries no mix, so the deck may apply it to
    // whichever one is selected when it arrives; the capture says that cannot be
    // the whole story.
    //
    // The record is spaced from what surrounds it. Sent with no gap on either
    // side, it did not land: a cold deck came up on the audience mix still
    // wearing the handshake's creator blue.
    //
    // Which of the two changes here fixes that is not settled. The vendor's own
    // capture writes the colour four commands *ahead* of the switch and it works,
    // which argues against the order being what matters -- but it also leaves 289
    // ms in front of that record and 126 ms behind it, where this sent it with
    // none. See hardware-facts.md; a deck settles it, and this is the guess until
    // one does.
    auto const& colour = protocol::MixRingColours[indexOf(mix)];
    auto const record = protocol::knobColourRecord(colour.red, colour.green, colour.blue);

    // A gap is a step of the sequence like the writes are, so it reads as one.
    auto const pace = [this]() -> std::expected<void, DeviceError> {
        _clock.sleepFor(CommandGap);
        return {};
    };

    return writeProperty(fence, open)
        .and_then([&] { return writeProperty(ringSelect, rings); })
        .and_then([&] { return writeProperty(selector, chosen); })
        .and_then(pace)
        .and_then([&] { return writeProperty(protocol::ButtonColourAddress, record); })
        .and_then(pace)
        .and_then([&] { return writeProperty(fence, close); })
        .transform([&] {
            std::lock_guard<std::mutex> const lock { _stateMutex };
            _mix = mix;
        });
}

MixId Device::selectedMix() const
{
    std::lock_guard<std::mutex> const lock { _stateMutex };
    return _mix;
}

std::expected<MixId, DeviceError> Device::readSelectedMix()
{
    // Asked for again rather than taken from the snapshot connect() holds. That
    // one was read before the handshake; this is read after the restore, so it
    // is what the deck ended up on rather than what it started on -- and a
    // restore that did not take is exactly the case worth catching.
    std::array<std::uint8_t, 1> values {};
    return readProperty(std::to_underlying(protocol::Property::SelectedMix), values)
        .and_then([&values]() -> std::expected<MixId, DeviceError> {
            // Anything outside the two the register is documented to carry is a
            // reply this driver cannot act on, and guessing a mix would put the
            // rings and the audio somewhere nobody asked for.
            if (values[0] >= MixCount)
                return std::unexpected(DeviceError::ReadFailed);

            return static_cast<MixId>(values[0]);
        });
}

std::expected<void, DeviceError> Device::setParameter(protocol::Parameter parameter, int value)
{
    auto const& info = protocol::describe(parameter);
    auto const slot = protocol::framedDefaultIndex(info.command);
    if (!slot)
        return std::unexpected(DeviceError::WriteFailed);

    auto& entry = _framedBodies[*slot];
    auto const clamped = std::clamp(value, info.minimum, info.maximum);
    _parameterChosen[indexOf(parameter)] = true;

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

std::expected<void, DeviceError> Device::setButtonColour(Button button, std::uint8_t red,
                                                         std::uint8_t green, std::uint8_t blue)
{
    if (!_transport.isOpen())
        return std::unexpected(DeviceError::NotConnected);

    auto const record =
        protocol::buttonColourRecord(protocol::selectorFor(button), red, green, blue, true);
    auto const framed =
        protocol::frameFeatureReport(protocol::setPropertyAt(protocol::ButtonColourAddress, record));

    std::lock_guard<std::mutex> const lock { _writeMutex };

    // Checked inside the lock, for the reason setKnobLedBrightness gives.
    if (!_transport.isOpen())
        return std::unexpected(DeviceError::NotConnected);

    return _transport.sendFeatureReport(framed);
}

void Device::setEffectState(EffectState const& state)
{
    _effectState = state;
}

EffectState Device::effectState() const
{
    EffectState state;
    state.enabled = _effectState.enabled;

    // Only what somebody chose. Reporting every parameter would report the
    // defaults the cache starts from, and those defaults are captured vendor
    // payloads: stored and handed back on the next connect they put the whole
    // microphone chain back on the deck.
    for (auto const& info: protocol::Parameters)
        if (_parameterChosen[indexOf(info.id)])
            state.parameters[indexOf(info.id)] = parameter(info.id);

    return state;
}

std::expected<void, DeviceError> Device::setEffectEnabled(protocol::FramedCommand command,
                                                          bool enabled)
{
    // The position rather than the iterator, and never named as one. A
    // std::array iterator is a raw pointer in libstdc++ and a class type in
    // MSVC's library, so a variable holding it is either `auto const*` -- which
    // MSVC cannot deduce -- or `auto`, which clang-tidy's readability-qualified-
    // auto rejects on libstdc++. An index is neither, and it is what the line
    // below wanted anyway. Past the end means the command is not one of these.
    auto const index = static_cast<std::size_t>(std::distance(
        protocol::EffectEnables.begin(), std::ranges::find(protocol::EffectEnables, command)));

    if (index >= protocol::EffectEnables.size())
        return std::unexpected(DeviceError::WriteFailed);

    std::array<std::uint8_t, 1> const value { static_cast<std::uint8_t>(enabled ? 0x01 : 0x00) };
    auto const sent = sendFramed(command, value);
    if (sent)
        _effectState.enabled[index] = enabled;

    return sent;
}

void Device::applyEffectState()
{
    // Parameters first, enables last, and the order is load-bearing. A parameter
    // write sends its whole framed body, and the delay effect's body carries the
    // byte that says which effect is running -- so writing a parameter after an
    // enable can switch back on what the enable just switched off. The enables
    // have the final word this way round.
    for (auto const& info: protocol::Parameters)
    {
        auto const wanted = _effectState.parameters[indexOf(info.id)];
        if (!wanted)
            continue;

        if (auto const sent = setParameter(info.id, *wanted); !sent)
            logTo(_logger,
                  LogLevel::Warning,
                  "could not restore {}: {}",
                  info.name,
                  describe(sent.error()));
    }

    for (std::size_t index = 0; index < protocol::EffectEnables.size(); ++index)
    {
        auto const command = protocol::EffectEnables[index];
        std::array<std::uint8_t, 1> const value {
            static_cast<std::uint8_t>(_effectState.enabled[index] ? 0x01 : 0x00)
        };

        if (auto const sent = sendFramed(command, value); !sent)
            logTo(_logger,
                  LogLevel::Warning,
                  "could not set {}: {}",
                  protocol::nameIn(protocol::FramedCommandNames, std::to_underlying(command)),
                  describe(sent.error()));
    }
}

void Device::adoptTheDecksOwnState()
{
    // The restore above puts the deck back on whichever mix it was monitoring
    // before the handshake, so which mix that is has to be asked rather than
    // assumed -- and the ring colour cannot be restored with it, because it lives
    // in a 0xc0 record rather than a property and the handshake has just written
    // creator blue over it. Selecting the mix the deck came back on writes that
    // colour, so the rings, the audio and whatever draws this agree again.
    auto const asked = readSelectedMix();
    if (!asked)
        logTo(_logger, LogLevel::Warning, "could not read the selected mix: {}", describe(asked.error()));

    // Chosen even when the read did not answer, rather than left. The restore
    // above may have put the deck on the audience mix, and a driver holding
    // Creator against that writes the block the deck is neither monitoring nor
    // displaying -- audio moving in a mix nobody hears. Settling it either way
    // is what keeps the deck and this cache saying the same thing.
    auto const wanted = asked.value_or(MixId::Creator);
    if (auto const chosen = selectMix(wanted); !chosen)
        logTo(_logger,
              LogLevel::Warning,
              "could not select the {} mix: {}",
              nameOf(wanted),
              describe(chosen.error()));

    // Twelve round trips, one per address. The blocks can be read whole -- the
    // vendor does, and PreservedAddresses already reads 0x27 with length seven --
    // but which of the seven bytes carries which track has never been checked
    // against the hardware, and getting that wrong assigns every track the wrong
    // level without saying anything. Per address is unambiguous.
    //
    // Worth the trips either way: a relative knob turn needs an absolute level to
    // move, and this is the only place that level comes from the hardware.
    if (auto const read = readLevels(); !read)
        logTo(_logger, LogLevel::Warning, "could not read the levels: {}", describe(read.error()));
}

void Device::lightButtonsWithDefaults()
{
    for (auto const button: AllButtons)
    {
        // Spaced, because the deck applies only some of a run of records sent
        // back to back -- the same reason the shutdown sequence is paced. Without
        // this only the last of the four lights, and the other three stay dark.
        //
        // Before each rather than after, so the first is separated from the ring
        // record the mix selection has just sent to this same address, and the
        // last does not pay for a gap with nothing on the other side of it.
        _clock.sleepFor(CommandGap);

        auto const& colour = protocol::DefaultButtonColours[indexOf(button)];
        auto const level = protocol::DefaultButtonBrightnessPercent;
        auto const lit = setButtonColour(button,
                                         protocol::scaledChannel(colour.red, level),
                                         protocol::scaledChannel(colour.green, level),
                                         protocol::scaledChannel(colour.blue, level));
        if (!lit)
            logTo(_logger,
                  LogLevel::Warning,
                  "could not light the {} button: {}",
                  nameOf(button),
                  describe(lit.error()));
    }
}

std::expected<void, DeviceError> Device::sendScreen(std::span<std::uint8_t const> frame)
{
    // One frame at a time, for the whole frame. The chunks carry a sequence the
    // deck reassembles into one image and a marker saying which one ends it, so
    // two frames sent at once arrive as a single frame made of both -- and what
    // the panel then shows is a blend of two moments, or the older one kept
    // because the newer never completed. Held around the per-chunk lock below
    // rather than instead of it: that one also excludes the property writes, and
    // holding it for a whole frame would make a knob turn wait on the screen.
    std::lock_guard<std::mutex> const frameLock { _screenMutex };

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

        // The turn is relative, so it moves whatever the deck last said this
        // track was -- in the mix the deck is monitoring, because that is the one
        // the knob is operating. Reading the base from anywhere else is what made
        // a turn after a mix switch jump to a level nobody had set.
        //
        // Taken as a copy and the lock let go before the write below, which takes
        // it again to record what landed.
        MixId mix {};
        std::optional<Level> held;
        {
            std::lock_guard<std::mutex> const lock { _stateMutex };
            mix = _mix;
            held = _levels[indexOf(mix)][index];
        }

        // Nothing to be relative to. A read that failed leaves no level here, and
        // inventing one would write a number nobody chose onto a deck that may be
        // carrying a live call -- so the turn is refused and said out loud.
        if (!held)
        {
            logTo(_logger,
                  LogLevel::Warning,
                  "ignoring the {} knob: the deck has not said what its {} level is",
                  nameOf(knob),
                  nameOf(mix));
            continue;
        }

        auto const wanted = Level::fromPercent(held->asPercent() + (delta * Level::PercentPerStep));
        if (wanted == *held)
            continue;

        // Applied here rather than left to a listener: the deck reports the turn
        // and changes nothing itself, so a driver that only announces it leaves
        // the knobs moving, the numbers moving, and the audio where it was.
        if (auto const written = setLevel(mix, knob, wanted); !written)
        {
            logTo(_logger,
                  LogLevel::Warning,
                  "could not apply the {} knob's turn: {}",
                  nameOf(knob),
                  describe(written.error()));
            continue;
        }

        _listener.onDeviceEvent(
            KnobVolumeChanged { .mix = mix, .knob = knob, .volume = wanted.asPercent() });
    }
}

} // namespace ax310
