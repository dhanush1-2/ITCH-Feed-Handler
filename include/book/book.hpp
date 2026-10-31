// include/book/book.hpp
#pragma once

// Full-depth book for one symbol: every price level on both sides, no window.
//
// Each side is a vector of Level sorted worst-to-best, so the best price is at the BACK.
// Bids ascend, asks descend. The touch is where all the traffic is, so a lookup scans up to
// kScan levels from the back and only then binary-searches. A new level inserts by shifting
// the (usually short) tail; an empty level is erased.
//
// Orders live in the shared ob::OrderPool; a Level only holds the head and tail of its queue,
// in reference-number order, linked through Order::next / Order::prev. The Book never allocates or
// frees a slot. Callers set Order::{price, remaining, side} before add().

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <itch/wire.hpp>
#include <ob/order.hpp>
#include <ob/order_pool.hpp>
#include <ob/types.hpp>
#include <optional>
#include <ranges>
#include <span>
#include <vector>

namespace book {

struct Level {
    std::int32_t  price = 0;
    ob::Slot      head  = ob::kInvalidSlot;
    ob::Slot      tail  = ob::kInvalidSlot;
    std::uint32_t count = 0;
    std::uint64_t total = 0;  // sum of `remaining` over the level's orders
};
static_assert(sizeof(Level) == 24);

class Book {
public:
    static constexpr std::size_t kInitialLevels = 64;
    // Levels scanned from the back before binary search. 64 not 8: measured +8% throughput on
    // the full day (docs/OPTIMIZATION-LOG.md); most lookups end within a few levels of the touch.
    static constexpr std::size_t kScan = 64;

    // Places the order in its price level by reference number: the exchange's queue order, not
    // message arrival. Orders received before the open can be displayed after later ones and
    // still rank ahead (docs/DISCREPANCIES.md). A new reference is nearly always the highest,
    // so the walk starts at the tail and is O(1) in the common case. reduce() does not move an
    // order, and a U's new reference is placed here like any other. Callers set Order::id too.
    void add(ob::OrderPool& pool, ob::Slot s) {
        ob::Order& o = pool.at(s);
        auto&      v = side_vec(o.side);
        if (v.capacity() == 0) {
            v.reserve(kInitialLevels);  // lazy: 65,536 locates, most never trade
        }
        const std::size_t i = lower_bound(v, o.side, o.price);
        if (i == v.size() || v[i].price != o.price) {
            v.insert(v.begin() + static_cast<std::ptrdiff_t>(i),
                     Level{o.price, ob::kInvalidSlot, ob::kInvalidSlot, 0, 0});
        }
        Level&   lv    = v[i];
        ob::Slot after = lv.tail;
        while (after != ob::kInvalidSlot && pool.at(after).id > o.id) {
            after = pool.at(after).prev;
        }
        const ob::Slot before = after == ob::kInvalidSlot ? lv.head : pool.at(after).next;
        o.prev                = after;
        o.next                = before;
        if (after != ob::kInvalidSlot) {
            pool.at(after).next = s;
        } else {
            lv.head = s;
        }
        if (before != ob::kInvalidSlot) {
            pool.at(before).prev = s;
        } else {
            lv.tail = s;
        }
        ++lv.count;
        lv.total += o.remaining;
        ++orders_;
    }

    // Unlinks the order and takes its CURRENT remaining out of the level total. The slot is
    // not freed. Nothing may change `remaining` between add() and remove() except reduce(),
    // or the total drifts.
    void remove(ob::OrderPool& pool, ob::Slot s) {
        ob::Order&        o = pool.at(s);
        auto&             v = side_vec(o.side);
        const std::size_t i = lower_bound(v, o.side, o.price);
        ITCH_ASSERT(i < v.size() && v[i].price == o.price);
        Level& lv = v[i];
        if (o.prev != ob::kInvalidSlot) {
            pool.at(o.prev).next = o.next;
        } else {
            lv.head = o.next;
        }
        if (o.next != ob::kInvalidSlot) {
            pool.at(o.next).prev = o.prev;
        } else {
            lv.tail = o.prev;
        }
        o.next = ob::kInvalidSlot;
        o.prev = ob::kInvalidSlot;
        --lv.count;
        lv.total -= o.remaining;
        --orders_;
        if (lv.count == 0) {
            v.erase(v.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

    // Takes `by` shares off a resting order, which keeps its place in the queue. The level
    // total is reduced BEFORE `remaining`, and by the amount actually taken, so the two
    // cannot disagree. `by` must be less than remaining; a full removal is remove().
    void reduce(ob::OrderPool& pool, ob::Slot s, std::uint32_t by) {
        ob::Order& o = pool.at(s);
        ITCH_ASSERT(by < o.remaining);
        auto&             v = side_vec(o.side);
        const std::size_t i = lower_bound(v, o.side, o.price);
        ITCH_ASSERT(i < v.size() && v[i].price == o.price);
        v[i].total -= by;
        o.remaining -= by;
    }

    [[nodiscard]] std::optional<std::int32_t> best_bid() const noexcept {
        return bids_.empty() ? std::nullopt : std::optional<std::int32_t>(bids_.back().price);
    }
    [[nodiscard]] std::optional<std::int32_t> best_ask() const noexcept {
        return asks_.empty() ? std::nullopt : std::optional<std::int32_t>(asks_.back().price);
    }
    // Orders resting in this book, kept as a running count so a checker can read it in O(1).
    [[nodiscard]] std::size_t order_count() const noexcept { return orders_; }
    [[nodiscard]] std::size_t level_count(ob::Side side) const noexcept {
        return (side == ob::Side::Buy ? bids_ : asks_).size();
    }

    // Heap bytes held by the two level vectors (capacity, not size). A vector keeps its capacity
    // when levels empty, so this is the book's high-water mark. The benchmark reports it.
    [[nodiscard]] std::size_t reserved_bytes() const noexcept {
        return (bids_.capacity() + asks_.capacity()) * sizeof(Level);
    }

    // Best to worst.
    [[nodiscard]] auto bids() const noexcept {
        return std::views::reverse(std::span<const Level>(bids_));
    }
    [[nodiscard]] auto asks() const noexcept {
        return std::views::reverse(std::span<const Level>(asks_));
    }

private:
    // True when `a` sits closer to the front (farther from the touch) than `b`.
    [[nodiscard]] static bool worse(ob::Side side, std::int32_t a, std::int32_t b) noexcept {
        return side == ob::Side::Buy ? a < b : a > b;
    }

    // Index of the first level at least as good as `price`: the level itself if it exists,
    // otherwise the insertion point.
    [[nodiscard]] static std::size_t lower_bound(const std::vector<Level>& v, ob::Side side,
                                                 std::int32_t price) noexcept {
        std::size_t i = v.size();
        for (std::size_t steps = 0; i > 0 && !worse(side, v[i - 1].price, price);) {
            --i;
            if (++steps == kScan) {
                const auto it = std::partition_point(
                    v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i),
                    [&](const Level& l) { return worse(side, l.price, price); });
                return static_cast<std::size_t>(it - v.begin());
            }
        }
        return i;
    }

    [[nodiscard]] std::vector<Level>& side_vec(ob::Side s) noexcept {
        return s == ob::Side::Buy ? bids_ : asks_;
    }

    std::vector<Level> bids_;  // ascending: best bid at the back
    std::vector<Level> asks_;  // descending: best ask at the back
    std::size_t        orders_ = 0;
};

}  // namespace book
