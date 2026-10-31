// tests/test_loss_injection.cpp
//
// The loss-injection proof on the real-data slice (testdata/slice.itch.gz, expanded by the
// slice_prepare fixture; its path comes in the ITCH_SLICE environment variable).
//
// The slice is sent as MoldUDP64 packets through a hostile simulated network into the
// Receiver and a BookBuilder, and what comes out is held against the same file replayed
// directly into a second BookBuilder: the delivered (seq, bytes) stream by hash, and the
// resting books by their dump bytes at feed second 43,200 (noon), when the slice's books are
// populated. The slice ends with empty books like the full day, so an end-of-file comparison
// would be vacuous; every state comparison here first asserts it is not.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <itch/mapped_file.hpp>
#include <mold/sim.hpp>
#include <string>
#include <tuple>
#include <vector>

namespace {

constexpr std::uint64_t kNoon      = 43'200;
constexpr std::size_t   kMaxLive   = 1U << 18;
constexpr std::size_t   kRetention = 1U << 16;

const itch::MappedFile& slice() {
    static const itch::MappedFile mf = [] {
        const char* p = std::getenv("ITCH_SLICE");
        if (p == nullptr || *p == '\0') {
            throw std::runtime_error(
                "ITCH_SLICE is not set: run through ctest, which expands the "
                "slice first (fixture slice_prepare)");
        }
        return itch::MappedFile::open(p);
    }();
    return mf;
}

mold::DumpSpec noon() {
    mold::DumpSpec d;
    d.enabled = true;
    d.at_set  = true;
    d.at_s    = kNoon;
    return d;
}

// The file replayed with no network at all, computed once.
const mold::ReplayOutcome& direct() {
    static const mold::ReplayOutcome d = mold::direct_replay(slice().bytes(), kMaxLive, noon());
    return d;
}

mold::SimConfig hostile(std::uint64_t seed, double loss) {
    mold::SimConfig c;
    c.channel.seed            = seed;
    c.channel.loss            = loss;
    c.channel.dup             = 0.005;
    c.channel.reorder_window  = 16;
    c.channel.burst_start     = 0.0005;
    c.channel.burst_len       = 64;
    c.channel.retransmit_loss = 0.01;
    c.retention               = kRetention;
    c.max_live_orders         = kMaxLive;
    c.dump                    = noon();
    return c;
}

const std::vector<std::uint64_t> kSeeds  = {1, 2, 3, 4, 5};
const std::vector<double>        kLosses = {0.001, 0.01, 0.05};

}  // namespace

class LossInjection : public ::testing::TestWithParam<std::tuple<std::uint64_t, double>> {};

TEST_P(LossInjection, StateDumpByteIdenticalToDirectFileReplay) {
    const auto [seed, loss] = GetParam();
    const auto& want        = direct();
    // Not vacuous: the books at noon hold resting orders.
    ASSERT_TRUE(want.dump.taken);
    ASSERT_FALSE(want.dump.at_end);
    ASSERT_GT(want.dump.bytes, 0u);
    ASSERT_GT(want.dump.orders(), 100u);

    const mold::SimResult r = mold::run_sim(slice().bytes(), hostile(seed, loss));
    ASSERT_TRUE(r.finished()) << "state " << mold::to_string(r.state) << " ("
                              << mold::to_string(r.stale) << ")";
    ASSERT_TRUE(r.out.dump.taken);
    ASSERT_FALSE(r.out.dump.at_end);
    EXPECT_TRUE(r.out.dump.complete);
    EXPECT_GT(r.out.dump.bytes, 0u);
    EXPECT_EQ(r.out.dump.at_message, want.dump.at_message);
    EXPECT_EQ(r.out.dump.data, want.dump.data);  // every byte
    EXPECT_EQ(r.out.dump.sha256, want.dump.sha256);
}

TEST_P(LossInjection, DeliveredMessageStreamIdentical) {
    const auto [seed, loss] = GetParam();
    const mold::SimResult r = mold::run_sim(slice().bytes(), hostile(seed, loss));
    ASSERT_TRUE(r.finished());
    EXPECT_EQ(r.out.delivered, direct().delivered);
    EXPECT_EQ(r.out.delivered, r.source_messages);
    EXPECT_EQ(r.out.stream_sha256, direct().stream_sha256);
    EXPECT_EQ(r.out.stream_sha256, mold::source_stream_sha256(slice().bytes()));
    EXPECT_EQ(r.out.handled, direct().handled);
    EXPECT_EQ(r.out.skipped, direct().skipped);
    EXPECT_EQ(r.out.live_orders, direct().live_orders);
}

INSTANTIATE_TEST_SUITE_P(Slice, LossInjection,
                         ::testing::Combine(::testing::ValuesIn(kSeeds),
                                            ::testing::ValuesIn(kLosses)));

// The network really was hostile and the receiver really did recover: a clean run would pass
// the tests above too.
TEST(LossInjection, GapsActuallyOccurred) {
    for (const std::uint64_t seed : kSeeds) {
        for (const double loss : kLosses) {
            SCOPED_TRACE("seed " + std::to_string(seed) + " loss " + std::to_string(loss));
            const mold::SimResult r = mold::run_sim(slice().bytes(), hostile(seed, loss));
            ASSERT_TRUE(r.finished());
            EXPECT_GT(r.rx.gaps_detected, 0u);
            EXPECT_GT(r.rx.messages_recovered, 0u);
            EXPECT_GT(r.rx.retransmit_requests, 0u);
            EXPECT_GT(r.channel.reordered, 0u);
            EXPECT_GT(r.channel.duplicated, 0u);
            EXPECT_GT(r.rx.dup_packets, 0u);
            if (loss >= 0.01) {
                EXPECT_GT(r.channel.dropped, 0u);
            }
        }
    }
}

TEST(LossInjection, SameSeedSameRun) {
    const mold::SimResult a = mold::run_sim(slice().bytes(), hostile(3, 0.05));
    const mold::SimResult b = mold::run_sim(slice().bytes(), hostile(3, 0.05));
    EXPECT_EQ(a.rx.gaps_detected, b.rx.gaps_detected);
    EXPECT_EQ(a.rx.retransmit_requests, b.rx.retransmit_requests);
    EXPECT_EQ(a.channel.dropped, b.channel.dropped);
    EXPECT_EQ(a.channel.reordered, b.channel.reordered);
    const mold::SimResult c = mold::run_sim(slice().bytes(), hostile(4, 0.05));
    EXPECT_NE(a.channel.reordered, c.channel.reordered);
}

TEST(LossInjection, ExpiredRetentionFailsVisibly) {
    mold::SimConfig c       = hostile(1, 0.0);
    c.channel.burst_start   = 0.01;
    c.channel.burst_len     = 1000;
    c.retention             = 100;
    const mold::SimResult r = mold::run_sim(slice().bytes(), c);
    EXPECT_EQ(r.state, mold::RxState::Stale);
    EXPECT_EQ(r.stale, mold::StaleReason::RetriesExhausted);
    EXPECT_FALSE(r.finished());
    EXPECT_LT(r.out.delivered, direct().delivered);
    EXPECT_GT(r.rx.retransmit_requests, 0u);
    EXPECT_GT(r.channel.dropped, 100u);
    // The dump is partial and says so; it is not the real book.
    ASSERT_TRUE(r.out.dump.taken);
    EXPECT_FALSE(r.out.dump.complete);
}
