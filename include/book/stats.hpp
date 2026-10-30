// include/book/stats.hpp
#pragma once

// Statistics gathered while a file is replayed. StatsObserver plugs into BookBuilder (alone, or
// as the Extra of BasicVerifyObserver) and sees every frame and every book-changing message.
//
// Per symbol (stock_locate): messages, shares added / executed / cancelled / deleted /
// replaced, printed volume and trades. The share ledger must balance at all times:
//
//   added + replaced_in == executed + cancelled + deleted + replaced_out + live
//
// where `added` counts A/F orders, `replaced_in` the new order of each accepted U,
// `replaced_out` the remaining shares of the order each U removed, and `live` is what the
// books hold now. conservation_failures() checks that against the books themselves, so it is
// a comparison of two independent tallies, not a restatement.
//
// Printed volume = E shares + C shares with printable 'Y' + P shares + Q shares, minus the
// prints named by a broken trade (B). A B arrives after its trade, so the broken match
// numbers must be known up front (set_broken_matches, from itch::scan_broken). The same match
// number on more than one print is counted (finish()).
//
// Watched symbols get the top of book recorded once per feed second: the state after the
// LAST change in that second. Seconds with no change to that book have no row; a consumer
// carries the previous row forward. A missing side is kNone and exports as null, never 0.

#include <algorithm>
#include <array>
#include <book/book.hpp>
#include <book/builder.hpp>
#include <cstddef>
#include <cstdint>
#include <itch/messages.hpp>
#include <itch/scan.hpp>
#include <ob/order.hpp>
#include <ob/types.hpp>
#include <ostream>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace book {

struct SymbolStats {
    std::uint64_t messages     = 0;  // frames carrying this locate
    std::uint64_t added        = 0;  // shares, A/F
    std::uint64_t replaced_in  = 0;  // shares, new order of an accepted U
    std::uint64_t executed     = 0;
    std::uint64_t cancelled    = 0;
    std::uint64_t deleted      = 0;
    std::uint64_t replaced_out = 0;
    std::uint64_t volume       = 0;  // printed volume, after the broken-trade rule
    std::uint64_t trades       = 0;
    std::uint64_t orders_added = 0;  // A/F messages accepted
};

struct WatchSeries {
    std::string                symbol;
    std::uint16_t              locate = 0;
    std::vector<std::uint32_t> t;  // feed second
    std::vector<std::int64_t>  bid, ask, bid_sz, ask_sz;
};

class StatsObserver {
public:
    static constexpr std::size_t  kSeconds = 86'400;
    static constexpr std::int64_t kNone    = INT64_MIN;  // a side with no orders

    StatsObserver() : sym_(65536), name_(65536), watch_idx_(65536, -1), per_second_(kSeconds, 0) {}

    // Lets after_apply read the book of a watched symbol. B is the BookBuilder in use.
    template <class B>
    void bind(const B& b) noexcept {
        ctx_  = &b;
        book_ = [](const void* c, std::uint16_t l) -> const Book& {
            return static_cast<const B*>(c)->book(l);
        };
    }

    void watch(const std::string& symbol, std::uint16_t locate) {
        watch_idx_[locate] = static_cast<std::int32_t>(watch_.size());
        watch_.push_back(WatchSeries{symbol, locate, {}, {}, {}, {}, {}});
    }

    void set_broken_matches(std::unordered_set<std::uint64_t> m) { broken_ = std::move(m); }

    // Every frame, before it is decoded, including types the decoder skips.
    void on_frame(std::span<const std::byte> msg) {
        if (msg.empty()) {
            return;
        }
        ++frames_;
        ++by_type_[std::to_integer<unsigned char>(msg[0])];
        if (msg.size() < 11) {
            ++short_frames_;
            return;
        }
        const itch::HeaderView h(msg.data());
        const std::uint64_t    ts = h.timestamp_ns();
        if (!have_ts_) {
            first_ts_ = ts;
            have_ts_  = true;
        }
        last_ts_ = ts;
        ++sym_[h.locate()].messages;
        const std::uint64_t sec = ts / 1'000'000'000ULL;
        if (sec < kSeconds) {
            ++per_second_[sec];
        } else {
            ++out_of_range_seconds_;
        }
    }

    // ---- BookBuilder hooks ------------------------------------------------------------

