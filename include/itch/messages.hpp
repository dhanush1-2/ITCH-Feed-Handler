// include/itch/messages.hpp
#pragma once

// Zero-copy views over ITCH 5.0 messages. A view owns nothing: it holds a pointer to
// the first byte of the message (the type byte) and reads a field on demand.
//
// Layouts are from the Nasdaq TotalView-ITCH 5.0 specification (docs/spec/). Every
// message starts with the same 11-byte header:
//   type 0 (1), stock locate 1 (2), tracking number 3 (2), timestamp 5 (6).
//
// A view is only valid for a buffer of at least kSize bytes. decode() checks the exact
// length before it builds one; constructing a view by hand is the caller's guarantee.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <itch/wire.hpp>
#include <span>

namespace itch {

namespace detail {

[[nodiscard]] inline char char_at(const std::byte* p) noexcept {
    return static_cast<char>(std::to_integer<unsigned char>(*p));
}

template <std::size_t N>
[[nodiscard]] inline std::array<char, N> chars_at(const std::byte* p) noexcept {
    std::array<char, N> out;
    std::memcpy(out.data(), p, N);
    return out;
}

}  // namespace detail

struct HeaderView {
    const std::byte* p;

    explicit HeaderView(const std::byte* msg) noexcept : p(msg) { ITCH_ASSERT(msg != nullptr); }

    [[nodiscard]] char          type() const noexcept { return detail::char_at(p); }
    [[nodiscard]] std::uint16_t locate() const noexcept { return load_be16(p + 1); }
    [[nodiscard]] std::uint16_t tracking() const noexcept { return load_be16(p + 3); }
    [[nodiscard]] std::uint64_t timestamp_ns() const noexcept { return load_be48(p + 5); }
};

// 'A' (36 bytes) and 'F' (40 bytes, adds a 4-byte MPID attribution).
struct AddOrderView : HeaderView {
    static constexpr std::size_t kSize         = 36;
    static constexpr std::size_t kSizeWithMpid = 40;

    explicit AddOrderView(const std::byte* msg) noexcept : HeaderView(msg) {
        ITCH_ASSERT(type() == 'A' || type() == 'F');
    }

