// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "IHidTransport.hpp"
#include "Protocol.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <utility>

namespace ax310
{

/// An IHidTransport backed by a script instead of a USB bus.
///
/// Lives beside the interface it implements rather than in the test directory,
/// the same way NullLogger and NullDeviceListener do: it touches nothing outside
/// this directory, and a fake that is hard to reach is a fake that gets copied.
///
/// A case describes what the bus looks like -- which interfaces answer for which
/// product id, what reads produce, which call fails -- and afterwards reads back
/// what the driver sent.
class FakeHidTransport final: public IHidTransport
{
  public:
    /// One recorded outgoing report, tagged with how it was sent, because the
    /// driver's command path and its screen path use different calls.
    enum class Channel : std::uint8_t
    {
        FeatureReport,
        Write
    };

    /// One report the driver sent.
    struct Sent
    {
        Channel channel;
        std::vector<std::uint8_t> bytes;
    };

    // --- Scripting ---------------------------------------------------------

    /// Makes @p interfaces the answer to enumerate() for @p productId.
    /// @param productId The product id to answer for.
    /// @param interfaces What to report, in the order given.
    void presentDevice(std::uint16_t productId, std::vector<HidInterface> interfaces)
    {
        _byProduct[productId] = std::move(interfaces);
    }

    /// Queues one report for a later read() to return.
    /// @param report The bytes to hand over.
    void queueRead(std::vector<std::uint8_t> report) { _reads.push_back(std::move(report)); }

    /// Queues a read that reports a timeout: zero bytes, no error.
    void queueTimeout() { _reads.emplace_back(); }

    /// Queues one reply for a later getFeatureReport() to return.
    /// @param reply The bytes to hand over, report id included.
    void queueFeatureReport(std::vector<std::uint8_t> reply)
    {
        _featureReplies.push_back(std::move(reply));
    }

    /// Makes the next getFeatureReport() fail with @p error.
    void failGetFeatureReport(DeviceError error) { _getFeatureError = error; }

    /// Gives one register a value, so property reads answer from it.
    ///
    /// A queue of replies is fine for a case that makes one read, but the driver
    /// reads thirteen registers during connect before it reads anything a test
    /// cares about, and counting those out is both tedious and brittle. Holding
    /// registers instead models what the deck actually is, and reads land on the
    /// address the driver asked for rather than on whatever is next in line.
    ///
    /// @param address Which register.
    /// @param values Its contents.
    void setRegister(std::uint8_t address, std::vector<std::uint8_t> values)
    {
        _registers[address] = std::move(values);
    }

    /// Makes the next initialize() fail with @p error.
    void failInitialize(DeviceError error) { _initializeError = error; }

    /// Makes every open() fail with @p error.
    void failOpen(DeviceError error) { _openError = error; }

    /// Closes the transport once @p count reports have gone out, standing in for
    /// a disconnect that lands part-way through a frame.
    /// @param count How many sends succeed before the close.
    void closeAfterWrites(std::size_t count) { _closeAfter = count; }

    /// Makes the next read() fail with @p error, ahead of anything queued.
    void failRead(DeviceError error) { _readError = error; }

    /// Makes write() and sendFeatureReport() fail with @p error from the
    /// @p afterCount -th call onwards, so a case can let a transfer start and
    /// then break it partway.
    /// @param error What to report.
    /// @param afterCount How many sends succeed first.
    void failSendAfter(DeviceError error, std::size_t afterCount)
    {
        _sendError = error;
        _sendFailsAfter = afterCount;
    }

    // --- Readback ----------------------------------------------------------

    /// @return Every report the driver sent, in order.
    [[nodiscard]] std::vector<Sent> const& sent() const noexcept { return _sent; }

    /// @param channel The channel to count.
    /// @return How many reports went out on @p channel.
    [[nodiscard]] std::size_t sentCount(Channel channel) const
    {
        return static_cast<std::size_t>(
            std::ranges::count_if(_sent, [channel](Sent const& one) { return one.channel == channel; }));
    }

    /// Forgets everything sent so far.
    void clearSent() noexcept { _sent.clear(); }

    /// @return The path most recently passed to open().
    [[nodiscard]] std::string const& openedPath() const noexcept { return _openedPath; }

    /// @return Every (vendorId, productId) pair enumerate() was asked about, in
    ///         order, so a case can assert which personality was probed first.
    [[nodiscard]] std::vector<std::pair<std::uint16_t, std::uint16_t>> const& probes() const noexcept
    {
        return _probes;
    }

    /// @return How many feature-report reads the driver asked for.
    [[nodiscard]] std::size_t featureReadCount() const noexcept { return _featureReadCount; }

    /// @return How many times close() was called.
    [[nodiscard]] std::size_t closeCount() const noexcept { return _closeCount; }

    /// @return The timeout the last read() was given.
    [[nodiscard]] std::chrono::milliseconds lastReadTimeout() const noexcept { return _lastReadTimeout; }

    // --- IHidTransport -----------------------------------------------------

    std::expected<void, DeviceError> initialize() override
    {
        if (_initializeError)
            return std::unexpected(*_initializeError);

        _isInitialized = true;
        return {};
    }

    void shutdown() override { _isInitialized = false; }

    std::vector<HidInterface> enumerate(std::uint16_t vendorId, std::uint16_t productId) override
    {
        _probes.emplace_back(vendorId, productId);

        auto const found = _byProduct.find(productId);
        if (found == _byProduct.end())
            return {};

        return found->second;
    }