    template <class View>
    void pass(const View& v) {
        ts_ = v.timestamp_ns();
        if constexpr (std::is_same_v<View, itch::ExecutedView>) {
            print(v.locate(), v.match(), v.shares(), PrintKind::E, true);
        } else if constexpr (std::is_same_v<View, itch::ExecutedPriceView>) {
            print(v.locate(), v.match(), v.shares(), PrintKind::C, v.printable() == 'Y');
        } else if constexpr (std::is_same_v<View, itch::TradeView>) {
            print(v.locate(), v.match(), v.shares(), PrintKind::P, true);
        } else if constexpr (std::is_same_v<View, itch::CrossTradeView>) {
            print(v.locate(), v.match(), v.shares(), PrintKind::Q, true);
        } else if constexpr (std::is_same_v<View, itch::BrokenTradeView>) {
            ++broken_messages_;
        } else if constexpr (std::is_same_v<View, itch::SystemEventView>) {
            last_event_ = v.event_code();
            ++system_events_;
        } else if constexpr (std::is_same_v<View, itch::StockDirectoryView>) {
            name_[v.locate()] = itch::symbol_name(v.stock());
        }
    }

    void before_execute(const Book&, const ob::Order&, ob::Side, std::uint32_t, std::uint64_t,
                        std::uint32_t) noexcept {}

    void added(std::uint16_t locate, std::uint32_t shares, bool replacement) {
        SymbolStats& s = sym_[locate];
        if (replacement) {
            s.replaced_in += shares;
        } else {
            s.added += shares;
            ++s.orders_added;
        }
    }

    void removed(std::uint16_t locate, RemoveKind kind, std::uint32_t shares) {
        SymbolStats& s = sym_[locate];
        switch (kind) {
            case RemoveKind::Executed:
                s.executed += shares;
                break;
            case RemoveKind::Cancelled:
                s.cancelled += shares;
                break;
            case RemoveKind::Deleted:
                s.deleted += shares;
                break;
            case RemoveKind::ReplacedOut:
                s.replaced_out += shares;
                break;
        }
    }

