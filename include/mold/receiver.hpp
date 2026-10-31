// include/mold/receiver.hpp
#pragma once

// MoldUDP64 sequence checker and gap recovery.
//
// Receiver<Sink, Requester> takes packets in whatever order and number the network delivers
// them and hands every message to Sink::on_message(seq, bytes) exactly once, in strict
// sequence order. A book is therefore never updated out of order, whatever the network did.
//
//   Sink       void on_message(std::uint64_t seq, std::span<const std::byte> msg)
//   Requester  void request(std::uint64_t seq, std::uint16_t count)   (a re-request packet)
//
// Model. `expected` is the next sequence to deliver. `hi` is the next sequence after the
// highest message the receiver knows exists, from data, a heartbeat or an end-of-session
// packet. hi == expected means Live; hi > expected means Recovering, and the holes are what
// lies between expected and hi that is not in the buffer.
//
//   end <= expected          duplicate: counted, discarded
//   start < expected < end   partial overlap: only the new tail is delivered
//   start <= expected        delivered at once, then the buffer drains while it is contiguous
//   start > expected         buffered by start sequence (a copy of the packet), and if it is
//                            beyond hi the hole [hi, start) is requested at once
//   heartbeat seq > hi       a hole [hi, seq): this is how tail loss is found
//   end of session           Finished once expected reaches its sequence; with a gap before
//                            it, the receiver keeps recovering first
//
// The buffer holds at most max_buffered_packets packets. A packet that does not fit is dropped
// and counted, and a hole is not marked as known beyond its start, so the next packet or
// heartbeat past it reveals it as a hole and requests it: the retransmit heals it.
//
// Time is a logical tick the caller advances with on_tick(), so tests are deterministic. While
// Recovering, timeout_ticks ticks without progress re-request every remaining hole. A head
// position that has been requested max_attempts times without the stream advancing makes the
// receiver Stale. The protocol has no negative acknowledgement, so a range that the server
// no longer holds (expired) shows up the same way, as requests that are never answered.
// Stale is final: delivery stops, and the owner must treat the stream as broken.
//
// A request is split into messages of at most 65,534 each (0xFFFF is the end-of-session value
// of the count field).
//
// Validation, all before anything of a packet is delivered or buffered: the whole payload
// must parse (parse_packet), no block may have zero length, the sequence must be nonzero and
// seq + count must not overflow 64 bits. Anything else is `malformed`: counted, nothing
// applied. A malformed packet is simply a lost one, and retransmit heals it.
//
// The first well-formed packet locks the session; a packet from another session is counted as
// wrong_session and dropped. A gap that reaches more than max_gap_messages past `expected` is
// not chased (a corrupt sequence number would otherwise ask for 2^48 requests): the receiver
// goes Stale.
//
// THIS CODE READS UNTRUSTED BYTES.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <itch/wire.hpp>
#include <map>
#include <mold/packet.hpp>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mold {

struct RecoveryConfig {
    std::uint32_t timeout_ticks        = 8;           // ticks without progress before a re-request
    std::uint32_t max_attempts         = 16;          // requests of one head position before Stale
    std::size_t   max_buffered_packets = 4096;        // out-of-order packets held
    std::uint64_t max_gap_messages     = 1ULL << 24;  // furthest reach past `expected`
};

enum class RxState { Live, Recovering, Stale, Finished };

enum class StaleReason { None, RetriesExhausted, GapTooLarge };

inline const char* to_string(RxState s) noexcept {
    switch (s) {
        case RxState::Live:
            return "Live";
        case RxState::Recovering:
            return "Recovering";
        case RxState::Stale:
            return "Stale";
        case RxState::Finished:
            return "Finished";
    }
    return "Unknown";
}

inline const char* to_string(StaleReason r) noexcept {
    switch (r) {
        case StaleReason::None:
            return "none";
        case StaleReason::RetriesExhausted:
            return "retries_exhausted";
        case StaleReason::GapTooLarge:
            return "gap_too_large";
    }
    return "unknown";
}

