// tests/test_ladder_book.cpp
//
// bench/ladder_book.hpp against the production book::Book. The ladder is only for the A/B in
// docs/OPTIMIZATION-LOG.md, but a comparison between two books that disagree measures nothing,
// so both are driven by the same messages and must agree on the top of book after every one, and
// on every level (price, order count, share total) at checkpoints. Queue order inside a level
// is NOT compared: Book queues by reference number, the ladder by arrival.
//
// Two sources: a seeded random stream that deliberately puts prices outside the ladder window
// (so the std::map overflow is exercised), and the synthetic day from ITCH_SLICE, compared
// at the populated mid-point (feed second 43,200) because a full day ends with empty books.

#include <gtest/gtest.h>

#include <book/builder.hpp>
#include <cstdint>
#include <cstdlib>
#include <itch/dispatch.hpp>
#include <itch/framer.hpp>
#include <itch/mapped_file.hpp>
#include <ladder_book.hpp>
#include <random>
#include <vector>

#include "support/itch_builder.hpp"

namespace {

using itch::test::Bytes;
using itch::test::Hdr;
using VecBuilder    = book::BookBuilder<book::NullObserver>;
using LadderBuilder = book::BookBuilder<book::NullObserver, ob::IdIndex, bench::LadderBook>;

// Feeds one message to both builders.
struct Tee {
    book::NullObserver obs;
    VecBuilder         vec;
    LadderBuilder      lad;

    explicit Tee(std::size_t cap) : vec(cap, obs), lad(cap, obs) {}

    template <class View>
    void on(const View& v) {
        vec.on(v);
        lad.on(v);
    }
};

std::vector<bench::LevelRow> rows(const book::Book& b, ob::Side side) {
    std::vector<bench::LevelRow> out;
    if (side == ob::Side::Buy) {
        for (const book::Level& l : b.bids()) {
            out.push_back({l.price, l.count, l.total});
        }
    } else {
        for (const book::Level& l : b.asks()) {
            out.push_back({l.price, l.count, l.total});
        }
    }
    return out;
}

void expect_top(const Tee& t, std::uint16_t loc) {
    EXPECT_EQ(t.vec.book(loc).best_bid(), t.lad.book(loc).best_bid()) << "locate " << loc;
    EXPECT_EQ(t.vec.book(loc).best_ask(), t.lad.book(loc).best_ask()) << "locate " << loc;
    EXPECT_EQ(t.vec.book(loc).order_count(), t.lad.book(loc).order_count()) << "locate " << loc;
}

// Every level of every locate that holds an order in either book. Returns how many locates did.
std::size_t expect_all_levels(const Tee& t) {
    std::size_t populated = 0;
    for (std::size_t loc = 0; loc < VecBuilder::kLocates; ++loc) {
        const auto& v = t.vec.book(static_cast<std::uint16_t>(loc));
        const auto& l = t.lad.book(static_cast<std::uint16_t>(loc));
        EXPECT_EQ(v.order_count(), l.order_count()) << "locate " << loc;
        if (v.order_count() == 0 && l.order_count() == 0) {
            continue;
        }
        ++populated;
        EXPECT_EQ(rows(v, ob::Side::Buy), l.levels(ob::Side::Buy)) << "bids, locate " << loc;
        EXPECT_EQ(rows(v, ob::Side::Sell), l.levels(ob::Side::Sell)) << "asks, locate " << loc;
    }
    return populated;
}

}  // namespace

