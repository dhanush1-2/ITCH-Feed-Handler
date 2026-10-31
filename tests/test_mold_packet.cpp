// tests/test_mold_packet.cpp
//
// MoldUDP64 framing (include/mold/packet.hpp), the packetizer and the retransmit server.
// Expected bytes are written out by hand from docs/spec/moldudp64.pdf, not through the code
// under test: a wrong offset in the header code cannot hide in the test.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <itch/framer.hpp>
#include <mold/packet.hpp>
#include <mold/packetizer.hpp>
#include <mold/retransmit_server.hpp>
#include <span>
#include <vector>

namespace {
using Bytes = std::vector<std::byte>;

constexpr std::array<char, 10> kSession{'T', 'E', 'S', 'T', 'S', 'E', 'S', 'S', '0', '1'};

std::byte b(unsigned v) {
    return static_cast<std::byte>(v);
}

// A source file: n frames of [u16 len][body], length (i % 47) + 1, body bytes derived from i.
Bytes make_source(std::size_t n) {
    Bytes out;
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t len = (i % 47) + 1;
        out.push_back(b(static_cast<unsigned>(len >> 8)));
        out.push_back(b(static_cast<unsigned>(len & 0xFF)));
        for (std::size_t k = 0; k < len; ++k) {
            out.push_back(b(static_cast<unsigned>((i * 31 + k * 7) & 0xFF)));
        }
    }
    return out;
}

struct Collected : mold::PacketSink {
    std::vector<Bytes> packets;
    void send(std::span<const std::byte> p) override { packets.emplace_back(p.begin(), p.end()); }
};

// Hand-built packet: header then raw payload.
Bytes hand_packet(std::uint64_t seq, std::uint16_t count, const Bytes& payload) {
    Bytes p;
    for (char c : kSession) {
        p.push_back(static_cast<std::byte>(c));
    }
    for (int i = 7; i >= 0; --i) {
        p.push_back(b(static_cast<unsigned>((seq >> (8 * i)) & 0xFF)));
    }
    p.push_back(b(count >> 8));
    p.push_back(b(count & 0xFF));
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}
}  // namespace

TEST(MoldPacket, HeaderIs20BytesBigEndian) {
    static_assert(mold::kHeaderSize == 20);
    const Bytes payload = {b(0), b(2), b(0xAA), b(0xBB)};
    const Bytes pkt     = hand_packet(0x0102030405060708ULL, 1, payload);
    ASSERT_EQ(pkt.size(), 24u);
    EXPECT_EQ(pkt[10], b(0x01));
    EXPECT_EQ(pkt[17], b(0x08));

    mold::PacketHeader         h{};
    std::span<const std::byte> blocks;
    ASSERT_EQ(mold::parse_packet(pkt, h, blocks), mold::PacketStatus::Ok);
    EXPECT_EQ(h.session, kSession);
    EXPECT_EQ(h.seq, 0x0102030405060708ULL);
    EXPECT_EQ(h.count, 1);
    EXPECT_EQ(blocks.size(), 4u);

    // The writer must produce the same 20 bytes as the hand-built header.
    Bytes out(mold::kHeaderSize);
    mold::write_header(out.data(), kSession, 0x0102030405060708ULL, 1);
    EXPECT_TRUE(std::equal(out.begin(), out.end(), pkt.begin()));
}

TEST(MoldPacket, RoundTripBlocks) {
    const Bytes                src = make_source(40);
    Bytes                      payload;
    itch::Framer               fr(src);
    std::span<const std::byte> m;
    std::uint16_t              count = 0;
    while (fr.next(m) == itch::FrameStatus::Ok && count < 5) {
        payload.push_back(b(static_cast<unsigned>(m.size() >> 8)));
        payload.push_back(b(static_cast<unsigned>(m.size() & 0xFF)));
        payload.insert(payload.end(), m.begin(), m.end());
        ++count;
    }
    const Bytes pkt = hand_packet(77, count, payload);

    mold::PacketHeader         h{};
    std::span<const std::byte> blocks;
    ASSERT_EQ(mold::parse_packet(pkt, h, blocks), mold::PacketStatus::Ok);
    EXPECT_EQ(h.seq, 77u);
    EXPECT_EQ(h.count, 5);
    ASSERT_EQ(blocks.size(), payload.size());
    EXPECT_TRUE(std::equal(blocks.begin(), blocks.end(), payload.begin()));
}

