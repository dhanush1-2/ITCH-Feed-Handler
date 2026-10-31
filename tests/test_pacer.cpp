// tests/test_pacer.cpp
//
// Pacer against a fake clock: the mapping from feed time to wall time, the unpaced mode, the
// lag record, and how often the clock is read.

#include <gtest/gtest.h>

#include <cstdint>
#include <replay/pacer.hpp>

namespace {

// A clock the test moves by hand. sleep_until jumps straight to the target and counts.
struct FakeClock {
    std::uint64_t now      = 1'000;
    std::uint64_t reads    = 0;
    std::uint64_t sleeps   = 0;
    std::uint64_t slept_to = 0;
    std::uint64_t now_ns() {
        ++reads;
        return now;
    }
    void sleep_until_ns(std::uint64_t t) {
        ++sleeps;
        slept_to = t;
        if (t > now) {
            now = t;
        }
    }
};

constexpr std::uint64_t kSec = 1'000'000'000ULL;

}  // namespace

TEST(Pacer, TenXMapsFeedToWallTime) {
    replay::Pacer<FakeClock> p(10.0, FakeClock{});
    const std::uint64_t      feed0 = 11'000 * kSec;
    p.wait_until(feed0);  // anchors: feed0 <-> wall 1000
    EXPECT_EQ(p.clock().sleeps, 0u);
    p.wait_until(feed0 + 10 * kSec);  // ten feed seconds later
    EXPECT_EQ(p.clock().sleeps, 1u);
    EXPECT_EQ(p.clock().slept_to, 1'000 + 1 * kSec);  // one wall second after the anchor
    EXPECT_EQ(p.max_lag_ns(), 0u);
}

TEST(Pacer, OneXIsRealTime) {
    replay::Pacer<FakeClock> p(1.0, FakeClock{});
    p.wait_until(5 * kSec);
    p.wait_until(7 * kSec);
    EXPECT_EQ(p.clock().slept_to, 1'000 + 2 * kSec);
}

TEST(Pacer, MaxNeverWaits) {
    replay::Pacer<FakeClock> p(0.0, FakeClock{});
    for (std::uint64_t i = 0; i < 100'000; ++i) {
        p.wait_until(i * kSec);
    }
    EXPECT_EQ(p.clock().reads, 0u);
    EXPECT_EQ(p.clock().sleeps, 0u);
    EXPECT_EQ(p.max_lag_ns(), 0u);
    EXPECT_FALSE(p.paced());
}

TEST(Pacer, RecordsLagWhenBehind) {
    replay::Pacer<FakeClock> p(10.0, FakeClock{});
    p.wait_until(0);                    // anchor at wall 1000
    p.clock().now = 1'000 + 5'000'000;  // the consumer was slow: 5 ms of wall time passed
    p.wait_until(10'000'000);           // due at wall 1000 + 1 ms
    EXPECT_EQ(p.clock().sleeps, 0u);
    EXPECT_EQ(p.max_lag_ns(), 4'000'000u);
    // Lag is a maximum: a later message that is on time does not lower it.
    p.clock().now = 1'000 + 5'000'000;
    p.wait_until(100'000'000);  // due at 1000 + 10 ms: ahead, so it waits
    EXPECT_EQ(p.max_lag_ns(), 4'000'000u);
    EXPECT_EQ(p.clock().sleeps, 1u);
}

TEST(Pacer, ChecksTheClockEvery256MessagesWhenNothingIsFarAway) {
    replay::Pacer<FakeClock> p(10.0, FakeClock{});
    p.wait_until(0);  // anchor: one clock read
    const std::uint64_t base = p.clock().reads;
    // 1000 messages 200 ns of feed time apart: each is 20 ns of wall time, all within 50 us of
    // the last reading, so only the 256-message cadence reads the clock.
    for (std::uint64_t i = 1; i <= 1000; ++i) {
        p.wait_until(i * 200);
    }
    EXPECT_EQ(p.clock().reads - base, 1000u / 256u);
}

TEST(Pacer, ReadsTheClockForAMessageMoreThan50UsAway) {
    replay::Pacer<FakeClock> p(1.0, FakeClock{});
    p.wait_until(0);
    const std::uint64_t base = p.clock().reads;
    p.wait_until(60'000);  // 60 us away at 1x: read the clock at once and wait
    EXPECT_GT(p.clock().reads, base);
    EXPECT_EQ(p.clock().sleeps, 1u);
    // Within 50 us of the last reading and not the 256th message: no read, no wait.
    const std::uint64_t reads = p.clock().reads;
    p.wait_until(60'000 + 40'000);
    EXPECT_EQ(p.clock().reads, reads);
}

TEST(Pacer, FeedTimeGoingBackwardsDoesNotWait) {
    replay::Pacer<FakeClock> p(10.0, FakeClock{});
    p.wait_until(100 * kSec);
    p.wait_until(50 * kSec);
    EXPECT_EQ(p.clock().sleeps, 0u);
}