    void after_apply(std::uint16_t locate) {
        const std::int32_t w = watch_idx_[locate];
        if (w < 0) {
            return;
        }
        const Book&  bk  = book_(ctx_, locate);
        WatchSeries& ws  = watch_[static_cast<std::size_t>(w)];
        const auto   sec = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(ts_ / 1'000'000'000ULL, 0xFFFFFFFFULL));
        if (ws.t.empty() || ws.t.back() != sec) {
            ws.t.push_back(sec);
            ws.bid.push_back(kNone);
            ws.ask.push_back(kNone);
            ws.bid_sz.push_back(kNone);
            ws.ask_sz.push_back(kNone);
        }
        const auto bb    = bk.best_bid();
        const auto ba    = bk.best_ask();
        ws.bid.back()    = bb ? *bb : kNone;
        ws.ask.back()    = ba ? *ba : kNone;
        ws.bid_sz.back() = bb ? static_cast<std::int64_t>(bk.bids().front().total) : kNone;
        ws.ask_sz.back() = ba ? static_cast<std::int64_t>(bk.asks().front().total) : kNone;
    }

    // Call once after the last message: sorts the match numbers and counts collisions.
    void finish() {
        if (finished_) {
            return;
        }
        finished_ = true;
        std::sort(prints_.begin(), prints_.end());
        for (std::size_t i = 0; i < prints_.size();) {
            std::size_t j     = i + 1;
            bool        mixed = false;
            while (j < prints_.size() && prints_[j].first == prints_[i].first) {
                mixed |= prints_[j].second != prints_[i].second;
                ++j;
            }
            if (j - i > 1) {
                ++match_collisions_;
                match_extra_prints_ += j - i - 1;
                match_collisions_cross_kind_ += mixed;
            }
            i = j;
        }
        broken_without_print_ = broken_.size() - broken_hit_.size();
    }

    // ---- results ----------------------------------------------------------------------

    [[nodiscard]] const SymbolStats& symbol(std::uint16_t l) const noexcept { return sym_[l]; }
    [[nodiscard]] const std::string& name(std::uint16_t l) const noexcept { return name_[l]; }
    [[nodiscard]] const std::vector<WatchSeries>&   watched() const noexcept { return watch_; }
    [[nodiscard]] const std::vector<std::uint64_t>& per_second() const noexcept {
        return per_second_;
    }
    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] std::uint64_t short_frames() const noexcept { return short_frames_; }
    [[nodiscard]] std::uint64_t count_of_type(char t) const noexcept {
        return by_type_[static_cast<unsigned char>(t)];
    }
    [[nodiscard]] std::uint64_t first_ts() const noexcept { return first_ts_; }
    [[nodiscard]] std::uint64_t last_ts() const noexcept { return last_ts_; }
    [[nodiscard]] std::uint64_t out_of_range_seconds() const noexcept {
        return out_of_range_seconds_;
    }
    [[nodiscard]] char          last_system_event() const noexcept { return last_event_; }
    [[nodiscard]] std::uint64_t system_events() const noexcept { return system_events_; }
    // Printed volume by message kind (after the broken-trade rule), indexed E, C, P, Q.
    [[nodiscard]] std::uint64_t volume_of(char kind) const noexcept {
        return volume_by_kind_[kind_index(kind)];
    }
    [[nodiscard]] std::uint64_t printed_volume() const noexcept {
        std::uint64_t t = 0;
        for (const std::uint64_t v : volume_by_kind_) {
            t += v;
        }
        return t;
    }
    [[nodiscard]] std::uint64_t trade_count() const noexcept {
        std::uint64_t t = 0;
        for (const std::uint64_t v : trades_by_kind_) {
            t += v;
        }
        return t;
    }
    [[nodiscard]] std::uint64_t broken_messages() const noexcept { return broken_messages_; }
    [[nodiscard]] std::uint64_t broken_prints() const noexcept { return broken_prints_; }
    [[nodiscard]] std::uint64_t broken_shares() const noexcept { return broken_shares_; }
    [[nodiscard]] std::uint64_t broken_without_print() const noexcept {
        return broken_without_print_;
    }
    [[nodiscard]] std::uint64_t nonprintable_prints() const noexcept { return nonprintable_; }
    [[nodiscard]] std::uint64_t match_collisions() const noexcept { return match_collisions_; }
    [[nodiscard]] std::uint64_t match_extra_prints() const noexcept { return match_extra_prints_; }
    [[nodiscard]] std::uint64_t match_collisions_cross_kind() const noexcept {
        return match_collisions_cross_kind_;
    }

    template <class F>
    void for_each_type(F&& f) const {
        for (std::size_t t = 0; t < by_type_.size(); ++t) {
            if (by_type_[t] != 0) {
                f(static_cast<char>(t), by_type_[t]);
            }
        }
    }

private:
    enum class PrintKind : std::uint8_t { E = 0, C, P, Q };

    static constexpr std::size_t kind_index(char k) noexcept {
        return k == 'E' ? 0 : k == 'C' ? 1 : k == 'P' ? 2 : 3;
    }

    void print(std::uint16_t locate, std::uint64_t match, std::uint64_t shares, PrintKind kind,
               bool printable) {
        const auto k = static_cast<std::size_t>(kind);
        prints_.emplace_back(match, static_cast<char>(k));
        if (!printable) {
            ++nonprintable_;
            return;
        }
        if (!broken_.empty() && broken_.contains(match)) {
            ++broken_prints_;
            broken_shares_ += shares;
            broken_hit_.insert(match);
            return;
        }
        volume_by_kind_[k] += shares;
        ++trades_by_kind_[k];
        sym_[locate].volume += shares;
        ++sym_[locate].trades;
    }

    std::vector<SymbolStats>                    sym_;
    std::vector<std::string>                    name_;
    std::vector<std::int32_t>                   watch_idx_;
    std::vector<WatchSeries>                    watch_;
    std::vector<std::uint64_t>                  per_second_;
    std::array<std::uint64_t, 256>              by_type_{};
    std::array<std::uint64_t, 4>                volume_by_kind_{};
    std::array<std::uint64_t, 4>                trades_by_kind_{};
    std::unordered_set<std::uint64_t>           broken_;
    std::unordered_set<std::uint64_t>           broken_hit_;
    std::vector<std::pair<std::uint64_t, char>> prints_;  // match number, kind
    const void*                                 ctx_ = nullptr;
    const Book& (*book_)(const void*, std::uint16_t) = nullptr;
    std::uint64_t ts_                                = 0;
    std::uint64_t frames_ = 0, short_frames_ = 0, out_of_range_seconds_ = 0;
    std::uint64_t first_ts_ = 0, last_ts_ = 0;
    std::uint64_t system_events_   = 0;
    std::uint64_t broken_messages_ = 0, broken_prints_ = 0, broken_shares_ = 0;
    std::uint64_t broken_without_print_ = 0, nonprintable_ = 0;
    std::uint64_t match_collisions_ = 0, match_extra_prints_ = 0;
    std::uint64_t match_collisions_cross_kind_ = 0;
    char          last_event_                  = 0;
    bool          have_ts_                     = false;
    bool          finished_                    = false;
};

