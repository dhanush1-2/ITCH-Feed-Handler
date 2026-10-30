// tests/support/itch_builder.hpp
#pragma once

// Builds ITCH 5.0 messages for tests. Written from the offset table in the spec PDF
// (docs/spec/NQTVITCHspecification.pdf), and deliberately independent of the views:
// it shares no code with include/itch, so a wrong offset in one cannot hide in the other.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace itch::test {

using Bytes = std::vector<std::byte>;

struct Hdr {
    std::uint16_t locate   = 0;
    std::uint16_t tracking = 0;
    std::uint64_t ts       = 0;  // 48-bit nanoseconds since midnight
};

// Big-endian write of the low `n` bytes of `v` at `at`.
inline void put(Bytes& b, std::size_t at, std::uint64_t v, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        b[at + i] = static_cast<std::byte>((v >> (8 * (n - 1 - i))) & 0xFF);
    }
}

inline void put_chars(Bytes& b, std::size_t at, std::string_view s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        b[at + i] = static_cast<std::byte>(i < s.size() ? s[i] : ' ');  // right padded
    }
}

inline Bytes start(char type, std::size_t size, const Hdr& h) {
    Bytes b(size, std::byte{0});
    b[0] = static_cast<std::byte>(type);
    put(b, 1, h.locate, 2);
    put(b, 3, h.tracking, 2);
    put(b, 5, h.ts, 6);
    return b;
}

inline Bytes make_add(const Hdr& h, std::uint64_t ref, char side, std::uint32_t shares,
                      std::string_view stock, std::uint32_t price) {
    Bytes b = start('A', 36, h);
    put(b, 11, ref, 8);
    b[19] = static_cast<std::byte>(side);
    put(b, 20, shares, 4);
    put_chars(b, 24, stock, 8);
    put(b, 32, price, 4);
    return b;
}

inline Bytes make_add_mpid(const Hdr& h, std::uint64_t ref, char side, std::uint32_t shares,
                           std::string_view stock, std::uint32_t price, std::string_view mpid) {
    Bytes b = start('F', 40, h);
    put(b, 11, ref, 8);
    b[19] = static_cast<std::byte>(side);
    put(b, 20, shares, 4);
    put_chars(b, 24, stock, 8);
    put(b, 32, price, 4);
    put_chars(b, 36, mpid, 4);
    return b;
}

inline Bytes make_executed(const Hdr& h, std::uint64_t ref, std::uint32_t shares,
                           std::uint64_t match) {
    Bytes b = start('E', 31, h);
    put(b, 11, ref, 8);
    put(b, 19, shares, 4);
    put(b, 23, match, 8);
    return b;
}

inline Bytes make_executed_price(const Hdr& h, std::uint64_t ref, std::uint32_t shares,
                                 std::uint64_t match, char printable, std::uint32_t price) {
    Bytes b = start('C', 36, h);
    put(b, 11, ref, 8);
    put(b, 19, shares, 4);
    put(b, 23, match, 8);
    b[31] = static_cast<std::byte>(printable);
    put(b, 32, price, 4);
    return b;
}

inline Bytes make_cancel(const Hdr& h, std::uint64_t ref, std::uint32_t shares) {
    Bytes b = start('X', 23, h);
    put(b, 11, ref, 8);
    put(b, 19, shares, 4);
    return b;
}

inline Bytes make_delete(const Hdr& h, std::uint64_t ref) {
    Bytes b = start('D', 19, h);
    put(b, 11, ref, 8);
    return b;
}

inline Bytes make_replace(const Hdr& h, std::uint64_t old_ref, std::uint64_t new_ref,
                          std::uint32_t shares, std::uint32_t price) {
    Bytes b = start('U', 35, h);
    put(b, 11, old_ref, 8);
    put(b, 19, new_ref, 8);
    put(b, 27, shares, 4);
    put(b, 31, price, 4);
    return b;
}

inline Bytes make_trade(const Hdr& h, std::uint64_t ref, char side, std::uint32_t shares,
                        std::string_view stock, std::uint32_t price, std::uint64_t match) {
    Bytes b = start('P', 44, h);
    put(b, 11, ref, 8);
    b[19] = static_cast<std::byte>(side);
    put(b, 20, shares, 4);
    put_chars(b, 24, stock, 8);
    put(b, 32, price, 4);
    put(b, 36, match, 8);
    return b;
}

inline Bytes make_cross(const Hdr& h, std::uint64_t shares, std::string_view stock,
                        std::uint32_t price, std::uint64_t match, char cross_type) {
    Bytes b = start('Q', 40, h);
    put(b, 11, shares, 8);
    put_chars(b, 19, stock, 8);
    put(b, 27, price, 4);
    put(b, 31, match, 8);
    b[39] = static_cast<std::byte>(cross_type);
    return b;
}

inline Bytes make_broken(const Hdr& h, std::uint64_t match) {
    Bytes b = start('B', 19, h);
    put(b, 11, match, 8);
    return b;
}

inline Bytes make_system_event(const Hdr& h, char code) {
    Bytes b = start('S', 12, h);
    b[11]   = static_cast<std::byte>(code);
    return b;
}

inline Bytes make_trading_action(const Hdr& h, std::string_view stock, char state, char reserved,
                                 std::string_view reason) {
    Bytes b = start('H', 25, h);
    put_chars(b, 11, stock, 8);
    b[19] = static_cast<std::byte>(state);
    b[20] = static_cast<std::byte>(reserved);
    put_chars(b, 21, reason, 4);
    return b;
}

// `rest` fills offsets 25..38 (the 14 bytes after Round Lot Size), padded with zeros.
inline Bytes make_stock_directory(const Hdr& h, std::string_view stock, char market_category,
                                  char financial_status, std::uint32_t lot_size,
                                  const std::array<std::uint8_t, 14>& rest) {
    Bytes b = start('R', 39, h);
    put_chars(b, 11, stock, 8);
    b[19] = static_cast<std::byte>(market_category);
    b[20] = static_cast<std::byte>(financial_status);
    put(b, 21, lot_size, 4);
    for (std::size_t i = 0; i < rest.size(); ++i) {
        b[25 + i] = static_cast<std::byte>(rest[i]);
    }
    return b;
}

// A message with an arbitrary type byte and length, for unknown-type tests.
inline Bytes make_raw(char type, std::size_t size, const Hdr& h = {}) {
    if (size < 11) {  // too short for a full header: just the type byte and zeros
        Bytes b(size, std::byte{0});
        if (size > 0) {
            b[0] = static_cast<std::byte>(type);
        }
        return b;
    }
    return start(type, size, h);
}

// Prefix a 2-byte big-endian length, as in an ITCH file.
inline Bytes frame(const Bytes& msg) {
    Bytes out(2 + msg.size());
    put(out, 0, msg.size(), 2);
    std::memcpy(out.data() + 2, msg.data(), msg.size());
    return out;
}

inline void append(Bytes& dst, const Bytes& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

inline Bytes bytes_of(std::initializer_list<unsigned> v) {
    Bytes b;
    for (unsigned x : v) {
        b.push_back(static_cast<std::byte>(x));
    }
    return b;
}

}  // namespace itch::test
