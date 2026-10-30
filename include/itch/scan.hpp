// include/itch/scan.hpp
#pragma once

// Light passes over a whole file that read only framing and a few fields, no book:
//   resolve_symbols   stock names -> stock_locate, from the 'R' (stock directory) messages
//   scan_broken       the match numbers named by 'B' (broken trade) messages
//
// Both stop silently at the first bad frame; the replay's own framing check reports that.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <itch/framer.hpp>
#include <itch/messages.hpp>
#include <map>
#include <set>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace itch {

// Upper-case, trim spaces. "aapl " -> "AAPL".
[[nodiscard]] inline std::string normalize_symbol(std::string s) {
    while (!s.empty() && s.back() == ' ') {
        s.pop_back();
    }
    std::size_t b = 0;
    while (b < s.size() && s[b] == ' ') {
        ++b;
    }
    s.erase(0, b);
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

// The 8-character, space-padded stock field as a plain name.
[[nodiscard]] inline std::string symbol_name(const std::array<char, 8>& f) {
    return normalize_symbol(std::string(f.data(), f.size()));
}

struct Resolved {
    std::map<std::string, std::uint16_t> locate;      // name -> stock_locate
    std::vector<std::string>             unresolved;  // requested names with no 'R' message
};

// Scans the whole file unless every name is found earlier. The 'R' messages sit at the start
// of a normal day, but a stock listed intraday sends one later, so a name that is not found
// is only called unknown after the entire file has been read.
[[nodiscard]] inline Resolved resolve_symbols(std::span<const std::byte>      data,
                                              const std::vector<std::string>& names) {
    Resolved                   r;
    std::set<std::string>      want(names.begin(), names.end());
    Framer                     fr(data);
    std::span<const std::byte> msg;
    while (r.locate.size() < want.size() && fr.next(msg) == FrameStatus::Ok) {
        if (msg.size() == StockDirectoryView::kSize && static_cast<char>(msg[0]) == 'R') {
            const StockDirectoryView v(msg.data());
            const std::string        name = symbol_name(v.stock());
            if (want.contains(name) && !r.locate.contains(name)) {
                r.locate[name] = v.locate();
            }
        }
    }
    for (const std::string& n : want) {
        if (!r.locate.contains(n)) {
            r.unresolved.push_back(n);
        }
    }
    return r;
}

[[nodiscard]] inline std::unordered_set<std::uint64_t> scan_broken(
    std::span<const std::byte> data) {
    std::unordered_set<std::uint64_t> out;
    Framer                            fr(data);
    std::span<const std::byte>        msg;
    while (fr.next(msg) == FrameStatus::Ok) {
        if (msg.size() == BrokenTradeView::kSize && static_cast<char>(msg[0]) == 'B') {
            out.insert(BrokenTradeView(msg.data()).match());
        }
    }
    return out;
}

}  // namespace itch
