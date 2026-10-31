// include/mold/sim.hpp
#pragma once

// The loss-injection rig shared by tools/mold_sim.cpp and the tests: an ITCH file goes through
// the packetizer, a lossy channel and the Receiver into a BookBuilder, and the result can be
// held against the same file replayed directly.
//
//   run_sim(source, cfg)          the whole path, with a retransmit server retaining from
//                                 `source` (so `source` must stay mapped for the call)
//   direct_replay(source, ...)    the same sink fed in file order with no network at all
//   summarize_source(source)      the file's own stream hash, message count and framing status
//
// "Stream hash" is SHA-256 over the record [seq u64 BE][length u16 BE][message bytes] of every
// message in delivery order. Two runs delivered the same messages in the same order, once
// each, exactly when their stream hashes agree. "State" is book::write_state at a feed second
// (itch_replay's --dump-at-s semantics: just before the first message whose timestamp is at
// or after that second) or, without one, at the end. A full day ends with an empty book, so
// an end-of-day state proves nothing; callers pick a second and check the dump is non-empty.
//
// Time in the sim is one tick per source packet sent; after the last packet the end-of-session
// packet is re-sent every tick (it can be lost too) until the receiver finishes or goes Stale.

#include <book/builder.hpp>
#include <book/state_dump.hpp>
#include <cstddef>
#include <cstdint>
#include <itch/dispatch.hpp>
#include <itch/framer.hpp>
#include <itch/messages.hpp>
#include <itch/sha256.hpp>
#include <mold/lossy_channel.hpp>
#include <mold/packet.hpp>
#include <mold/packetizer.hpp>
#include <mold/receiver.hpp>
#include <mold/retransmit_server.hpp>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mold {

class StreamHash {
public:
    void add(std::uint64_t seq, std::span<const std::byte> msg) noexcept {
        std::uint8_t head[10];
        for (int i = 0; i < 8; ++i) {
            head[i] = static_cast<std::uint8_t>(seq >> (56 - 8 * i));
        }
        head[8] = static_cast<std::uint8_t>(msg.size() >> 8);
        head[9] = static_cast<std::uint8_t>(msg.size() & 0xFF);
        sha_.update(head, sizeof head);
        sha_.update(msg.data(), msg.size());
    }
    [[nodiscard]] std::string hex() noexcept { return sha_.hex(); }

private:
    itch::Sha256 sha_;
};

struct DumpSpec {
    bool          enabled = false;
    bool          at_set  = false;  // false: dump at the end of the run
    std::uint64_t at_s    = 0;
};

struct DumpResult {
    bool          taken      = false;
    bool          complete   = false;  // false: the run did not finish; this state is partial
    bool          at_end     = true;
    std::uint64_t at_second  = 0;
    std::uint64_t at_message = 0;  // messages applied before the dump
    std::uint64_t bytes      = 0;
    std::string   sha256;
    std::string   data;  // the state bytes (19 per resting order)
    [[nodiscard]] std::uint64_t orders() const noexcept { return bytes / 19; }
};

struct ReplayOutcome {
    std::uint64_t delivered    = 0;
    std::uint64_t handled      = 0;
    std::uint64_t skipped      = 0;
    std::uint64_t wrong_length = 0;
    std::uint64_t live_orders  = 0;  // resting at the end of the stream
    std::string   stream_sha256;
    DumpResult    dump;
};

