// tests/test_invariants.cpp
//
// Hard structural checks (check_touched, check_full) and the crossed-book counters.

#include <gtest/gtest.h>

#include <book/invariants.hpp>
#include <functional>
#include <string>

#include "support/verify_rig.hpp"

namespace {

using itch::test::Hdr;
using itch::test::VerifyRig;

// Two bid levels and two ask levels, two orders at each best level.
void populate(VerifyRig& r) {
    r.add(1, 1, 'B', 100, 5000);
    r.add(1, 2, 'B', 50, 5000);
    r.add(1, 3, 'B', 70, 4900);
    r.add(1, 4, 'S', 80, 5100);
    r.add(1, 5, 'S', 20, 5100);
    r.add(1, 6, 'S', 30, 5200);
}

book::Level& bid(VerifyRig& r, std::size_t i) {
    return const_cast<book::Level&>(*std::next(r.b.book(1).bids().begin(), static_cast<long>(i)));
}
ob::OrderPool& pool(VerifyRig& r) {
    return const_cast<ob::OrderPool&>(r.b.pool());
}

}  // namespace

TEST(Invariants, CleanBookPassesBothChecks) {
    VerifyRig r;
    populate(r);
    EXPECT_FALSE(book::check_full(r.b));
    book::OrderTally t;
    for (std::uint16_t loc = 0; loc < 3; ++loc) {
        (void)book::check_touched(r.b, t, loc, 1);  // seed the tally per book
    }
    EXPECT_FALSE(book::check_touched(r.b, t, 1, 1));
}

TEST(Invariants, CheckerCatchesEveryCorruption) {
    struct Case {
        const char*                     name;
        std::function<void(VerifyRig&)> corrupt;
    };
    const Case cases[] = {
        {"level total", [](VerifyRig& r) { bid(r, 0).total += 1; }},
        {"level count", [](VerifyRig& r) { bid(r, 0).count += 1; }},
        {"order side", [](VerifyRig& r) { pool(r).at(bid(r, 0).head).side = ob::Side::Sell; }},
        {"order locate", [](VerifyRig& r) { pool(r).at(bid(r, 1).head).pad = 99; }},
        {"order price", [](VerifyRig& r) { pool(r).at(bid(r, 1).head).price = 4800; }},
        {"order remaining", [](VerifyRig& r) { pool(r).at(bid(r, 0).head).remaining += 1; }},
        {"fifo link", [](VerifyRig& r) { pool(r).at(bid(r, 0).head).next = ob::kInvalidSlot; }},
        {"fifo back link",
         [](VerifyRig& r) { pool(r).at(bid(r, 0).tail).prev = ob::kInvalidSlot; }},
        {"level tail", [](VerifyRig& r) { bid(r, 0).tail = bid(r, 0).head; }},
        {"level order", [](VerifyRig& r) { bid(r, 0).price = 4800; }},
        {"pool size", [](VerifyRig& r) { (void)pool(r).alloc(); }},
    };
    for (const Case& c : cases) {
        VerifyRig r;
        populate(r);
        ASSERT_FALSE(book::check_full(r.b)) << c.name;
        c.corrupt(r);
        const auto v = book::check_full(r.b, 42);
        ASSERT_TRUE(v.has_value()) << c.name;
        EXPECT_EQ(v->msg_index, 42u) << c.name;
    }
}

TEST(Invariants, TouchedCatchesBestLevelAndAggregateCorruption) {
    {
        VerifyRig r;
        populate(r);
        book::OrderTally t;
        pool(r).at(bid(r, 0).head).pad = 99;
        const auto v                   = book::check_touched(r.b, t, 1, 7);
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(v->locate, 1u);
    }
    {
        VerifyRig r;
        populate(r);
        book::OrderTally t;
        (void)pool(r).alloc();  // pool grows with no book order to match
        EXPECT_TRUE(book::check_touched(r.b, t, 1, 7).has_value());
    }
    {
        VerifyRig r;
        populate(r);
        book::OrderTally t;
        auto&            l0 = bid(r, 0);
        std::swap(l0.price, bid(r, 1).price);  // best two levels out of order
        EXPECT_TRUE(book::check_touched(r.b, t, 1, 7).has_value());
    }
}

