// fuzz/fuzz_parser.cpp
//
// Feeds libFuzzer's bytes to the parser with no sanitising, two ways:
//   1. The framer loop, then decode() of every frame, reading every accessor of every view.
//   2. decode() directly on the raw input, as if the whole input were one message.
//
// Properties under test: no out-of-bounds read (ASan), no undefined behaviour (UBSan), and
// forward progress (a framer that returns Ok without advancing would spin forever).
// ITCH_VERIFY is on, so the view type assertions also run.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <itch/dispatch.hpp>
#include <itch/framer.hpp>
#include <itch/messages.hpp>
#include <span>

namespace {

volatile std::uint64_t g_sink = 0;

void eat(std::uint64_t v) {
    g_sink = g_sink + v;
}

template <std::size_t N>
void eat(const std::array<char, N>& a) {
    for (const char c : a) {
        eat(static_cast<std::uint64_t>(static_cast<unsigned char>(c)));
    }
}

// Reads every accessor of every view type.
struct ReadAll {
    void header(const itch::HeaderView& v) {
        eat(static_cast<std::uint64_t>(v.type()));
        eat(v.locate());
        eat(v.tracking());
        eat(v.timestamp_ns());
    }
    void on(const itch::AddOrderView& v) {
        header(v);
        eat(v.ref());
        eat(static_cast<std::uint64_t>(v.side()));
        eat(v.shares());
        eat(v.stock());
        eat(v.price());
        if (v.has_mpid()) {
            eat(v.attribution());
        }
    }
    void on(const itch::ExecutedView& v) {
        header(v);
        eat(v.ref());
        eat(v.shares());
        eat(v.match());
    }
    void on(const itch::ExecutedPriceView& v) {
        header(v);
        eat(v.ref());
        eat(v.shares());
        eat(v.match());
        eat(static_cast<std::uint64_t>(v.printable()));
        eat(v.price());
    }
    void on(const itch::CancelView& v) {
        header(v);
        eat(v.ref());
        eat(v.shares());
    }
    void on(const itch::DeleteView& v) {
        header(v);
        eat(v.ref());
    }
    void on(const itch::ReplaceView& v) {
        header(v);
        eat(v.old_ref());
        eat(v.new_ref());
        eat(v.shares());
        eat(v.price());
    }
    void on(const itch::TradeView& v) {
        header(v);
        eat(v.ref());
        eat(static_cast<std::uint64_t>(v.side()));
        eat(v.shares());
        eat(v.stock());
        eat(v.price());
        eat(v.match());
    }
    void on(const itch::CrossTradeView& v) {
        header(v);
        eat(v.shares());
        eat(v.stock());
        eat(v.price());
        eat(v.match());
        eat(static_cast<std::uint64_t>(v.cross_type()));
    }
    void on(const itch::BrokenTradeView& v) {
        header(v);
        eat(v.match());
    }
    void on(const itch::SystemEventView& v) {
        header(v);
        eat(static_cast<std::uint64_t>(v.event_code()));
    }
    void on(const itch::TradingActionView& v) {
        header(v);
        eat(v.stock());
        eat(static_cast<std::uint64_t>(v.state()));
        eat(static_cast<std::uint64_t>(v.reserved()));
        eat(v.reason());
    }
    void on(const itch::StockDirectoryView& v) {
        header(v);
        eat(v.stock());
        eat(static_cast<std::uint64_t>(v.market_category()));
        eat(static_cast<std::uint64_t>(v.financial_status()));
        eat(v.lot_size());
        for (const std::byte b : v.rest()) {
            eat(std::to_integer<std::uint64_t>(b));
        }
    }
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto*                      bytes = reinterpret_cast<const std::byte*>(data);
    const std::span<const std::byte> in(bytes, size);
    ReadAll                          reader;

    // 1. Framer loop. Each Ok consumes at least the 2-byte prefix plus one byte, so more
    //    than size/3 + 1 frames from `size` bytes means the framer stopped advancing.
    itch::Framer               framer(in);
    std::span<const std::byte> msg;
    std::size_t                frames = 0;
    std::size_t                prev   = 0;
    while (framer.next(msg) == itch::FrameStatus::Ok) {
        if (++frames > size / 3 + 1 || (frames > 1 && framer.offset() <= prev)) {
            std::fprintf(stderr, "NO FORWARD PROGRESS at offset %zu\n", framer.offset());
            std::abort();
        }
        prev = framer.offset();
        (void)itch::decode(msg, reader);
    }

    // 2. The raw input as a single message.
    (void)itch::decode(in, reader);
    return 0;
}
