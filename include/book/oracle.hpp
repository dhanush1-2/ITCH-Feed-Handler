// include/book/oracle.hpp
#pragma once

// The feed oracle: the exchange's own executions as ground truth for the book.
//
// An E or C names the order that actually traded. If the book is right, that order was at
// the best price on its side and at the head of that level's queue. Each execution is
// checked BEFORE the fill is applied (the builder's before_execute hook), and each exception
// is counted, not thrown: a mismatch is a finding about the data or the model, not a crash.
//
//   exec_not_best   the executed order's level is not the best level of its side
//   exec_not_head   it is at the best level but not first in that level's FIFO
//                   (the two are exclusive: not_best takes precedence)
//
// A crossed book (best bid strictly above best ask) is counted after each message that
// touched a book, split by whether the symbol was in continuous trading at that moment.
// A locked book (bid == ask) is not crossed.
//
// The first kKeep instances of each exception are kept with timestamp, locate and ref.

#include <array>
#include <book/book.hpp>
#include <cstddef>
#include <cstdint>
#include <ob/order.hpp>
#include <ob/order_pool.hpp>
#include <vector>

namespace book {

struct OracleCounts {
    std::uint64_t exec_checked       = 0;
    std::uint64_t exec_not_best      = 0;
    std::uint64_t exec_not_head      = 0;
    std::uint64_t crossed_continuous = 0;  // touches that left a crossed book, in continuous
    std::uint64_t crossed_other      = 0;  // ... and outside it
    // Denominators and splits. The *_continuous fields are subsets of the totals above.
    std::uint64_t exec_checked_continuous  = 0;
    std::uint64_t exec_not_best_continuous = 0;
    std::uint64_t exec_not_head_continuous = 0;
    std::uint64_t touches_continuous = 0;  // book-changing messages seen in continuous trading
    std::uint64_t touches_other      = 0;
    // A crossed episode starts when a book goes from not crossed to crossed (and ends when it
    // is uncrossed), classified by continuous() at its start. Lasting crossed books add
    // crossed_* once per touch but an episode only once.
    std::uint64_t episodes_continuous = 0;
    std::uint64_t episodes_other      = 0;
};

enum class OracleKind : std::uint8_t { NotBest = 0, NotHead, CrossedContinuous, CrossedOther };

struct OracleException {
    OracleKind    kind;
    std::uint64_t timestamp_ns;
    std::uint16_t locate;
    std::uint64_t ref;  // the executed order, or the message's ref for a crossed book (0 if none)
};

class Oracle {
public:
    static constexpr std::size_t kKeep = 100;

    // Call before the fill is applied.
    void on_execute(const Book& bk, const ob::OrderPool& pool, const ob::Order& o,
                    std::uint64_t ts_ns, bool continuous) {
        ++counts_.exec_checked;
        counts_.exec_checked_continuous += continuous;
        const bool   buy   = o.side == ob::Side::Buy;
        const bool   empty = bk.level_count(o.side) == 0;
        if (empty) {
            record(OracleKind::NotBest, counts_.exec_not_best, ts_ns, o.pad, o.id);
            counts_.exec_not_best_continuous += continuous;
            return;
        }
        const Level& best = buy ? bk.bids().front() : bk.asks().front();
        if (best.price != o.price) {
            record(OracleKind::NotBest, counts_.exec_not_best, ts_ns, o.pad, o.id);
            counts_.exec_not_best_continuous += continuous;
        } else if (best.head == ob::kInvalidSlot || pool.at(best.head).id != o.id) {
            record(OracleKind::NotHead, counts_.exec_not_head, ts_ns, o.pad, o.id);
            counts_.exec_not_head_continuous += continuous;
        }
    }

    // Called after every book-changing message with the book's crossed state before (`was`) and
    // after (`now`) it. Counts the touch, the crossed touch, and any new episode.
    void on_touch(bool continuous, bool was_crossed, bool now_crossed, std::uint64_t ts_ns,
                  std::uint16_t locate, std::uint64_t ref) {
        (continuous ? counts_.touches_continuous : counts_.touches_other)++;
        if (now_crossed) {
            on_crossed(continuous, ts_ns, locate, ref);
            if (!was_crossed) {
                (continuous ? counts_.episodes_continuous : counts_.episodes_other)++;
            }
        }
    }

    void on_crossed(bool continuous, std::uint64_t ts_ns, std::uint16_t locate, std::uint64_t ref) {
        if (continuous) {
            record(OracleKind::CrossedContinuous, counts_.crossed_continuous, ts_ns, locate, ref);
        } else {
            record(OracleKind::CrossedOther, counts_.crossed_other, ts_ns, locate, ref);
        }
    }

    [[nodiscard]] const OracleCounts& counts() const noexcept { return counts_; }
    [[nodiscard]] const std::vector<OracleException>& exceptions(OracleKind k) const noexcept {
        return kept_[static_cast<std::size_t>(k)];
    }

private:
    void record(OracleKind k, std::uint64_t& counter, std::uint64_t ts, std::uint16_t loc,
                std::uint64_t ref) {
        ++counter;
        auto& v = kept_[static_cast<std::size_t>(k)];
        if (v.size() < kKeep) {
            v.push_back(OracleException{k, ts, loc, ref});
        }
    }

    OracleCounts                                counts_{};
    std::array<std::vector<OracleException>, 4> kept_{};
};

}  // namespace book
