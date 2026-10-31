// include/mold/packetizer.hpp
#pragma once

// Turns the messages of an ITCH file into MoldUDP64 downstream packets.
//
// Greedy fill: each packet takes messages in order until the next one would push it past the
// MTU. Sequences start at 1 and run on contiguously across packets. The packet buffer is
// reused: the span returned by next() is valid until the following call.
//
// A message that cannot fit in an empty packet (20 + 2 + length > mtu) can never be sent, so
// next() throws std::length_error rather than skip it silently. A framing error in the source
// stops the stream; status() says why next() returned nullopt.

#include <cstddef>
#include <cstdint>
#include <itch/framer.hpp>
#include <mold/packet.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace mold {

class Packetizer {
public:
    Packetizer(const Session& session, itch::Framer framer, std::size_t mtu = 1400)
        : session_(session), framer_(framer), mtu_(mtu), buf_(mtu) {
        if (mtu < kHeaderSize + kBlockPrefix + 1) {
            throw std::invalid_argument("Packetizer: mtu " + std::to_string(mtu) +
                                        " cannot hold a header and one message");
        }
        msgs_.reserve(64);
    }

    // The next data packet, or nullopt when the source is exhausted or broken (see status()).
    [[nodiscard]] std::optional<std::span<const std::byte>> next() {
        msgs_.clear();
        std::size_t   used  = kHeaderSize;
        std::uint16_t count = 0;
        while (count < kMaxDataCount) {
            if (!pending_valid_) {
                status_ = framer_.next(pending_);
                if (status_ != itch::FrameStatus::Ok) {
                    break;
                }
                pending_valid_ = true;
            }
            const std::size_t need = kBlockPrefix + pending_.size();
            if (kHeaderSize + need > mtu_) {
                throw std::length_error("Packetizer: message of " +
                                        std::to_string(pending_.size()) +
                                        " bytes does not fit mtu " + std::to_string(mtu_));
            }
            if (used + need > mtu_) {
                break;
            }
            store_be16(buf_.data() + used, static_cast<std::uint16_t>(pending_.size()));
            std::memcpy(buf_.data() + used + kBlockPrefix, pending_.data(), pending_.size());
            used += need;
            msgs_.push_back(pending_);
            pending_valid_ = false;
            ++count;
        }
        if (count == 0) {
            return std::nullopt;
        }
        write_header(buf_.data(), session_, next_seq_, count);
        next_seq_ += count;
        return std::span<const std::byte>(buf_.data(), used);
    }

    // The source messages carried by the packet next() just returned, in order. They point
    // into the source buffer, so the caller can retain them without copying.
    [[nodiscard]] std::span<const std::span<const std::byte>> last_messages() const noexcept {
        return msgs_;
    }

    // Sequence number of the first message of the packet after the last one returned.
    [[nodiscard]] std::uint64_t next_seq() const noexcept { return next_seq_; }

    // Why the last attempt to read a message stopped: End for a clean finish.
    [[nodiscard]] itch::FrameStatus status() const noexcept { return status_; }

    // A heartbeat or end-of-session packet carrying next_seq(). Valid until the next call to
    // next(), heartbeat() or end_of_session().
    [[nodiscard]] std::span<const std::byte> heartbeat() noexcept { return control(0); }
    [[nodiscard]] std::span<const std::byte> end_of_session() noexcept {
        return control(kEndOfSession);
    }

private:
    std::span<const std::byte> control(std::uint16_t count) noexcept {
        write_header(buf_.data(), session_, next_seq_, count);
        return std::span<const std::byte>(buf_.data(), kHeaderSize);
    }

    Session                                 session_;
    itch::Framer                            framer_;
    std::size_t                             mtu_;
    std::vector<std::byte>                  buf_;
    std::vector<std::span<const std::byte>> msgs_;
    std::span<const std::byte>              pending_;
    bool                                    pending_valid_ = false;
    itch::FrameStatus                       status_        = itch::FrameStatus::Ok;
    std::uint64_t                           next_seq_      = 1;
};

}  // namespace mold
