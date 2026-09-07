// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

namespace ax310
{

/// The passage of time, as the driver is allowed to observe it.
///
/// Injected rather than called directly so the initialisation sequence's delays
/// and the reconnect backoff can be exercised without a test waiting for real
/// seconds to pass.
class IClock
{
  public:
    IClock() = default;
    virtual ~IClock() = default;

    // Injected by reference and owned as a concrete type, never copied and never
    // sliced. Saying so is the Rule of Five: a class that declares a destructor
    // and nothing else gets copy and move implicitly, which for a polymorphic
    // base is how slicing happens quietly.
    IClock(IClock const&) = delete;
    IClock& operator=(IClock const&) = delete;
    IClock(IClock&&) = delete;
    IClock& operator=(IClock&&) = delete;

    /// @return The current instant on a monotonic clock.
    [[nodiscard]] virtual std::chrono::steady_clock::time_point now() const = 0;

    /// Blocks the calling thread.
    /// @param duration How long to block for.
    virtual void sleepFor(std::chrono::milliseconds duration) = 0;
};

/// The real clock: std::chrono::steady_clock and a sleeping thread.
class SystemClock final: public IClock
{
  public:
    [[nodiscard]] std::chrono::steady_clock::time_point now() const override
    {
        return std::chrono::steady_clock::now();
    }

    void sleepFor(std::chrono::milliseconds duration) override { std::this_thread::sleep_for(duration); }
};

/// A clock a test drives by hand.
///
/// sleepFor() advances the clock and records the duration instead of blocking,
/// which is what makes the initialisation handshake free to test: it is 74
/// payloads with a ten-millisecond gap after each, so a real clock would cost
/// three quarters of a second per connect case. Tests assert on what was slept
/// for, which is the behaviour worth pinning anyway.
class ManualClock final: public IClock
{
  public:
    /// @param start The instant now() reports until something advances it.
    explicit ManualClock(std::chrono::steady_clock::time_point start = {}) noexcept: _now { start } {}

    [[nodiscard]] std::chrono::steady_clock::time_point now() const override
    {
        std::lock_guard<std::mutex> const lock { _mutex };
        return _now;
    }

    void sleepFor(std::chrono::milliseconds duration) override
    {
        std::lock_guard<std::mutex> const lock { _mutex };
        _sleeps.push_back(duration);
        _now += duration;
    }

    /// Moves the clock forward without recording a sleep.
    /// @param duration How far forward.
    void advance(std::chrono::milliseconds duration)
    {
        std::lock_guard<std::mutex> const lock { _mutex };
        _now += duration;
    }

    /// @return Every duration sleepFor() was asked for, in order.
    [[nodiscard]] std::vector<std::chrono::milliseconds> sleeps() const
    {
        std::lock_guard<std::mutex> const lock { _mutex };
        return _sleeps;
    }

    /// @return The sum of every recorded sleep.
    [[nodiscard]] std::chrono::milliseconds totalSlept() const
    {
        std::lock_guard<std::mutex> const lock { _mutex };
        return std::accumulate(_sleeps.begin(), _sleeps.end(), std::chrono::milliseconds { 0 });
    }

  private:
    // Guarded, because the driver reaches a clock from whichever thread is
    // writing to the deck, and the application writes from two: the poll loop
    // applies a knob turn while the interface's thread drags a fader. An
    // unguarded push_back from both is a reallocation under another thread.
    mutable std::mutex _mutex;
    std::chrono::steady_clock::time_point _now;
    std::vector<std::chrono::milliseconds> _sleeps;
};

} // namespace ax310