TEST(MoldPacket, HeartbeatAndEndOfSession) {
    mold::PacketHeader         h{};
    std::span<const std::byte> blocks;

    const Bytes hb = hand_packet(1234, 0, {});
    ASSERT_EQ(hb.size(), 20u);
    EXPECT_EQ(mold::parse_packet(hb, h, blocks), mold::PacketStatus::Heartbeat);
    EXPECT_EQ(h.seq, 1234u);  // the next expected message
    EXPECT_TRUE(blocks.empty());

    const Bytes eos = hand_packet(1234, 0xFFFF, {});
    EXPECT_EQ(mold::parse_packet(eos, h, blocks), mold::PacketStatus::EndOfSession);
    EXPECT_EQ(h.seq, 1234u);
    EXPECT_EQ(h.count, 0xFFFF);

    // Neither carries a payload: bytes after the header are a malformed packet.
    EXPECT_EQ(mold::parse_packet(hand_packet(1, 0, {b(0)}), h, blocks),
              mold::PacketStatus::BadBlock);
    EXPECT_EQ(mold::parse_packet(hand_packet(1, 0xFFFF, {b(0)}), h, blocks),
              mold::PacketStatus::BadBlock);
}

TEST(MoldPacket, TooShort) {
    mold::PacketHeader         h{};
    std::span<const std::byte> blocks;
    const Bytes                full = hand_packet(1, 0, {});
    for (std::size_t n = 0; n < 20; ++n) {
        EXPECT_EQ(mold::parse_packet(std::span<const std::byte>(full).first(n), h, blocks),
                  mold::PacketStatus::TooShort)
            << n;
    }
}

TEST(MoldPacket, BadBlockOnOverrunAndTrailingBytes) {
    mold::PacketHeader         h{};
    std::span<const std::byte> blocks;
    const Bytes                two = {b(0), b(1), b(9), b(0), b(2), b(7), b(7)};

    EXPECT_EQ(mold::parse_packet(hand_packet(1, 2, two), h, blocks), mold::PacketStatus::Ok);
    // Count promises 3 blocks but the payload holds 2.
    EXPECT_EQ(mold::parse_packet(hand_packet(1, 3, two), h, blocks), mold::PacketStatus::BadBlock);
    // Count 1 leaves the second block as trailing bytes.
    EXPECT_EQ(mold::parse_packet(hand_packet(1, 1, two), h, blocks), mold::PacketStatus::BadBlock);
    // A length that runs past the end of the packet.
    Bytes overrun = two;
    overrun[3]    = b(0);
    overrun[4]    = b(9);
    EXPECT_EQ(mold::parse_packet(hand_packet(1, 2, overrun), h, blocks),
              mold::PacketStatus::BadBlock);
    // A lone length byte.
    EXPECT_EQ(mold::parse_packet(hand_packet(1, 1, {b(0)}), h, blocks),
              mold::PacketStatus::BadBlock);
    // Count 1 with an empty payload.
    EXPECT_EQ(mold::parse_packet(hand_packet(1, 1, {}), h, blocks), mold::PacketStatus::BadBlock);
}

TEST(MoldPacket, ZeroLengthBlockIsLegalOnTheWire) {
    // The spec allows zero-length message data. parse_packet follows the spec; the ITCH
    // Framer is the layer that rejects a zero-length ITCH message.
    mold::PacketHeader         h{};
    std::span<const std::byte> blocks;
    EXPECT_EQ(mold::parse_packet(hand_packet(5, 1, {b(0), b(0)}), h, blocks),
              mold::PacketStatus::Ok);
}

TEST(Packetizer, NeverExceedsMtu) {
    const Bytes src = make_source(3000);
    for (std::size_t mtu : {std::size_t{69}, std::size_t{100}, std::size_t{1400}}) {
        mold::Packetizer pz(kSession, itch::Framer(src), mtu);
        std::size_t      packets = 0;
        while (auto p = pz.next()) {
            EXPECT_LE(p->size(), mtu);
            EXPECT_GE(p->size(), mold::kHeaderSize + 2u);
            ++packets;
        }
        EXPECT_GT(packets, 1u);
    }
}

