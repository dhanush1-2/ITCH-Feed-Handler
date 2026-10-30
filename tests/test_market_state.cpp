// tests/test_market_state.cpp
//
// MarketState: when is a symbol in continuous trading?

#include <book/market_state.hpp>
#include <cstdint>
#include <gtest/gtest.h>
#include "support/itch_builder.hpp"

namespace {

using book::MarketState;
using itch::test::Bytes;
using itch::test::Hdr;

constexpr std::uint64_t kSec = 1'000'000'000ULL;

void sys(MarketState& ms, char code, std::uint64_t ts = 0) {
    const Bytes m = itch::test::make_system_event(Hdr{0, 0, ts}, code);
    ms.on(itch::SystemEventView(m.data()));
}
void dir(MarketState& ms, std::uint16_t loc, char category, std::uint64_t ts = 0) {
    const Bytes m = itch::test::make_stock_directory(Hdr{loc, 0, ts}, "TEST", category, ' ', 100, {});
    ms.on(itch::StockDirectoryView(m.data()));
}
void action(MarketState& ms, std::uint16_t loc, char state, std::uint64_t ts = 0) {
    const Bytes m = itch::test::make_trading_action(Hdr{loc, 0, ts}, "TEST", state, ' ', "    ");
    ms.on(itch::TradingActionView(m.data()));
}
void cross(MarketState& ms, std::uint16_t loc, char type, std::uint64_t ts = 0) {
    const Bytes m = itch::test::make_cross(Hdr{loc, 0, ts}, 100, "TEST", 5000, 1, type);
    ms.on(itch::CrossTradeView(m.data()));
}

}  // namespace

TEST(MarketState, PreMarketNotContinuous) {
    MarketState ms;
    dir(ms, 1, 'N');
    EXPECT_FALSE(ms.continuous(1));
    sys(ms, 'O');
    sys(ms, 'S');
    EXPECT_FALSE(ms.continuous(1));
    sys(ms, 'Q');
    EXPECT_TRUE(ms.continuous(1));
    sys(ms, 'M');
    EXPECT_FALSE(ms.continuous(1));
}

TEST(MarketState, AwaitsOpeningCrossForNasdaqListed) {
    for (const char cat : {'Q', 'G', 'S'}) {
        MarketState ms;
        dir(ms, 2, cat);
        sys(ms, 'Q');
        EXPECT_FALSE(ms.continuous(2)) << cat;
        cross(ms, 2, 'C');  // the wrong cross does not release it
        EXPECT_FALSE(ms.continuous(2)) << cat;
        cross(ms, 2, 'O');
        EXPECT_TRUE(ms.continuous(2)) << cat;
        cross(ms, 3, 'O');  // another symbol's cross is irrelevant
    }
}

TEST(MarketState, NonNasdaqListedContinuousAtOpen) {
    MarketState ms;
    dir(ms, 1, 'N');  // NYSE
    dir(ms, 2, 'A');  // NYSE American
    sys(ms, 'Q');
    EXPECT_TRUE(ms.continuous(1));
    EXPECT_TRUE(ms.continuous(2));
}

TEST(MarketState, HaltThenResumeRespectsGrace) {
    MarketState ms;  // default grace 5 s
    dir(ms, 1, 'N');
    sys(ms, 'Q');
    action(ms, 1, 'H', 10 * kSec);
    EXPECT_FALSE(ms.continuous(1));
    action(ms, 1, 'T', 20 * kSec);
    EXPECT_FALSE(ms.continuous(1));  // resumed, still inside the grace
    ms.advance(24 * kSec + 999'999'999ULL);
    EXPECT_FALSE(ms.continuous(1));
    ms.advance(25 * kSec);
    EXPECT_TRUE(ms.continuous(1));
}

TEST(MarketState, HaltCrossEndsTheWaitEarlyAndGraceIsConfigurable) {
    MarketState ms;
    dir(ms, 1, 'N');
    sys(ms, 'Q');
    action(ms, 1, 'H', 10 * kSec);
    action(ms, 1, 'T', 20 * kSec);
    EXPECT_FALSE(ms.continuous(1));
    cross(ms, 1, 'H', 21 * kSec);
    EXPECT_TRUE(ms.continuous(1));

    MarketState quick(kSec);
    dir(quick, 1, 'N');
    sys(quick, 'Q');
    action(quick, 1, 'P', 10 * kSec);
    action(quick, 1, 'T', 20 * kSec);
    quick.advance(21 * kSec);
    EXPECT_TRUE(quick.continuous(1));
}

TEST(MarketState, HaltBeforeDirectoryIsSafe) {
    MarketState ms;
    action(ms, 9, 'H', kSec);  // no R for locate 9 yet
    sys(ms, 'Q', 2 * kSec);
    EXPECT_FALSE(ms.continuous(9));
    dir(ms, 9, 'N', 3 * kSec);  // the directory arrives later; the halt stays
    EXPECT_FALSE(ms.continuous(9));
    action(ms, 9, 'T', 4 * kSec);
    ms.advance(10 * kSec);
    EXPECT_TRUE(ms.continuous(9));
    (void)ms.continuous(65535);  // edge of the locate array: in range, no crash
}
