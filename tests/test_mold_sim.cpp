// tests/test_mold_sim.cpp
//
// LossyChannel: the deterministic network simulator under the receiver tests.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <mold/lossy_channel.hpp>
#include <span>
#include <vector>

namespace {
using Bytes = std::vector<std::byte>;

// A 20-byte packet whose first 8 bytes carry its index, so the tests can tell packets apart.
Bytes indexed(std::uint64_t i) {
    Bytes p(20, std::byte{0});
    for (int k = 0; k < 8; ++k) {
        p[static_cast<std::size_t>(k)] = static_cast<std::byte>((i >> (8 * (7 - k))) & 0xFF);
    }
    return p;
}

std::uint64_t index_of(const Bytes& p) {
    std::uint64_t v = 0;
    for (int k = 0; k < 8; ++k) {
        v = (v << 8) | static_cast<std::uint64_t>(p[static_cast<std::size_t>(k)]);
    }
    return v;
}

// Sends n packets (every fifth on the retransmit path when mix is set), drains the channel,
// and returns the delivered indices in order.
std::vector<std::uint64_t> run(mold::LossyChannel& ch, std::uint64_t n, bool mix = false) {
    std::vector<std::uint64_t> out;
    Bytes                      got;
    for (std::uint64_t i = 0; i < n; ++i) {
        const Bytes p = indexed(i);
        if (mix && i % 5 == 0) {
            ch.retransmit_path().send(p);
        } else {
            ch.send(p);
        }
        while (ch.poll(got)) {
            out.push_back(index_of(got));
        }
    }
    ch.finish();
    while (ch.poll(got)) {
        out.push_back(index_of(got));
    }
    return out;
}

mold::ChannelConfig hostile(std::uint64_t seed) {
    mold::ChannelConfig c{};
    c.seed            = seed;
    c.loss            = 0.05;
    c.dup             = 0.03;
    c.reorder_window  = 6;
    c.burst_start     = 0.01;
    c.burst_len       = 4;
    c.retransmit_loss = 0.2;
    return c;
}
}  // namespace

TEST(LossyChannel, SameSeedSameSequence) {
    mold::LossyChannel a(hostile(42));
    mold::LossyChannel b(hostile(42));
    mold::LossyChannel c(hostile(43));
    const auto         ra = run(a, 20000, true);
    const auto         rb = run(b, 20000, true);
    const auto         rc = run(c, 20000, true);
    EXPECT_EQ(ra, rb);
    EXPECT_NE(ra, rc);
    EXPECT_EQ(a.counts().dropped, b.counts().dropped);
    EXPECT_EQ(a.counts().reordered, b.counts().reordered);
}

TEST(LossyChannel, ZeroConfigIsPassThroughInOrder) {
    mold::LossyChannel ch(mold::ChannelConfig{});
    const auto         r = run(ch, 5000, true);
    ASSERT_EQ(r.size(), 5000u);
    for (std::uint64_t i = 0; i < r.size(); ++i) {
        ASSERT_EQ(r[i], i);
    }
    const auto& n = ch.counts();
    EXPECT_EQ(n.sent, 5000u);
    EXPECT_EQ(n.delivered, 5000u);
    EXPECT_EQ(n.dropped, 0u);
    EXPECT_EQ(n.duplicated, 0u);
    EXPECT_EQ(n.reordered, 0u);
}

TEST(LossyChannel, OwnsACopyOfInFlightPackets) {
    mold::ChannelConfig cfg{};
    cfg.reorder_window = 3;
    cfg.seed           = 9;
    mold::LossyChannel ch(cfg);
    Bytes              p = indexed(7);
    ch.send(p);
    p.assign(20, std::byte{0xEE});  // caller reuses its buffer while the packet is held
    ch.finish();
    Bytes got;
    ASSERT_TRUE(ch.poll(got));
    EXPECT_EQ(index_of(got), 7u);
}

TEST(LossyChannel, ObservedRatesNearConfigured) {
    constexpr std::uint64_t kN = 1'000'000;

    mold::ChannelConfig cfg{};
    cfg.seed = 2024;
    cfg.loss = 0.05;
    cfg.dup  = 0.02;
    mold::LossyChannel ch(cfg);
    const auto         r = run(ch, kN);
    const auto&        n = ch.counts();
    EXPECT_EQ(n.sent, kN);
    EXPECT_NEAR(static_cast<double>(n.dropped) / static_cast<double>(kN), 0.05, 0.001);
    EXPECT_NEAR(static_cast<double>(n.duplicated) / static_cast<double>(kN - n.dropped), 0.02,
                0.001);
    EXPECT_EQ(n.delivered, kN - n.dropped + n.duplicated);
    EXPECT_EQ(r.size(), n.delivered);

    // The retransmit path has its own loss rate.
    mold::ChannelConfig rc{};
    rc.seed            = 7;
    rc.loss            = 0.0;
    rc.retransmit_loss = 0.3;
    mold::LossyChannel rch(rc);
    Bytes              got;
    std::uint64_t      delivered = 0;
    for (std::uint64_t i = 0; i < kN; ++i) {
        rch.retransmit_path().send(indexed(i));
        while (rch.poll(got)) {
            ++delivered;
        }
    }
    EXPECT_NEAR(1.0 - static_cast<double>(delivered) / static_cast<double>(kN), 0.3, 0.001);
}

TEST(LossyChannel, BurstsDropRunsAndReorderingHappens) {
    mold::ChannelConfig cfg{};
    cfg.seed           = 5;
    cfg.burst_start    = 0.01;
    cfg.burst_len      = 8;
    cfg.reorder_window = 4;
    mold::LossyChannel ch(cfg);
    const auto         r = run(ch, 50000);
    const auto&        n = ch.counts();
    EXPECT_GT(n.dropped, 0u);
    EXPECT_GT(n.reordered, 0u);
    // Runs of at least burst_len consecutive missing indices must exist.
    std::vector<bool> seen(50000, false);
    for (auto i : r) {
        seen[i] = true;
    }
    std::size_t run_len = 0, longest = 0;
    for (bool s : seen) {
        run_len = s ? 0 : run_len + 1;
        longest = std::max(longest, run_len);
    }
    EXPECT_GE(longest, 8u);
}
