// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <variant>
#include <vector>

#include "Protocol.hpp"
#include "Types.hpp"

namespace ax310
{

/// A physical button went down.
struct ButtonPressed
{
    Button button;
};

/// A knob was pushed in.
struct KnobPushed
{
    KnobId knob;
};

/// A knob's capacitive surface was touched or released.
struct KnobTouched
{
    KnobId knob;
    Touch touch;
};

/// A knob's tracked volume changed.
struct KnobVolumeChanged
{
    KnobId knob;
    int volume; ///< Percent.
};

/// The touch screen was contacted or released.
struct ScreenTouched
{
    int x;
    int y;
    TouchPhase phase;
};

/// The deck's audio meters moved.
///
/// One per track, in the deck's printed knob order, each a percentage of full
/// scale. Sent as a set because they arrive as a set and a UI draws them
/// together.
struct AudioMetersChanged
{
    std::array<int, protocol::AudioMeterCount> levels;
};

/// The device moved through its connect / re-enumerate / run cycle.
struct ConnectionChanged
{
    ConnectionState state;
};

/// Anything the device can tell us about.
///
/// A variant rather than a listener interface with one method per event: a new
/// event kind is a new alternative and one more arm wherever the events are
/// visited, instead of a new pure virtual that every existing listener must
/// grow an override for.
using DeviceEvent = std::variant<ButtonPressed,
                                 KnobPushed,
                                 KnobTouched,
                                 KnobVolumeChanged,
                                 ScreenTouched,
                                 AudioMetersChanged,
                                 ConnectionChanged>;

/// Where a Device sends what it decoded.
///
/// Implemented by whoever owns the device -- in this project a Qt adapter that
/// turns each event into a signal. The driver knows nothing about it beyond
/// this one call, which is what keeps the driver free of Qt.
class IDeviceListener
{
  public:
    IDeviceListener() = default;
    virtual ~IDeviceListener() = default;

    // Injected by reference and owned as a concrete type, never copied and never
    // sliced. Saying so is the Rule of Five: a class that declares a destructor
    // and nothing else gets copy and move implicitly, which for a polymorphic
    // base is how slicing happens quietly.
    IDeviceListener(IDeviceListener const&) = delete;
    IDeviceListener& operator=(IDeviceListener const&) = delete;
    IDeviceListener(IDeviceListener&&) = delete;
    IDeviceListener& operator=(IDeviceListener&&) = delete;

    /// Called from whichever thread drives Device::poll().
    /// @param event What happened.
    virtual void onDeviceEvent(DeviceEvent const& event) = 0;
};

/// An IDeviceListener that discards everything, for callers that only write.
class NullDeviceListener final: public IDeviceListener
{
  public:
    void onDeviceEvent(DeviceEvent const& /*event*/) override {}
};

/// An IDeviceListener that keeps every event, for tests to assert against.
///
/// The typed accessors exist so a case reads as what it means -- `events.count<
/// ButtonPressed>() == 1` rather than a std::get spelled out at every site.
class RecordingListener final: public IDeviceListener
{
  public:
    void onDeviceEvent(DeviceEvent const& event) override { _events.push_back(event); }

    /// @return Every event received, in order.
    [[nodiscard]] std::vector<DeviceEvent> const& events() const noexcept { return _events; }

    /// @return How many events were received.
    [[nodiscard]] std::size_t size() const noexcept { return _events.size(); }

    /// @return Whether nothing was received.
    [[nodiscard]] bool empty() const noexcept { return _events.empty(); }

    /// Forgets everything received so far, so a case can assert on one step of a
    /// sequence without the previous steps in the way.
    void clear() noexcept { _events.clear(); }

    /// @tparam T The alternative to count.
    /// @return How many received events hold a T.
    template <typename T>
    [[nodiscard]] std::size_t count() const
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            _events, [](DeviceEvent const& event) { return std::holds_alternative<T>(event); }));
    }

    /// @tparam T The alternative to look for.
    /// @param index Which T to return, counting only Ts.
    /// @return The index-th T received; a default-constructed T when there is
    ///         none, so a failed REQUIRE reports the mismatch rather than
    ///         terminating on a bad variant access.
    template <typename T>
    [[nodiscard]] T nth(std::size_t index = 0) const
    {
        std::size_t seen = 0;
        for (auto const& event: _events)
        {
            if (!std::holds_alternative<T>(event))
                continue;
            if (seen == index)
                return std::get<T>(event);
            ++seen;
        }
        return T {};
    }

  private:
    std::vector<DeviceEvent> _events;
};

} // namespace ax310
