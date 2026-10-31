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
        return add_ref(next_id++, side, price, qty);
    }

    Slot add_ref(std::uint64_t ref, Side side, std::int32_t price, std::uint32_t qty) {
        const Slot s = pool.alloc();
        auto&      o = pool.at(s);
        o.id         = ref;
        o.price      = price;
        o.remaining  = qty;
        o.side       = side;
        b.add(pool, s);
        return s;
    }
};

// The level's references, head to tail, checking both directions of the chain agree.
std::vector<std::uint64_t> queue(const ob::OrderPool& pool, const book::Level& lv) {
    std::vector<std::uint64_t> out;
    Slot                       last = ob::kInvalidSlot;
    for (Slot s = lv.head; s != ob::kInvalidSlot; s = pool.at(s).next) {
        EXPECT_EQ(pool.at(s).prev, last);
        out.push_back(pool.at(s).id);
        last = s;
    }
    EXPECT_EQ(lv.tail, last);
    return out;
}

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
    const auto& lv = f.b.best_level(Side::Buy);
    EXPECT_EQ(lv.total, 110u);
    EXPECT_EQ(lv.count, 2u);
    EXPECT_EQ(lv.head, a);
    EXPECT_EQ(f.pool.at(a).remaining, 60u);
    // Removing afterwards subtracts what is left, not what was added.
    f.b.remove(f.pool, a);
    EXPECT_EQ(f.b.bids().front().total, 50u);
}

// The exchange's queue runs in reference-number order, not message-arrival order: every E of two
// full days hits the lowest reference at its level (docs/DISCREPANCIES.md).
TEST(Book, OutOfOrderRefLandsAheadOfHigherOne) {
    Fixture f;
    f.add_ref(20, Side::Buy, 100, 1);
    f.add_ref(30, Side::Buy, 100, 1);
    f.add_ref(10, Side::Buy, 100, 1);  // new head
    f.add_ref(25, Side::Buy, 100, 1);  // middle
    f.add_ref(40, Side::Buy, 100, 1);  // new tail
    const auto& lv = *f.b.bids().begin();
    EXPECT_EQ(queue(f.pool, lv), (std::vector<std::uint64_t>{10, 20, 25, 30, 40}));
    EXPECT_EQ(lv.count, 5u);
    EXPECT_EQ(lv.total, 5u);
}

TEST(Book, EqualPriceChurnKeepsRefOrder) {
    Fixture    f;
    const Slot a = f.add_ref(10, Side::Sell, 100, 5);
    const Slot b = f.add_ref(20, Side::Sell, 100, 5);
    f.add_ref(30, Side::Sell, 100, 5);
    f.b.reduce(f.pool, b, 2);  // a partial fill or cancel does not move an order
    f.add_ref(15, Side::Sell, 100, 5);
    EXPECT_EQ(queue(f.pool, *f.b.asks().begin()), (std::vector<std::uint64_t>{10, 15, 20, 30}));
    f.b.remove(f.pool, a);
    f.b.remove(f.pool, b);
    f.add_ref(12, Side::Sell, 100, 5);  // into the gap, at the new head
    f.add_ref(50, Side::Sell, 100, 5);
    const auto& lv = *f.b.asks().begin();
    EXPECT_EQ(queue(f.pool, lv), (std::vector<std::uint64_t>{12, 15, 30, 50}));
    EXPECT_EQ(lv.total, 20u);
}

TEST(Book, BestLevelIsTheTouchOnEachSide) {
    Fixture f;
    f.add(Side::Buy, 100, 1);
    f.add(Side::Buy, 102, 7);
    f.add(Side::Sell, 105, 3);
    f.add(Side::Sell, 103, 9);
    EXPECT_EQ(f.b.best_level(Side::Buy).price, 102);
    EXPECT_EQ(f.b.best_level(Side::Buy).total, 7u);
    EXPECT_EQ(f.b.best_level(Side::Sell).price, 103);
    EXPECT_EQ(f.b.best_level(Side::Sell).total, 9u);
}

// A lookup scans kScan levels from the touch, then binary-searches. Hit an existing level on both
// sides of that boundary, and insert new ones there: a mistake at the join would duplicate a level
// or break the order.
TEST(Book, LookupAcrossTheScanBoundary) {
    for (const Side side : {Side::Buy, Side::Sell}) {
        constexpr std::size_t  kLevels = Book::kScan + 40;
        constexpr std::int32_t kMid    = 100000;
        // Depth d (0 = touch) has price kMid - 2d for bids, kMid + 2d for asks.
        const auto price = [&](std::size_t d, std::int32_t extra = 0) {
            const auto off = static_cast<std::int32_t>(2 * d) + extra;
            return side == Side::Buy ? kMid - off : kMid + off;
        };
        Fixture f;
        for (std::size_t d = 0; d < kLevels; ++d) {
            f.add(side, price(d), 10);
        }
        ASSERT_EQ(f.b.level_count(side), kLevels);
        for (const std::size_t d :
             {Book::kScan - 2, Book::kScan - 1, Book::kScan, Book::kScan + 1}) {
            f.add(side, price(d), 5);  // an existing level: must join it
            EXPECT_EQ(f.b.level_count(side), kLevels) << "depth " << d;
        }
        std::size_t added = 0;
        for (const std::size_t d :
             {Book::kScan - 2, Book::kScan - 1, Book::kScan, Book::kScan + 1}) {
            f.add(side, price(d, 1), 1);  // between two levels: must insert in order
            EXPECT_EQ(f.b.level_count(side), kLevels + ++added) << "depth " << d;
        }
        std::int32_t last  = 0;
        bool         first = true;
        const auto   check = [&](auto levels) {
            for (const auto& lv : levels) {
                if (!first) {
                    EXPECT_TRUE(side == Side::Buy ? lv.price < last : lv.price > last);
                }
                last  = lv.price;
                first = false;
            }
        };
        if (side == Side::Buy) {
            check(f.b.bids());
        } else {
            check(f.b.asks());
        }
    }
}
