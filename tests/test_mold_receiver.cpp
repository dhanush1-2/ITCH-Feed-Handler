// tests/test_mold_receiver.cpp
//
// The MoldUDP64 receiver (include/mold/receiver.hpp): sequence checking, buffering, gap
// requests, retries, and the cases where it must give up. Packets are built by hand here, not
// through the packetizer, so a framing mistake in one cannot hide a mistake in the other.

#include <gtest/gtest.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <itch/framer.hpp>
#include <map>
#include <mold/packet.hpp>
#include <mold/packetizer.hpp>
#include <mold/receiver.hpp>
#include <mold/retransmit_server.hpp>
#include <span>
#include <utility>
#include <vector>

namespace {
using Bytes = std::vector<std::byte>;
using mold::RxState;

constexpr mold::Session kSession{'T', 'E', 'S', 'T', 'S', 'E', 'S', 'S', '0', '1'};
constexpr mold::Session kOther{'O', 'T', 'H', 'E', 'R', 'S', 'E', 'S', 'S', '9'};

Bytes msg_of(std::uint64_t seq) {
    Bytes m(seq % 7 + 1);
    for (std::size_t k = 0; k < m.size(); ++k) {
        m[k] = static_cast<std::byte>((seq * 13 + k * 5 + 1) & 0xFF);
    }
    return m;
}

void put_header(Bytes& p, const mold::Session& s, std::uint64_t seq, std::uint16_t count) {
    p.resize(mold::kHeaderSize);
    mold::write_header(p.data(), s, seq, count);
}

void put_block(Bytes& p, const Bytes& m) {
    const std::size_t at = p.size();
    p.resize(at + 2 + m.size());
    mold::store_be16(p.data() + at, static_cast<std::uint16_t>(m.size()));
    std::copy(m.begin(), m.end(), p.begin() + static_cast<std::ptrdiff_t>(at + 2));
}

// Messages seq .. seq+count-1.
Bytes data(std::uint64_t seq, std::uint16_t count, const mold::Session& s = kSession) {
    Bytes p;
    put_header(p, s, seq, count);
    for (std::uint16_t i = 0; i < count; ++i) {
        put_block(p, msg_of(seq + i));
    }
    return p;
}

Bytes control(std::uint64_t seq, std::uint16_t count, const mold::Session& s = kSession) {
    Bytes p;
    put_header(p, s, seq, count);
    return p;
}
Bytes heartbeat(std::uint64_t next) {
    return control(next, 0);
}
Bytes eos(std::uint64_t next) {
    return control(next, mold::kEndOfSession);
}

struct Sink {
    std::vector<std::pair<std::uint64_t, Bytes>> got;
    void on_message(std::uint64_t seq, std::span<const std::byte> m) {
        got.emplace_back(seq, Bytes(m.begin(), m.end()));
    }
};

struct Requests {
    std::vector<std::pair<std::uint64_t, std::uint16_t>> asked;
    void request(std::uint64_t seq, std::uint16_t count) { asked.emplace_back(seq, count); }
};

// Delivered exactly 1..n, once each, in order, with the right bytes.
::testing::AssertionResult delivered_1_to(const Sink& s, std::uint64_t n) {
    if (s.got.size() != n) {
        return ::testing::AssertionFailure() << "delivered " << s.got.size() << ", want " << n;
    }
    for (std::uint64_t i = 0; i < n; ++i) {
        if (s.got[i].first != i + 1 || s.got[i].second != msg_of(i + 1)) {
            return ::testing::AssertionFailure() << "wrong message at position " << i;
        }
    }
    return ::testing::AssertionSuccess();
}

struct Rig {
    mold::RecoveryConfig           cfg;
    Sink                           sink;
    Requests                       req;
    mold::Receiver<Sink, Requests> rx;
    explicit Rig(mold::RecoveryConfig c = {}) : cfg(c), rx(cfg, sink, req) {}
    void feed(const Bytes& p, bool retransmit = false) { rx.on_packet(p, retransmit); }
};

mold::RecoveryConfig quick() {
    mold::RecoveryConfig c;
    c.timeout_ticks = 2;
    c.max_attempts  = 3;
    return c;
}
}  // namespace

