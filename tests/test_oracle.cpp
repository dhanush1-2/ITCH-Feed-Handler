// tests/test_oracle.cpp
//
// The feed oracle: an execution must hit the best level and the head of its queue.

#include <gtest/gtest.h>
#include "support/verify_rig.hpp"

namespace {
using itch::test::Hdr;
using itch::test::VerifyRig;
constexpr Hdr H1{1, 0, 123};
}  // namespace

TEST(Oracle, ExecutionAtBestAndHead) {
    VerifyRig r;
    r.add(1, 1, 'B', 100, 5000);
    r.add(1, 2, 'B', 100, 5000);
    r.add(1, 3, 'S', 100, 5100);
    r.feed(itch::test::make_executed(H1, 1, 40, 9));
    r.feed(itch::test::make_executed_price(H1, 3, 100, 10, 'Y', 5105));
    EXPECT_EQ(r.obs.counts().exec_checked, 2u);
    EXPECT_EQ(r.obs.counts().exec_not_best, 0u);
    EXPECT_EQ(r.obs.counts().exec_not_head, 0u);
}

TEST(Oracle, ExecutionBehindBetterLevelCounted) {
    VerifyRig r;
    r.add(1, 1, 'B', 100, 5000);
    r.add(1, 2, 'B', 100, 4900);
    r.feed(itch::test::make_executed(H1, 2, 100, 9));  // a worse bid traded first
    EXPECT_EQ(r.obs.counts().exec_checked, 1u);
    EXPECT_EQ(r.obs.counts().exec_not_best, 1u);
    EXPECT_EQ(r.obs.counts().exec_not_head, 0u);
    const auto& ex = r.obs.oracle().exceptions(book::OracleKind::NotBest);
    ASSERT_EQ(ex.size(), 1u);
    EXPECT_EQ(ex[0].ref, 2u);
    EXPECT_EQ(ex[0].locate, 1u);
    EXPECT_EQ(ex[0].timestamp_ns, 123u);
}

TEST(Oracle, ExecutionBehindEarlierOrderCounted) {
    VerifyRig r;
    r.add(1, 1, 'S', 100, 5100);
    r.add(1, 2, 'S', 100, 5100);
    r.feed(itch::test::make_executed(H1, 2, 100, 9));  // second in the queue traded
    EXPECT_EQ(r.obs.counts().exec_not_best, 0u);
    EXPECT_EQ(r.obs.counts().exec_not_head, 1u);
    EXPECT_EQ(r.obs.oracle().exceptions(book::OracleKind::NotHead)[0].ref, 2u);
}

TEST(Oracle, OracleRunsBeforeTheFillIsApplied) {
    VerifyRig r;
    r.add(1, 1, 'B', 1, 5000);  // one share: the fill empties the book
    r.feed(itch::test::make_executed(H1, 1, 1, 9));
    EXPECT_EQ(r.b.live_orders(), 0u);
    EXPECT_EQ(r.obs.counts().exec_checked, 1u);
    EXPECT_EQ(r.obs.counts().exec_not_best, 0u);  // checked while the order was still resting
    EXPECT_EQ(r.obs.counts().exec_not_head, 0u);
}

TEST(Oracle, KeepsOnlyTheFirstHundredInstances) {
    VerifyRig r(4096);
    r.add(1, 1, 'B', 1000, 5000);
    for (std::uint64_t i = 0; i < 150; ++i) {
        r.add(1, 100 + i, 'B', 10, 4900);
        r.feed(itch::test::make_executed(H1, 100 + i, 10, i));
    }
    EXPECT_EQ(r.obs.counts().exec_not_best, 150u);
    EXPECT_EQ(r.obs.oracle().exceptions(book::OracleKind::NotBest).size(), 100u);
}

TEST(Oracle, ExceptionsAreSplitByContinuous) {
    VerifyRig r;
    r.add(1, 1, 'B', 100, 5000);
    r.add(1, 2, 'B', 100, 4900);
    r.feed(itch::test::make_executed(H1, 2, 50, 1));  // before market hours: not continuous
    r.open_market(1);
    r.feed(itch::test::make_executed(H1, 2, 50, 2));  // continuous
    const auto& c = r.obs.counts();
    EXPECT_EQ(c.exec_checked, 2u);
    EXPECT_EQ(c.exec_checked_continuous, 1u);
    EXPECT_EQ(c.exec_not_best, 2u);
    EXPECT_EQ(c.exec_not_best_continuous, 1u);
    EXPECT_EQ(c.exec_not_head_continuous, 0u);
}
