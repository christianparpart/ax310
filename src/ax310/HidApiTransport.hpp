// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "IHidTransport.hpp"

struct hid_device_;

namespace ax310
{

/// IHidTransport on top of hidapi.
///
/// The only translation unit in the library that includes <hidapi.h>; everything
/// else reaches the bus through the interface. Owns at most one open handle and
/// closes it on destruction.
class HidApiTransport final: public IHidTransport
{
  public:
    HidApiTransport() = default;
    ~HidApiTransport() override;

    HidApiTransport(HidApiTransport const&) = delete;
    HidApiTransport& operator=(HidApiTransport const&) = delete;
    HidApiTransport(HidApiTransport&&) = delete;
    HidApiTransport& operator=(HidApiTransport&&) = delete;

    [[nodiscard]] std::expected<void, DeviceError> initialize() override;
    void shutdown() override;
    [[nodiscard]] std::vector<HidInterface> enumerate(std::uint16_t vendorId,
                                                      std::uint16_t productId) override;
    [[nodiscard]] std::expected<void, DeviceError> open(std::string_view path) override;
    void close() override;
    [[nodiscard]] bool isOpen() const override;
    [[nodiscard]] std::expected<void, DeviceError> sendFeatureReport(
        std::span<std::uint8_t const> report) override;
    [[nodiscard]] std::expected<std::size_t, DeviceError> getFeatureReport(
        std::span<std::uint8_t> buffer) override;
    [[nodiscard]] std::expected<void, DeviceError> write(std::span<std::uint8_t const> report) override;
    [[nodiscard]] std::expected<std::size_t, DeviceError> read(std::span<std::uint8_t> buffer,
                                                               std::chrono::milliseconds timeout) override;
    [[nodiscard]] std::string lastError() const override;

  private:
    hid_device_* _handle = nullptr;
    bool _isInitialized = false;
};

} // namespace ax310