TEST(Packetizer, GreedyFillLeavesNoRoomForTheNextMessage) {
    const Bytes              src = make_source(500);
    mold::Packetizer         pz(kSession, itch::Framer(src), 200);
    std::vector<std::size_t> sizes, first_block;
    while (auto p = pz.next()) {
        sizes.push_back(p->size());
        first_block.push_back(2 + pz.last_messages().front().size());
    }
    ASSERT_GT(sizes.size(), 3u);
    // Packet i did not take the first message of packet i+1 because it would not fit.
    for (std::size_t i = 0; i + 1 < sizes.size(); ++i) {
        EXPECT_GT(sizes[i] + first_block[i + 1], 200u) << i;
    }
}

TEST(Packetizer, SequencesAreContiguous) {
    const Bytes      src = make_source(2000);
    mold::Packetizer pz(kSession, itch::Framer(src), 500);
    std::uint64_t    expected = 1;  // sequences start at 1
    std::uint64_t    total    = 0;
    while (auto p = pz.next()) {
        mold::PacketHeader         h{};
        std::span<const std::byte> blocks;
        ASSERT_EQ(mold::parse_packet(*p, h, blocks), mold::PacketStatus::Ok);
        EXPECT_EQ(h.session, kSession);
        EXPECT_EQ(h.seq, expected);
        EXPECT_GT(h.count, 0);
        expected += h.count;
        total += h.count;
    }
    EXPECT_EQ(total, 2000u);
    EXPECT_EQ(pz.next_seq(), 2001u);
    EXPECT_EQ(pz.status(), itch::FrameStatus::End);
}

TEST(Packetizer, ReassemblyEqualsSourceBytes) {
    const Bytes      src = make_source(2500);
    mold::Packetizer pz(kSession, itch::Framer(src), 1400);
    Bytes            rebuilt;
    while (auto p = pz.next()) {
        mold::PacketHeader         h{};
        std::span<const std::byte> blocks;
        ASSERT_EQ(mold::parse_packet(*p, h, blocks), mold::PacketStatus::Ok);
        rebuilt.insert(rebuilt.end(), blocks.begin(), blocks.end());
    }
    EXPECT_EQ(rebuilt, src);
}

TEST(Packetizer, LastMessagesPointIntoTheSource) {
    const Bytes      src = make_source(50);
    mold::Packetizer pz(kSession, itch::Framer(src), 300);
    auto             p = pz.next();
    ASSERT_TRUE(p.has_value());
    mold::PacketHeader         h{};
    std::span<const std::byte> blocks;
    ASSERT_EQ(mold::parse_packet(*p, h, blocks), mold::PacketStatus::Ok);
    const auto msgs = pz.last_messages();
    ASSERT_EQ(msgs.size(), h.count);
    itch::Framer               fr(src);
    std::span<const std::byte> m;
    for (const auto& s : msgs) {
        ASSERT_EQ(fr.next(m), itch::FrameStatus::Ok);
        EXPECT_EQ(s.data(), m.data());
        EXPECT_EQ(s.size(), m.size());
    }
}

TEST(Packetizer, HeartbeatAndEndOfSessionCarryTheNextSequence) {
    const Bytes      src = make_source(10);
    mold::Packetizer pz(kSession, itch::Framer(src), 1400);
    while (pz.next()) {
    }
    mold::PacketHeader         h{};
    std::span<const std::byte> blocks;
    EXPECT_EQ(mold::parse_packet(pz.heartbeat(), h, blocks), mold::PacketStatus::Heartbeat);
    EXPECT_EQ(h.seq, 11u);
    EXPECT_EQ(mold::parse_packet(pz.end_of_session(), h, blocks), mold::PacketStatus::EndOfSession);
    EXPECT_EQ(h.seq, 11u);
}

TEST(Packetizer, MessageThatCannotFitAnyPacketThrows) {
    const Bytes      src = make_source(60);                // lengths up to 47
    mold::Packetizer pz(kSession, itch::Framer(src), 40);  // 20 header + 20 payload
    EXPECT_THROW(
        {
            while (pz.next()) {
            }
        },
        std::length_error);
}