TEST(Receiver, InOrderDeliversAll) {
    Rig r;
    r.feed(data(1, 3));
    r.feed(data(4, 3));
    r.feed(data(7, 1));
    EXPECT_TRUE(delivered_1_to(r.sink, 7));
    EXPECT_EQ(r.rx.state(), RxState::Live);
    EXPECT_EQ(r.rx.next_expected(), 8u);
    EXPECT_TRUE(r.req.asked.empty());
    const auto& c = r.rx.counts();
    EXPECT_EQ(c.gaps_detected + c.dup_packets + c.partial_overlaps + c.malformed + c.wrong_session,
              0u);
}

TEST(Receiver, DuplicateDiscarded) {
    Rig r;
    r.feed(data(1, 3));
    r.feed(data(1, 3));
    r.feed(data(2, 2));  // wholly below expected too
    EXPECT_TRUE(delivered_1_to(r.sink, 3));
    EXPECT_EQ(r.rx.counts().dup_packets, 2u);
    EXPECT_EQ(r.rx.state(), RxState::Live);
}

TEST(Receiver, PartialOverlapDeliversTailOnce) {
    Rig r;
    r.feed(data(1, 3));
    r.feed(data(2, 4));  // 2..5: 2 and 3 are old, 4 and 5 are new
    EXPECT_TRUE(delivered_1_to(r.sink, 5));
    EXPECT_EQ(r.rx.counts().partial_overlaps, 1u);
    EXPECT_EQ(r.rx.counts().dup_packets, 0u);
    EXPECT_EQ(r.rx.state(), RxState::Live);
}

TEST(Receiver, GapRequestsExactRange) {
    Rig r;
    r.feed(data(1, 3));
    r.feed(data(7, 3));  // 4, 5, 6 are missing
    ASSERT_EQ(r.req.asked.size(), 1u);
    EXPECT_EQ(r.req.asked[0], (std::pair<std::uint64_t, std::uint16_t>{4, 3}));
    EXPECT_EQ(r.rx.state(), RxState::Recovering);
    EXPECT_EQ(r.sink.got.size(), 3u);  // 7..9 wait for the gap
    EXPECT_EQ(r.rx.counts().gaps_detected, 1u);
    EXPECT_EQ(r.rx.counts().max_gap_len, 3u);
    EXPECT_EQ(r.rx.counts().retransmit_requests, 1u);
}

TEST(Receiver, BufferedDuringRecoveryThenDrainedInOrder) {
    Rig r;
    r.feed(data(1, 2));
    r.feed(data(5, 2));
    r.feed(data(7, 2));
    EXPECT_EQ(r.sink.got.size(), 2u);
    EXPECT_EQ(r.rx.buffered_packets(), 2u);
    r.feed(data(3, 2), true);  // the retransmit
    EXPECT_TRUE(delivered_1_to(r.sink, 8));
    EXPECT_EQ(r.rx.counts().messages_from_retransmit, 2u);  // 5..8 came from the original packets
    EXPECT_EQ(r.rx.state(), RxState::Live);
    EXPECT_EQ(r.rx.buffered_packets(), 0u);
    EXPECT_EQ(r.rx.counts().messages_delivered_late, 2u);
    EXPECT_EQ(r.rx.counts().messages_buffered_peak, 4u);
    EXPECT_EQ(r.req.asked.size(), 1u);  // the second packet extends no hole
}

TEST(Receiver, MultipleGapsHealedInOrder) {
    Rig r;
    r.feed(data(1, 1));
    r.feed(data(4, 1));
    r.feed(data(7, 1));
    r.feed(data(10, 1));
    const std::vector<std::pair<std::uint64_t, std::uint16_t>> want = {{2, 2}, {5, 2}, {8, 2}};
    EXPECT_EQ(r.req.asked, want);
    EXPECT_EQ(r.rx.counts().gaps_detected, 3u);
    r.feed(data(8, 2));
    r.feed(data(5, 2));
    EXPECT_EQ(r.sink.got.size(), 1u);  // 2 and 3 still missing: nothing may jump ahead
    r.feed(data(2, 2));
    EXPECT_TRUE(delivered_1_to(r.sink, 10));
    EXPECT_EQ(r.rx.state(), RxState::Live);
}

