// include/book/builder.hpp
#pragma once

// Applies ITCH 5.0 messages to per-symbol books. Usable directly as a decode() handler.
//
// Only E, C, X, D and U remove displayed liquidity; A and F add it. P, Q and B never touch a
// book, and the handler never matches. Routing is by stock_locate into a dense array.
//
// Order::pad stores the stock_locate of the book that owns the order. Anything that names an
// existing order (E, C, X, D, U) is applied to the OWNER's book; a header locate that
// disagrees is counted in Anomalies::locate_mismatch rather than trusted.
//
// Bad input is counted, never fabricated and never silently dropped:
//   unknown_ref     E/C/X/D/U (or a malformed add) naming a ref that is not live
//   duplicate_add   A/F or U new-ref reusing a live ref; the live order is left intact
//   locate_mismatch header locate differs from the owning book's locate
//   over_execute    E/C for more shares than remain; clamped, order removed
//   over_cancel     X for more shares than remain; clamped, order removed
//   zero_shares     an add, replace, execute or cancel of 0 shares (a no-op, or for U/A no order)
// Running out of order capacity is different: it is fatal (CapacityExceeded), because a
// dropped order silently corrupts every later message for that symbol.

#include <book/book.hpp>
#include <cstddef>
#include <cstdint>
#include <itch/messages.hpp>
#include <ob/id_index.hpp>
#include <ob/order_pool.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace book {

struct Anomalies {
    std::uint64_t unknown_ref      = 0;
    std::uint64_t duplicate_add    = 0;
    std::uint64_t locate_mismatch  = 0;
    std::uint64_t over_execute     = 0;
    std::uint64_t over_cancel      = 0;
    std::uint64_t zero_shares      = 0;
};

struct CapacityExceeded : std::runtime_error {
    explicit CapacityExceeded(std::size_t live)
        : std::runtime_error("order capacity exceeded with " + std::to_string(live) +
                             " live orders; rerun with a larger --max-live-orders") {}
};

// Hooks, all called inline (no virtual dispatch):
//   pass(view)            every message, before it is applied; any of the 13 view types
//   before_execute(...)   E or C, before the book changes. `shares` is what will actually be
//                         taken (clamped); `price` is the print price for C and the resting
//                         price for E
//   after_apply(locate)   once per message that changed a book (U fires once, not twice);
//                         `locate` is the owning book
struct NullObserver {
    template <class View>
    void pass(const View&) noexcept {}
    void before_execute(const Book&, const ob::Order&, ob::Side, std::uint32_t, std::uint64_t,
                        std::uint32_t) noexcept {}
    void after_apply(std::uint16_t) noexcept {}
};

template <class Obs>
class BookBuilder {
public:
    static constexpr std::size_t kLocates = 65536;

    BookBuilder(std::size_t max_live_orders, Obs& obs)
        : pool_(max_live_orders), index_(max_live_orders), books_(kLocates), obs_(obs) {}

    void on(const itch::AddOrderView& v) {
        obs_.pass(v);
        add_order(v.locate(), v.ref(), v.side(), v.shares(), v.price());
    }
    void on(const itch::ExecutedView& v) {
        obs_.pass(v);
        execute(v.locate(), v.ref(), v.shares(), v.match(), 0, false);
    }
    void on(const itch::ExecutedPriceView& v) {
        obs_.pass(v);
        execute(v.locate(), v.ref(), v.shares(), v.match(), v.price(), true);
    }
    void on(const itch::CancelView& v) {
        obs_.pass(v);
        cancel(v.locate(), v.ref(), v.shares());
    }
    void on(const itch::DeleteView& v) {
        obs_.pass(v);
        remove_order(v.locate(), v.ref());
    }
    void on(const itch::ReplaceView& v) {
        obs_.pass(v);
        replace(v.locate(), v.old_ref(), v.new_ref(), v.shares(), v.price());
    }
    void on(const itch::TradeView& v) { obs_.pass(v); }
    void on(const itch::CrossTradeView& v) { obs_.pass(v); }
    void on(const itch::BrokenTradeView& v) { obs_.pass(v); }
    void on(const itch::SystemEventView& v) { obs_.pass(v); }
    void on(const itch::TradingActionView& v) { obs_.pass(v); }
    void on(const itch::StockDirectoryView& v) { obs_.pass(v); }

    [[nodiscard]] const Book& book(std::uint16_t locate) const noexcept { return books_[locate]; }
    [[nodiscard]] const Anomalies& anomalies() const noexcept { return anomalies_; }
    [[nodiscard]] std::size_t live_orders() const noexcept { return pool_.size(); }
    [[nodiscard]] std::size_t peak_live_orders() const noexcept { return peak_; }
    // For state_dump: order data by slot.
    [[nodiscard]] const ob::OrderPool& pool() const noexcept { return pool_; }
    // For the invariant checker: the id index must agree with the pool.
    [[nodiscard]] std::size_t index_size() const noexcept { return index_.size(); }

private:
    void add_order(std::uint16_t locate, std::uint64_t ref, char side_ch, std::uint32_t shares,
                   std::uint32_t price) {
        if (shares == 0) {
            ++anomalies_.zero_shares;
            return;
        }
        if ((side_ch != 'B' && side_ch != 'S') || ref == 0) {
            ++anomalies_.unknown_ref;  // not a usable add; there is no counter for it
            return;
        }
        if (index_.find(ref) != ob::kInvalidSlot) {
            ++anomalies_.duplicate_add;
            return;
        }
        // Both limits are checked before anything is allocated or linked.
        if (pool_.full() || index_.full()) {
            throw CapacityExceeded(pool_.size());
        }
        const ob::Slot s = pool_.alloc();
        ob::Order&     o = pool_.at(s);
        o.id             = ref;
        o.price          = static_cast<ob::Ticks>(price);
        o.remaining      = shares;
        o.side           = side_ch == 'B' ? ob::Side::Buy : ob::Side::Sell;
        o.flags          = 0;
        o.pad            = locate;  // stock_locate of the owning book
        [[maybe_unused]] const bool inserted = index_.insert(ref, s);
        ITCH_ASSERT(inserted);
        books_[locate].add(pool_, s);
        if (pool_.size() > peak_) {
            peak_ = pool_.size();
        }
        obs_.after_apply(locate);
    }

