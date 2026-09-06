// SPDX-License-Identifier: Apache-2.0
#include "HidApiTransport.hpp"

#include <hidapi.h>

#include <string>

namespace ax310
{

namespace
{
    /// hidapi reports its errors as wide strings; the rest of the project speaks
    /// std::string. Narrowing character by character is lossy for anything
    /// outside ASCII, which these messages are not.
    ///
    /// @param wide The message hidapi returned, may be null.
    /// @return The same message as a narrow string.
    std::string narrow(wchar_t const* wide)
    {
        if (!wide)
            return "unknown error";

        std::wstring const text(wide);
        return { text.begin(), text.end() };
    }
} // namespace

HidApiTransport::~HidApiTransport()
{
    close();
    shutdown();
}

std::expected<void, DeviceError> HidApiTransport::initialize()
{
    if (_isInitialized)
        return {};

    if (hid_init() != 0)
        return std::unexpected(DeviceError::HidInitFailed);

    _isInitialized = true;
    return {};
}

void HidApiTransport::shutdown()
{
    if (!_isInitialized)
        return;

    hid_exit();
    _isInitialized = false;
}

std::vector<HidInterface> HidApiTransport::enumerate(std::uint16_t vendorId, std::uint16_t productId)
{
    std::vector<HidInterface> found;

    hid_device_info* devices = hid_enumerate(vendorId, productId);
    for (auto const* current = devices; current; current = current->next)
    {
        found.push_back(HidInterface {
            .path = current->path ? current->path : "",
            .interfaceNumber = current->interface_number,
            .usagePage = current->usage_page,
            .usage = current->usage,
        });
    }
    hid_free_enumeration(devices);

    return found;
}

std::expected<void, DeviceError> HidApiTransport::open(std::string_view path)
{
    close();

    std::string const nullTerminated(path);
    _handle = hid_open_path(nullTerminated.c_str());
    if (!_handle)
        return std::unexpected(DeviceError::OpenFailed);

    // Blocking reads; the timeout is passed per read instead.
    hid_set_nonblocking(_handle, 0);
    return {};
}

void HidApiTransport::close()
{
    if (!_handle)
        return;

    hid_close(_handle);
    _handle = nullptr;
}

bool HidApiTransport::isOpen() const
{
    return _handle != nullptr;
}

std::expected<void, DeviceError> HidApiTransport::sendFeatureReport(std::span<std::uint8_t const> report)
{
    if (!_handle)
        return std::unexpected(DeviceError::NotConnected);

    if (hid_send_feature_report(_handle, report.data(), report.size()) >= 0)
        return {};

    // Some backends reject feature reports for this device; the deck accepts the
    // same payload as an ordinary write, which is what the vendor software does.
    return write(report);
}

std::expected<std::size_t, DeviceError> HidApiTransport::getFeatureReport(std::span<std::uint8_t> buffer)
{
    if (!_handle)
        return std::unexpected(DeviceError::NotConnected);

    int const bytesRead = hid_get_feature_report(_handle, buffer.data(), buffer.size());
    if (bytesRead < 0)
        return std::unexpected(DeviceError::ReadFailed);

    return static_cast<std::size_t>(bytesRead);
}

std::expected<void, DeviceError> HidApiTransport::write(std::span<std::uint8_t const> report)
{
    if (!_handle)
        return std::unexpected(DeviceError::NotConnected);

    if (hid_write(_handle, report.data(), report.size()) < 0)
        return std::unexpected(DeviceError::WriteFailed);

    return {};
}

std::expected<std::size_t, DeviceError> HidApiTransport::read(std::span<std::uint8_t> buffer,
                                                              std::chrono::milliseconds timeout)
{
    if (!_handle)
        return std::unexpected(DeviceError::NotConnected);

    int const bytesRead =
        hid_read_timeout(_handle, buffer.data(), buffer.size(), static_cast<int>(timeout.count()));
    if (bytesRead < 0)
        return std::unexpected(DeviceError::ReadFailed);

    return static_cast<std::size_t>(bytesRead);
}

std::string HidApiTransport::lastError() const
{
    if (!_handle)
        return "no device";

    return narrow(hid_error(_handle));
}

} // namespace ax310
