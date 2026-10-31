// tools/tui.hpp
#pragma once

// Live terminal view for itch_replay --tui: a table of the watched symbols and a ten-level
// depth ladder for the selected one.
//
// Modelled on Project 1's tools/tui.hpp and kept as its own file so this repo builds without
// it. The risk worth care is LEAVING THE TERMINAL BROKEN. The alternate screen, the hidden
// cursor and the raw (no echo, no line buffering) input mode are all undone in the
// destructor, so every exit path, including a thrown exception, restores them. The signal
// handler only sets a flag. render() returns a string and takes explicit dimensions so it can
// be tested without a terminal.
//
// Keys: n or Tab = next symbol, p = previous, q = quit.

#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <replay/snapshot.hpp>
#include <string>
#include <vector>

namespace replay::tui {

// Set by SIGINT and SIGTERM. A handler may not allocate or do I/O, so it does nothing else.
inline std::atomic<bool> g_interrupted{false};

inline void on_signal(int) {
    g_interrupted.store(true, std::memory_order_relaxed);
}

class Terminal {
public:
    Terminal() {
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        if (isatty(STDIN_FILENO) != 0 && tcgetattr(STDIN_FILENO, &saved_) == 0) {
            termios raw = saved_;
            raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
            raw.c_cc[VMIN]  = 0;  // read() returns at once with whatever is there
            raw.c_cc[VTIME] = 0;
            if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
                raw_ = true;
            }
        }
        std::fputs("\033[?1049h", stdout);  // alternate screen
        std::fputs("\033[?25l", stdout);    // hide cursor
        std::fflush(stdout);
        active_ = true;
    }

    ~Terminal() { restore(); }

    Terminal(const Terminal&)            = delete;
    Terminal& operator=(const Terminal&) = delete;

    void restore() noexcept {
        if (!active_) {
            return;
        }
        if (raw_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &saved_);
            raw_ = false;
        }
        std::fputs("\033[?25h", stdout);    // show cursor
        std::fputs("\033[?1049l", stdout);  // leave alternate screen
        std::fflush(stdout);
        active_ = false;
    }

    // Queried per frame, so a resize needs no signal handling.
    static void size(int& rows, int& cols) noexcept {
        winsize w{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0 && w.ws_col > 0) {
            rows = w.ws_row;
            cols = w.ws_col;
        } else {
            rows = 24;
            cols = 80;
        }
    }

    // One pending key, or 0.
    static char key() noexcept {
        char c = 0;
        return read(STDIN_FILENO, &c, 1) == 1 ? c : 0;
    }

    static void draw(const std::string& frame) {
        std::fputs("\033[H", stdout);  // cursor home; no clear, to avoid flicker
        std::fwrite(frame.data(), 1, frame.size(), stdout);
        std::fflush(stdout);
    }

private:
    bool    active_ = false;
    bool    raw_    = false;
    termios saved_{};
};

struct View {
    std::vector<std::string>    names;  // parallel to snaps
    std::vector<SymbolSnapshot> snaps;
    std::size_t                 selected     = 0;
    std::uint64_t               feed_ns      = 0;
    std::uint64_t               messages     = 0;
    double                      speed        = 0;  // 0 = max
    std::uint64_t               lag_ns       = 0;  // max lag so far
    double                      msgs_per_sec = 0;
};

namespace detail {

inline std::string price(std::int32_t px) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d.%04d", px / 10000, px % 10000);
    return buf;
}

inline std::string clock_text(std::uint64_t ns) {
    const std::uint64_t s = ns / 1'000'000'000ULL;
    char                buf[32];
    std::snprintf(buf, sizeof buf, "%02u:%02u:%02u", static_cast<unsigned>(s / 3600),
                  static_cast<unsigned>(s / 60 % 60), static_cast<unsigned>(s % 60));
    return buf;
}

inline std::string bar(std::uint64_t qty, std::uint64_t max_qty, int width) {
    if (width <= 0 || max_qty == 0) {
        return {};
    }
    const auto n =
        static_cast<std::size_t>(static_cast<double>(qty) / static_cast<double>(max_qty) * width);
    return std::string(n, '#');
}

inline std::string rate_text(double r) {
    char buf[32];
    if (r >= 1e6) {
        std::snprintf(buf, sizeof buf, "%.2f M msgs/s", r / 1e6);
    } else if (r >= 1e3) {
        std::snprintf(buf, sizeof buf, "%.1f K msgs/s", r / 1e3);
    } else {
        std::snprintf(buf, sizeof buf, "%.0f msgs/s", r);
    }
    return buf;
}

}  // namespace detail

