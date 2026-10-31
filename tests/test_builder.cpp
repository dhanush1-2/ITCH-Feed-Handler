// tests/test_builder.cpp
//
// BookBuilder: ITCH apply semantics and counted anomalies, driven through itch::decode so
// the same entry point the CLI uses is what is tested.

#include <gtest/gtest.h>

#include <book/builder.hpp>
#include <book/state_dump.hpp>
#include <cstdint>
#include <itch/dispatch.hpp>
#include <sstream>
#include <string>

#include "support/itch_builder.hpp"

namespace {

using book::Anomalies;
using book::BookBuilder;
using book::CapacityExceeded;
using book::NullObserver;
using itch::test::Bytes;
using itch::test::Hdr;

struct Rig {
    NullObserver              obs;
    BookBuilder<NullObserver> b;
    explicit Rig(std::size_t cap = 1024) : b(cap, obs) {}

    void feed(const Bytes& m) {
        const auto st = itch::decode(std::span<const std::byte>(m), b);
        ASSERT_EQ(st, itch::DecodeStatus::Handled);
    }
    void add(std::uint16_t loc, std::uint64_t ref, char side, std::uint32_t sh, std::uint32_t px) {
        feed(itch::test::make_add(Hdr{loc}, ref, side, sh, "TEST", px));
    }
    std::string dump() const {
        std::ostringstream os;
        book::write_state(os, b);
        return os.str();
    }
};

constexpr Hdr H1{1};

}  // namespace

TEST(Builder, AddThenDeleteLeavesEmpty) {
    Rig r;
    r.add(1, 1, 'B', 100, 5000);
    EXPECT_EQ(r.b.live_orders(), 1u);
    r.feed(itch::test::make_delete(H1, 1));
    EXPECT_EQ(r.b.live_orders(), 0u);
    EXPECT_EQ(r.b.book(1).level_count(ob::Side::Buy), 0u);
    EXPECT_TRUE(r.dump().empty());
    EXPECT_EQ(r.b.peak_live_orders(), 1u);
}

TEST(Builder, FIFOOrderPreserved) {
    Rig r;
    r.add(1, 1, 'B', 10, 5000);
    r.add(1, 2, 'B', 10, 5000);
    r.add(1, 3, 'B', 10, 5000);
    const std::string d = r.dump();
    ASSERT_EQ(d.size(), 3u * 19u);
    // ref is bytes 7..14 of each 19-byte record: locate2 side1 price4 ref8 shares4.
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(static_cast<unsigned char>(d[i * 19 + 14]), i + 1);
    }
}

TEST(Builder, PartialExecuteKeepsPriority) {
    Rig r;
    r.add(1, 1, 'B', 100, 5000);
    r.add(1, 2, 'B', 100, 5000);
    r.feed(itch::test::make_executed(H1, 1, 40, 77));
    const auto& lv = r.b.book(1).best_level(ob::Side::Buy);
    EXPECT_EQ(lv.total, 160u);
    EXPECT_EQ(lv.count, 2u);
    EXPECT_EQ(r.b.live_orders(), 2u);
    const std::string d = r.dump();
    EXPECT_EQ(static_cast<unsigned char>(d[14]), 1u);   // ref 1 is still first
    EXPECT_EQ(static_cast<unsigned char>(d[18]), 60u);  // 60 shares left
}

TEST(Builder, FullExecuteImplicitDelete) {
    Rig r;
    r.add(1, 1, 'S', 100, 5000);
    r.feed(itch::test::make_executed(H1, 1, 100, 77));
    EXPECT_EQ(r.b.live_orders(), 0u);
    EXPECT_EQ(r.b.book(1).level_count(ob::Side::Sell), 0u);
    EXPECT_EQ(r.b.anomalies().over_execute, 0u);
}

TEST(Builder, CancelIsPartial) {
    Rig r;
    r.add(1, 1, 'B', 100, 5000);
    r.feed(itch::test::make_cancel(H1, 1, 30));
    EXPECT_EQ(r.b.live_orders(), 1u);
    EXPECT_EQ(r.b.book(1).bids().front().total, 70u);
}

