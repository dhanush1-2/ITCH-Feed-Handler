// include/mold/lossy_channel.hpp
#pragma once

// A deterministic lossy network between the packetizer / retransmit server and a receiver.
//
// Everything random comes from ONE std::mt19937_64 seeded from the config, and every
// probability is derived from it with the same arithmetic on every platform (no
// std::*_distribution, whose output differs between standard libraries). The same seed and
// the same sends therefore always produce the same deliveries.
//
// Per packet sent:
//   downstream path:  a burst in progress drops it; otherwise with probability burst_start a
//                     burst of burst_len consecutive drops begins with this packet; otherwise
//                     it is dropped with probability `loss`.
//   retransmit path:  dropped with probability `retransmit_loss` (a lost re-request answer is
//                     what makes recovery hard). No bursts.
//   survivors:        duplicated with probability `dup` (both paths), then each copy enters
//                     the reorder pool.
//   reorder pool:     with reorder_window = W > 0 the pool holds up to W packets; each new
//                     arrival beyond that releases a uniformly random pool member. W = 0 is
//                     strict FIFO. finish() releases what is still held.
//
// The channel owns copies of everything in flight, so callers may reuse their buffers.
// `reordered` counts deliveries of a packet that was sent before one already delivered.
// After finish() and a full drain: sent - dropped + duplicated == delivered.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mold/packet.hpp>
#include <random>
#include <span>
#include <vector>

namespace mold {

struct ChannelConfig {
    std::uint64_t seed            = 1;
    double        loss            = 0.0;  // downstream drop probability per packet
    double        dup             = 0.0;  // duplicate probability per surviving packet
    std::uint32_t reorder_window  = 0;    // 0 = in order
    double        burst_start     = 0.0;  // downstream probability a burst of drops starts
    std::uint32_t burst_len       = 0;    // packets dropped per burst, including the first
    double        retransmit_loss = 0.0;  // drop probability on the retransmit path
};

struct ChannelCounts {
    std::uint64_t sent       = 0;
    std::uint64_t dropped    = 0;
    std::uint64_t duplicated = 0;
    std::uint64_t reordered  = 0;
    std::uint64_t delivered  = 0;
};

class LossyChannel final : public PacketSink {
public:
    explicit LossyChannel(const ChannelConfig& cfg) : cfg_(cfg), rng_(cfg.seed), retx_(*this) {}

    LossyChannel(const LossyChannel&)            = delete;
    LossyChannel& operator=(const LossyChannel&) = delete;

    // Downstream path.
    void send(std::span<const std::byte> packet) override { accept(packet, false); }

    // The retransmit path: give this to RetransmitServer::serve.
    [[nodiscard]] PacketSink& retransmit_path() noexcept { return retx_; }

    // Moves the next deliverable packet into `out`. False when nothing is ready yet.
    [[nodiscard]] bool poll(std::vector<std::byte>& out) {
        if (ready_.empty()) {
            return false;
        }
        Held& h = ready_.front();
        if (h.idx < max_delivered_) {
            ++counts_.reordered;
        } else {
            max_delivered_ = h.idx;
        }
        out.swap(h.bytes);
        ready_.pop_front();
        ++counts_.delivered;
        return true;
    }

    // No more sends: release everything still held in the reorder pool.
    void finish() {
        while (!pool_.empty()) {
            release_random();
        }
    }

    [[nodiscard]] const ChannelCounts& counts() const noexcept { return counts_; }

private:
    struct Held {
        std::uint64_t          idx;  // send order
        std::vector<std::byte> bytes;
    };

    class RetxPath final : public PacketSink {
    public:
        explicit RetxPath(LossyChannel& c) : c_(c) {}
        void send(std::span<const std::byte> p) override { c_.accept(p, true); }

    private:
        LossyChannel& c_;
    };

    // Uniform in [0, 1) from the top 53 bits of one draw.
    double u01() { return static_cast<double>(rng_() >> 11) * 0x1.0p-53; }
    bool   chance(double p) { return p > 0.0 && u01() < p; }

    bool drops(bool retransmit) {
        if (retransmit) {
            return chance(cfg_.retransmit_loss);
        }
        if (burst_left_ > 0) {
            --burst_left_;
            return true;
        }
        if (cfg_.burst_len > 0 && chance(cfg_.burst_start)) {
            burst_left_ = cfg_.burst_len - 1;
            return true;
        }
        return chance(cfg_.loss);
    }

    void accept(std::span<const std::byte> packet, bool retransmit) {
        const std::uint64_t idx = next_idx_++;
        ++counts_.sent;
        if (drops(retransmit)) {
            ++counts_.dropped;
            return;
        }
        enqueue(idx, packet);
        if (chance(cfg_.dup)) {
            ++counts_.duplicated;
            enqueue(idx, packet);
        }
    }

    void enqueue(std::uint64_t idx, std::span<const std::byte> packet) {
        Held h{idx, std::vector<std::byte>(packet.begin(), packet.end())};
        if (cfg_.reorder_window == 0) {
            ready_.push_back(std::move(h));
            return;
        }
        pool_.push_back(std::move(h));
        while (pool_.size() > cfg_.reorder_window) {
            release_random();
        }
    }

    void release_random() {
        const std::size_t i = static_cast<std::size_t>(rng_() % pool_.size());
        ready_.push_back(std::move(pool_[i]));
        pool_[i] = std::move(pool_.back());
        pool_.pop_back();
    }

    ChannelConfig     cfg_;
    std::mt19937_64   rng_;
    RetxPath          retx_;
    ChannelCounts     counts_;
    std::vector<Held> pool_;
    std::deque<Held>  ready_;
    std::uint64_t     next_idx_      = 0;
    std::uint64_t     max_delivered_ = 0;
    std::uint32_t     burst_left_    = 0;
};

}  // namespace mold