TEST(Receiver, ReorderInsideWindowHeals) {
    Rig r;
    r.feed(data(1, 2));
    r.feed(data(5, 2));  // arrives before 3..4
    r.feed(data(3, 2));
    EXPECT_TRUE(delivered_1_to(r.sink, 6));
    EXPECT_EQ(r.rx.state(), RxState::Live);
    EXPECT_EQ(r.rx.counts().dup_packets, 0u);
    // A late retransmit answer is just a duplicate now.
    r.feed(data(3, 2));
    EXPECT_EQ(r.rx.counts().dup_packets, 1u);
    EXPECT_TRUE(delivered_1_to(r.sink, 6));
}

TEST(Receiver, LostRetransmitIsRetried) {
    Rig r(quick());
    r.feed(data(1, 1));
    r.feed(data(5, 1));
    ASSERT_EQ(r.req.asked.size(), 1u);
    r.rx.on_tick();
    EXPECT_EQ(r.req.asked.size(), 1u);  // not yet
    r.rx.on_tick();                     // timeout: the first answer never came
    ASSERT_EQ(r.req.asked.size(), 2u);
    EXPECT_EQ(r.req.asked[1], r.req.asked[0]);
    EXPECT_EQ(r.rx.state(), RxState::Recovering);
    r.feed(data(2, 3));
    EXPECT_TRUE(delivered_1_to(r.sink, 5));
    EXPECT_EQ(r.rx.state(), RxState::Live);
    // Live receivers ask for nothing, however long they tick.
    for (int i = 0; i < 100; ++i) {
        r.rx.on_tick();
    }
    EXPECT_EQ(r.req.asked.size(), 2u);
}

TEST(Receiver, TailLossFoundByHeartbeat) {
    Rig r;
    r.feed(data(1, 3));
    r.feed(heartbeat(4));  // nothing lost
    EXPECT_EQ(r.rx.state(), RxState::Live);
    EXPECT_TRUE(r.req.asked.empty());
    r.feed(heartbeat(8));  // 4..7 were sent and never arrived, and nothing followed them
    ASSERT_EQ(r.req.asked.size(), 1u);
    EXPECT_EQ(r.req.asked[0], (std::pair<std::uint64_t, std::uint16_t>{4, 4}));
    EXPECT_EQ(r.rx.state(), RxState::Recovering);
    r.feed(data(4, 4), true);
    EXPECT_TRUE(delivered_1_to(r.sink, 7));
    EXPECT_EQ(r.rx.counts().messages_from_retransmit, 4u);
    EXPECT_EQ(r.rx.state(), RxState::Live);
    EXPECT_EQ(r.rx.counts().messages_delivered_late, 4u);
}

TEST(Receiver, EndOfSessionWithGapWaitsForRecovery) {
    {
        Rig r;
        r.feed(data(1, 3));
        r.feed(eos(4));
        EXPECT_EQ(r.rx.state(), RxState::Finished);
        EXPECT_TRUE(delivered_1_to(r.sink, 3));
    }
    Rig r;
    r.feed(data(1, 3));
    r.feed(eos(7));  // 4..6 never arrived
    EXPECT_EQ(r.rx.state(), RxState::Recovering);
    ASSERT_EQ(r.req.asked.size(), 1u);
    EXPECT_EQ(r.req.asked[0], (std::pair<std::uint64_t, std::uint16_t>{4, 3}));
    r.feed(data(4, 3));
    EXPECT_EQ(r.rx.state(), RxState::Finished);
    EXPECT_TRUE(delivered_1_to(r.sink, 6));
    r.feed(data(7, 1));  // nothing is delivered after the end
    EXPECT_EQ(r.sink.got.size(), 6u);
}

TEST(Receiver, WrongSessionDropped) {
    Rig r;
    r.feed(data(1, 2));          // locks kSession
    r.feed(data(3, 2, kOther));  // another session's data: dropped, not a gap, not a dup
    r.feed(heartbeat(50));       // kSession heartbeat is fine and opens a hole
    r.feed(control(90, 0, kOther));
    EXPECT_EQ(r.rx.counts().wrong_session, 2u);
    EXPECT_EQ(r.sink.got.size(), 2u);
    EXPECT_EQ(r.rx.counts().gaps_detected, 1u);
    EXPECT_EQ(r.req.asked.size(), 1u);
    EXPECT_EQ(r.req.asked[0].first, 3u);
}

