// tests/test_snapshot.cpp
//
// SnapshotBoard: a single-writer seqlock per watched symbol. The torn-read and dead-reader
// tests are the ones to run under the tsan preset. Also the terminal renderer, which is a pure
// function of its inputs.

#include <gtest/gtest.h>

#include <atomic>
#include <book/builder.hpp>
#include <chrono>
#include <cstdint>
#include <itch/dispatch.hpp>
#include <replay/snapshot.hpp>
#include <string>
#include <thread>
#include <tui.hpp>

#include "support/itch_builder.hpp"

namespace {

using replay::SnapshotBoard;
using replay::SymbolSnapshot;

// Every field derives from k, so a snapshot mixed from two writes fails the check.
SymbolSnapshot pattern(std::uint64_t k) {
    SymbolSnapshot s{};
    s.locate = 7;
    for (int i = 0; i < replay::kDepth; ++i) {
        s.bid_px[i]  = static_cast<std::int32_t>(k + static_cast<std::uint64_t>(i));
        s.ask_px[i]  = static_cast<std::int32_t>(k + 100 + static_cast<std::uint64_t>(i));
        s.bid_qty[i] = k * 3 + static_cast<std::uint64_t>(i);
        s.ask_qty[i] = k * 5 + static_cast<std::uint64_t>(i);
    }
    s.last_trade_ts = k;
    return s;
}

bool consistent(const SymbolSnapshot& s) {
    const std::uint64_t k = s.last_trade_ts;
    if (s.locate != 7) {
        return false;
    }
    for (int i = 0; i < replay::kDepth; ++i) {
        const auto u = static_cast<std::uint64_t>(i);
        if (s.bid_px[i] != static_cast<std::int32_t>(k + u) ||
            s.ask_px[i] != static_cast<std::int32_t>(k + 100 + u) || s.bid_qty[i] != k * 3 + u ||
            s.ask_qty[i] != k * 5 + u) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST(Snapshot, ReaderNeverSeesTornSnapshot) {
    SnapshotBoard board;
    board.watch(7);
    std::atomic<bool> stop{false};
    std::thread       writer([&] {
        for (std::uint64_t k = 1; !stop.load(std::memory_order_relaxed); ++k) {
            board.store(0, pattern(k));
        }
    });
    std::uint64_t     good = 0, torn = 0;
    const auto        until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < until) {
        SymbolSnapshot s;
        if (!board.try_read(0, s)) {
            continue;  // a write was in progress: the reader retries
        }
        if (s.last_trade_ts == 0) {
            continue;  // the board's initial empty snapshot, before the first write
        }
        consistent(s) ? ++good : ++torn;
    }
    stop.store(true);
    writer.join();
    EXPECT_EQ(torn, 0u);
    EXPECT_GT(good, 0u);
}

TEST(Snapshot, DeadReaderDoesNotBlockWriter) {
    SnapshotBoard board;
    board.watch(7);
    // A reader that took one look and then went away mid-run, never reading again.
    std::thread reader([&] {
        SymbolSnapshot s;
        (void)board.try_read(0, s);
    });
    reader.join();
    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint64_t k = 1; k <= 200'000; ++k) {
        board.store(0, pattern(k));
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    SymbolSnapshot s;
    ASSERT_TRUE(board.try_read(0, s));
    EXPECT_EQ(s.last_trade_ts, 200'000u);
    EXPECT_LT(ms, 5000);  // generous even under tsan; a blocked writer would hang the test
}

TEST(Snapshot, OnlyWatchedSymbolsArePublished) {
    book::NullObserver                    obs;
    book::BookBuilder<book::NullObserver> b(64, obs);
    SnapshotBoard                         board;
    board.watch(1);
    ASSERT_EQ(board.index_of(1), 0);
    ASSERT_EQ(board.index_of(2), -1);
    auto feed = [&](const itch::test::Bytes& m) {
        ASSERT_EQ(itch::decode(std::span<const std::byte>(m), b), itch::DecodeStatus::Handled);
    };
    feed(itch::test::make_add(itch::test::Hdr{1}, 1, 'B', 100, "AAA", 50'000));
    feed(itch::test::make_add(itch::test::Hdr{2}, 2, 'B', 100, "BBB", 60'000));
    board.publish(b, 1, 123);
    board.publish(b, 2, 456);  // not watched: ignored
    SymbolSnapshot s;
    ASSERT_TRUE(board.try_read(0, s));
    EXPECT_EQ(s.locate, 1);
    EXPECT_EQ(s.bid_px[0], 50'000);
    EXPECT_EQ(s.bid_qty[0], 100u);
    EXPECT_EQ(s.last_trade_ts, 123u);
    EXPECT_EQ(board.size(), 1u);
}

TEST(Snapshot, LevelsAreBestFirstAndShortSidesAreZero) {
    book::NullObserver                    obs;
    book::BookBuilder<book::NullObserver> b(64, obs);
    auto                                  feed = [&](const itch::test::Bytes& m) {
        ASSERT_EQ(itch::decode(std::span<const std::byte>(m), b), itch::DecodeStatus::Handled);
    };
    feed(itch::test::make_add(itch::test::Hdr{1}, 1, 'B', 10, "AAA", 100));
    feed(itch::test::make_add(itch::test::Hdr{1}, 2, 'B', 20, "AAA", 102));
    feed(itch::test::make_add(itch::test::Hdr{1}, 3, 'B', 30, "AAA", 102));
    feed(itch::test::make_add(itch::test::Hdr{1}, 4, 'S', 5, "AAA", 110));
    const SymbolSnapshot s = replay::make_snapshot(b.book(1), 1, 0);
    EXPECT_EQ(s.bid_px[0], 102);
    EXPECT_EQ(s.bid_qty[0], 50u);
    EXPECT_EQ(s.bid_px[1], 100);
    EXPECT_EQ(s.bid_qty[1], 10u);
    EXPECT_EQ(s.bid_qty[2], 0u);
    EXPECT_EQ(s.ask_px[0], 110);
    EXPECT_EQ(s.ask_qty[1], 0u);
}

TEST(Tui, RenderFitsTheTerminalAndShowsPricesAsDollars) {
    SymbolSnapshot s{};
    s.locate     = 1;
    s.bid_px[0]  = 2'890'100;  // $289.0100
    s.bid_qty[0] = 300;
    s.ask_px[0]  = 2'890'500;
    s.ask_qty[0] = 200;
    replay::tui::View v;
    v.names               = {"AAPL"};
    v.snaps               = {s};
    v.selected            = 0;
    v.feed_ns             = (9ULL * 3600 + 30 * 60) * 1'000'000'000ULL;
    v.messages            = 1234567;
    v.speed               = 10.0;
    const std::string out = replay::tui::render(v, 24, 100);
    EXPECT_NE(out.find("AAPL"), std::string::npos);
    EXPECT_NE(out.find("289.0100"), std::string::npos);
    EXPECT_NE(out.find("289.0500"), std::string::npos);
    EXPECT_NE(out.find("09:30:00"), std::string::npos);
    std::size_t lines = 0;
    for (char c : out) {
        lines += c == '\n';
    }
    EXPECT_LE(lines, 24u);
    EXPECT_EQ(replay::tui::render(v, 2, 10), "terminal too small\n");
}

TEST(Tui, OneSidedBookIsShownAsEmptyNotZero) {
    SymbolSnapshot s{};
    s.locate     = 1;
    s.bid_px[0]  = 1'000'000;
    s.bid_qty[0] = 10;
    replay::tui::View v;
    v.names               = {"ZZZ"};
    v.snaps               = {s};
    v.selected            = 0;
    const std::string out = replay::tui::render(v, 24, 100);
    EXPECT_NE(out.find("one side empty"), std::string::npos);
}
