// include/itch/wire.hpp
#pragma once

// Big-endian loads for ITCH 5.0, which is a big-endian protocol.
//
// THIS CODE READS UNTRUSTED BYTES. Each load is a memcpy into a local integer followed
// by a byte swap. A reinterpret_cast onto the buffer would be a strict-aliasing
// violation and would assume an alignment the wire does not guarantee; memcpy compiles
// to the same single load. C++20 has no std::byteswap, so this uses __builtin_bswap*.
//
// Callers are responsible for passing a pointer with at least N readable bytes. The
// Framer and decode() establish that before any view is constructed.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(ITCH_VERIFY) && ITCH_VERIFY
#define ITCH_ASSERT(cond)                                                                        \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            std::fprintf(stderr, "ITCH_ASSERT failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
            std::abort();                                                                        \
        }                                                                                        \
    } while (false)
#else
#define ITCH_ASSERT(cond) ((void)0)
#endif

namespace itch {

static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big,
              "mixed-endian hosts are not supported");

[[nodiscard]] inline std::uint16_t load_be16(const std::byte* p) noexcept {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof v);
    if constexpr (std::endian::native == std::endian::little) {
        v = __builtin_bswap16(v);
    }
    return v;
}

[[nodiscard]] inline std::uint32_t load_be32(const std::byte* p) noexcept {
    std::uint32_t v;
    std::memcpy(&v, p, sizeof v);
    if constexpr (std::endian::native == std::endian::little) {
        v = __builtin_bswap32(v);
    }
    return v;
}

// The 6-byte nanosecond timestamp of the ITCH header.
[[nodiscard]] inline std::uint64_t load_be48(const std::byte* p) noexcept {
    return (static_cast<std::uint64_t>(load_be16(p)) << 32) | load_be32(p + 2);
}

[[nodiscard]] inline std::uint64_t load_be64(const std::byte* p) noexcept {
    std::uint64_t v;
    std::memcpy(&v, p, sizeof v);
    if constexpr (std::endian::native == std::endian::little) {
        v = __builtin_bswap64(v);
    }
    return v;
}

}  // namespace itch