TEST(Receiver, SessionLockedByFirstWellFormedPacket) {
    Rig   r;
    Bytes junk = data(1, 2, kOther);
    junk.pop_back();  // malformed: must not lock kOther
    r.feed(junk);
    EXPECT_EQ(r.rx.counts().malformed, 1u);
    r.feed(data(1, 2));  // the real session
    r.feed(data(3, 1, kOther));
    EXPECT_EQ(r.rx.counts().wrong_session, 1u);
    EXPECT_TRUE(delivered_1_to(r.sink, 2));
}

TEST(Receiver, MalformedPacketTreatedAsLoss) {
    Rig r;
    r.feed(data(1, 3));

    // 4..6 arrives with a zero-length block in the middle: nothing of it may be applied.
    Bytes zero;
    put_header(zero, kSession, 4, 3);
    put_block(zero, msg_of(4));
    put_block(zero, Bytes{});
    put_block(zero, msg_of(6));
    r.feed(zero);
    // Sequence arithmetic that overflows 64 bits.
    r.feed(data(UINT64_MAX, 2));
    // A truncated packet, a short one, and a header-only packet claiming messages.
    Bytes cut = data(4, 3);
    cut.resize(cut.size() - 1);
    r.feed(cut);
    r.feed(Bytes(7, std::byte{1}));
    r.feed(control(4, 3));
    // Sequence 0 does not exist.
    r.feed(data(0, 1));
    EXPECT_EQ(r.rx.counts().malformed, 6u);
    EXPECT_EQ(r.sink.got.size(), 3u);
    EXPECT_EQ(r.rx.state(), RxState::Live);
    EXPECT_TRUE(r.req.asked.empty());

    r.feed(data(7, 3));  // the first thing past the loss reveals it
    ASSERT_EQ(r.req.asked.size(), 1u);
    EXPECT_EQ(r.req.asked[0], (std::pair<std::uint64_t, std::uint16_t>{4, 3}));
    EXPECT_EQ(r.sink.got.size(), 3u);
    r.feed(data(4, 3));  // recovered by retransmit
    EXPECT_TRUE(delivered_1_to(r.sink, 9));
    EXPECT_EQ(r.rx.state(), RxState::Live);
}

TEST(Receiver, BufferOverflowStillRecovers) {
    mold::RecoveryConfig cfg;
    cfg.timeout_ticks        = 2;
    cfg.max_attempts         = 20;
    cfg.max_buffered_packets = 2;
    Rig                     r(cfg);
    constexpr std::uint64_t kN = 40;

    r.feed(data(1, 1));
    // Odd sequences arrive, even ones are lost: far more out-of-order packets than fit.
    for (std::uint64_t s = 3; s <= kN; s += 2) {
        r.feed(data(s, 1));
    }
    r.feed(heartbeat(kN + 1));
    EXPECT_GT(r.rx.counts().buffer_overflow_drops, 0u);
    EXPECT_EQ(r.rx.counts().messages_buffered_peak, 2u);
    EXPECT_EQ(r.rx.state(), RxState::Recovering);

    // A retransmit server that answers every request, one message per packet. Dropped answers
    // are asked for again on the timeout.
    for (int round = 0; round < 200 && r.rx.state() == RxState::Recovering; ++round) {
        const auto asked = r.req.asked;
        r.req.asked.clear();
        for (const auto& [seq, count] : asked) {
            for (std::uint64_t s = seq; s < seq + count; ++s) {
                r.feed(data(s, 1));
            }
        }
        r.rx.on_tick();
        r.rx.on_tick();
    }
    EXPECT_EQ(r.rx.state(), RxState::Live);
    EXPECT_TRUE(delivered_1_to(r.sink, kN));
}

