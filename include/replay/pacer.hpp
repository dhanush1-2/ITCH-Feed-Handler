// include/replay/pacer.hpp
#pragma once

// Maps feed time to wall time so a recorded day can be replayed at 1x, 10x or flat out.
//
// The first call to wait_until() anchors the mapping: that feed timestamp is "now". After that
// a message stamped `feed_ns` is due at anchor_wall + (feed_ns - anchor_feed) / speed. Speed 0
// means max: wait_until() returns at once and never reads the clock.
//
// Reading the clock costs more than applying a message, so the pacer does not read it for
// every one. It reads it when 256 messages have passed since the last reading, or when the
// message is due more than 50 us after the last reading (a gap worth waiting for). Between
// readings a message goes through unchecked. The consequence for max_lag_ns(): it is the
// largest lag seen AT A READING, so it can miss lag that built up and cleared inside 256
// messages. At 10x that is some tens of microseconds of wall time.
//
// A message that is already late is applied at once (the pacer never skips or drops), and the
// shortfall is recorded. A feed timestamp that goes backwards is treated as due now.
//
// Clock needs: std::uint64_t now_ns() and void sleep_until_ns(std::uint64_t). The pacer owns
// its clock by value so a test can drive a fake one.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

namespace replay {

struct SteadyClock {
    // Optional: a wait ends early when this becomes true (the live view's quit flag), so a
    // long gap in the feed cannot hold up a ctrl-c.
    const std::atomic<bool>* stop = nullptr;

    [[nodiscard]] std::uint64_t now_ns() const noexcept {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    }

    // Sleeps for most of the wait, in slices of at most 20 ms, and spins for the last stretch:
    // a sleep may overshoot by tens of microseconds and the spin lands within a clock read of
    // the target.
    void sleep_until_ns(std::uint64_t target) const noexcept {
        constexpr std::uint64_t kSpinNs  = 100'000;
        constexpr std::uint64_t kSliceNs = 20'000'000;
        for (;;) {
            const std::uint64_t now = now_ns();
            if (now >= target || (stop != nullptr && stop->load(std::memory_order_relaxed))) {
                return;
            }
            const std::uint64_t left = target - now;
            if (left > 2 * kSpinNs) {
                const std::uint64_t nap = left - kSpinNs;
                std::this_thread::sleep_for(
                    std::chrono::nanoseconds(nap < kSliceNs ? nap : kSliceNs));
            }
        }
    }
};

template <class Clock = SteadyClock>
class Pacer {
public:
    static constexpr std::uint32_t kCheckEvery = 256;
    static constexpr std::uint64_t kFarNs      = 50'000;

    // speed 0 = max. Otherwise feed seconds per wall second (1 = real time, 10 = ten times).
    explicit Pacer(double speed, Clock clock = Clock{}) : speed_(speed), clock_(std::move(clock)) {}

    void wait_until(std::uint64_t feed_ns) {
        if (speed_ <= 0.0) {
            return;
        }
        if (!anchored_) {
            anchored_    = true;
            anchor_feed_ = feed_ns;
            anchor_wall_ = clock_.now_ns();
            last_now_    = anchor_wall_;
            return;
        }
        const std::uint64_t delta = feed_ns > anchor_feed_ ? feed_ns - anchor_feed_ : 0;
        const std::uint64_t target =
            anchor_wall_ + static_cast<std::uint64_t>(static_cast<double>(delta) / speed_);
        if (++since_check_ < kCheckEvery && target <= last_now_ + kFarNs) {
            return;
        }
        since_check_            = 0;
        const std::uint64_t now = clock_.now_ns();
        if (now >= target) {
            const std::uint64_t lag = now - target;
            if (lag > max_lag_ns_) {
                max_lag_ns_      = lag;
                max_lag_feed_ns_ = feed_ns;  // where in the feed the worst lag was seen
            }
            last_now_ = now;
        } else {
            clock_.sleep_until_ns(target);
            ++waits_;
            last_now_ = target;
        }
    }

    [[nodiscard]] bool          paced() const noexcept { return speed_ > 0.0; }
    [[nodiscard]] double        speed() const noexcept { return speed_; }
    [[nodiscard]] std::uint64_t max_lag_ns() const noexcept { return max_lag_ns_; }
    // Feed timestamp of the message at which max_lag_ns() was recorded (0 if never behind).
    [[nodiscard]] std::uint64_t max_lag_feed_ns() const noexcept { return max_lag_feed_ns_; }
    [[nodiscard]] std::uint64_t waits() const noexcept { return waits_; }
    [[nodiscard]] Clock&        clock() noexcept { return clock_; }
    [[nodiscard]] const Clock&  clock() const noexcept { return clock_; }

private:
    double        speed_;
    Clock         clock_;
    bool          anchored_        = false;
    std::uint64_t anchor_feed_     = 0;
    std::uint64_t anchor_wall_     = 0;
    std::uint64_t last_now_        = 0;
    std::uint64_t max_lag_feed_ns_ = 0;
    std::uint64_t max_lag_ns_      = 0;
    std::uint64_t waits_           = 0;
    std::uint32_t since_check_     = 0;
};

}  // namespace replay
