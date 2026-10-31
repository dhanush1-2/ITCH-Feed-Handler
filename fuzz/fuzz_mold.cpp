// fuzz/fuzz_mold.cpp
//
// Feeds libFuzzer's bytes to the MoldUDP64 Receiver as a sequence of network events and
// checks its promises after every one:
//   - delivery is in strict sequence order, each sequence exactly once, every message
//     non-empty, starting at 1;
//   - nothing is delivered once the receiver is Stale or Finished;
//   - every re-request is for 1..65,534 messages in [next_expected, highest_known);
//   - the state agrees with the sequence bookkeeping (Live: nothing outstanding), the buffer
//     stays within its bound.
// Plus no out-of-bounds read (ASan), no undefined behaviour (UBSan), and termination.
//
// Input format, a stream of records (a truncated record is ignored):
//   0 L <L bytes>              a raw packet of L bytes (arbitrary bytes: the parser is the target)
//   1                          one tick of the retry clock
//   2 sh sl n                  a well-formed data packet, seq = sh:sl, n (1..8) tiny messages
//   3 k sh sl                  k even: heartbeat, k odd: end of session, seq = sh:sl
// The raw packet records give libFuzzer the freedom to corrupt anything; the synthesized ones
// get it past the parser into the recovery logic quickly. The seed corpus is
// fuzz/corpus_mold (tools/seed_corpus.cpp --mold).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mold/packet.hpp>
#include <mold/receiver.hpp>
#include <span>
#include <vector>

namespace {

[[noreturn]] void fail(const char* what) {
    std::fprintf(stderr, "INVARIANT VIOLATED: %s\n", what);
    std::abort();
}

constexpr mold::Session kSession{'F', 'U', 'Z', 'Z', 'S', 'E', 'S', 'S', '0', '1'};

struct Sink {
    std::uint64_t delivered = 0;
    void          on_message(std::uint64_t seq, std::span<const std::byte> msg) {
        if (seq != delivered + 1) {
            fail("delivery out of order, duplicated or skipped");
        }
        if (msg.empty()) {
            fail("empty message delivered");
        }
        ++delivered;
    }
};

struct Req;
using Rx = mold::Receiver<Sink, Req>;

struct Req {
    const Rx* rx = nullptr;
    void      request(std::uint64_t seq, std::uint16_t count) const {
        if (count == 0 || count > mold::kMaxDataCount) {
            fail("request count out of range");
        }
        if (seq < rx->next_expected() || seq + count < seq || seq + count > rx->highest_known()) {
            fail("request outside [next_expected, highest_known)");
        }
    }
};

void put_header(std::vector<std::byte>& p, std::uint64_t seq, std::uint16_t count) {
    p.resize(mold::kHeaderSize);
    mold::write_header(p.data(), kSession, seq, count);
}

void put_block(std::vector<std::byte>& p, std::uint64_t seq) {
    const std::size_t len = seq % 3 + 1;
    const std::size_t at  = p.size();
    p.resize(at + mold::kBlockPrefix + len, static_cast<std::byte>(seq & 0xFF));
    mold::store_be16(p.data() + at, static_cast<std::uint16_t>(len));
}

void check(const Rx& rx, const Sink& sink, const mold::RecoveryConfig& cfg) {
    if (sink.delivered != rx.next_expected() - 1) {
        fail("delivered count disagrees with next_expected");
    }
    if (rx.highest_known() < rx.next_expected()) {
        fail("highest_known below next_expected");
    }
    if (rx.buffered_packets() > cfg.max_buffered_packets) {
        fail("buffer over its bound");
    }
    switch (rx.state()) {
        case mold::RxState::Live:
            if (rx.highest_known() != rx.next_expected() || rx.buffered_packets() != 0) {
                fail("Live with something outstanding");
            }
            break;
        case mold::RxState::Recovering:
            if (rx.highest_known() <= rx.next_expected()) {
                fail("Recovering with nothing outstanding");
            }
            break;
        case mold::RxState::Stale:
        case mold::RxState::Finished:
            if (rx.buffered_packets() != 0) {
                fail("terminal state holding packets");
            }
            break;
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    mold::RecoveryConfig cfg;
    cfg.timeout_ticks        = 2;
    cfg.max_attempts         = 3;
    cfg.max_buffered_packets = 4;
    cfg.max_gap_messages     = 5000;

    Sink sink;
    Req  req;
    Rx   rx(cfg, sink, req);
    req.rx = &rx;

    std::vector<std::byte> pkt;
    std::size_t            i = 0;
    while (i < size) {
        const std::uint8_t op = data[i++];
        const bool         was_term =
            rx.state() == mold::RxState::Stale || rx.state() == mold::RxState::Finished;
        const std::uint64_t before = sink.delivered;
        switch (op & 3) {
            case 0: {
                if (i >= size) {
                    return 0;
                }
                const std::size_t want = data[i++];
                const std::size_t len  = std::min(want, size - i);
                const auto*       p    = reinterpret_cast<const std::byte*>(data + i);
                i += len;
                rx.on_packet(std::span<const std::byte>(p, len));
                break;
            }
            case 1:
                rx.on_tick();
                break;
            case 2: {
                if (size - i < 3) {
                    return 0;
                }
                const std::uint64_t seq   = (std::uint64_t{data[i]} << 8) | data[i + 1];
                const unsigned      count = data[i + 2] % 8 + 1;
                i += 3;
                put_header(pkt, seq, static_cast<std::uint16_t>(count));
                for (unsigned k = 0; k < count; ++k) {
                    put_block(pkt, seq + k);
                }
                rx.on_packet(pkt);
                break;
            }
            default: {
                if (size - i < 3) {
                    return 0;
                }
                const bool          end = (data[i] & 1) != 0;
                const std::uint64_t seq = (std::uint64_t{data[i + 1]} << 8) | data[i + 2];
                i += 3;
                put_header(pkt, seq, end ? mold::kEndOfSession : 0);
                rx.on_packet(pkt);
                break;
            }
        }
        if (was_term && sink.delivered != before) {
            fail("delivered after Stale or Finished");
        }
        check(rx, sink, cfg);
    }
    return 0;
}