TEST(Receiver, UnrecoverableGapExpiredGoesStale) {
    // The real server, with a window too small to still hold the lost messages.
    Bytes source;
    for (std::uint64_t s = 1; s <= 30; ++s) {
        const Bytes m = msg_of(s);
        source.push_back(static_cast<std::byte>(m.size() >> 8));
        source.push_back(static_cast<std::byte>(m.size() & 0xFF));
        source.insert(source.end(), m.begin(), m.end());
    }
    mold::Packetizer       pk(kSession, itch::Framer(source), 40);
    mold::RetransmitServer server(kSession, 4);
    std::vector<Bytes>     packets;
    while (const auto p = pk.next()) {
        const auto          msgs = pk.last_messages();
        const std::uint64_t seq0 = pk.next_seq() - msgs.size();
        for (std::size_t i = 0; i < msgs.size(); ++i) {
            server.retain(seq0 + i, msgs[i]);
        }
        packets.emplace_back(p->begin(), p->end());
    }
    ASSERT_GE(packets.size(), 6u);

    struct Answering {
        mold::RetransmitServer* server;
        std::vector<Bytes>*     out;
        struct Collect : mold::PacketSink {
            std::vector<Bytes>* out;
            void                send(std::span<const std::byte> p) override {
                out->emplace_back(p.begin(), p.end());
            }
        } collect;
        std::size_t asked = 0;
        void        request(std::uint64_t seq, std::uint16_t count) {
            ++asked;
            collect.out = out;
            (void)server->serve(seq, count, collect);
        }
    };
    Sink               sink;
    std::vector<Bytes> answers;
    Answering          req{&server, &answers, {}};
    mold::Receiver     rx(quick(), sink, req);
    rx.on_packet(packets[0]);
    const std::size_t first = sink.got.size();
    rx.on_packet(packets[4]);  // packets 1..3 lost, and their messages are long gone
    EXPECT_EQ(rx.state(), RxState::Recovering);
    EXPECT_TRUE(answers.empty());  // the server had nothing for the first lost message
    for (int i = 0; i < 100 && rx.state() == RxState::Recovering; ++i) {
        rx.on_tick();
    }
    EXPECT_EQ(rx.state(), RxState::Stale);
    EXPECT_EQ(rx.stale_reason(), mold::StaleReason::RetriesExhausted);
    // Terminated, and nothing more is ever delivered.
    for (const Bytes& p : packets) {
        rx.on_packet(p);
    }
    for (int i = 0; i < 10; ++i) {
        rx.on_tick();
    }
    EXPECT_EQ(sink.got.size(), first);
    EXPECT_EQ(rx.state(), RxState::Stale);
}

TEST(Receiver, UnrecoverableGapRetriesExhaustedGoesStale) {
    Rig r(quick());  // timeout 2 ticks, 3 attempts
    r.feed(data(1, 2));
    r.feed(data(6, 2));
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(r.rx.state(), RxState::Recovering) << "tick " << i;
        r.rx.on_tick();
    }
    EXPECT_EQ(r.rx.state(), RxState::Stale);
    EXPECT_EQ(r.rx.stale_reason(), mold::StaleReason::RetriesExhausted);
    // Exactly max_attempts requests of the same range, then it stops asking.
    ASSERT_EQ(r.req.asked.size(), 3u);
    for (const auto& a : r.req.asked) {
        EXPECT_EQ(a, (std::pair<std::uint64_t, std::uint16_t>{3, 3}));
    }
    // The answer arriving now is too late: nothing is delivered after Stale.
    r.feed(data(3, 3));
    r.feed(data(8, 1));
    for (int i = 0; i < 20; ++i) {
        r.rx.on_tick();
    }
    EXPECT_EQ(r.sink.got.size(), 2u);
    EXPECT_EQ(r.req.asked.size(), 3u);
}

TEST(Receiver, ProgressResetsTheAttemptBudget) {
    Rig r(quick());
    r.feed(data(1, 1));
    r.feed(data(10, 1));
    r.feed(data(20, 1));  // holes 2..9 and 11..19
    // Many ticks, but every few of them the head moves, so the receiver never gives up.
    std::uint64_t next = 2;
    for (int i = 0; i < 40 && next < 10; ++i) {
        r.rx.on_tick();
        r.rx.on_tick();
        r.rx.on_tick();
        r.feed(data(next++, 1));
        EXPECT_EQ(r.rx.state(), RxState::Recovering);
    }
    EXPECT_EQ(r.sink.got.size(), 10u);  // 1..9 and the buffered 10
}

