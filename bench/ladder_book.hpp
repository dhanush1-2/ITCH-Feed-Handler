// bench/ladder_book.hpp
#pragma once

// Project 1's price ladder wrapped as a book, for the A/B in docs/OPTIMIZATION-LOG.md ONLY.
// It is not on the production path: ob::PriceLadder is a flat 65,536-level array per side
// (about 3.16 MB per symbol), which cannot cover the whole market, and its window is 65,536 raw
// ticks, $6.5536 at ITCH's 1/10000 dollar price scale.
//
// So this is the design Project 1's own notes imply: a per-symbol BASE places the window, and a
// std::map holds every level outside it. The base is set once, by set_base() before the run or
// else from the first order, and never moves. The ladders are allocated on the first order, so a
// symbol that never trades costs a pointer.
//
// Differences from book::Book that matter to a comparison:
//   * a level queues by ARRIVAL (PriceLadder::push_back), not by reference number. Levels and
//     totals are identical to Book's; the order of orders inside a level is not.
//   * the best price comes from a bitmap in the window and from the map's end outside it.

#include <algorithm>
#include <book/book.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <ob/order_pool.hpp>
#include <ob/price_ladder.hpp>
#include <ob/types.hpp>
#include <optional>
#include <vector>

namespace bench {

// One price level as the comparison sees it.
struct LevelRow {
    std::int32_t  price                             = 0;
    std::uint32_t count                             = 0;
    std::uint64_t total                             = 0;
    bool          operator==(const LevelRow&) const = default;
};

class LadderBook {
public:
    // Heap bytes of the two ladders once allocated: the level arrays plus the bitmaps.
    static constexpr std::size_t kLadderBytes =
        2 * (ob::kLadderSize * sizeof(ob::PriceLevel) + sizeof(ob::LevelBitmap));

    // Centers the window on `price`. Call before the first order; later calls are ignored.
    void set_base(std::int32_t price) noexcept {
        if (!based_) {
            base_ =
                static_cast<std::int64_t>(price) - static_cast<std::int64_t>(ob::kLadderSize / 2);
            based_ = true;
        }
    }

    // Allocates and pre-faults the ladders now (their constructors write every byte), so a
    // benchmark can keep that cost out of its timed span.
    void allocate() {
        if (!lad_) {
            lad_ = std::make_unique<Ladders>();
        }
    }

    void add(ob::OrderPool& pool, ob::Slot s) {
        ob::Order& o = pool.at(s);
        allocate();
        set_base(o.price);
        const bool buy = o.side == ob::Side::Buy;
        if (in_window(o.price)) {
            ++in_window_adds_;
            if (buy) {
                lad_->bid.push_back(tick_of(o.price), s, pool);
            } else {
                lad_->ask.push_back(tick_of(o.price), s, pool);
            }
        } else {
            ++overflow_adds_;
            ob::PriceLevel& lv = (buy ? lad_->bid_over : lad_->ask_over)[o.price];
            link_back(lv, pool, s);
        }
        ++orders_;
    }

    void remove(ob::OrderPool& pool, ob::Slot s) {
        ob::Order& o = pool.at(s);
        if (in_window(o.price)) {
            if (o.side == ob::Side::Buy) {
                (void)lad_->bid.unlink(tick_of(o.price), s, pool);
            } else {
                (void)lad_->ask.unlink(tick_of(o.price), s, pool);
            }
        } else {
            auto& m  = o.side == ob::Side::Buy ? lad_->bid_over : lad_->ask_over;
            auto  it = m.find(o.price);
            unlink(it->second, pool, s);
            if (it->second.count == 0) {
                m.erase(it);
            }
        }
        --orders_;
    }

    void reduce(ob::OrderPool& pool, ob::Slot s, std::uint32_t by) {
        ob::Order& o = pool.at(s);
        if (in_window(o.price)) {
            if (o.side == ob::Side::Buy) {
                lad_->bid.reduce(tick_of(o.price), by);
            } else {
                lad_->ask.reduce(tick_of(o.price), by);
            }
        } else {
            auto& m = o.side == ob::Side::Buy ? lad_->bid_over : lad_->ask_over;
            m.find(o.price)->second.total -= by;
        }
        o.remaining -= by;
    }