TEST(Packetizer, FramerErrorStopsAndIsReported) {
    Bytes src = make_source(3);
    src.pop_back();  // truncate the last body
    mold::Packetizer pz(kSession, itch::Framer(src), 1400);
    auto             p = pz.next();
    ASSERT_TRUE(p.has_value());  // the two intact messages
    EXPECT_FALSE(pz.next().has_value());
    EXPECT_EQ(pz.status(), itch::FrameStatus::TruncatedBody);
}

namespace {
// Retain every message of `src` and return the retained messages for comparison.
std::vector<std::span<const std::byte>> retain_all(mold::RetransmitServer& rs, const Bytes& src) {
    std::vector<std::span<const std::byte>> msgs;
    itch::Framer                            fr(src);
    std::span<const std::byte>              m;
    std::uint64_t                           seq = 1;
    while (fr.next(m) == itch::FrameStatus::Ok) {
        rs.retain(seq++, m);
        msgs.push_back(m);
    }
    return msgs;
}
}  // namespace

TEST(RetransmitServer, ServesRetainedRange) {
    const Bytes            src = make_source(100);
    mold::RetransmitServer rs(kSession, 100, 300);
    const auto             msgs = retain_all(rs, src);

    Collected  sink;
    const auto sent = rs.serve(11, 40, sink);
    EXPECT_EQ(sent, 40u);
    EXPECT_GT(sink.packets.size(), 1u);  // 40 messages do not fit one 300-byte packet

    std::uint64_t expect = 11;
    for (const auto& pk : sink.packets) {
        EXPECT_LE(pk.size(), 300u);
        mold::PacketHeader         h{};
        std::span<const std::byte> blocks;
        ASSERT_EQ(mold::parse_packet(pk, h, blocks), mold::PacketStatus::Ok);
        EXPECT_EQ(h.session, kSession);
        EXPECT_EQ(h.seq, expect);
        // Each block must equal the original message.
        std::size_t off = 0;
        for (std::uint16_t i = 0; i < h.count; ++i) {
            const std::size_t len = (static_cast<std::size_t>(blocks[off]) << 8) |
                                    static_cast<std::size_t>(blocks[off + 1]);
            const auto& orig = msgs[expect - 1 + i];
            ASSERT_EQ(len, orig.size());
            EXPECT_TRUE(std::equal(orig.begin(), orig.end(),
                                   blocks.begin() + static_cast<std::ptrdiff_t>(off + 2)));
            off += 2 + len;
        }
        expect += h.count;
    }
    EXPECT_EQ(expect, 51u);
}

TEST(RetransmitServer, SingleMessageRequest) {
    const Bytes            src = make_source(20);
    mold::RetransmitServer rs(kSession, 20);
    retain_all(rs, src);
    Collected sink;
    EXPECT_EQ(rs.serve(7, 1, sink), 1u);
    ASSERT_EQ(sink.packets.size(), 1u);
}

TEST(RetransmitServer, ExpiredRangeSendsNothing) {
    const Bytes            src = make_source(100);
    mold::RetransmitServer rs(kSession, 10);  // keeps only 91..100
    retain_all(rs, src);

    Collected sink;
    EXPECT_EQ(rs.serve(1, 5, sink), 0u);    // long gone
    EXPECT_EQ(rs.serve(90, 3, sink), 0u);   // 90 just expired, so the range start is gone
    EXPECT_EQ(rs.serve(500, 3, sink), 0u);  // never sent
    EXPECT_EQ(rs.serve(95, 0, sink), 0u);   // nothing asked
    EXPECT_TRUE(sink.packets.empty());

    EXPECT_EQ(rs.serve(91, 10, sink), 10u);  // the oldest retained message still works
}

TEST(RetransmitServer, RangePastTheNewestIsClipped) {
    const Bytes            src = make_source(30);
    mold::RetransmitServer rs(kSession, 100);
    retain_all(rs, src);
    Collected sink;
    EXPECT_EQ(rs.serve(25, 100, sink), 6u);  // 25..30 exist
}
