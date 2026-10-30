// tests/support/verify_rig.hpp
#pragma once

// A BookBuilder wired to a VerifyObserver, fed through itch::decode.

#include <book/invariants.hpp>
#include <cstdint>
#include <gtest/gtest.h>
#include <itch/dispatch.hpp>
#include "support/itch_builder.hpp"

namespace itch::test {

struct VerifyRig {
    book::VerifyObserver                       obs;
    book::BookBuilder<book::VerifyObserver>    b;

    explicit VerifyRig(std::size_t cap = 1024, book::VerifyObserver::Options o = {})
        : obs(o), b(cap, obs) {
        obs.bind(b);
    }

    void feed(const Bytes& m) {
        ASSERT_EQ(itch::decode(std::span<const std::byte>(m), b), itch::DecodeStatus::Handled);
    }
    void add(std::uint16_t loc, std::uint64_t ref, char side, std::uint32_t sh, std::uint32_t px,
             std::uint64_t ts = 0) {
        feed(make_add(Hdr{loc, 0, ts}, ref, side, sh, "TEST", px));
    }
    // Directory entry for a non-Nasdaq-listed symbol plus the start of market hours.
    void open_market(std::uint16_t loc, char category = 'N') {
        feed(make_stock_directory(Hdr{loc}, "TEST", category, ' ', 100, {}));
        feed(make_system_event(Hdr{0}, 'Q'));
    }
};

}  // namespace itch::test