TEST(Builder, ReplaceLosesPriority) {
    Rig r;
    r.add(1, 1, 'B', 10, 5000);
    r.add(1, 2, 'B', 10, 5000);
    r.feed(itch::test::make_replace(H1, 1, 3, 10, 5000));
    const std::string d = r.dump();
    ASSERT_EQ(d.size(), 2u * 19u);
    EXPECT_EQ(static_cast<unsigned char>(d[14]), 2u);
    EXPECT_EQ(static_cast<unsigned char>(d[19 + 14]), 3u);
    r.feed(itch::test::make_delete(H1, 1));  // old ref is gone
    EXPECT_EQ(r.b.anomalies().unknown_ref, 1u);
    EXPECT_EQ(r.b.live_orders(), 2u);
}

TEST(Builder, ReplaceKeepsSideAndLocate) {
    Rig r;
    r.add(7, 1, 'S', 10, 5000);
    r.feed(itch::test::make_replace(Hdr{7}, 1, 2, 25, 5100));
    EXPECT_EQ(r.b.book(7).level_count(ob::Side::Sell), 1u);
    EXPECT_EQ(r.b.book(7).level_count(ob::Side::Buy), 0u);
    EXPECT_EQ(r.b.book(7).best_ask(), 5100);
    EXPECT_EQ(r.b.book(7).asks().front().total, 25u);
    EXPECT_EQ(r.b.anomalies().locate_mismatch, 0u);
}

TEST(Builder, CExecutesAtRestingPriceNotPrintPrice) {
    Rig r;
    r.add(1, 1, 'B', 100, 100);
    r.feed(itch::test::make_executed_price(H1, 1, 30, 9, 'Y', 101));
    EXPECT_EQ(r.b.book(1).best_bid(), 100);
    EXPECT_EQ(r.b.book(1).level_count(ob::Side::Buy), 1u);
    EXPECT_EQ(r.b.book(1).bids().front().total, 70u);
}

TEST(Builder, TradeCrossBrokenDoNotTouchBook) {
    Rig r;
    r.add(1, 1, 'B', 100, 5000);
    r.add(1, 2, 'S', 100, 5100);
    const std::string before = r.dump();
    r.feed(itch::test::make_trade(H1, 0, 'B', 50, "TEST", 5000, 1));
    r.feed(itch::test::make_cross(H1, 1000, "TEST", 5000, 2, 'O'));
    r.feed(itch::test::make_broken(H1, 1));
    EXPECT_EQ(r.dump(), before);
    EXPECT_EQ(r.b.live_orders(), 2u);
}

TEST(Builder, UnknownRefCountedNotFabricated) {
    Rig r;
    r.feed(itch::test::make_executed(H1, 999, 10, 1));
    r.feed(itch::test::make_cancel(H1, 999, 10));
    r.feed(itch::test::make_delete(H1, 999));
    r.feed(itch::test::make_replace(H1, 999, 1000, 10, 5000));
    EXPECT_EQ(r.b.anomalies().unknown_ref, 4u);
    EXPECT_TRUE(r.dump().empty());
    EXPECT_EQ(r.b.live_orders(), 0u);
}

TEST(Builder, DuplicateLiveAddCounted) {
    Rig r;
    r.add(1, 1, 'B', 100, 5000);
    r.add(1, 1, 'S', 7, 5200);
    EXPECT_EQ(r.b.anomalies().duplicate_add, 1u);
    EXPECT_EQ(r.b.live_orders(), 1u);
    EXPECT_EQ(r.b.book(1).bids().front().total, 100u);
    EXPECT_EQ(r.b.book(1).level_count(ob::Side::Sell), 0u);
}

TEST(Builder, RefReusableAfterDelete) {
    Rig r;
    r.add(1, 1, 'B', 100, 5000);
    r.feed(itch::test::make_delete(H1, 1));
    r.add(1, 1, 'B', 50, 5000);
    EXPECT_EQ(r.b.live_orders(), 1u);
    EXPECT_EQ(r.b.anomalies().duplicate_add, 0u);
}

