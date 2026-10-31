// include/replay/snapshot.hpp
#pragma once

// What the live terminal view reads. The replay thread is the only writer: after a message for
// a WATCHED symbol has been fully applied it copies the top ten levels of that book into a
// SymbolSnapshot and stores it through Project 1's ob::Seqlock. The view thread reads whenever
// it likes. The writer never waits for a reader: a reader that stalls, or has gone, costs the
// replay nothing, and a reader that lands mid-write retries (ob/seqlock.hpp).
//
// Unwatched symbols are never copied, so a replay with no --tui does none of this work.
//
// Levels are best first. A side with fewer than ten levels has qty 0 and px 0 in the unused
// slots; qty 0 is how to tell "no level" from a price. Prices are raw (1/10000 dollar).

#include <book/book.hpp>
#include <cstdint>
#include <deque>
#include <ob/seqlock.hpp>
#include <type_traits>
#include <vector>

namespace replay {

inline constexpr int kDepth = 10;

struct SymbolSnapshot {
    std::uint16_t locate = 0;
    std::int32_t  bid_px[kDepth]{};
    std::int32_t  ask_px[kDepth]{};
    std::uint64_t bid_qty[kDepth]{};
    std::uint64_t ask_qty[kDepth]{};
    std::uint64_t last_trade_ts = 0;  // feed ns of the last execution or trade print
};
static_assert(std::is_trivially_copyable_v<SymbolSnapshot>);
static_assert(sizeof(SymbolSnapshot) % sizeof(std::uint64_t) == 0);

// Pure copy of the top of one book.
inline SymbolSnapshot make_snapshot(const book::Book& bk, std::uint16_t locate,
                                    std::uint64_t last_trade_ts) {
    SymbolSnapshot s{};
    s.locate        = locate;
    s.last_trade_ts = last_trade_ts;
    int i           = 0;
    for (const book::Level& lv : bk.bids()) {
        if (i == kDepth) {
            break;
        }
        s.bid_px[i]  = lv.price;
        s.bid_qty[i] = lv.total;
        ++i;
    }
    i = 0;
    for (const book::Level& lv : bk.asks()) {
        if (i == kDepth) {
            break;
        }
        s.ask_px[i]  = lv.price;
        s.ask_qty[i] = lv.total;
        ++i;
    }
    return s;
}

class SnapshotBoard {
public:
    SnapshotBoard() : index_(65536, -1) {}

    // Setup, before the replay starts and before any reader does. Returns the symbol's slot.
    int watch(std::uint16_t locate) {
        if (index_[locate] >= 0) {
            return index_[locate];
        }
        index_[locate] = static_cast<std::int16_t>(locks_.size());
        locks_.emplace_back();
        return index_[locate];
    }

    // The slot of a watched symbol, or -1. One array lookup: cheap enough to ask per message.
    [[nodiscard]] int index_of(std::uint16_t locate) const noexcept { return index_[locate]; }
    [[nodiscard]] std::size_t size() const noexcept { return locks_.size(); }

    // Writer. Call after the message for `locate` has been fully applied. No-op if unwatched.
    template <class Builder>
    void publish(const Builder& b, std::uint16_t locate, std::uint64_t last_trade_ts) {
        const int i = index_[locate];
        if (i >= 0) {
            locks_[static_cast<std::size_t>(i)].store(
                make_snapshot(b.book(locate), locate, last_trade_ts));
        }
    }

    // Writer, for a snapshot built elsewhere.
    void store(std::size_t i, const SymbolSnapshot& s) noexcept { locks_[i].store(s); }

    // Reader. False if a write was in progress; the caller keeps its last good copy.
    [[nodiscard]] bool try_read(std::size_t i, SymbolSnapshot& out) const noexcept {
        return locks_[i].try_load(out);
    }

private:
    std::vector<std::int16_t> index_;
    std::deque<ob::Seqlock<SymbolSnapshot>>
        locks_;  // deque: Seqlock is neither movable nor copyable
};

}  // namespace replay