TEST(Invariants, VerifyObserverThrowsFromTheAddWithIndexAndLocate) {
    VerifyRig r;
    populate(r);  // messages 1..6
    ASSERT_EQ(r.obs.messages(), 6u);
    pool(r).at(bid(r, 0).head).pad = 99;  // the best bid's head now claims another book
    const auto m                   = itch::test::make_add(Hdr{1}, 7, 'S', 10, "TEST", 5300);
    try {
        (void)itch::decode(std::span<const std::byte>(m), r.b);
        FAIL() << "add did not throw";
    } catch (const book::VerificationFailure& e) {
        EXPECT_EQ(e.violation.msg_index, 7u);
        EXPECT_EQ(e.violation.locate, 1u);
        const std::string w = e.what();
        EXPECT_NE(w.find("message 7 locate 1"), std::string::npos) << w;
        EXPECT_NE(w.find("locate does not match"), std::string::npos) << w;
    }
    EXPECT_EQ(r.obs.messages(), 7u);
}

TEST(Invariants, PeriodicFullCheckRuns) {
    VerifyRig r(1024, {.full_every = 3});
    populate(r);
    EXPECT_EQ(r.obs.full_checks(), 1u);  // six messages, every 3: one check, before message 4
}

TEST(Invariants, CrossedInAuctionOnlyCountedAsOther) {
    VerifyRig r;
    r.add(1, 1, 'B', 100, 5100);
    r.add(1, 2, 'S', 100, 5000);  // bid above ask, but no market hours yet
    EXPECT_EQ(r.obs.counts().crossed_other, 1u);
    EXPECT_EQ(r.obs.counts().crossed_continuous, 0u);
    ASSERT_EQ(r.obs.oracle().exceptions(book::OracleKind::CrossedOther).size(), 1u);
    EXPECT_EQ(r.obs.oracle().exceptions(book::OracleKind::CrossedOther)[0].ref, 2u);
}

TEST(Invariants, CrossedInContinuousCounted) {
    VerifyRig r;
    r.open_market(1);
    r.add(1, 1, 'B', 100, 5100, 77);
    r.add(1, 2, 'S', 100, 5000, 88);
    EXPECT_EQ(r.obs.counts().crossed_continuous, 1u);
    EXPECT_EQ(r.obs.counts().crossed_other, 0u);
    const auto& ex = r.obs.oracle().exceptions(book::OracleKind::CrossedContinuous);
    ASSERT_EQ(ex.size(), 1u);
    EXPECT_EQ(ex[0].timestamp_ns, 88u);
    EXPECT_EQ(ex[0].locate, 1u);
}

TEST(Invariants, LockedBookIsNotCrossed) {
    VerifyRig r;
    r.open_market(1);
    r.add(1, 1, 'B', 100, 5000);
    r.add(1, 2, 'S', 100, 5000);
    EXPECT_EQ(r.obs.counts().crossed_continuous, 0u);
    EXPECT_EQ(r.obs.counts().crossed_other, 0u);
}

TEST(Invariants, CrossedBeforeAndAfterOpeningCrossOfNasdaqListed) {
    VerifyRig r;
    r.open_market(1, 'Q');  // Nasdaq-listed: awaits its O cross
    const auto& c = r.obs.counts();
    r.add(1, 1, 'B', 100, 5100);
    r.add(1, 2, 'S', 100, 5000);  // crossed, episode starts before the O cross
    r.add(1, 3, 'S', 100, 5050);  // still crossed: one more crossed touch, same episode
    EXPECT_EQ(c.touches_other, 3u);
    EXPECT_EQ(c.touches_continuous, 0u);
    EXPECT_EQ(c.crossed_other, 2u);
    EXPECT_EQ(c.episodes_other, 1u);
    r.feed(itch::test::make_cross(Hdr{1}, 1000, "TEST", 5050, 9, 'O'));
    r.feed(itch::test::make_delete(Hdr{1}, 2));
    // Order 2's delete leaves 5100 over 5050: still the same crossed episode, now continuous.
    EXPECT_EQ(c.crossed_continuous, 1u);
    EXPECT_EQ(c.episodes_continuous, 0u);
    r.feed(itch::test::make_delete(Hdr{1}, 3));  // uncrossed
    r.add(1, 4, 'S', 100, 5000);                 // crossed again, a new continuous episode
    EXPECT_EQ(c.touches_other, 3u);
    EXPECT_EQ(c.touches_continuous, 3u);
    EXPECT_EQ(c.crossed_continuous, 2u);
    EXPECT_EQ(c.episodes_continuous, 1u);
    EXPECT_EQ(c.episodes_other, 1u);
}