    void execute(std::uint16_t hdr_locate, std::uint64_t ref, std::uint32_t shares,
                 std::uint64_t match, std::uint32_t print_price, bool has_price) {
        const ob::Slot s = index_.find(ref);
        if (s == ob::kInvalidSlot) {
            ++anomalies_.unknown_ref;
            return;
        }
        ob::Order&          o   = pool_.at(s);
        const std::uint16_t loc = o.pad;
        if (loc != hdr_locate) {
            ++anomalies_.locate_mismatch;
        }
        if (shares == 0) {
            ++anomalies_.zero_shares;
            return;
        }
        std::uint32_t take = shares;
        if (shares > o.remaining) {
            ++anomalies_.over_execute;
            take = o.remaining;
        }
        // C executes at the order's resting price: the level is untouched by the print price.
        obs_.before_execute(books_[loc], o, o.side, take, match,
                            has_price ? print_price : static_cast<std::uint32_t>(o.price));
        if (take == o.remaining) {
            retire(s);
        } else {
            books_[loc].reduce(pool_, s, take);
        }
        obs_.after_apply(loc);
    }

    void cancel(std::uint16_t hdr_locate, std::uint64_t ref, std::uint32_t shares) {
        const ob::Slot s = index_.find(ref);
        if (s == ob::kInvalidSlot) {
            ++anomalies_.unknown_ref;
            return;
        }
        ob::Order&          o   = pool_.at(s);
        const std::uint16_t loc = o.pad;
        if (loc != hdr_locate) {
            ++anomalies_.locate_mismatch;
        }
        if (shares == 0) {
            ++anomalies_.zero_shares;
            return;
        }
        std::uint32_t take = shares;
        if (shares > o.remaining) {
            ++anomalies_.over_cancel;
            take = o.remaining;
        }
        if (take == o.remaining) {
            retire(s);
        } else {
            books_[loc].reduce(pool_, s, take);
        }
        obs_.after_apply(loc);
    }

    void remove_order(std::uint16_t hdr_locate, std::uint64_t ref) {
        const ob::Slot s = index_.find(ref);
        if (s == ob::kInvalidSlot) {
            ++anomalies_.unknown_ref;
            return;
        }
        const std::uint16_t loc = pool_.at(s).pad;
        if (loc != hdr_locate) {
            ++anomalies_.locate_mismatch;
        }
        retire(s);
        obs_.after_apply(loc);
    }

    // Remove plus add in one call. The new order reuses the old slot, so a replace never
    // needs capacity, and it goes to the back of its level under the new ref. If the
    // replacement itself is unusable the old order is still removed: U cancels it.
    void replace(std::uint16_t hdr_locate, std::uint64_t old_ref, std::uint64_t new_ref,
                 std::uint32_t shares, std::uint32_t price) {
        const ob::Slot s = index_.find(old_ref);
        if (s == ob::kInvalidSlot) {
            ++anomalies_.unknown_ref;
            return;
        }
        ob::Order&          o   = pool_.at(s);
        const std::uint16_t loc = o.pad;
        if (loc != hdr_locate) {
            ++anomalies_.locate_mismatch;
        }
        const bool dup = new_ref != old_ref && index_.find(new_ref) != ob::kInvalidSlot;
        if (shares == 0 || new_ref == 0 || dup) {
            if (shares == 0) {
                ++anomalies_.zero_shares;
            } else if (dup) {
                ++anomalies_.duplicate_add;
            } else {
                ++anomalies_.unknown_ref;
            }
            retire(s);
            obs_.after_apply(loc);
            return;
        }
        books_[loc].remove(pool_, s);
        index_.erase(old_ref);
        o.id        = new_ref;
        o.price     = static_cast<ob::Ticks>(price);
        o.remaining = shares;
        [[maybe_unused]] const bool inserted = index_.insert(new_ref, s);
        ITCH_ASSERT(inserted);
        books_[loc].add(pool_, s);
        obs_.after_apply(loc);
    }

    // Unlink, drop from the index, free. The id is still set when free() runs.
    void retire(ob::Slot s) {
        const ob::Order& o = pool_.at(s);
        books_[o.pad].remove(pool_, s);
        index_.erase(o.id);
        pool_.free(s);
    }

    ob::OrderPool     pool_;
    ob::IdIndex       index_;
    std::vector<Book> books_;
    Obs&              obs_;
    Anomalies         anomalies_{};
    std::size_t       peak_ = 0;
};

}  // namespace book
