// include/book/market_state.hpp
#pragma once

// Scoped market state: is a symbol in continuous trading right now?
//
// continuous(locate) is true only when ALL of these hold:
//   1. the system is between the 'Q' (start of market hours) and 'M' (end of market hours)
//      system events;
//   2. the symbol's last trading state is 'T' (a symbol with no 'H' yet is treated as 'T');
//   3. the symbol is not awaiting a cross:
//        - a Nasdaq-listed symbol (market category Q, G or S) awaits its opening cross
//          (cross type 'O') from the 'Q' event until it arrives;
//        - a symbol that resumes to 'T' from any other state awaits a halt or IPO cross
//          (cross type 'H' or 'I'), or a grace period of feed time after the resume.
//
// Feed time is the timestamp of the latest message seen. The grace default is 5 s, a
// constructor parameter, kept after a sensitivity run (results/halt_grace_12302019.json).
//
// A trading action may arrive before the stock directory for its locate; the state is kept
// per locate and nothing here assumes a directory entry exists.

#include <array>
#include <cstdint>
#include <itch/messages.hpp>
#include <vector>

namespace book {

class MarketState {
public:
    static constexpr std::uint64_t kDefaultGraceNs = 5'000'000'000ULL;

    explicit MarketState(std::uint64_t halt_grace_ns = kDefaultGraceNs)
        : grace_ns_(halt_grace_ns), sym_(65536) {}

    void advance(std::uint64_t ts_ns) noexcept { now_ = ts_ns; }

    void on(const itch::SystemEventView& v) {
        advance(v.timestamp_ns());
        switch (v.event_code()) {
            case 'Q':
                in_hours_ = true;
                for (Sym& s : sym_) {
                    s.open_cross_seen = false;  // a fresh session: every opening cross is due
                }
                break;
            case 'M':
            case 'E':
            case 'C':
            case 'O':
            case 'S':
                in_hours_ = false;
                break;
            default:
                break;
        }
    }

    void on(const itch::StockDirectoryView& v) {
        advance(v.timestamp_ns());
        const char c    = v.market_category();
        Sym&       s    = sym_[v.locate()];
        s.nasdaq_listed = (c == 'Q' || c == 'G' || c == 'S');
    }

    void on(const itch::TradingActionView& v) {
        advance(v.timestamp_ns());
        Sym&       s    = sym_[v.locate()];
        const char next = v.state();
        if (next == 'T' && s.state != 'T') {
            s.awaiting_halt_cross = true;
            s.grace_until_ns      = v.timestamp_ns() + grace_ns_;
        }
        s.state = next;
    }

    void on(const itch::CrossTradeView& v) {
        advance(v.timestamp_ns());
        Sym&       s = sym_[v.locate()];
        const char t = v.cross_type();
        if (t == 'O') {
            s.open_cross_seen = true;
        } else if (t == 'H' || t == 'I') {
            s.awaiting_halt_cross = false;
        }
    }

    [[nodiscard]] bool continuous(std::uint16_t locate) const noexcept {
        if (!in_hours_) {
            return false;
        }
        const Sym& s = sym_[locate];
        if (s.state != 'T') {
            return false;
        }
        if (s.nasdaq_listed && !s.open_cross_seen) {
            return false;
        }
        return !s.awaiting_halt_cross || now_ >= s.grace_until_ns;
    }

    [[nodiscard]] bool          in_market_hours() const noexcept { return in_hours_; }
    [[nodiscard]] std::uint64_t now_ns() const noexcept { return now_; }

private:
    struct Sym {
        char          state               = 'T';
        bool          nasdaq_listed       = false;
        bool          open_cross_seen     = false;
        bool          awaiting_halt_cross = false;
        std::uint64_t grace_until_ns      = 0;
    };

    std::uint64_t    grace_ns_;
    std::uint64_t    now_      = 0;
    bool             in_hours_ = false;
    std::vector<Sym> sym_;
};

}  // namespace book
