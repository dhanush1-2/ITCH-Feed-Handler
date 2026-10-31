// include/mold/packet.hpp
#pragma once

// MoldUDP64 packet framing (docs/spec/moldudp64.pdf, V 1.00).
//
//   offset  len  field
//        0   10  session          ANUM
//       10    8  sequence number  first message in the packet, big endian
//       18    2  message count    big endian; 0 = heartbeat, 0xFFFF = end of session
//       20    *  message blocks   each [u16 big-endian length][message data]
//
// The block format is the same [u16 length][message] framing the ITCH file uses, so the
// payload of a packet can be fed straight to itch::Framer. One difference: the spec allows a
// zero-length message block; Framer reports ZeroLength for one. parse_packet follows the spec.
//
// A request packet is the same 20 bytes with the count meaning "messages requested".
// Heartbeat and end-of-session packets carry the NEXT expected sequence number and no payload.
//
// THIS CODE READS UNTRUSTED BYTES (parse_packet). Sequences start at 1.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <itch/wire.hpp>
#include <span>

namespace mold {

inline constexpr std::size_t   kHeaderSize   = 20;
inline constexpr std::size_t   kBlockPrefix  = 2;
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;
// The most messages one data packet can announce: 0xFFFF is reserved for end of session.
inline constexpr std::uint16_t kMaxDataCount = 0xFFFE;

using Session = std::array<char, 10>;

struct PacketHeader {
    Session       session;
    std::uint64_t seq;
    std::uint16_t count;
};

enum class PacketStatus { Ok, Heartbeat, EndOfSession, TooShort, BadBlock };

// Where packets go: the retransmit server writes to one, and so does the lossy channel.
class PacketSink {
public:
    virtual ~PacketSink() = default;
    // The span is valid only for the duration of the call; keep a copy to keep the bytes.
    virtual void send(std::span<const std::byte> packet) = 0;
};

inline void store_be16(std::byte* p, std::uint16_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        v = __builtin_bswap16(v);
    }
    std::memcpy(p, &v, sizeof v);
}

inline void store_be64(std::byte* p, std::uint64_t v) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        v = __builtin_bswap64(v);
    }
    std::memcpy(p, &v, sizeof v);
}

// Writes the 20-byte header at `out`, which must have kHeaderSize writable bytes.
inline void write_header(std::byte* out, const Session& session, std::uint64_t seq,
                         std::uint16_t count) noexcept {
    std::memcpy(out, session.data(), session.size());
    store_be64(out + 10, seq);
    store_be16(out + 18, count);
}

// Parses and validates a packet. On Ok, `blocks` is the payload (all count blocks, no
// trailing bytes). On Heartbeat and EndOfSession `blocks` is empty. BadBlock means the count
// and the block lengths disagree with the payload size: an overrun, a missing block, or
// bytes left over. `header` is filled whenever the packet is at least a header long.
[[nodiscard]] inline PacketStatus parse_packet(std::span<const std::byte> pkt, PacketHeader& header,
                                               std::span<const std::byte>& blocks) noexcept {
    blocks = {};
    if (pkt.size() < kHeaderSize) {
        return PacketStatus::TooShort;
    }
    std::memcpy(header.session.data(), pkt.data(), header.session.size());
    header.seq   = itch::load_be64(pkt.data() + 10);
    header.count = itch::load_be16(pkt.data() + 18);

    const std::span<const std::byte> payload = pkt.subspan(kHeaderSize);
    if (header.count == 0 || header.count == kEndOfSession) {
        if (!payload.empty()) {
            return PacketStatus::BadBlock;
        }
        return header.count == 0 ? PacketStatus::Heartbeat : PacketStatus::EndOfSession;
    }

    std::size_t off = 0;
    for (std::uint16_t i = 0; i < header.count; ++i) {
        if (payload.size() - off < kBlockPrefix) {
            return PacketStatus::BadBlock;
        }
        const std::size_t len = itch::load_be16(payload.data() + off);
        off += kBlockPrefix;
        if (payload.size() - off < len) {
            return PacketStatus::BadBlock;
        }
        off += len;
    }
    if (off != payload.size()) {
        return PacketStatus::BadBlock;
    }
    blocks = payload;
    return PacketStatus::Ok;
}

}  // namespace mold
