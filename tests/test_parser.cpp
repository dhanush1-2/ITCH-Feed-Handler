#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <itch/dispatch.hpp>
#include <itch/framer.hpp>
#include <itch/messages.hpp>
#include <itch/wire.hpp>
#include <span>
#include <string>
#include <vector>

#include "support/itch_builder.hpp"

namespace {

using namespace itch;
using namespace itch::test;

std::span<const std::byte> as_span(const Bytes& b) {
    return {b.data(), b.size()};
}

// Counts every callback; keeps the last view of each kind.
struct Recorder {
    int  total     = 0;
    char last_type = 0;

    template <class V>
    void on(const V& v) {
        ++total;
        last_type = static_cast<char>(std::to_integer<unsigned char>(v.p[0]));
    }
};

// ---- Wire ----

TEST(Wire, LoadsAtEveryAlignment) {
    std::array<std::byte, 16> buf{};
    for (std::size_t off = 0; off < 8; ++off) {
        buf.fill(std::byte{0xEE});
        for (std::size_t i = 0; i < 8; ++i) {
            buf[off + i] = static_cast<std::byte>(i + 1);
        }
        EXPECT_EQ(load_be64(buf.data() + off), 0x0102030405060708ULL) << "offset " << off;
        EXPECT_EQ(load_be32(buf.data() + off), 0x01020304U) << "offset " << off;
        EXPECT_EQ(load_be16(buf.data() + off), 0x0102U) << "offset " << off;
        EXPECT_EQ(load_be48(buf.data() + off), 0x010203040506ULL) << "offset " << off;
    }
}

TEST(Wire, Timestamp48) {
    const Bytes b = bytes_of({0x0a, 0x11, 0xea, 0x0e, 0x8c, 0x43});
    EXPECT_EQ(load_be48(b.data()), 0x0a11ea0e8c43ULL);
}

// ---- Layout ----

TEST(Layout, SizesMatchSpec) {
    EXPECT_EQ(AddOrderView::kSize, 36U);
    EXPECT_EQ(AddOrderView::kSizeWithMpid, 40U);
    EXPECT_EQ(ExecutedView::kSize, 31U);
    EXPECT_EQ(ExecutedPriceView::kSize, 36U);
    EXPECT_EQ(CancelView::kSize, 23U);
    EXPECT_EQ(DeleteView::kSize, 19U);
    EXPECT_EQ(ReplaceView::kSize, 35U);
    EXPECT_EQ(TradeView::kSize, 44U);
    EXPECT_EQ(CrossTradeView::kSize, 40U);
    EXPECT_EQ(BrokenTradeView::kSize, 19U);
    EXPECT_EQ(SystemEventView::kSize, 12U);
    EXPECT_EQ(TradingActionView::kSize, 25U);
    EXPECT_EQ(StockDirectoryView::kSize, 39U);

    // The builder is written from the spec table, so these also pin the builder.
    const Hdr h{};
    EXPECT_EQ(make_add(h, 1, 'B', 1, "X", 1).size(), 36U);
    EXPECT_EQ(make_add_mpid(h, 1, 'B', 1, "X", 1, "NSDQ").size(), 40U);
    EXPECT_EQ(make_executed(h, 1, 1, 1).size(), 31U);
    EXPECT_EQ(make_executed_price(h, 1, 1, 1, 'Y', 1).size(), 36U);
    EXPECT_EQ(make_cancel(h, 1, 1).size(), 23U);
    EXPECT_EQ(make_delete(h, 1).size(), 19U);
    EXPECT_EQ(make_replace(h, 1, 2, 1, 1).size(), 35U);
    EXPECT_EQ(make_trade(h, 0, 'B', 1, "X", 1, 1).size(), 44U);
    EXPECT_EQ(make_cross(h, 1, "X", 1, 1, 'C').size(), 40U);
    EXPECT_EQ(make_broken(h, 1).size(), 19U);
    EXPECT_EQ(make_system_event(h, 'O').size(), 12U);
    EXPECT_EQ(make_trading_action(h, "X", 'T', ' ', "").size(), 25U);
    EXPECT_EQ(make_stock_directory(h, "X", 'N', ' ', 100, {}).size(), 39U);
}

// Distinct, asymmetric values so a byte-order or offset slip cannot cancel out.
constexpr Hdr kH{0xABCD, 0x1234, 0x0a11ea0e8c43ULL};

template <class V>
void expect_header(const V& v) {
    EXPECT_EQ(v.locate(), 0xABCD);
    EXPECT_EQ(v.tracking(), 0x1234);
    EXPECT_EQ(v.timestamp_ns(), 0x0a11ea0e8c43ULL);
}

TEST(Layout, EveryFieldRoundTrips) {
    {
        const Bytes b = make_add(kH, 0x0102030405060708ULL, 'S', 0xDEADBEEF, "AAPL", 1234567);
        const AddOrderView v(b.data());
        expect_header(v);
        EXPECT_EQ(v.ref(), 0x0102030405060708ULL);
        EXPECT_EQ(v.side(), 'S');
        EXPECT_EQ(v.shares(), 0xDEADBEEFU);
        EXPECT_EQ(std::string(v.stock().data(), 8), "AAPL    ");
        EXPECT_EQ(v.price(), 1234567U);
        EXPECT_FALSE(v.has_mpid());
    }
    {
        const Bytes        b = make_add_mpid(kH, 77, 'B', 500, "MSFT", 99, "GSCO");
        const AddOrderView v(b.data());
        expect_header(v);
        EXPECT_TRUE(v.has_mpid());
        EXPECT_EQ(v.ref(), 77U);
        EXPECT_EQ(v.side(), 'B');
        EXPECT_EQ(v.shares(), 500U);
        EXPECT_EQ(std::string(v.stock().data(), 8), "MSFT    ");
        EXPECT_EQ(v.price(), 99U);
        EXPECT_EQ(std::string(v.attribution().data(), 4), "GSCO");
    }
    {
        const Bytes b = make_executed(kH, 0x1122334455667788ULL, 0x01020304, 0x8877665544332211ULL);
        const ExecutedView v(b.data());
        expect_header(v);
        EXPECT_EQ(v.ref(), 0x1122334455667788ULL);
        EXPECT_EQ(v.shares(), 0x01020304U);
        EXPECT_EQ(v.match(), 0x8877665544332211ULL);
    }
    {
        const Bytes             b = make_executed_price(kH, 5, 6, 7, 'N', 0xFFFFFFFE);
        const ExecutedPriceView v(b.data());
        expect_header(v);
        EXPECT_EQ(v.ref(), 5U);
        EXPECT_EQ(v.shares(), 6U);
        EXPECT_EQ(v.match(), 7U);
        EXPECT_EQ(v.printable(), 'N');
        EXPECT_EQ(v.price(), 0xFFFFFFFEU);
    }
    {
        const Bytes      b = make_cancel(kH, 0xAABBCCDDEEFF0011ULL, 0x0A0B0C0D);
        const CancelView v(b.data());
        expect_header(v);
        EXPECT_EQ(v.ref(), 0xAABBCCDDEEFF0011ULL);
        EXPECT_EQ(v.shares(), 0x0A0B0C0DU);
    }
    {
        const Bytes      b = make_delete(kH, 0x0000000100000002ULL);
        const DeleteView v(b.data());
        expect_header(v);
        EXPECT_EQ(v.ref(), 0x0000000100000002ULL);
    }
    {
        const Bytes b = make_replace(kH, 0x1111111122222222ULL, 0x3333333344444444ULL, 4321, 8765);
        const ReplaceView v(b.data());
        expect_header(v);
        EXPECT_EQ(v.old_ref(), 0x1111111122222222ULL);
        EXPECT_EQ(v.new_ref(), 0x3333333344444444ULL);
        EXPECT_EQ(v.shares(), 4321U);
        EXPECT_EQ(v.price(), 8765U);
    }
    {
        const Bytes     b = make_trade(kH, 0, 'B', 300, "TSLA", 2500000, 0x0102030405060708ULL);
        const TradeView v(b.data());
        expect_header(v);
        EXPECT_EQ(v.ref(), 0U);
        EXPECT_EQ(v.side(), 'B');
        EXPECT_EQ(v.shares(), 300U);
        EXPECT_EQ(std::string(v.stock().data(), 8), "TSLA    ");
        EXPECT_EQ(v.price(), 2500000U);
        EXPECT_EQ(v.match(), 0x0102030405060708ULL);
    }
    {
        const Bytes          b = make_cross(kH, 0x0000000200000003ULL, "NVDA", 1750000, 99, 'C');
        const CrossTradeView v(b.data());
        expect_header(v);
        EXPECT_EQ(v.shares(), 0x0000000200000003ULL);
        EXPECT_EQ(std::string(v.stock().data(), 8), "NVDA    ");
        EXPECT_EQ(v.price(), 1750000U);
        EXPECT_EQ(v.match(), 99U);
        EXPECT_EQ(v.cross_type(), 'C');
    }
    {
        const Bytes           b = make_broken(kH, 0xFEDCBA9876543210ULL);
        const BrokenTradeView v(b.data());
        expect_header(v);
        EXPECT_EQ(v.match(), 0xFEDCBA9876543210ULL);
    }
    {
        const Bytes           b = make_system_event(kH, 'C');
        const SystemEventView v(b.data());
        expect_header(v);
        EXPECT_EQ(v.event_code(), 'C');
    }
    {
        const Bytes             b = make_trading_action(kH, "IBM", 'H', 'x', "LUDP");
        const TradingActionView v(b.data());
        expect_header(v);
        EXPECT_EQ(std::string(v.stock().data(), 8), "IBM     ");
        EXPECT_EQ(v.state(), 'H');
        EXPECT_EQ(v.reserved(), 'x');
        EXPECT_EQ(std::string(v.reason().data(), 4), "LUDP");
    }
    {
        const std::array<std::uint8_t, 14> rest{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
        const Bytes              b = make_stock_directory(kH, "GE", 'Q', 'D', 0x00000064, rest);
        const StockDirectoryView v(b.data());
        expect_header(v);
        EXPECT_EQ(std::string(v.stock().data(), 8), "GE      ");
        EXPECT_EQ(v.market_category(), 'Q');
        EXPECT_EQ(v.financial_status(), 'D');
        EXPECT_EQ(v.lot_size(), 100U);
        ASSERT_EQ(v.rest().size(), 14U);
        for (std::size_t i = 0; i < 14; ++i) {
            EXPECT_EQ(std::to_integer<unsigned>(v.rest()[i]), i + 1);
        }
    }
}

// ---- Framer: real bytes from 12302019.NASDAQ_ITCH50 ----

// First two messages of the real file, copied from its decompressed head.
const Bytes kRealHead = bytes_of({
    0x00, 0x0c, 0x53, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x11, 0xea, 0x0e, 0x8c, 0x43, 0x4f,  //
    0x00, 0x27, 0x52, 0x00, 0x01, 0x00, 0x00, 0x0a, 0x53, 0xa2, 0x88, 0x70, 0x58, 0x41,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x4e, 0x20, 0x00, 0x00, 0x00, 0x64, 0x4e,
    0x43, 0x5a, 0x20, 0x50, 0x4e, 0x20, 0x31, 0x4e, 0x00, 0x00, 0x00, 0x00, 0x4e,
});

TEST(Framer, RealFirstMessage) {
    Framer                     f(as_span(kRealHead));
    std::span<const std::byte> m;
    ASSERT_EQ(f.next(m), FrameStatus::Ok);
    ASSERT_EQ(m.size(), SystemEventView::kSize);
    EXPECT_EQ(f.offset(), 14U);
    const SystemEventView v(m.data());
    EXPECT_EQ(static_cast<char>(std::to_integer<unsigned char>(m[0])), 'S');
    EXPECT_EQ(v.timestamp_ns(), 0x0a11ea0e8c43ULL);
    EXPECT_EQ(v.event_code(), 'O');
    EXPECT_EQ(v.locate(), 0);
}

TEST(Framer, RealSecondMessage) {
    Framer                     f(as_span(kRealHead));
    std::span<const std::byte> m;
    ASSERT_EQ(f.next(m), FrameStatus::Ok);
    ASSERT_EQ(f.next(m), FrameStatus::Ok);
    ASSERT_EQ(m.size(), 0x27U);
    const StockDirectoryView v(m.data());
    EXPECT_EQ(v.locate(), 1);
    EXPECT_EQ(std::string(v.stock().data(), 8), "A       ");
    EXPECT_EQ(v.market_category(), 'N');
    EXPECT_EQ(v.financial_status(), ' ');
    EXPECT_EQ(v.lot_size(), 100U);
    EXPECT_EQ(f.offset(), kRealHead.size());
    EXPECT_EQ(f.next(m), FrameStatus::End);
}

TEST(Framer, TruncatedHeader) {
    Bytes b = frame(make_delete(kH, 1));
    b.push_back(std::byte{0x00});  // one stray byte: half a length prefix
    Framer                     f(as_span(b));
    std::span<const std::byte> m;
    ASSERT_EQ(f.next(m), FrameStatus::Ok);
    const std::size_t before = f.offset();
    EXPECT_EQ(f.next(m), FrameStatus::TruncatedHeader);
    EXPECT_EQ(f.offset(), before);
    EXPECT_EQ(f.next(m), FrameStatus::TruncatedHeader);  // stable, not advancing
}

TEST(Framer, TruncatedBody) {
    Bytes b = bytes_of({0x00, 36});
    b.resize(2 + 10, std::byte{0x41});
    Framer                     f(as_span(b));
    std::span<const std::byte> m;
    EXPECT_EQ(f.next(m), FrameStatus::TruncatedBody);
    EXPECT_EQ(f.offset(), 0U);
}

TEST(Framer, ZeroLength) {
    const Bytes                b = bytes_of({0x00, 0x00, 0x41, 0x41});
    Framer                     f(as_span(b));
    std::span<const std::byte> m;
    EXPECT_EQ(f.next(m), FrameStatus::ZeroLength);
    EXPECT_EQ(f.offset(), 0U);
}

TEST(Framer, EmptyInputIsEnd) {
    Framer                     f(std::span<const std::byte>{});
    std::span<const std::byte> m;
    EXPECT_EQ(f.next(m), FrameStatus::End);
}

// ---- Decode ----

TEST(Decode, DispatchesEveryInScopeType) {
    const std::vector<Bytes> msgs = {
        make_add(kH, 1, 'B', 1, "X", 1),
        make_add_mpid(kH, 1, 'B', 1, "X", 1, "NSDQ"),
        make_executed(kH, 1, 1, 1),
        make_executed_price(kH, 1, 1, 1, 'Y', 1),
        make_cancel(kH, 1, 1),
        make_delete(kH, 1),
        make_replace(kH, 1, 2, 1, 1),
        make_trade(kH, 0, 'B', 1, "X", 1, 1),
        make_cross(kH, 1, "X", 1, 1, 'O'),
        make_broken(kH, 1),
        make_system_event(kH, 'O'),
        make_trading_action(kH, "X", 'T', ' ', ""),
        make_stock_directory(kH, "X", 'N', ' ', 100, {}),
    };
    const std::string expect = "AFECXDUPQBSHR";
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        Recorder r;
        EXPECT_EQ(decode(as_span(msgs[i]), r), DecodeStatus::Handled) << expect[i];
        EXPECT_EQ(r.total, 1) << expect[i];
        EXPECT_EQ(r.last_type, expect[i]);
    }
}

TEST(Decode, WrongLengthCallsNoHandler) {
    Recorder r;
    Bytes    shortA = make_add(kH, 1, 'B', 1, "X", 1);
    shortA.pop_back();  // 35-byte 'A'
    EXPECT_EQ(decode(as_span(shortA), r), DecodeStatus::WrongLength);
    Bytes longF = make_add_mpid(kH, 1, 'B', 1, "X", 1, "NSDQ");
    longF.push_back(std::byte{0});
    EXPECT_EQ(decode(as_span(longF), r), DecodeStatus::WrongLength);
    // an 'A' padded to the 'F' size is still wrong
    Bytes paddedA = make_add(kH, 1, 'B', 1, "X", 1);
    paddedA.resize(40);
    EXPECT_EQ(decode(as_span(paddedA), r), DecodeStatus::WrongLength);
    // a one-byte message of an in-scope type
    EXPECT_EQ(decode(as_span(bytes_of({'D'})), r), DecodeStatus::WrongLength);
    EXPECT_EQ(r.total, 0);
}

TEST(Decode, UnknownSkippedByLength) {
    Bytes stream;
    append(stream, frame(make_system_event(kH, 'O')));
    append(stream, frame(make_raw('O', 48)));  // out-of-scope, modern length
    append(stream, frame(make_raw('L', 26)));
    append(stream, frame(make_raw('Y', 20)));
    append(stream, frame(make_raw('?', 7)));  // not a defined type at all
    const Bytes marker = make_delete(kH, 0x42);
    append(stream, frame(marker));

    Framer                     f(as_span(stream));
    Recorder                   r;
    std::span<const std::byte> m;
    int                        skipped = 0;
    int                        handled = 0;
    while (f.next(m) == FrameStatus::Ok) {
        const DecodeStatus s = decode(m, r);
        ASSERT_NE(s, DecodeStatus::WrongLength);
        (s == DecodeStatus::Skipped ? skipped : handled)++;
    }
    EXPECT_EQ(skipped, 4);
    EXPECT_EQ(handled, 2);
    EXPECT_EQ(f.offset(), stream.size());  // still aligned: every frame consumed
    EXPECT_EQ(r.last_type, 'D');           // the frame after the unknowns decoded correctly
}

TEST(Decode, EmptyMessageIsSkipped) {
    Recorder r;
    EXPECT_EQ(decode(std::span<const std::byte>{}, r), DecodeStatus::Skipped);
    EXPECT_EQ(r.total, 0);
}

}  // namespace