TEST(Receiver, HugeGapSplitsRequests) {
    Rig r;
    r.feed(data(1, 1));
    r.feed(heartbeat(200'001));  // 2..200000 are missing: 199,999 messages
    std::uint64_t total = 0;
    std::uint64_t next  = 2;
    for (const auto& [seq, count] : r.req.asked) {
        EXPECT_EQ(seq, next);
        EXPECT_GT(count, 0u);
        EXPECT_LE(count, 65'534u);
        total += count;
        next += count;
    }
    EXPECT_EQ(total, 199'999u);
    ASSERT_EQ(r.req.asked.size(), 4u);
    EXPECT_EQ(r.req.asked[0].second, 65'534u);
    EXPECT_EQ(r.rx.counts().retransmit_requests, 4u);
    EXPECT_EQ(r.rx.counts().max_gap_len, 199'999u);
    EXPECT_EQ(r.rx.counts().gaps_detected, 1u);
}

TEST(Receiver, GapBeyondTheReachLimitGoesStaleWithoutAFlood) {
    mold::RecoveryConfig cfg;
    cfg.max_gap_messages = 1000;
    Rig r(cfg);
    r.feed(data(1, 1));
    r.feed(data(1 + 1000, 1));  // within reach
    EXPECT_EQ(r.rx.state(), RxState::Recovering);
    r.feed(heartbeat(UINT64_MAX));  // a corrupt or hostile sequence number
    EXPECT_EQ(r.rx.state(), RxState::Stale);
    EXPECT_EQ(r.rx.stale_reason(), mold::StaleReason::GapTooLarge);
    EXPECT_LE(r.req.asked.size(), 1u);
}

TEST(Receiver, EndOfSessionBelowKnownDataIsMalformed) {
    Rig r;
    r.feed(data(1, 3));
    r.feed(data(8, 2));  // known data reaches 10
    r.feed(eos(5));      // claims the session ended before it
    EXPECT_EQ(r.rx.counts().malformed, 1u);
    EXPECT_EQ(r.rx.state(), RxState::Recovering);
    r.feed(eos(10));
    r.feed(data(4, 4));
    EXPECT_EQ(r.rx.state(), RxState::Finished);
    EXPECT_TRUE(delivered_1_to(r.sink, 9));
}

TEST(Receiver, ConfigIsValidated) {
    Sink                 s;
    Requests             q;
    mold::RecoveryConfig c;
    c.timeout_ticks = 0;
    EXPECT_THROW((mold::Receiver<Sink, Requests>(c, s, q)), std::invalid_argument);
    c              = {};
    c.max_attempts = 0;
    EXPECT_THROW((mold::Receiver<Sink, Requests>(c, s, q)), std::invalid_argument);
}

TEST(Receiver, BufferedPacketStraddlingExpectedDeliversOnlyItsTail) {
    Rig r;
    r.feed(data(3, 3));  // 3..5 buffered; 1 and 2 missing
    ASSERT_EQ(r.req.asked.size(), 1u);
    EXPECT_EQ(r.req.asked[0], (std::pair<std::uint64_t, std::uint16_t>{1, 2}));
    // The answer overshoots: 1..4, so the buffered packet now straddles expected (5).
    r.feed(data(1, 4), true);
    EXPECT_TRUE(delivered_1_to(r.sink, 5));
    EXPECT_EQ(r.rx.counts().partial_overlaps, 1u);
    EXPECT_EQ(r.rx.counts().dup_packets, 0u);
    EXPECT_EQ(r.rx.state(), RxState::Live);
    EXPECT_EQ(r.rx.counts().messages_from_retransmit, 4u);
}

TEST(Receiver, PartialRetransmitThenExactRemainderIsRequested) {
    Rig r(quick());  // timeout 2 ticks, 3 attempts
    r.feed(data(1, 1));
    r.feed(data(8, 1));  // 2..7 missing
    ASSERT_EQ(r.req.asked.size(), 1u);
    EXPECT_EQ(r.req.asked[0], (std::pair<std::uint64_t, std::uint16_t>{2, 6}));
    r.feed(data(2, 3), true);  // the server answers only 2..4 (the rest was lost on the way)
    EXPECT_EQ(r.sink.got.size(), 4u);
    r.rx.on_tick();
    r.rx.on_tick();  // no further progress: ask again, for what is still missing and no more
    ASSERT_EQ(r.req.asked.size(), 2u);
    EXPECT_EQ(r.req.asked[1], (std::pair<std::uint64_t, std::uint16_t>{5, 3}));
    r.feed(data(5, 3), true);
    EXPECT_TRUE(delivered_1_to(r.sink, 8));
    EXPECT_EQ(r.rx.state(), RxState::Live);
    EXPECT_EQ(r.rx.counts().messages_from_retransmit, 6u);
}
