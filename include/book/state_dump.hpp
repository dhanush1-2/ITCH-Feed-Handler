// include/book/state_dump.hpp
#pragma once

// Canonical serialisation of every resting order, for comparing two builders byte for byte.
//
// For each locate ascending, bids then asks (each best to worst), each order in FIFO order:
//   locate u16, side u8 (0 buy, 1 sell), price i32, ref u64, shares u32   (all big-endian)
// 19 bytes per order. Slot numbers never appear, so the bytes depend only on book content
// and queue order, not on how the pool was churned to get there.

#include <array>
#include <book/builder.hpp>
#include <cstdint>
#include <ostream>

namespace book {

namespace detail {

template <std::size_t N>
inline void put_be(std::array<char, 19>& rec, std::size_t at, std::uint64_t v) {
    for (std::size_t i = 0; i < N; ++i) {
        rec[at + i] = static_cast<char>((v >> (8 * (N - 1 - i))) & 0xFF);
    }
}

template <class Range>
inline void dump_side(std::ostream& os, const ob::OrderPool& pool, std::uint16_t locate,
                      ob::Side side, const Range& levels) {
    for (const Level& lv : levels) {
        for (ob::Slot s = lv.head; s != ob::kInvalidSlot; s = pool.at(s).next) {
            const ob::Order&     o = pool.at(s);
            std::array<char, 19> rec{};
            put_be<2>(rec, 0, locate);
            put_be<1>(rec, 2, static_cast<std::uint8_t>(side));
            put_be<4>(rec, 3, static_cast<std::uint32_t>(o.price));
            put_be<8>(rec, 7, o.id);
            put_be<4>(rec, 15, o.remaining);
            os.write(rec.data(), static_cast<std::streamsize>(rec.size()));
        }
    }
}

}  // namespace detail

template <class Obs>
void write_state(std::ostream& os, const BookBuilder<Obs>& b) {
    for (std::size_t loc = 0; loc < BookBuilder<Obs>::kLocates; ++loc) {
        const auto  l  = static_cast<std::uint16_t>(loc);
        const Book& bk = b.book(l);
        if (bk.level_count(ob::Side::Buy) == 0 && bk.level_count(ob::Side::Sell) == 0) {
            continue;
        }
        detail::dump_side(os, b.pool(), l, ob::Side::Buy, bk.bids());
        detail::dump_side(os, b.pool(), l, ob::Side::Sell, bk.asks());
    }
}

}  // namespace book