struct ConservationFailure {
    std::uint16_t locate;
    std::int64_t
        imbalance;  // (added + replaced_in) - (executed + cancelled + deleted + out + live)
};

// Shares the books hold for one symbol: the sum of its level totals.
template <class B>
[[nodiscard]] std::uint64_t live_shares(const B& b, std::uint16_t locate) {
    std::uint64_t t = 0;
    for (const Level& lv : b.book(locate).bids()) {
        t += lv.total;
    }
    for (const Level& lv : b.book(locate).asks()) {
        t += lv.total;
    }
    return t;
}

// Every symbol whose ledger does not balance against its book. Empty means conservation holds.
template <class B>
[[nodiscard]] std::vector<ConservationFailure> conservation_failures(const B&             b,
                                                                     const StatsObserver& st) {
    std::vector<ConservationFailure> bad;
    for (std::size_t i = 0; i < B::kLocates; ++i) {
        const auto          l  = static_cast<std::uint16_t>(i);
        const SymbolStats&  s  = st.symbol(l);
        const std::uint64_t in = s.added + s.replaced_in;
        const std::uint64_t out =
            s.executed + s.cancelled + s.deleted + s.replaced_out + live_shares(b, l);
        if (in != out) {
            bad.push_back({l, static_cast<std::int64_t>(in) - static_cast<std::int64_t>(out)});
        }
    }
    return bad;
}

namespace detail {

inline void json_series(std::ostream& os, const std::vector<std::int64_t>& v) {
    os << '[';
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i != 0) {
            os << ',';
        }
        if (v[i] == StatsObserver::kNone) {
            os << "null";
        } else {
            os << v[i];
        }
    }
    os << ']';
}

}  // namespace detail

// {"SYM": {"t":[...], "bid":[...], "ask":[...], "bid_sz":[...], "ask_sz":[...], "spread":[...]}}
// Prices are raw ticks (1/10000 dollar). A missing side is null, and so is the spread when
// either side is missing; a crossed book keeps its negative spread.
inline void write_watch_json(std::ostream& os, const StatsObserver& st, const std::string& ind) {
    os << "{";
    bool first = true;
    for (const WatchSeries& w : st.watched()) {
        os << (first ? "\n" : ",\n") << ind << "  \"" << w.symbol << "\": {\n";
        os << ind << "    \"locate\": " << w.locate << ",\n";
        os << ind << "    \"t\": [";
        for (std::size_t i = 0; i < w.t.size(); ++i) {
            os << (i != 0 ? "," : "") << w.t[i];
        }
        os << "],\n" << ind << "    \"bid\": ";
        detail::json_series(os, w.bid);
        os << ",\n" << ind << "    \"ask\": ";
        detail::json_series(os, w.ask);
        os << ",\n" << ind << "    \"bid_sz\": ";
        detail::json_series(os, w.bid_sz);
        os << ",\n" << ind << "    \"ask_sz\": ";
        detail::json_series(os, w.ask_sz);
        std::vector<std::int64_t> spread(w.t.size(), StatsObserver::kNone);
        for (std::size_t i = 0; i < w.t.size(); ++i) {
            if (w.bid[i] != StatsObserver::kNone && w.ask[i] != StatsObserver::kNone) {
                spread[i] = w.ask[i] - w.bid[i];
            }
        }
        os << ",\n" << ind << "    \"spread\": ";
        detail::json_series(os, spread);
        os << "\n" << ind << "  }";
        first = false;
    }
    os << (first ? "}" : "\n" + ind + "}");
}

}  // namespace book