TEST(Builder, OverExecuteClampedAndCounted) {
    Rig r;
    r.add(1, 1, 'B', 100, 5000);
    r.feed(itch::test::make_executed(H1, 1, 150, 3));
    EXPECT_EQ(r.b.live_orders(), 0u);
    EXPECT_EQ(r.b.anomalies().over_execute, 1u);
    EXPECT_EQ(r.b.book(1).level_count(ob::Side::Buy), 0u);
}

TEST(Builder, OverCancelClampedAndCounted) {
    Rig r;
    r.add(1, 1, 'B', 100, 5000);
    r.add(1, 2, 'B', 40, 5000);
    r.feed(itch::test::make_cancel(H1, 1, 101));
    EXPECT_EQ(r.b.anomalies().over_cancel, 1u);
    EXPECT_EQ(r.b.live_orders(), 1u);
    EXPECT_EQ(r.b.book(1).bids().front().total, 40u);
}

TEST(Builder, ZeroSharesCounted) {
    Rig r;
    r.add(1, 1, 'B', 0, 5000);
    EXPECT_EQ(r.b.anomalies().zero_shares, 1u);
    EXPECT_EQ(r.b.live_orders(), 0u);
    r.add(1, 2, 'B', 10, 5000);
    r.feed(itch::test::make_cancel(H1, 2, 0));
    EXPECT_EQ(r.b.anomalies().zero_shares, 2u);
    EXPECT_EQ(r.b.book(1).bids().front().total, 10u);
}

TEST(Builder, LocateMismatchCounted) {
    Rig r;
    r.add(1, 5, 'B', 100, 5000);
    r.feed(itch::test::make_delete(Hdr{2}, 5));
    EXPECT_EQ(r.b.anomalies().locate_mismatch, 1u);
    EXPECT_EQ(r.b.book(1).level_count(ob::Side::Buy), 0u);
    EXPECT_EQ(r.b.live_orders(), 0u);
}

TEST(Builder, CapacityIsFatal) {
    Rig r(2);
    r.add(1, 1, 'B', 10, 5000);
    r.add(1, 2, 'B', 10, 5001);
    const auto m = itch::test::make_add(H1, 3, 'B', 10, "TEST", 5002);
    static_assert(!noexcept(itch::decode(std::span<const std::byte>(m), r.b)));
    try {
        (void)itch::decode(std::span<const std::byte>(m), r.b);
        FAIL() << "expected CapacityExceeded";
    } catch (const CapacityExceeded& e) {
        const std::string w = e.what();
        EXPECT_NE(w.find("--max-live-orders"), std::string::npos);
        EXPECT_NE(w.find('2'), std::string::npos);
    }
    EXPECT_EQ(r.b.live_orders(), 2u);  // nothing was mutated
    EXPECT_EQ(r.b.book(1).level_count(ob::Side::Buy), 2u);
}

TEST(Builder, ReplaceAtFullCapacityDoesNotThrow) {
    Rig r(2);
    r.add(1, 1, 'B', 10, 5000);
    r.add(1, 2, 'B', 10, 5001);
    r.feed(itch::test::make_replace(H1, 1, 3, 10, 5002));
    EXPECT_EQ(r.b.live_orders(), 2u);
}

namespace {
struct CountingObserver {
    int           passes          = 0;
    int           before          = 0;
    int           after           = 0;
    std::uint32_t last_shares     = 0;
    std::uint32_t last_book_total = 0;
    template <class V>
    void pass(const V&) noexcept {
        ++passes;
    }
    void before_execute(const book::Book& bk, const ob::Order&, ob::Side, std::uint32_t shares,
                        std::uint64_t, std::uint32_t) noexcept {
        ++before;
        last_shares     = shares;
        last_book_total = static_cast<std::uint32_t>(bk.bids().front().total);
    }
    void after_apply(std::uint16_t) noexcept { ++after; }
};
}  // namespace