    [[nodiscard]] std::optional<std::int32_t> best_bid() const noexcept {
        if (!lad_) {
            return std::nullopt;
        }
        std::optional<std::int32_t> best;
        if (const ob::Ticks t = lad_->bid.best(); t != ob::kNoPrice) {
            best = price_of(t);
        }
        if (!lad_->bid_over.empty()) {
            const std::int32_t p = lad_->bid_over.rbegin()->first;
            if (!best || p > *best) {
                best = p;
            }
        }
        return best;
    }
    [[nodiscard]] std::optional<std::int32_t> best_ask() const noexcept {
        if (!lad_) {
            return std::nullopt;
        }
        std::optional<std::int32_t> best;
        if (const ob::Ticks t = lad_->ask.best(); t != ob::kNoPrice) {
            best = price_of(t);
        }
        if (!lad_->ask_over.empty()) {
            const std::int32_t p = lad_->ask_over.begin()->first;
            if (!best || p < *best) {
                best = p;
            }
        }
        return best;
    }

    [[nodiscard]] std::size_t order_count() const noexcept { return orders_; }

    // Every level of one side, best to worst. For tests and checks, not the hot path.
    [[nodiscard]] std::vector<LevelRow> levels(ob::Side side) const {
        std::vector<LevelRow> out;
        if (!lad_) {
            return out;
        }
        const auto take = [&](std::int32_t p, const ob::PriceLevel& lv) {
            out.push_back(LevelRow{p, lv.count, lv.total});
        };
        if (side == ob::Side::Buy) {
            lad_->bid.for_each_level([&](ob::Ticks t, const ob::PriceLevel& lv) {
                take(price_of(t), lv);
                return true;
            });
            for (const auto& [p, lv] : lad_->bid_over) {
                take(p, lv);
            }
            std::sort(out.begin(), out.end(),
                      [](const LevelRow& a, const LevelRow& b) { return a.price > b.price; });
        } else {
            lad_->ask.for_each_level([&](ob::Ticks t, const ob::PriceLevel& lv) {
                take(price_of(t), lv);
                return true;
            });
            for (const auto& [p, lv] : lad_->ask_over) {
                take(p, lv);
            }
            std::sort(out.begin(), out.end(),
                      [](const LevelRow& a, const LevelRow& b) { return a.price < b.price; });
        }
        return out;
    }

    [[nodiscard]] bool          allocated() const noexcept { return lad_ != nullptr; }
    [[nodiscard]] std::uint64_t in_window_adds() const noexcept { return in_window_adds_; }
    [[nodiscard]] std::uint64_t overflow_adds() const noexcept { return overflow_adds_; }

private:
    struct Ladders {
        ob::PriceLadder<ob::Side::Buy>         bid;
        ob::PriceLadder<ob::Side::Sell>        ask;
        std::map<std::int32_t, ob::PriceLevel> bid_over;
        std::map<std::int32_t, ob::PriceLevel> ask_over;
    };

    [[nodiscard]] bool in_window(std::int32_t price) const noexcept {
        const std::int64_t d = static_cast<std::int64_t>(price) - base_;
        return d >= 0 && d < static_cast<std::int64_t>(ob::kLadderSize);
    }
    [[nodiscard]] ob::Ticks tick_of(std::int32_t price) const noexcept {
        return static_cast<ob::Ticks>(static_cast<std::int64_t>(price) - base_ + ob::kMinTick);
    }
    [[nodiscard]] std::int32_t price_of(ob::Ticks tick) const noexcept {
        return static_cast<std::int32_t>(static_cast<std::int64_t>(tick) - ob::kMinTick + base_);
    }

    static void link_back(ob::PriceLevel& lv, ob::OrderPool& pool, ob::Slot s) {
        ob::Order& o = pool.at(s);
        o.next       = ob::kInvalidSlot;
        o.prev       = lv.tail;
        if (lv.tail != ob::kInvalidSlot) {
            pool.at(lv.tail).next = s;
        } else {
            lv.head = s;
        }
        lv.tail = s;
        lv.total += o.remaining;
        ++lv.count;
    }

    static void unlink(ob::PriceLevel& lv, ob::OrderPool& pool, ob::Slot s) {
        ob::Order& o = pool.at(s);
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
        lv.total -= o.remaining;
        --lv.count;
    }

    std::unique_ptr<Ladders> lad_;
    std::int64_t             base_           = 0;
    bool                     based_          = false;
    std::size_t              orders_         = 0;
    std::uint64_t            in_window_adds_ = 0;
    std::uint64_t            overflow_adds_  = 0;
};

}  // namespace bench
