// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Types.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ax310
{

/// One HID interface the transport found on the bus.
///
/// A value type rather than a borrowed handle: the driver keeps what it needs
/// after the enumeration is released, and a test can hand over a literal.
struct HidInterface
{
    std::string path;              ///< Backend-specific address to open.
    int interfaceNumber { -1 };    ///< USB interface number; -1 when unreported.
    std::uint16_t usagePage { 0 }; ///< HID usage page, for logging.
    std::uint16_t usage { 0 };     ///< HID usage, for logging.
};

/// Everything the driver needs from the USB HID stack.
///
/// The single place hidapi is allowed to be named. Everything above this
/// interface -- enumeration order, the initialisation sequence, report decoding,
/// the reconnect policy -- is testable against a fake that replays a recorded
/// capture, with no device attached.
class IHidTransport
{
  public:
    IHidTransport() = default;
    virtual ~IHidTransport() = default;

    // Injected by reference and owned as a concrete type, never copied and never
    // sliced. Saying so is the Rule of Five: a class that declares a destructor
    // and nothing else gets copy and move implicitly, which for a polymorphic
    // base is how slicing happens quietly.
    IHidTransport(IHidTransport const&) = delete;
    IHidTransport& operator=(IHidTransport const&) = delete;
    IHidTransport(IHidTransport&&) = delete;
    IHidTransport& operator=(IHidTransport&&) = delete;

    /// Brings the underlying library up.
    /// @return Nothing, or why it could not be initialised.
    [[nodiscard]] virtual std::expected<void, DeviceError> initialize() = 0;

    /// Releases the underlying library. Safe to call when not initialised.
    virtual void shutdown() = 0;

    /// @param vendorId USB vendor id to match.
    /// @param productId USB product id to match.
    /// @return Every matching HID interface, empty if none is present.
    [[nodiscard]] virtual std::vector<HidInterface> enumerate(std::uint16_t vendorId,
                                                              std::uint16_t productId) = 0;

    /// Opens one interface, replacing any currently open one.
    /// @param path An address from enumerate().
    /// @return Nothing, or why it could not be opened.
    [[nodiscard]] virtual std::expected<void, DeviceError> open(std::string_view path) = 0;

    /// Closes the open interface. Safe to call when none is open.
    virtual void close() = 0;

    /// @return Whether an interface is currently open.
    [[nodiscard]] virtual bool isOpen() const = 0;

    /// Sends a HID feature report, falling back to an interrupt write where the
    /// backend rejects it -- the AX310 accepts its command payloads either way.
    /// @param report The bytes to send.
    /// @return Nothing, or why the send failed.
    [[nodiscard]] virtual std::expected<void, DeviceError> sendFeatureReport(
        std::span<std::uint8_t const> report) = 0;

    /// Reads a HID feature report back from the device.
    ///
    /// The deck answers a property read this way: send the `0x81` command, then
    /// ask for the report. The reply echoes the address and length that were
    /// asked for, which is what tells a real answer from the remains of the
    /// previous one -- the device leaves stale bytes past the answer's length.
    ///
    /// @param buffer Where to put it; its first byte must hold the report id on
    ///        entry, which is how hidapi selects the report.
    /// @return Bytes read, or why the read failed.
    [[nodiscard]] virtual std::expected<std::size_t, DeviceError> getFeatureReport(
        std::span<std::uint8_t> buffer) = 0;

    /// Writes an output report.
    /// @param report The bytes to write, first byte being the report id.
    /// @return Nothing, or why the write failed.
    [[nodiscard]] virtual std::expected<void, DeviceError> write(std::span<std::uint8_t const> report) = 0;

    /// Reads one input report.
    /// @param buffer Where to put it.
    /// @param timeout How long to wait before giving up.
    /// @return Bytes read, zero on timeout, or why the read failed.
    [[nodiscard]] virtual std::expected<std::size_t, DeviceError> read(std::span<std::uint8_t> buffer,
                                                                       std::chrono::milliseconds timeout) = 0;

    /// @return The backend's description of its last failure, for logging.
    [[nodiscard]] virtual std::string lastError() const = 0;
};

} // namespace ax310
