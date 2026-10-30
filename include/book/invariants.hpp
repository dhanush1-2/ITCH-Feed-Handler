// include/book/invariants.hpp
#pragma once

// Structural checks on the book builder's state. These are HARD invariants: a violation
// means the book or the checker is wrong, never that the feed is odd. (A crossed book is a
// feed property and is counted by the oracle instead.)
//
//   check_touched   O(1), run after each message on the book it touched: the best level of
//                   each side is well formed and the next level is strictly worse, and
//                   live == pool.size == index.size == sum of every book's order count.
//   check_full      walks every book and every FIFO: level total and count equal their
//                   queue, links are consistent, prices strictly ordered, every order's
//                   locate, side and price match its level, and the grand totals agree.
//
// Both return the first violation, or nothing. VerifyObserver (bottom of this file) turns one
// into a throw and also runs MarketState and the Oracle.

#include <book/builder.hpp>
#include <book/market_state.hpp>
#include <book/oracle.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace book {

struct InvariantViolation {
    std::uint64_t msg_index;
    std::uint16_t locate;
    const char*   what;
};

struct VerificationFailure : std::runtime_error {
    explicit VerificationFailure(const InvariantViolation& v)
        : std::runtime_error("verification failed at message " + std::to_string(v.msg_index) +
                             " locate " + std::to_string(v.locate) + ": " + v.what),
          violation(v) {}
    InvariantViolation violation;
};

// Running sum of every book's order count, updated only for the book a message touched. If a
// book changes without being reported as touched, this drifts from the pool and is caught.
class OrderTally {
public:
    OrderTally() : cached_(65536, 0) {}
    // Re-reads one book's count; returns the new grand sum.
    std::uint64_t refresh(std::uint16_t locate, std::size_t now) noexcept {
        sum_ = sum_ - cached_[locate] + now;
        cached_[locate] = static_cast<std::uint32_t>(now);
        return sum_;
    }

private:
    std::vector<std::uint32_t> cached_;
    std::uint64_t              sum_ = 0;
};