struct RxCounts {
    std::uint64_t gaps_detected       = 0;  // holes found (each is requested once at once)
    std::uint64_t dup_packets         = 0;  // data packets wholly below `expected`
    std::uint64_t partial_overlaps    = 0;  // data packets that straddled `expected`
    std::uint64_t wrong_session       = 0;
    std::uint64_t malformed           = 0;
    std::uint64_t retransmit_requests = 0;  // request packets handed to the Requester
    // Delivered late: by a packet that arrived while a later message was already known. Reordered
    // packets count here as well as retransmitted ones; it does not measure loss.
    std::uint64_t messages_delivered_late = 0;
    // Messages whose first delivery came from a packet marked as a retransmit answer.
    std::uint64_t messages_from_retransmit = 0;
    std::uint64_t messages_buffered_peak   = 0;  // most messages held in the buffer at once
    std::uint64_t buffer_overflow_drops    = 0;
    std::uint64_t max_gap_len              = 0;  // longest single hole found, in messages
};

template <class Sink, class Requester>
class Receiver {
public:
    Receiver(const RecoveryConfig& cfg, Sink& sink, Requester& requester)
        : cfg_(cfg), sink_(sink), req_(requester) {
        if (cfg.timeout_ticks == 0 || cfg.max_attempts == 0 || cfg.max_gap_messages == 0) {
            throw std::invalid_argument(
                "RecoveryConfig: timeout_ticks, max_attempts and max_gap_messages must be > 0");
        }
    }

    // `retransmit` says the packet is an answer to a re-request (the transport knows which
    // socket it came in on); it only feeds messages_from_retransmit.
    void on_packet(std::span<const std::byte> pkt, bool retransmit = false) {
        if (state_ == RxState::Stale || state_ == RxState::Finished) {
            return;
        }
        PacketHeader               h{};
        std::span<const std::byte> blocks;
        const PacketStatus         st = parse_packet(pkt, h, blocks);
        if (st == PacketStatus::TooShort) {
            ++counts_.malformed;
            return;
        }
        if (locked_ && h.session != session_) {
            ++counts_.wrong_session;
            return;
        }
        switch (st) {
            case PacketStatus::BadBlock:
                ++counts_.malformed;
                return;
            case PacketStatus::Heartbeat:
                if (h.seq == 0) {
                    ++counts_.malformed;
                    return;
                }
                lock(h.session);
                on_heartbeat(h.seq);
                return;
            case PacketStatus::EndOfSession:
                if (h.seq == 0 || (end_seen_ && h.seq != end_seq_) || h.seq < hi_) {
                    ++counts_.malformed;  // data past the end, or a second, different end
                    return;
                }
                lock(h.session);
                on_end(h.seq);
                return;
            case PacketStatus::Ok:
                break;
            case PacketStatus::TooShort:
                return;
        }
        if (!well_formed_data(h, blocks)) {
            ++counts_.malformed;
            return;
        }
        lock(h.session);
        on_data(pkt, h, blocks, retransmit);
    }

    void on_tick() {
        if (state_ == RxState::Stale || state_ == RxState::Finished) {
            return;
        }
        ++tick_;
        if (state_ != RxState::Recovering || tick_ - timer_start_ < cfg_.timeout_ticks) {
            return;
        }
        if (attempts_ >= cfg_.max_attempts) {
            go_stale(StaleReason::RetriesExhausted);
            return;
        }
        ++attempts_;
        timer_start_ = tick_;
        request_all_holes();
    }