// The Receiver's Sink: applies each message to a BookBuilder, hashes it, takes the dump.
class ReplaySink {
public:
    ReplaySink(std::size_t max_live_orders, const DumpSpec& spec)
        : builder_(max_live_orders, obs_), spec_(spec), at_ns_(spec.at_s * 1'000'000'000ULL) {}

    void on_message(std::uint64_t seq, std::span<const std::byte> msg) {
        if (spec_.enabled && spec_.at_set && !dump_.taken && msg.size() >= 11 &&
            itch::HeaderView(msg.data()).timestamp_ns() >= at_ns_) {
            take_dump(false, spec_.at_s);
        }
        hash_.add(seq, msg);
        ++out_.delivered;
        switch (itch::decode(msg, builder_)) {
            case itch::DecodeStatus::Handled:
                ++out_.handled;
                break;
            case itch::DecodeStatus::Skipped:
                ++out_.skipped;
                break;
            case itch::DecodeStatus::WrongLength:
                ++out_.wrong_length;
                break;
        }
    }

    // Ends the run. `complete` is whether the whole stream was delivered. A dump asked for at
    // a second the stream never reached is an error for a complete run; an incomplete run
    // takes it where it stopped and marks it partial.
    ReplayOutcome finish(bool complete) {
        if (spec_.enabled && !dump_.taken) {
            if (spec_.at_set && complete) {
                throw std::runtime_error("the stream never reaches feed second " +
                                         std::to_string(spec_.at_s) +
                                         ", so there is no state to dump there");
            }
            take_dump(!spec_.at_set, 0);
        }
        dump_.complete     = complete;
        out_.live_orders   = builder_.live_orders();
        out_.stream_sha256 = hash_.hex();
        out_.dump          = std::move(dump_);
        return std::move(out_);
    }

private:
    void take_dump(bool at_end, std::uint64_t second) {
        std::ostringstream os(std::ios::binary);
        book::write_state(os, builder_);
        dump_.data       = os.str();
        dump_.bytes      = dump_.data.size();
        dump_.at_end     = at_end;
        dump_.at_second  = second;
        dump_.at_message = out_.delivered;
        itch::Sha256 sha;
        sha.update(dump_.data.data(), dump_.data.size());
        dump_.sha256 = sha.hex();
        dump_.taken  = true;
    }

    book::NullObserver                    obs_;
    book::BookBuilder<book::NullObserver> builder_;
    DumpSpec                              spec_;
    std::uint64_t                         at_ns_;
    StreamHash                            hash_;
    ReplayOutcome                         out_;
    DumpResult                            dump_;
};

// The file straight into the book: what the network path has to reproduce.
[[nodiscard]] inline ReplayOutcome direct_replay(std::span<const std::byte> source,
                                                 std::size_t                max_live_orders,
                                                 const DumpSpec&            spec) {
    ReplaySink                 sink(max_live_orders, spec);
    itch::Framer               framer(source);
    std::span<const std::byte> msg;
    std::uint64_t              seq = 1;
    itch::FrameStatus          st;
    while ((st = framer.next(msg)) == itch::FrameStatus::Ok) {
        sink.on_message(seq++, msg);
    }
    if (st != itch::FrameStatus::End) {
        throw std::runtime_error("framing error in the source at offset " +
                                 std::to_string(framer.offset()));
    }
    return sink.finish(true);
}

// What the file itself says: the stream hash and message count of its frames, and why the walk
// stopped (End for a clean file).
struct SourceSummary {
    std::string       sha256;
    std::uint64_t     messages = 0;
    itch::FrameStatus status   = itch::FrameStatus::End;
    std::size_t       offset   = 0;  // where the walk stopped
};

[[nodiscard]] inline SourceSummary summarize_source(std::span<const std::byte> source) {
    StreamHash                 h;
    SourceSummary              out;
    itch::Framer               framer(source);
    std::span<const std::byte> msg;
    while ((out.status = framer.next(msg)) == itch::FrameStatus::Ok) {
        h.add(++out.messages, msg);
    }
    out.offset = framer.offset();
    out.sha256 = h.hex();
    return out;
}

// The byte length of the first `max_messages` frames (all of them if the file has fewer).
[[nodiscard]] inline std::size_t prefix_bytes(std::span<const std::byte> source,
                                              std::uint64_t              max_messages) {
    itch::Framer               framer(source);
    std::span<const std::byte> msg;
    std::uint64_t              n = 0;
    while (n < max_messages && framer.next(msg) == itch::FrameStatus::Ok) {
        ++n;
    }
    return framer.offset();
}

struct SimConfig {
    ChannelConfig  channel;
    RecoveryConfig rec;
    std::size_t    retention       = 1U << 20;  // messages the retransmit server remembers
    std::uint32_t  heartbeat_every = 32;        // source packets between heartbeats; 0 = none
    std::size_t    mtu             = 1400;
    Session        session         = {'M', 'O', 'L', 'D', 'S', 'I', 'M', '0', '0', '1'};
    std::size_t    max_live_orders = 16'000'000;
    DumpSpec       dump;
    std::uint64_t  max_end_ticks = 100'000;  // bound on re-sending end of session
};

struct SimResult {
    RxState       state = RxState::Live;
    StaleReason   stale = StaleReason::None;
    RxCounts      rx;
    ChannelCounts channel;
    std::uint64_t source_messages = 0;
    std::uint64_t source_packets  = 0;
    std::uint64_t end_ticks       = 0;
    std::uint64_t retained_ring   = 0;
    ReplayOutcome out;