    std::expected<void, DeviceError> open(std::string_view path) override
    {
        if (_openError)
            return std::unexpected(*_openError);

        _openedPath = std::string { path };
        _isOpen = true;
        return {};
    }

    void close() override
    {
        ++_closeCount;
        _isOpen = false;
    }

    [[nodiscard]] bool isOpen() const override { return _isOpen; }

    std::expected<void, DeviceError> sendFeatureReport(std::span<std::uint8_t const> report) override
    {
        return record(Channel::FeatureReport, report);
    }

    std::expected<std::size_t, DeviceError> getFeatureReport(std::span<std::uint8_t> buffer) override
    {
        ++_featureReadCount;

        if (_getFeatureError)
        {
            auto const error = *_getFeatureError;
            _getFeatureError.reset();
            return std::unexpected(error);
        }

        // An explicitly queued reply always wins: a case that scripts one is
        // usually testing what happens when the deck answers oddly.
        if (_featureReplies.empty())
            return answerFromRegisters(buffer);

        auto const reply = std::move(_featureReplies.front());
        _featureReplies.pop_front();

        auto const length = std::min(reply.size(), buffer.size());
        std::ranges::copy_n(reply.begin(), static_cast<std::ptrdiff_t>(length), buffer.begin());
        return length;
    }

    std::expected<void, DeviceError> write(std::span<std::uint8_t const> report) override
    {
        return record(Channel::Write, report);
    }

    std::expected<std::size_t, DeviceError> read(std::span<std::uint8_t> buffer,
                                                 std::chrono::milliseconds timeout) override
    {
        _lastReadTimeout = timeout;

        if (_readError)
        {
            auto const error = *_readError;
            _readError.reset();
            return std::unexpected(error);
        }

        if (_reads.empty())
            return std::size_t { 0 }; // Nothing scripted reads as an idle deck.

        auto const report = std::move(_reads.front());
        _reads.pop_front();

        auto const length = std::min(report.size(), buffer.size());
        std::ranges::copy_n(report.begin(), static_cast<std::ptrdiff_t>(length), buffer.begin());
        return length;
    }

    [[nodiscard]] std::string lastError() const override { return "fake transport"; }

  private:
    /// Answers a property read from the register map, if the last thing sent was
    /// a read of an address the map holds.
    ///
    /// The reply is shaped exactly as the deck shapes one -- the command header
    /// echoed back, then the values -- because that echo is what the driver
    /// checks to tell a real answer from the previous reply still sitting in the
    /// buffer.
    ///
    /// @param buffer Where to put it.
    /// @return Bytes written, or zero when nothing matches.
    [[nodiscard]] std::size_t answerFromRegisters(std::span<std::uint8_t> buffer)
    {
        if (_sent.empty() || _registers.empty())
            return 0;

        auto const& last = _sent.back().bytes;
        if (last.size() < 5 || last[0] != 0x00
            || last[1] != static_cast<std::uint8_t>(protocol::CommandKind::Get)
            || last[2] != protocol::PropertyGroup)
            return 0;

        auto const found = _registers.find(last[3]);
        if (found == _registers.end())
            return 0;

        auto const length = std::min<std::size_t>(last[4], found->second.size());
        if (buffer.size() < protocol::ReplyValuesOffset + length)
            return 0;

        buffer[0] = 0x00;
        buffer[1] = static_cast<std::uint8_t>(protocol::CommandKind::Get);
        buffer[2] = protocol::PropertyGroup;
        buffer[3] = last[3];
        buffer[4] = static_cast<std::uint8_t>(length);
        std::ranges::copy_n(found->second.begin(),
                            static_cast<std::ptrdiff_t>(length),
                            std::next(buffer.begin(), protocol::ReplyValuesOffset));

        return protocol::ReplyValuesOffset + length;
    }

    std::expected<void, DeviceError> record(Channel channel, std::span<std::uint8_t const> report)
    {
        if (_sendError && _sent.size() >= _sendFailsAfter)
            return std::unexpected(*_sendError);

        _sent.push_back(Sent { .channel = channel, .bytes = { report.begin(), report.end() } });

        if (_closeAfter && _sent.size() >= *_closeAfter)
            _isOpen = false;

        return {};
    }

    std::map<std::uint16_t, std::vector<HidInterface>> _byProduct;
    std::deque<std::vector<std::uint8_t>> _reads;
    std::deque<std::vector<std::uint8_t>> _featureReplies;
    std::map<std::uint8_t, std::vector<std::uint8_t>> _registers;
    std::vector<Sent> _sent;
    std::vector<std::pair<std::uint16_t, std::uint16_t>> _probes;

    std::optional<DeviceError> _initializeError;
    std::optional<DeviceError> _openError;
    std::optional<DeviceError> _readError;
    std::optional<DeviceError> _getFeatureError;
    std::optional<DeviceError> _sendError;
    std::optional<std::size_t> _closeAfter;
    std::size_t _sendFailsAfter = 0;

    std::string _openedPath;
    std::chrono::milliseconds _lastReadTimeout { 0 };
    std::size_t _closeCount = 0;
    std::size_t _featureReadCount = 0;
    bool _isInitialized = false;
    bool _isOpen = false;
};

} // namespace ax310