    [[nodiscard]] RxState         state() const noexcept { return state_; }
    [[nodiscard]] const RxCounts& counts() const noexcept { return counts_; }
    [[nodiscard]] StaleReason     stale_reason() const noexcept { return stale_; }
    // Next sequence to be delivered; everything below it has been delivered exactly once.
    [[nodiscard]] std::uint64_t next_expected() const noexcept { return expected_; }
    [[nodiscard]] std::uint64_t highest_known() const noexcept { return hi_; }
    [[nodiscard]] std::size_t   buffered_packets() const noexcept { return buf_.size(); }

private:
    struct Held {
        std::vector<std::byte> bytes;  // the whole packet
        std::uint16_t          count;
        bool                   retx;
    };

    // True when every block has a nonzero length and seq + count fits in 64 bits.
    static bool well_formed_data(const PacketHeader&        h,
                                 std::span<const std::byte> blocks) noexcept {
        if (h.seq == 0 || h.seq > UINT64_MAX - h.count) {
            return false;
        }
        std::size_t off = 0;
        for (std::uint16_t i = 0; i < h.count; ++i) {
            const std::size_t len = itch::load_be16(blocks.data() + off);
            if (len == 0) {
                return false;
            }
            off += kBlockPrefix + len;
        }
        return true;
    }

    void lock(const Session& s) noexcept {
        if (!locked_) {
            session_ = s;
            locked_  = true;
        }
    }

    // Delivers the messages of a validated payload whose first message is `first`, skipping
    // those below expected_. Returns the number delivered.
    std::uint64_t deliver(std::span<const std::byte> blocks, std::uint64_t first,
                          std::uint16_t count, bool retransmit) {
        std::size_t   off       = 0;
        std::uint64_t delivered = 0;
        for (std::uint16_t i = 0; i < count; ++i) {
            const std::size_t len = itch::load_be16(blocks.data() + off);
            off += kBlockPrefix;
            const std::uint64_t seq = first + i;
            if (seq >= expected_) {
                sink_.on_message(seq, blocks.subspan(off, len));
                ++expected_;
                ++delivered;
                if (retransmit) {
                    ++counts_.messages_from_retransmit;
                }
            }
            off += len;
        }
        return delivered;
    }

    void request_range(std::uint64_t from, std::uint64_t to) {
        while (from < to) {
            const std::uint64_t n = std::min<std::uint64_t>(to - from, kMaxDataCount);
            req_.request(from, static_cast<std::uint16_t>(n));
            ++counts_.retransmit_requests;
            from += n;
        }
    }

    void request_all_holes() {
        std::uint64_t cur = expected_;
        for (const auto& [start, held] : buf_) {
            if (start > cur) {
                request_range(cur, start);
            }
            cur = std::max<std::uint64_t>(cur, start + held.count);
        }
        if (hi_ > cur) {
            request_range(cur, hi_);
        }
    }

    // Something at sequence `seq` > expected_ exists. A new hole [hi_, seq) is requested.
    // False when the reach is beyond max_gap_messages (the receiver is Stale then).
    bool note_ahead(std::uint64_t reach, std::uint64_t seq) {
        if (reach - expected_ > cfg_.max_gap_messages) {
            go_stale(StaleReason::GapTooLarge);
            return false;
        }
        if (seq > hi_) {
            const std::uint64_t len = seq - hi_;
            ++counts_.gaps_detected;
            counts_.max_gap_len = std::max(counts_.max_gap_len, len);
            if (hi_ == expected_) {  // Live -> Recovering
                attempts_    = 1;
                timer_start_ = tick_;
            }
            const std::uint64_t from = hi_;
            hi_                      = seq;  // before the request, so a Requester sees it
            request_range(from, seq);
        }
        return true;
    }

    void on_heartbeat(std::uint64_t seq) {
        if (seq <= expected_) {
            return;
        }
        if (note_ahead(seq, seq)) {
            settle();
        }
    }

    void on_end(std::uint64_t seq) {
        if (seq > expected_ && !note_ahead(seq, seq)) {
            return;
        }
        end_seen_ = true;
        end_seq_  = seq;
        settle();
    }