namespace detail {

// An order at the end of a level's queue (head or tail) must belong to that level.
[[nodiscard]] inline const char* check_end_order(const ob::OrderPool& pool, ob::Slot s,
                                                 const Level& lv, ob::Side side,
                                                 std::uint16_t locate, bool is_head) {
    if (s >= pool.capacity()) {
        return "level end slot out of range";
    }
    const ob::Order& o = pool.at(s);
    if (o.id == 0) {
        return "level end order is a free slot";
    }
    if (o.pad != locate) {
        return "order locate does not match its book";
    }
    if (o.side != side) {
        return "order side does not match its book side";
    }
    if (o.price != lv.price) {
        return "order price does not match its level";
    }
    if (o.remaining == 0) {
        return "resting order has zero shares";
    }
    if (is_head ? o.prev != ob::kInvalidSlot : o.next != ob::kInvalidSlot) {
        return "level end order has a dangling link";
    }
    return nullptr;
}

[[nodiscard]] inline const char* check_best_level(const ob::OrderPool& pool, const Level& lv,
                                                  ob::Side side, std::uint16_t locate) {
    if (lv.count == 0) {
        return "best level has zero count";
    }
    if (lv.total == 0) {
        return "best level has zero total";
    }
    if (lv.head == ob::kInvalidSlot || lv.tail == ob::kInvalidSlot) {
        return "best level has no head or tail";
    }
    if (const char* w = check_end_order(pool, lv.head, lv, side, locate, true)) {
        return w;
    }
    if (const char* w = check_end_order(pool, lv.tail, lv, side, locate, false)) {
        return w;
    }
    if (lv.count == 1 && (lv.head != lv.tail || lv.total != pool.at(lv.head).remaining)) {
        return "single-order level disagrees with its order";
    }
    if (lv.total < pool.at(lv.head).remaining) {
        return "best level total is below its head order";
    }
    return nullptr;
}

template <class Range>
[[nodiscard]] inline const char* check_side_touched(const ob::OrderPool& pool, const Range& levels,
                                                    ob::Side side, std::uint16_t locate) {
    auto it = levels.begin();
    if (it == levels.end()) {
        return nullptr;
    }
    const Level& best = *it;
    if (const char* w = check_best_level(pool, best, side, locate)) {
        return w;
    }
    ++it;
    if (it != levels.end()) {
        const Level& next = *it;
        const bool   ok   = side == ob::Side::Buy ? best.price > next.price : best.price < next.price;
        if (!ok) {
            return "best two levels are not strictly ordered";
        }
    }
    return nullptr;
}

template <class Range>
[[nodiscard]] inline const char* check_side_full(const ob::OrderPool& pool, const Range& levels,
                                                 ob::Side side, std::uint16_t locate,
                                                 std::uint64_t& orders) {
    bool         have_prev  = false;
    std::int32_t prev_price = 0;
    for (const Level& lv : levels) {
        if (have_prev) {
            const bool ok = side == ob::Side::Buy ? prev_price > lv.price : prev_price < lv.price;
            if (!ok) {
                return "levels are not strictly ordered";
            }
        }
        have_prev  = true;
        prev_price = lv.price;
        if (lv.count == 0 || lv.head == ob::kInvalidSlot || lv.tail == ob::kInvalidSlot) {
            return "empty level left in the book";
        }
        std::uint64_t total = 0;
        std::uint64_t n     = 0;
        ob::Slot      prev  = ob::kInvalidSlot;
        for (ob::Slot s = lv.head; s != ob::kInvalidSlot;) {
            if (s >= pool.capacity()) {
                return "fifo link out of range";
            }
            if (++n > pool.size()) {
                return "fifo is longer than the pool (cycle)";
            }
            const ob::Order& o = pool.at(s);
            if (o.id == 0) {
                return "fifo reaches a free slot";
            }
            if (o.pad != locate) {
                return "order locate does not match its book";
            }
            if (o.side != side) {
                return "order side does not match its book side";
            }
            if (o.price != lv.price) {
                return "order price does not match its level";
            }
            if (o.remaining == 0) {
                return "resting order has zero shares";
            }
            if (o.prev != prev) {
                return "fifo prev link is inconsistent";
            }
            total += o.remaining;
            prev = s;
            s    = o.next;
        }
        if (prev != lv.tail) {
            return "level tail is not the last order of its fifo";
        }
        if (n != lv.count) {
            return "level count does not equal its fifo";
        }
        if (total != lv.total) {
            return "level total does not equal its fifo";
        }
        orders += n;
    }
    return nullptr;
}

}  // namespace detail

template <class Obs>
[[nodiscard]] std::optional<InvariantViolation> check_touched(const BookBuilder<Obs>& b,
                                                              OrderTally& tally,
                                                              std::uint16_t locate,
                                                              std::uint64_t msg_index) {
    const Book&         bk   = b.book(locate);
    const ob::OrderPool& pool = b.pool();
    if (const char* w = detail::check_side_touched(pool, bk.bids(), ob::Side::Buy, locate)) {
        return InvariantViolation{msg_index, locate, w};
    }
    if (const char* w = detail::check_side_touched(pool, bk.asks(), ob::Side::Sell, locate)) {
        return InvariantViolation{msg_index, locate, w};
    }
    const std::uint64_t sum = tally.refresh(locate, bk.order_count());
    if (b.live_orders() != pool.size() || pool.size() != b.index_size()) {
        return InvariantViolation{msg_index, locate, "live, pool and index sizes disagree"};
    }
    if (sum != pool.size()) {
        return InvariantViolation{msg_index, locate, "sum of book orders differs from the pool"};
    }
    return std::nullopt;
}

