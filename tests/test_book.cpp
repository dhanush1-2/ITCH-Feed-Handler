// tests/test_book.cpp
//
// Book alone: price-level storage, ordering, and the lookup path. The apply logic is
// covered in test_builder.cpp.

#include <gtest/gtest.h>

#include <book/book.hpp>
#include <cstdint>
#include <ob/order_pool.hpp>
#include <optional>
#include <vector>

namespace {

using book::Book;
using ob::Side;
using ob::Slot;

struct Fixture {
    ob::OrderPool pool{4096};
    Book          b;
    std::uint64_t next_id = 1;

    Slot add(Side side, std::int32_t price, std::uint32_t qty) {
        const Slot s = pool.alloc();
        auto&      o = pool.at(s);
        o.id         = next_id++;
        o.price      = price;
        o.remaining  = qty;
        o.side       = side;
        b.add(pool, s);
        return s;
    }
};

std::vector<std::int32_t> prices(const auto& levels) {
    std::vector<std::int32_t> out;
    for (const auto& l : levels) {
        out.push_back(l.price);
    }
    return out;
}

}  // namespace

TEST(Book, BidsAndAsksBestIsCorrect) {
    Fixture f;
    EXPECT_EQ(f.b.best_bid(), std::nullopt);
    EXPECT_EQ(f.b.best_ask(), std::nullopt);
    f.add(Side::Buy, 100, 1);
    f.add(Side::Buy, 102, 1);
    f.add(Side::Buy, 101, 1);
    f.add(Side::Sell, 105, 1);
    f.add(Side::Sell, 103, 1);
    f.add(Side::Sell, 104, 1);
    EXPECT_EQ(f.b.best_bid(), 102);
    EXPECT_EQ(f.b.best_ask(), 103);
    EXPECT_EQ(prices(f.b.bids()), (std::vector<std::int32_t>{102, 101, 100}));
    EXPECT_EQ(prices(f.b.asks()), (std::vector<std::int32_t>{103, 104, 105}));
    EXPECT_EQ(f.b.level_count(Side::Buy), 3u);
    EXPECT_EQ(f.b.level_count(Side::Sell), 3u);
}

TEST(Book, FarStubThenTouch) {
    Fixture f;
    f.add(Side::Buy, 1, 10);
    f.add(Side::Sell, 1'999'999'900, 10);
    f.add(Side::Buy, 99'0000, 5);
    f.add(Side::Sell, 101'0000, 5);
    EXPECT_EQ(f.b.best_bid(), 99'0000);
    EXPECT_EQ(f.b.best_ask(), 101'0000);
    EXPECT_EQ(prices(f.b.bids()), (std::vector<std::int32_t>{99'0000, 1}));
    EXPECT_EQ(prices(f.b.asks()), (std::vector<std::int32_t>{101'0000, 1'999'999'900}));
}

TEST(Book, SubPennyPriceIsExact) {
    Fixture f;
    f.add(Side::Buy, 9950, 1);
    f.add(Side::Buy, 9951, 1);
    EXPECT_EQ(f.b.level_count(Side::Buy), 2u);
    EXPECT_EQ(prices(f.b.bids()), (std::vector<std::int32_t>{9951, 9950}));
}

TEST(Book, LookupBeyondEightLevelsUsesBinarySearch) {
    Fixture f;
    for (std::int32_t p = 1; p <= 200; ++p) {
        f.add(Side::Buy, p * 2, 1);
        f.add(Side::Sell, 1000 - p * 2, 1);
    }
    // Hit deep levels (far from the back), insert between existing ones.
    const Slot deep = f.add(Side::Buy, 6, 7);
    f.add(Side::Buy, 7, 3);
    f.add(Side::Sell, 997, 3);
    EXPECT_EQ(f.b.level_count(Side::Buy), 201u);
    EXPECT_EQ(f.b.level_count(Side::Sell), 201u);
    f.b.remove(f.pool, deep);
    EXPECT_EQ(f.b.level_count(Side::Buy), 201u);  // price 6 still has the other order
    auto bids = prices(f.b.bids());
    for (std::size_t i = 1; i < bids.size(); ++i) {
        EXPECT_GT(bids[i - 1], bids[i]);
    }
    auto asks = prices(f.b.asks());
    for (std::size_t i = 1; i < asks.size(); ++i) {
        EXPECT_LT(asks[i - 1], asks[i]);
    }
}

TEST(Book, EmptyLevelIsErased) {
    Fixture    f;
    const Slot a = f.add(Side::Buy, 100, 5);
    f.add(Side::Buy, 99, 5);
    f.b.remove(f.pool, a);
    EXPECT_EQ(f.b.level_count(Side::Buy), 1u);
    EXPECT_EQ(f.b.best_bid(), 99);
}

TEST(Book, ReduceUpdatesLevelTotalAndKeepsPosition) {
    Fixture    f;
    const Slot a = f.add(Side::Buy, 100, 100);
    f.add(Side::Buy, 100, 50);
    f.b.reduce(f.pool, a, 40);
    const auto& lv = f.b.bids().front();
    EXPECT_EQ(lv.total, 110u);
    EXPECT_EQ(lv.count, 2u);
    EXPECT_EQ(lv.head, a);
    EXPECT_EQ(f.pool.at(a).remaining, 60u);
    // Removing afterwards subtracts what is left, not what was added.
    f.b.remove(f.pool, a);
    EXPECT_EQ(f.b.bids().front().total, 50u);
}