// Header, a row per watched symbol (the selected one marked), then the selected symbol's
// ladder: asks worst to best going down, the spread, bids best to worst. Clamped to the given
// size. A missing side is shown as missing, never as a price of zero.
inline std::string render(const View& v, int rows, int cols) {
    if (rows < 6 || cols < 40) {
        return "terminal too small\n";
    }
    std::string       out;
    char              line[512];
    const std::string speed =
        v.speed <= 0 ? "max" : (std::to_string(static_cast<int>(v.speed)) + "x");
    std::snprintf(line, sizeof line,
                  "\033[K  ITCH REPLAY  feed %s  speed %s  %llu msgs  %s  max lag %.1f us\n",
                  detail::clock_text(v.feed_ns).c_str(), speed.c_str(),
                  static_cast<unsigned long long>(v.messages),
                  detail::rate_text(v.msgs_per_sec).c_str(), static_cast<double>(v.lag_ns) / 1e3);
    out += line;
    int used = 1;

    std::snprintf(line, sizeof line, "\033[K  %-8s %12s %12s %10s %10s %10s\n", "symbol", "bid",
                  "ask", "bid qty", "ask qty", "spread");
    out += line;
    ++used;
    const std::size_t table_rows = std::min<std::size_t>(
        v.snaps.size(), static_cast<std::size_t>(std::max(1, (rows - 4) / 3)));
    for (std::size_t i = 0; i < table_rows; ++i) {
        const SymbolSnapshot& s       = v.snaps[i];
        const bool            has_bid = s.bid_qty[0] != 0;
        const bool            has_ask = s.ask_qty[0] != 0;
        const std::string     bid     = has_bid ? detail::price(s.bid_px[0]) : "-";
        const std::string     ask     = has_ask ? detail::price(s.ask_px[0]) : "-";
        const std::string     spread =
            has_bid && has_ask ? detail::price(s.ask_px[0] - s.bid_px[0]) : "-";
        std::snprintf(line, sizeof line, "\033[K%s %-8s %12s %12s %10llu %10llu %10s\n",
                      i == v.selected ? ">" : " ", v.names[i].c_str(), bid.c_str(), ask.c_str(),
                      static_cast<unsigned long long>(s.bid_qty[0]),
                      static_cast<unsigned long long>(s.ask_qty[0]), spread.c_str());
        out += line;
        ++used;
    }

    if (v.selected < v.snaps.size()) {
        const SymbolSnapshot& s = v.snaps[v.selected];
        out += "\033[K\n";
        ++used;
        const int     per_side = std::clamp((rows - used - 3) / 2, 0, kDepth);
        std::uint64_t maxq     = 0;
        int           nb = 0, na = 0;
        for (int i = 0; i < kDepth; ++i) {
            nb   = s.bid_qty[i] != 0 ? i + 1 : nb;
            na   = s.ask_qty[i] != 0 ? i + 1 : na;
            maxq = std::max({maxq, s.bid_qty[i], s.ask_qty[i]});
        }
        nb                  = std::min(nb, per_side);
        na                  = std::min(na, per_side);
        const int bar_width = cols > 40 ? cols - 40 : 0;
        for (int i = na; i-- > 0;) {
            std::snprintf(line, sizeof line, "\033[K  \033[31m%12s %10llu\033[0m  %s\n",
                          detail::price(s.ask_px[i]).c_str(),
                          static_cast<unsigned long long>(s.ask_qty[i]),
                          detail::bar(s.ask_qty[i], maxq, bar_width).c_str());
            out += line;
        }
        if (s.bid_qty[0] == 0 || s.ask_qty[0] == 0) {
            out += "\033[K  ------- one side empty -------\n";
        } else {
            std::snprintf(line, sizeof line, "\033[K  ------- %s spread %s -------\n",
                          v.names[v.selected].c_str(),
                          detail::price(s.ask_px[0] - s.bid_px[0]).c_str());
            out += line;
        }
        for (int i = 0; i < nb; ++i) {
            std::snprintf(line, sizeof line, "\033[K  \033[32m%12s %10llu\033[0m  %s\n",
                          detail::price(s.bid_px[i]).c_str(),
                          static_cast<unsigned long long>(s.bid_qty[i]),
                          detail::bar(s.bid_qty[i], maxq, bar_width).c_str());
            out += line;
        }
    }
    out += "\033[K\n\033[K  n/Tab next symbol   p previous   q or ctrl-c quit\033[J\n";
    return out;
}

}  // namespace replay::tui