template <class Obs>
[[nodiscard]] std::optional<InvariantViolation> check_full(const BookBuilder<Obs>& b,
                                                           std::uint64_t msg_index = 0) {
    const ob::OrderPool& pool  = b.pool();
    std::uint64_t        total = 0;
    for (std::size_t i = 0; i < BookBuilder<Obs>::kLocates; ++i) {
        const auto  loc = static_cast<std::uint16_t>(i);
        const Book& bk  = b.book(loc);
        if (bk.level_count(ob::Side::Buy) == 0 && bk.level_count(ob::Side::Sell) == 0) {
            if (bk.order_count() != 0) {
                return InvariantViolation{msg_index, loc, "empty book has a nonzero order count"};
            }
            continue;
        }
        std::uint64_t orders = 0;
        if (const char* w = detail::check_side_full(pool, bk.bids(), ob::Side::Buy, loc, orders)) {
            return InvariantViolation{msg_index, loc, w};
        }
        if (const char* w = detail::check_side_full(pool, bk.asks(), ob::Side::Sell, loc, orders)) {
            return InvariantViolation{msg_index, loc, w};
        }
        if (orders != bk.order_count()) {
            return InvariantViolation{msg_index, loc, "book order count does not equal its levels"};
        }
        total += orders;
    }
    if (b.live_orders() != pool.size() || pool.size() != b.index_size()) {
        return InvariantViolation{msg_index, 0, "live, pool and index sizes disagree"};
    }
    if (total != pool.size()) {
        return InvariantViolation{msg_index, 0, "sum of book orders differs from the pool"};
    }
    return std::nullopt;
}

// The observer a verifying run plugs into BookBuilder. Per message: market state is updated,
// every execution is checked by the oracle before it is applied, and after each message that
// changed a book that book is structurally checked (hard: throws VerificationFailure) and
// tested for a crossed touch (counted, split by continuous trading).
//
// msg_index is the 1-based count of messages that reached the handler (not file position).
// Construct, build the BookBuilder with this observer, then call bind().
class VerifyObserver {
public:
    using Builder = BookBuilder<VerifyObserver>;

    struct Options {
        std::uint64_t full_every    = 0;  // run check_full every N messages; 0 = never
        std::uint64_t halt_grace_ns = MarketState::kDefaultGraceNs;
    };

    VerifyObserver() : VerifyObserver(Options{}) {}
    explicit VerifyObserver(const Options& o) : opt_(o), market_(o.halt_grace_ns) {}

    void bind(const Builder& b) noexcept { builder_ = &b; }

    template <class View>
    void pass(const View& v) {
        if (opt_.full_every != 0 && idx_ != 0 && idx_ % opt_.full_every == 0) {
            full_check();  // state after the previous message
        }
        ++idx_;
        ts_  = v.timestamp_ns();
        ref_ = 0;
        if constexpr (requires { v.ref(); }) {
            ref_ = v.ref();
        } else if constexpr (requires { v.new_ref(); }) {
            ref_ = v.new_ref();
        }
        market_.advance(ts_);
        if constexpr (requires { market_.on(v); }) {
            market_.on(v);
        }
    }

    void before_execute(const Book& bk, const ob::Order& o, ob::Side, std::uint32_t, std::uint64_t,
                        std::uint32_t) {
        oracle_.on_execute(bk, builder_->pool(), o, ts_);
    }

    void after_apply(std::uint16_t locate) {
        if (auto v = check_touched(*builder_, tally_, locate, idx_)) {
            throw VerificationFailure(*v);
        }
        const Book& bk = builder_->book(locate);
        const auto  bb = bk.best_bid();
        const auto  ba = bk.best_ask();
        if (bb && ba && *bb > *ba) {
            oracle_.on_crossed(market_.continuous(locate), ts_, locate, ref_);
        }
    }

    // Runs check_full once more on the final state.
    void finish() { full_check(); }

    [[nodiscard]] std::uint64_t         messages() const noexcept { return idx_; }
    [[nodiscard]] const OracleCounts&   counts() const noexcept { return oracle_.counts(); }
    [[nodiscard]] const Oracle&         oracle() const noexcept { return oracle_; }
    [[nodiscard]] const MarketState&    market() const noexcept { return market_; }
    [[nodiscard]] std::uint64_t         full_checks() const noexcept { return full_checks_; }

private:
    void full_check() {
        ++full_checks_;
        if (auto v = check_full(*builder_, idx_)) {
            throw VerificationFailure(*v);
        }
    }

    Options          opt_;
    MarketState      market_;
    Oracle           oracle_;
    OrderTally       tally_;
    const Builder*   builder_     = nullptr;
    std::uint64_t    idx_         = 0;
    std::uint64_t    ts_          = 0;
    std::uint64_t    ref_         = 0;
    std::uint64_t    full_checks_ = 0;
};

}  // namespace book
