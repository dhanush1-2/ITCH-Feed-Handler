// include/mold/retransmit_server.hpp
#pragma once

// A re-request server over a bounded window of recent messages.
//
// retain(seq, msg) records where a message lives (a pointer into the source buffer, which
// must outlive the server) in a ring of `retained_messages` slots keyed by seq % capacity.
// Each slot stores its own sequence number, so a slot overwritten by a newer message reads
// as expired. Nothing is copied until a request is served.
//
// serve(seq, count, sink) answers a request packet: downstream packets, each limited to the
// MTU, holding the requested messages in order. Per the spec only messages that completely
// fit a packet go in it, and the rest of the range follows in further packets. It stops at
// the first message that is expired or was never retained, so a range whose first message has
// expired sends nothing. The return value is the number of messages sent.

#include <cstddef>
#include <cstdint>
#include <mold/packet.hpp>
#include <span>
#include <stdexcept>
#include <vector>

namespace mold {

class RetransmitServer {
public:
    RetransmitServer(const Session& session, std::size_t retained_messages, std::size_t mtu = 1400)
        : session_(session), mtu_(mtu), slots_(retained_messages), buf_(mtu) {
        if (retained_messages == 0) {
            throw std::invalid_argument("RetransmitServer: retained_messages must be > 0");
        }
        if (mtu < kHeaderSize + kBlockPrefix + 1) {
            throw std::invalid_argument("RetransmitServer: mtu cannot hold a header and a message");
        }
    }

    void retain(std::uint64_t seq, std::span<const std::byte> msg) noexcept {
        slots_[seq % slots_.size()] = Slot{seq, msg.data(), static_cast<std::uint16_t>(msg.size())};
    }

    std::size_t serve(std::uint64_t seq, std::uint16_t count, PacketSink& sink) {
        std::size_t   sent = 0;
        std::uint64_t cur  = seq;
        std::uint32_t left = count;
        while (left > 0) {
            std::size_t   used = kHeaderSize;
            std::uint16_t n    = 0;
            while (left > 0 && n < kMaxDataCount) {
                const Slot& s = slots_[cur % slots_.size()];
                if (s.seq != cur || s.data == nullptr) {
                    break;  // expired, or never retained
                }
                const std::size_t need = kBlockPrefix + s.len;
                if (used + need > mtu_) {
                    break;
                }
                store_be16(buf_.data() + used, s.len);
                std::memcpy(buf_.data() + used + kBlockPrefix, s.data, s.len);
                used += need;
                ++cur;
                --left;
                ++n;
            }
            if (n == 0) {
                break;
            }
            write_header(buf_.data(), session_, cur - n, n);
            sink.send(std::span<const std::byte>(buf_.data(), used));
            sent += n;
        }
        return sent;
    }

private:
    struct Slot {
        std::uint64_t    seq  = 0;  // sequences start at 1, so 0 means empty
        const std::byte* data = nullptr;
        std::uint16_t    len  = 0;
    };

    Session                session_;
    std::size_t            mtu_;
    std::vector<Slot>      slots_;
    std::vector<std::byte> buf_;
};

}  // namespace mold