TEST(LadderBook, AgreesWithVectorBookOnRandomStream) {
    constexpr std::uint16_t kLocates = 3;
    constexpr std::int32_t  kMid     = 1'000'000;  // $100.00
    Tee                     t(1 << 16);
    std::mt19937_64         rng(20261030);

    struct Live {
        std::uint64_t ref;
        std::uint16_t loc;
        std::uint32_t remaining;
    };
    std::vector<Live> live;
    std::uint64_t     next_ref = 1;

    // Mostly near the touch, sometimes outside the 65,536-tick window on either side.
    const auto price = [&]() -> std::uint32_t {
        const std::uint64_t r = rng() % 100;
        if (r < 4) {
            return static_cast<std::uint32_t>(kMid + 200'000 + static_cast<int>(rng() % 1000));
        }
        if (r < 8) {
            return static_cast<std::uint32_t>(kMid - 200'000 + static_cast<int>(rng() % 1000));
        }
        return static_cast<std::uint32_t>(kMid - 40 + static_cast<int>(rng() % 80));
    };

    const auto feed = [&](const Bytes& m) {
        ASSERT_EQ(itch::decode(std::span<const std::byte>(m), t), itch::DecodeStatus::Handled);
    };

    for (int i = 0; i < 60'000; ++i) {
        const std::uint64_t op  = rng() % 100;
        std::uint16_t       loc = static_cast<std::uint16_t>(1 + rng() % kLocates);
        if (live.empty() || op < 45) {
            const std::uint32_t sh = 1 + static_cast<std::uint32_t>(rng() % 500);
            feed(itch::test::make_add(Hdr{loc}, next_ref, rng() % 2 ? 'B' : 'S', sh, "TEST",
                                      price()));
            live.push_back({next_ref++, loc, sh});
        } else {
            const std::size_t k = rng() % live.size();
            Live&             o = live[k];
            loc                 = o.loc;
            bool gone           = false;
            if (op < 65) {  // delete
                feed(itch::test::make_delete(Hdr{loc}, o.ref));
                gone = true;
            } else if (op < 75) {  // execute: full or partial
                const std::uint32_t sh =
                    o.remaining > 1 && rng() % 2 ? 1 + o.remaining / 2 : o.remaining;
                feed(itch::test::make_executed(Hdr{loc}, o.ref, sh, static_cast<std::uint64_t>(i)));
                o.remaining -= sh;
                gone = o.remaining == 0;
            } else if (op < 82) {  // cancel part
                const std::uint32_t sh = o.remaining > 1 ? 1 : o.remaining;
                feed(itch::test::make_cancel(Hdr{loc}, o.ref, sh));
                o.remaining -= sh;
                gone = o.remaining == 0;
            } else {  // replace: new ref, new price, new size
                const std::uint32_t sh = 1 + static_cast<std::uint32_t>(rng() % 500);
                feed(itch::test::make_replace(Hdr{loc}, o.ref, next_ref, sh, price()));
                o.ref       = next_ref++;
                o.remaining = sh;
            }
            if (gone) {
                live[k] = live.back();
                live.pop_back();
            }
        }
        expect_top(t, loc);
        if (i % 500 == 0) {
            expect_all_levels(t);
        }
        if (::testing::Test::HasFailure()) {
            FAIL() << "diverged at message " << i;
        }
    }
    expect_all_levels(t);

    // The test only means something if both paths ran.
    std::uint64_t in_window = 0;
    std::uint64_t overflow  = 0;
    for (std::uint16_t loc = 1; loc <= kLocates; ++loc) {
        in_window += t.lad.book(loc).in_window_adds();
        overflow += t.lad.book(loc).overflow_adds();
    }
    EXPECT_GT(in_window, 1000u);
    EXPECT_GT(overflow, 1000u);
}

TEST(LadderBook, AgreesWithVectorBookOnSlice) {
    const char* path = std::getenv("ITCH_SLICE");
    if (path == nullptr || *path == '\0') {
        GTEST_SKIP()
            << "ITCH_SLICE is not set: run through ctest, which expands the synthetic day first";
    }
    const itch::MappedFile mf = itch::MappedFile::open(path);
    Tee                    t(1 << 18);

    constexpr std::uint64_t kNoonNs      = 43'200ULL * 1'000'000'000ULL;
    bool                    checked_noon = false;
    std::size_t             noon_locates = 0;
    std::size_t             n            = 0;

    itch::Framer               fr(mf.bytes());
    std::span<const std::byte> msg;
    while (fr.next(msg) == itch::FrameStatus::Ok) {
        if (!checked_noon && msg.size() >= 11 &&
            itch::HeaderView(msg.data()).timestamp_ns() >= kNoonNs) {
            noon_locates = expect_all_levels(t);
            checked_noon = true;
        }
        ASSERT_NE(itch::decode(msg, t), itch::DecodeStatus::WrongLength);
        if (msg.size() >= 11) {
            expect_top(t, itch::HeaderView(msg.data()).locate());
        }
        if (++n % 2000 == 0) {
            expect_all_levels(t);
        }
        if (::testing::Test::HasFailure()) {
            FAIL() << "diverged at message " << n;
        }
    }
    EXPECT_TRUE(checked_noon);
    EXPECT_GT(noon_locates, 0u) << "the mid-point comparison saw no populated book";
    // The end of a day is empty on both; this also checks nothing was left behind.
    EXPECT_EQ(expect_all_levels(t), 0u);
    EXPECT_EQ(t.vec.live_orders(), t.lad.live_orders());
}