    void on_data(std::span<const std::byte> pkt, const PacketHeader& h,
                 std::span<const std::byte> blocks, bool retransmit) {
        const std::uint64_t end = h.seq + h.count;
        if (end <= expected_) {
            ++counts_.dup_packets;
            return;
        }
        if (end_seen_ && end > end_seq_) {
            ++counts_.malformed;  // data past the announced end of the session
            return;
        }
        const std::uint64_t before = expected_;
        if (h.seq <= expected_) {
            const bool late = hi_ > expected_;
            if (h.seq < expected_) {
                ++counts_.partial_overlaps;
            }
            const std::uint64_t n = deliver(blocks, h.seq, h.count, retransmit);
            if (late) {
                counts_.messages_delivered_late += n;
            }
            hi_ = std::max(hi_, end);
            drain();
        } else {
            if (!note_ahead(end, h.seq)) {
                return;
            }
            buffer(pkt, h, retransmit);
        }
        if (expected_ != before) {  // progress restarts the retry clock
            attempts_    = 1;
            timer_start_ = tick_;
        }
        settle();
    }

    void buffer(std::span<const std::byte> pkt, const PacketHeader& h, bool retransmit) {
        const auto it = buf_.find(h.seq);
        if (it != buf_.end()) {
            if (h.count <= it->second.count) {
                ++counts_.dup_packets;
                return;
            }
            buffered_msgs_ -= it->second.count;  // a longer packet from the same start wins
            it->second = Held{{pkt.begin(), pkt.end()}, h.count, retransmit};
        } else {
            if (buf_.size() >= cfg_.max_buffered_packets) {
                ++counts_.buffer_overflow_drops;
                return;  // hi_ stays at h.seq: the next packet or heartbeat re-reveals it
            }
            buf_.emplace(h.seq, Held{{pkt.begin(), pkt.end()}, h.count, retransmit});
        }
        buffered_msgs_ += h.count;
        counts_.messages_buffered_peak = std::max(counts_.messages_buffered_peak, buffered_msgs_);
        hi_                            = std::max(hi_, h.seq + h.count);
    }

    // Delivers buffered packets while the lowest one starts at or before expected_.
    void drain() {
        while (!buf_.empty() && buf_.begin()->first <= expected_) {
            const std::uint64_t start = buf_.begin()->first;
            Held                held  = std::move(buf_.begin()->second);
            buf_.erase(buf_.begin());
            buffered_msgs_ -= held.count;
            if (start + held.count <= expected_) {
                ++counts_.dup_packets;
                continue;
            }
            if (start < expected_) {
                ++counts_.partial_overlaps;
            }
            deliver(std::span<const std::byte>(held.bytes).subspan(kHeaderSize), start, held.count,
                    held.retx);
        }
    }

    void settle() {
        if (end_seen_ && expected_ == end_seq_) {
            state_ = RxState::Finished;
            buf_.clear();
            buffered_msgs_ = 0;
            return;
        }
        state_ = hi_ > expected_ ? RxState::Recovering : RxState::Live;
    }

    void go_stale(StaleReason why) {
        state_ = RxState::Stale;
        stale_ = why;
        buf_.clear();
        buffered_msgs_ = 0;
    }

    RecoveryConfig cfg_;
    Sink&          sink_;
    Requester&     req_;
    RxState        state_ = RxState::Live;
    StaleReason    stale_ = StaleReason::None;
    RxCounts       counts_;

    Session       session_{};
    bool          locked_      = false;
    std::uint64_t expected_    = 1;
    std::uint64_t hi_          = 1;
    bool          end_seen_    = false;
    std::uint64_t end_seq_     = 0;
    std::uint64_t tick_        = 0;
    std::uint64_t timer_start_ = 0;
    std::uint32_t attempts_    = 0;

    std::map<std::uint64_t, Held> buf_;
    std::uint64_t                 buffered_msgs_ = 0;
};

}  // namespace mold
