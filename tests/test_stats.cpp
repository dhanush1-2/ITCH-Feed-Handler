// tests/test_stats.cpp
//
// StatsObserver: the share ledger, printed volume, the broken-trade rule, match-number
// collisions, the one-sided export, and the SHA-256 used to stamp the state dump.

#include <gtest/gtest.h>

#include <book/builder.hpp>
#include <book/invariants.hpp>
#include <book/stats.hpp>
#include <itch/dispatch.hpp>
#include <itch/scan.hpp>
#include <itch/sha256.hpp>
#include <sstream>
#include <string>

#include "support/itch_builder.hpp"

namespace {
using book::BookBuilder;
using book::StatsObserver;
using itch::test::Hdr;

constexpr Hdr H1{1, 0, 5'000'000'000};  // feed second 5

struct StatsRig {
    StatsObserver              st;
    BookBuilder<StatsObserver> b;
    explicit StatsRig(std::size_t cap = 256) : b(cap, st) { st.bind(b); }
    void feed(const itch::test::Bytes& m) {
        st.on_frame(m);
        ASSERT_EQ(itch::decode(std::span<const std::byte>(m), b), itch::DecodeStatus::Handled);
    }
    void add(std::uint64_t ref, char side, std::uint32_t sh, std::uint32_t px, const Hdr& h = H1) {
        feed(itch::test::make_add(h, ref, side, sh, "TEST", px));
    }
};
}  // namespace

TEST(Stats, ConservationHolds) {
    StatsRig r;
    r.add(1, 'B', 100, 5000);
    r.add(2, 'B', 50, 4990);
    r.add(3, 'S', 70, 5010);
    r.feed(itch::test::make_executed(H1, 1, 30, 1));                    // partial execute
    r.feed(itch::test::make_cancel(H1, 1, 20));                         // partial cancel
    r.feed(itch::test::make_delete(H1, 2));                             // delete whole
    r.feed(itch::test::make_replace(H1, 3, 4, 90, 5020));               // replaced out 70, in 90
    r.feed(itch::test::make_executed_price(H1, 4, 500, 2, 'Y', 5020));  // over-execute, clamped
    const auto& s = r.st.symbol(1);
    EXPECT_EQ(s.added, 220u);
    EXPECT_EQ(s.replaced_in, 90u);
    EXPECT_EQ(s.executed, 30u + 90u);
    EXPECT_EQ(s.cancelled, 20u);
    EXPECT_EQ(s.deleted, 50u);
    EXPECT_EQ(s.replaced_out, 70u);
    EXPECT_EQ(book::live_shares(r.b, 1), 50u);  // the rest of order 1
    EXPECT_TRUE(book::conservation_failures(r.b, r.st).empty());
    EXPECT_EQ(r.b.anomalies().over_execute, 1u);
}

TEST(Stats, ConservationDetectsAnUnaccountedChange) {
    StatsRig r;
    r.add(1, 'B', 100, 5000);
    // A second tally that missed a removal must disagree with the book.
    r.st.added(1, 25, false);
    const auto bad = book::conservation_failures(r.b, r.st);
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_EQ(bad[0].locate, 1u);
    EXPECT_EQ(bad[0].imbalance, 25);
}

TEST(Stats, NonPrintableExcludedFromVolume) {
    StatsRig r;
    r.add(1, 'S', 1000, 5000);
    r.feed(itch::test::make_executed(H1, 1, 100, 1));
    r.feed(itch::test::make_executed_price(H1, 1, 200, 2, 'Y', 5000));
    r.feed(itch::test::make_executed_price(H1, 1, 300, 3, 'N', 5000));
    r.feed(itch::test::make_trade(H1, 0, 'B', 40, "TEST", 5000, 4));
    r.feed(itch::test::make_cross(H1, 1000, "TEST", 5000, 5, 'C'));
    r.st.finish();
    EXPECT_EQ(r.st.printed_volume(), 100u + 200u + 40u + 1000u);
    EXPECT_EQ(r.st.trade_count(), 4u);
    EXPECT_EQ(r.st.nonprintable_prints(), 1u);
    EXPECT_EQ(r.st.volume_of('C'), 200u);
    // The non-printable C still removed liquidity from the book.
    EXPECT_EQ(r.st.symbol(1).executed, 600u);
}

TEST(Stats, BrokenTradeSubtracted) {
    // The B arrives AFTER the trade it breaks, so its match number is found by a pre-scan.
    itch::test::Bytes file;
    auto push = [&](const itch::test::Bytes& m) { itch::test::append(file, itch::test::frame(m)); };
    push(itch::test::make_add(H1, 1, 'S', 1000, "TEST", 5000));
    push(itch::test::make_executed(H1, 1, 100, 77));
    push(itch::test::make_trade(H1, 0, 'B', 40, "TEST", 5000, 78));
    push(itch::test::make_broken(H1, 77));
    push(itch::test::make_broken(H1, 999));  // names no print
    const auto broken = itch::scan_broken(std::span<const std::byte>(file));
    ASSERT_EQ(broken.size(), 2u);

    StatsRig r;
    r.st.set_broken_matches(broken);
    r.add(1, 'S', 1000, 5000);
    r.feed(itch::test::make_executed(H1, 1, 100, 77));
    r.feed(itch::test::make_trade(H1, 0, 'B', 40, "TEST", 5000, 78));
    r.feed(itch::test::make_broken(H1, 77));
    r.feed(itch::test::make_broken(H1, 999));
    r.st.finish();
    EXPECT_EQ(r.st.printed_volume(), 40u);
    EXPECT_EQ(r.st.trade_count(), 1u);
    EXPECT_EQ(r.st.broken_prints(), 1u);
    EXPECT_EQ(r.st.broken_shares(), 100u);
    EXPECT_EQ(r.st.broken_messages(), 2u);
    EXPECT_EQ(r.st.broken_without_print(), 1u);
    // The broken trade does not undo the execution in the book: the exchange removed it.
    EXPECT_EQ(r.st.symbol(1).executed, 100u);
}

TEST(Stats, MatchNumberCollisionCounted) {
    StatsRig r;
    r.add(1, 'S', 1000, 5000);
    r.feed(itch::test::make_executed(H1, 1, 100, 5));
    r.feed(itch::test::make_trade(H1, 0, 'B', 40, "TEST", 5000, 5));  // same match, E and P
    r.feed(itch::test::make_executed(H1, 1, 10, 6));
    r.feed(itch::test::make_executed(H1, 1, 10, 6));  // same match, E and E
    r.feed(itch::test::make_executed(H1, 1, 10, 7));
    r.st.finish();
    EXPECT_EQ(r.st.match_collisions(), 2u);
    EXPECT_EQ(r.st.match_extra_prints(), 2u);
    EXPECT_EQ(r.st.match_collisions_cross_kind(), 1u);
}

TEST(Stats, ExportOneSidedBook) {
    StatsRig r;
    r.st.watch("TEST", 1);
    r.add(1, 'B', 100, 5000, Hdr{1, 0, 1'000'000'000});            // bids only: second 1
    r.add(2, 'S', 60, 5010, Hdr{1, 0, 2'000'000'000});             // both sides: second 2
    r.feed(itch::test::make_delete(Hdr{1, 0, 3'000'000'000}, 1));  // asks only: second 3
    std::ostringstream os;
    book::write_watch_json(os, r.st, "");
    const std::string j = os.str();
    EXPECT_NE(j.find("\"t\": [1,2,3]"), std::string::npos) << j;
    EXPECT_NE(j.find("\"bid\": [5000,5000,null]"), std::string::npos) << j;
    EXPECT_NE(j.find("\"ask\": [null,5010,5010]"), std::string::npos) << j;
    EXPECT_NE(j.find("\"bid_sz\": [100,100,null]"), std::string::npos) << j;
    EXPECT_NE(j.find("\"ask_sz\": [null,60,60]"), std::string::npos) << j;
    EXPECT_NE(j.find("\"spread\": [null,10,null]"), std::string::npos) << j;
}

TEST(Stats, PerSecondAndTypeCounts) {
    StatsRig r;
    r.add(1, 'B', 100, 5000, Hdr{1, 0, 2'500'000'000});
    r.add(2, 'B', 100, 5000, Hdr{1, 0, 2'900'000'000});
    r.feed(itch::test::make_system_event(Hdr{0, 0, 3'000'000'000}, 'Q'));
    EXPECT_EQ(r.st.per_second()[2], 2u);
    EXPECT_EQ(r.st.per_second()[3], 1u);
    EXPECT_EQ(r.st.count_of_type('A'), 2u);
    EXPECT_EQ(r.st.symbol(1).messages, 2u);
    EXPECT_EQ(r.st.last_system_event(), 'Q');
}

TEST(Stats, WorksAsExtraOfTheVerifier) {
    using Obs = book::BasicVerifyObserver<StatsObserver>;
    Obs              obs(book::VerifyObserver::Options{});
    BookBuilder<Obs> b(64, obs);
    obs.bind(b);
    obs.extra().bind(b);
    auto go = [&](const itch::test::Bytes& m) {
        ASSERT_EQ(itch::decode(std::span<const std::byte>(m), b), itch::DecodeStatus::Handled);
    };
    go(itch::test::make_add(H1, 1, 'B', 100, "TEST", 5000));
    go(itch::test::make_executed(H1, 1, 30, 1));
    go(itch::test::make_delete(H1, 1));
    obs.finish();
    EXPECT_EQ(obs.extra().symbol(1).added, 100u);
    EXPECT_EQ(obs.extra().symbol(1).executed, 30u);
    EXPECT_EQ(obs.extra().symbol(1).deleted, 70u);
    EXPECT_EQ(obs.counts().exec_checked, 1u);
}

TEST(Sha256, KnownVectors) {
    itch::Sha256 h;
    h.update("abc", 3);
    EXPECT_EQ(h.hex(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    h.reset();
    EXPECT_EQ(h.hex(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    h.reset();
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";  // 56 bytes
    h.update(two.data(), two.size());
    EXPECT_EQ(h.hex(), "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    h.reset();
    const std::string million(1'000'000, 'a');
    h.update(million.data(), 400'000);  // split across calls and blocks
    h.update(million.data(), 600'000);
    EXPECT_EQ(h.hex(), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Scan, ResolveSymbolsNormalizesAndReportsUnknown) {
    itch::test::Bytes file;
    auto push = [&](const itch::test::Bytes& m) { itch::test::append(file, itch::test::frame(m)); };
    push(itch::test::make_stock_directory(Hdr{1}, "AAPL", 'Q', ' ', 100, {}));
    push(itch::test::make_stock_directory(Hdr{2}, "BRK.B", 'N', ' ', 100, {}));
    const auto r = itch::resolve_symbols(std::span<const std::byte>(file),
                                         {itch::normalize_symbol(" aapl "), "AAPLL"});
    ASSERT_EQ(r.locate.size(), 1u);
    EXPECT_EQ(r.locate.at("AAPL"), 1u);
    ASSERT_EQ(r.unresolved.size(), 1u);
    EXPECT_EQ(r.unresolved[0], "AAPLL");
}

// Spec 1.5.2: a cross with insufficient interest may show zero shares. No trade happened, so it
// must not count as one. Found by the Python reference: 12,189 of the day's 17,836 Q messages.
TEST(Stats, ZeroShareCrossIsNotATrade) {
    StatsRig r;
    r.feed(itch::test::make_cross(H1, 0, "TEST", 5000, 77, 'O'));
    EXPECT_EQ(r.st.trade_count(), 0u);
    EXPECT_EQ(r.st.symbol(1).trades, 0u);
    r.feed(itch::test::make_cross(H1, 10, "TEST", 5000, 78, 'C'));
    EXPECT_EQ(r.st.trade_count(), 1u);
    EXPECT_EQ(r.st.printed_volume(), 10u);
}