    [[nodiscard]] bool                has_mpid() const noexcept { return type() == 'F'; }
    [[nodiscard]] std::uint64_t       ref() const noexcept { return load_be64(p + 11); }
    [[nodiscard]] char                side() const noexcept { return detail::char_at(p + 19); }
    [[nodiscard]] std::uint32_t       shares() const noexcept { return load_be32(p + 20); }
    [[nodiscard]] std::array<char, 8> stock() const noexcept { return detail::chars_at<8>(p + 24); }
    [[nodiscard]] std::uint32_t       price() const noexcept { return load_be32(p + 32); }
    // Only meaningful when has_mpid(); reads past kSize otherwise.
    [[nodiscard]] std::array<char, 4> attribution() const noexcept {
        ITCH_ASSERT(has_mpid());
        return detail::chars_at<4>(p + 36);
    }
};

struct ExecutedView : HeaderView {
    static constexpr std::size_t kSize = 31;
    explicit ExecutedView(const std::byte* msg) noexcept : HeaderView(msg) {
        ITCH_ASSERT(type() == 'E');
    }
    [[nodiscard]] std::uint64_t ref() const noexcept { return load_be64(p + 11); }
    [[nodiscard]] std::uint32_t shares() const noexcept { return load_be32(p + 19); }
    [[nodiscard]] std::uint64_t match() const noexcept { return load_be64(p + 23); }
};

struct ExecutedPriceView : HeaderView {
    static constexpr std::size_t kSize = 36;
    explicit ExecutedPriceView(const std::byte* msg) noexcept : HeaderView(msg) {
        ITCH_ASSERT(type() == 'C');
    }
    [[nodiscard]] std::uint64_t ref() const noexcept { return load_be64(p + 11); }
    [[nodiscard]] std::uint32_t shares() const noexcept { return load_be32(p + 19); }
    [[nodiscard]] std::uint64_t match() const noexcept { return load_be64(p + 23); }
    [[nodiscard]] char          printable() const noexcept { return detail::char_at(p + 31); }
    [[nodiscard]] std::uint32_t price() const noexcept { return load_be32(p + 32); }
};

struct CancelView : HeaderView {
    static constexpr std::size_t kSize = 23;
    explicit CancelView(const std::byte* msg) noexcept : HeaderView(msg) {
        ITCH_ASSERT(type() == 'X');
    }
    [[nodiscard]] std::uint64_t ref() const noexcept { return load_be64(p + 11); }
    [[nodiscard]] std::uint32_t shares() const noexcept { return load_be32(p + 19); }
};

struct DeleteView : HeaderView {
    static constexpr std::size_t kSize = 19;
    explicit DeleteView(const std::byte* msg) noexcept : HeaderView(msg) {
        ITCH_ASSERT(type() == 'D');
    }
    [[nodiscard]] std::uint64_t ref() const noexcept { return load_be64(p + 11); }
};

struct ReplaceView : HeaderView {
    static constexpr std::size_t kSize = 35;
    explicit ReplaceView(const std::byte* msg) noexcept : HeaderView(msg) {
        ITCH_ASSERT(type() == 'U');
    }
    [[nodiscard]] std::uint64_t old_ref() const noexcept { return load_be64(p + 11); }
    [[nodiscard]] std::uint64_t new_ref() const noexcept { return load_be64(p + 19); }
    [[nodiscard]] std::uint32_t shares() const noexcept { return load_be32(p + 27); }
    [[nodiscard]] std::uint32_t price() const noexcept { return load_be32(p + 31); }
};

// Non-cross trade. It prints an execution of a non-displayed order and never touches a book.
struct TradeView : HeaderView {
    static constexpr std::size_t kSize = 44;
    explicit TradeView(const std::byte* msg) noexcept : HeaderView(msg) {
        ITCH_ASSERT(type() == 'P');
    }
    [[nodiscard]] std::uint64_t       ref() const noexcept { return load_be64(p + 11); }
    [[nodiscard]] char                side() const noexcept { return detail::char_at(p + 19); }
    [[nodiscard]] std::uint32_t       shares() const noexcept { return load_be32(p + 20); }
    [[nodiscard]] std::array<char, 8> stock() const noexcept { return detail::chars_at<8>(p + 24); }
    [[nodiscard]] std::uint32_t       price() const noexcept { return load_be32(p + 32); }
    [[nodiscard]] std::uint64_t       match() const noexcept { return load_be64(p + 36); }
};

struct CrossTradeView : HeaderView {
    static constexpr std::size_t kSize = 40;
    explicit CrossTradeView(const std::byte* msg) noexcept : HeaderView(msg) {
        ITCH_ASSERT(type() == 'Q');
    }
    // Shares is 8 bytes on the wire for a cross.
    [[nodiscard]] std::uint64_t       shares() const noexcept { return load_be64(p + 11); }
    [[nodiscard]] std::array<char, 8> stock() const noexcept { return detail::chars_at<8>(p + 19); }
    [[nodiscard]] std::uint32_t       price() const noexcept { return load_be32(p + 27); }
    [[nodiscard]] std::uint64_t       match() const noexcept { return load_be64(p + 31); }
    [[nodiscard]] char cross_type() const noexcept { return detail::char_at(p + 39); }
};

struct BrokenTradeView : HeaderView {
    static constexpr std::size_t kSize = 19;
    explicit BrokenTradeView(const std::byte* msg) noexcept : HeaderView(msg) {
        ITCH_ASSERT(type() == 'B');
    }
    [[nodiscard]] std::uint64_t match() const noexcept { return load_be64(p + 11); }
};

struct SystemEventView : HeaderView {
    static constexpr std::size_t kSize = 12;
    explicit SystemEventView(const std::byte* msg) noexcept : HeaderView(msg) {
        ITCH_ASSERT(type() == 'S');
    }
    [[nodiscard]] char event_code() const noexcept { return detail::char_at(p + 11); }
};

struct TradingActionView : HeaderView {
    static constexpr std::size_t kSize = 25;
    explicit TradingActionView(const std::byte* msg) noexcept : HeaderView(msg) {
        ITCH_ASSERT(type() == 'H');
    }
    [[nodiscard]] std::array<char, 8> stock() const noexcept { return detail::chars_at<8>(p + 11); }
    [[nodiscard]] char                state() const noexcept { return detail::char_at(p + 19); }
    [[nodiscard]] char                reserved() const noexcept { return detail::char_at(p + 20); }
    [[nodiscard]] std::array<char, 4> reason() const noexcept {
        return detail::chars_at<4>(p + 21);
    }
};

struct StockDirectoryView : HeaderView {
    static constexpr std::size_t kSize       = 39;
    static constexpr std::size_t kRestOffset = 25;
    explicit StockDirectoryView(const std::byte* msg) noexcept : HeaderView(msg) {
        ITCH_ASSERT(type() == 'R');
    }
    [[nodiscard]] std::array<char, 8> stock() const noexcept { return detail::chars_at<8>(p + 11); }
    [[nodiscard]] char market_category() const noexcept { return detail::char_at(p + 19); }
    [[nodiscard]] char financial_status() const noexcept { return detail::char_at(p + 20); }
    [[nodiscard]] std::uint32_t lot_size() const noexcept { return load_be32(p + 21); }
    // Round-lots-only through inverse indicator (offsets 25..38), not decoded here.
    [[nodiscard]] std::span<const std::byte, kSize - kRestOffset> rest() const noexcept {
        return std::span<const std::byte, kSize - kRestOffset>(p + kRestOffset,
                                                               kSize - kRestOffset);
    }
};

}  // namespace itch