TEST(Builder, ObserverHooks) {
    CountingObserver              obs;
    BookBuilder<CountingObserver> b(16, obs);
    auto go = [&](const Bytes& m) { (void)itch::decode(std::span<const std::byte>(m), b); };
    go(itch::test::make_add(H1, 1, 'B', 100, "TEST", 5000));
    go(itch::test::make_executed(H1, 1, 40, 1));  // book still holds 100 before the hook
    go(itch::test::make_replace(H1, 1, 2, 60, 5000));
    go(itch::test::make_trade(H1, 0, 'B', 1, "TEST", 5000, 1));
    EXPECT_EQ(obs.passes, 4);
    EXPECT_EQ(obs.before, 1);
    EXPECT_EQ(obs.last_shares, 40u);
    EXPECT_EQ(obs.last_book_total, 100u);
    EXPECT_EQ(obs.after, 3);  // A, E, U: one each; P none
}

TEST(StateDump, IndependentOfSlotHistory) {
    Rig direct;
    direct.add(1, 10, 'B', 5, 100);
    direct.add(1, 11, 'B', 6, 100);
    direct.add(1, 12, 'S', 7, 110);
    direct.add(2, 13, 'B', 8, 90);

    Rig churned;
    for (std::uint64_t i = 100; i < 140; ++i) {
        churned.add(3, i, i % 2 ? 'B' : 'S', 1, static_cast<std::uint32_t>(50 + i));
    }
    churned.add(2, 13, 'B', 8, 90);
    for (std::uint64_t i = 100; i < 140; ++i) {
        churned.feed(itch::test::make_delete(Hdr{3}, i));
    }
    churned.add(1, 12, 'S', 7, 110);
    churned.add(1, 10, 'B', 5, 100);
    churned.add(1, 11, 'B', 6, 100);

    EXPECT_FALSE(direct.dump().empty());
    EXPECT_EQ(direct.dump(), churned.dump());
}

TEST(StateDump, RecordLayoutIsBigEndian) {
    Rig r;
    r.add(0x0102, 0x1112131415161718ULL, 'S', 0x21222324, 0x31323334);
    const std::string d = r.dump();
    ASSERT_EQ(d.size(), 19u);
    const unsigned char expect[19] = {0x01, 0x02, 1,    0x31, 0x32, 0x33, 0x34, 0x11, 0x12, 0x13,
                                      0x14, 0x15, 0x16, 0x17, 0x18, 0x21, 0x22, 0x23, 0x24};
    for (std::size_t i = 0; i < 19; ++i) {
        EXPECT_EQ(static_cast<unsigned char>(d[i]), expect[i]) << i;
    }
}

// A replace's new reference is placed by number too, and the dump shows that order whatever the
// slot history was.
TEST(StateDump, RefOrderIndependentOfSlotHistory) {
    const auto feed = [](bool churn) {
        Rig r;
        if (churn) {
            r.feed(make_add(H1, 900, 'B', 1, "TEST", 7000));
            r.feed(make_add(H1, 901, 'B', 1, "TEST", 7000));
            r.feed(make_delete(H1, 900));
            r.feed(make_delete(H1, 901));
        }
        r.feed(make_add(H1, 30, 'B', 100, "TEST", 5000));
        r.feed(make_add(H1, 10, 'B', 100, "TEST", 5000));
        r.feed(make_add(H1, 50, 'B', 100, "TEST", 4000));
        r.feed(make_replace(H1, 50, 20, 100, 5000));
        return r.dump();
    };
    const std::string plain = feed(false);
    EXPECT_EQ(plain, feed(true));
    ASSERT_EQ(plain.size(), 3u * 19u);
    const auto ref_at = [&](std::size_t i) {
        return static_cast<unsigned char>(plain[i * 19 + 14]);  // low byte of the u64 ref
    };
    EXPECT_EQ(ref_at(0), 10);
    EXPECT_EQ(ref_at(1), 20);
    EXPECT_EQ(ref_at(2), 30);
}