    [[nodiscard]] bool finished() const noexcept { return state == RxState::Finished; }
};

namespace detail {

// Answers a re-request from the retransmit server over the lossy retransmit path.
struct ServerRequester {
    RetransmitServer* server;
    LossyChannel*     channel;
    void              request(std::uint64_t seq, std::uint16_t count) {
        (void)server->serve(seq, count, channel->retransmit_path());
    }
};

}  // namespace detail

[[nodiscard]] inline SimResult run_sim(std::span<const std::byte> source, const SimConfig& cfg) {
    Packetizer              pk(cfg.session, itch::Framer(source), cfg.mtu);
    RetransmitServer        server(cfg.session, cfg.retention, cfg.mtu);
    LossyChannel            channel(cfg.channel);
    ReplaySink              sink(cfg.max_live_orders, cfg.dump);
    detail::ServerRequester requester{&server, &channel};
    Receiver                rx(cfg.rec, sink, requester);

    std::vector<std::byte> buf;
    const auto             drain = [&] {
        bool retx = false;
        while (channel.poll(buf, &retx)) {
            rx.on_packet(buf, retx);
        }
    };

    SimResult res;
    while (const auto pkt = pk.next()) {
        const auto          msgs = pk.last_messages();
        const std::uint64_t seq0 = pk.next_seq() - msgs.size();
        for (std::size_t i = 0; i < msgs.size(); ++i) {
            server.retain(seq0 + i, msgs[i]);
        }
        channel.send(*pkt);
        ++res.source_packets;
        if (cfg.heartbeat_every != 0 && res.source_packets % cfg.heartbeat_every == 0) {
            channel.send(pk.heartbeat());
        }
        drain();
        rx.on_tick();
        if (rx.state() == RxState::Stale) {
            break;
        }
    }
    if (rx.state() != RxState::Stale && pk.status() != itch::FrameStatus::End) {
        throw std::runtime_error("framing error in the source: status " +
                                 std::to_string(static_cast<int>(pk.status())));
    }
    res.source_messages = pk.next_seq() - 1;

    if (rx.state() != RxState::Stale) {
        while (rx.state() != RxState::Finished && rx.state() != RxState::Stale &&
               res.end_ticks < cfg.max_end_ticks) {
            channel.send(pk.end_of_session());
            channel.finish();
            drain();
            rx.on_tick();
            ++res.end_ticks;
        }
    }

    // Nothing may stay in the channel: what is left is released and handed to the receiver
    // (which ignores it once Finished or Stale), so the channel counts balance exactly.
    channel.finish();
    drain();

    res.state         = rx.state();
    res.stale         = rx.stale_reason();
    res.rx            = rx.counts();
    res.channel       = channel.counts();
    res.retained_ring = cfg.retention;
    res.out           = sink.finish(res.state == RxState::Finished);
    return res;
}

}  // namespace mold
