// include/itch/framer.hpp
#pragma once

// Splits an ITCH 5.0 file into messages. The file format is a sequence of
// [u16 big-endian length][message of that many bytes]; the length does not count itself.
//
// Because every frame carries its own length, a message type this code has never seen
// can be skipped without losing sync.
//
// On any status other than Ok the offset does not move, so the caller can report
// exactly where the stream stopped making sense.

#include <cstddef>
#include <itch/wire.hpp>
#include <span>

namespace itch {

enum class FrameStatus { Ok, End, TruncatedHeader, TruncatedBody, ZeroLength };

class Framer {
public:
    explicit Framer(std::span<const std::byte> data) noexcept : data_(data) {}

    [[nodiscard]] FrameStatus next(std::span<const std::byte>& msg) noexcept {
        const std::size_t remaining = data_.size() - off_;
        if (remaining == 0) {
            return FrameStatus::End;
        }
        if (remaining < kPrefix) {
            return FrameStatus::TruncatedHeader;
        }
        const std::size_t len = load_be16(data_.data() + off_);
        if (len == 0) {
            return FrameStatus::ZeroLength;
        }
        if (remaining - kPrefix < len) {
            return FrameStatus::TruncatedBody;
        }
        msg = data_.subspan(off_ + kPrefix, len);
        off_ += kPrefix + len;
        return FrameStatus::Ok;
    }

    // Bytes consumed so far; the start of the next frame.
    [[nodiscard]] std::size_t offset() const noexcept { return off_; }

private:
    static constexpr std::size_t kPrefix = 2;
    std::span<const std::byte>   data_;
    std::size_t                  off_ = 0;
};

}  // namespace itch
