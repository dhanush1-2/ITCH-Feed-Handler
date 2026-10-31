// tools/seed_corpus.cpp
//
// Writes seed inputs for fuzz_parser into a directory: one framed message per supported
// type, a mixed stream, and truncated and wrong-length variants. Built from the test
// builders, which are independent of the views.
//
//   itch_seed_corpus <dir>          seeds for fuzz_parser
//   itch_seed_corpus --mold <dir>   seeds for fuzz_mold (record format in fuzz/fuzz_mold.cpp)

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mold/packet.hpp>
#include <string>
#include <support/itch_builder.hpp>

namespace {
using namespace itch::test;

void write(const std::filesystem::path& dir, const std::string& name, const Bytes& b) {
    std::ofstream f(dir / name, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

// fuzz_mold records: 0 L <packet>, 1 tick, 2 sh sl n (data), 3 k sh sl (heartbeat/end).
Bytes raw_rec(const Bytes& packet) {
    Bytes r{std::byte{0}, static_cast<std::byte>(packet.size())};
    r.insert(r.end(), packet.begin(), packet.end());
    return r;
}

Bytes tick() {
    return Bytes{std::byte{1}};
}

Bytes data_rec(std::uint16_t seq, std::uint8_t n) {
    return Bytes{std::byte{2}, static_cast<std::byte>(seq >> 8), static_cast<std::byte>(seq & 0xFF),
                 static_cast<std::byte>(n - 1)};
}

Bytes control_rec(bool end, std::uint16_t seq) {
    return Bytes{std::byte{3}, static_cast<std::byte>(end ? 1 : 0),
                 static_cast<std::byte>(seq >> 8), static_cast<std::byte>(seq & 0xFF)};
}

// A real MoldUDP64 packet carrying the given ITCH messages starting at `seq`.
Bytes mold_packet(std::uint64_t seq, const std::vector<Bytes>& msgs, char session_tag = 'F') {
    constexpr mold::Session base{'F', 'U', 'Z', 'Z', 'S', 'E', 'S', 'S', '0', '1'};
    mold::Session           s = base;
    s[0]                      = session_tag;
    Bytes p(mold::kHeaderSize);
    mold::write_header(p.data(), s, seq, static_cast<std::uint16_t>(msgs.size()));
    for (const Bytes& m : msgs) {
        const Bytes f = frame(m);
        p.insert(p.end(), f.begin(), f.end());
    }
    return p;
}

Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) {
        append(out, p);
    }
    return out;
}

int write_mold_seeds(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    const Hdr   h{7, 1, 34200000000000ULL};
    const Bytes a = make_add(h, 1, 'B', 100, "AAPL", 1500000);
    const Bytes e = make_executed(h, 1, 50, 99);
    const Bytes d = make_delete(h, 1);

    write(dir, "in_order.bin",
          cat({raw_rec(mold_packet(1, {a, e})), raw_rec(mold_packet(3, {d})), data_rec(4, 2)}));
    write(dir, "gap_then_fill.bin",
          cat({raw_rec(mold_packet(1, {a})), raw_rec(mold_packet(4, {d})), tick(), tick(),
               raw_rec(mold_packet(2, {e, e}))}));
    write(dir, "duplicate_overlap.bin",
          cat({raw_rec(mold_packet(1, {a, e})), raw_rec(mold_packet(1, {a, e})),
               raw_rec(mold_packet(2, {e, d}))}));
    write(dir, "reorder_overflow.bin",
          cat({data_rec(1, 1), data_rec(3, 1), data_rec(5, 1), data_rec(7, 1), data_rec(9, 1),
               data_rec(11, 1), tick(), tick(), tick(), data_rec(2, 1), data_rec(4, 1)}));
    write(dir, "tail_loss_heartbeat.bin",
          cat({data_rec(1, 3), control_rec(false, 4), control_rec(false, 9), tick(), tick(),
               data_rec(4, 5), control_rec(true, 9)}));
    write(dir, "end_with_gap.bin",
          cat({data_rec(1, 2), control_rec(true, 8), data_rec(3, 5), tick()}));
    write(dir, "stale_after_retries.bin",
          cat({data_rec(1, 1), data_rec(40, 1), tick(), tick(), tick(), tick(), tick(), tick(),
               tick(), tick(), data_rec(2, 8)}));
    write(dir, "huge_gap_limit.bin", cat({data_rec(1, 1), control_rec(false, 60000)}));
    write(dir, "wrong_session.bin",
          cat({raw_rec(mold_packet(1, {a})), raw_rec(mold_packet(2, {e}, 'X')),
               raw_rec(mold_packet(2, {e}))}));

    Bytes zero_len = mold_packet(1, {a});
    zero_len.push_back(std::byte{0});  // an extra empty block, count still 1: bytes left over
    zero_len.push_back(std::byte{0});
    Bytes empty_block(mold::kHeaderSize);
    mold::write_header(empty_block.data(), {'F', 'U', 'Z', 'Z', 'S', 'E', 'S', 'S', '0', '1'}, 1,
                       2);
    append(empty_block,
           bytes_of({0x00, 0x01, 0x41, 0x00, 0x00}));  // a block, then a zero-length one
    Bytes overflow(mold::kHeaderSize);
    mold::write_header(overflow.data(), {'F', 'U', 'Z', 'Z', 'S', 'E', 'S', 'S', '0', '1'},
                       UINT64_MAX, 2);
    append(overflow, bytes_of({0x00, 0x01, 0x41, 0x00, 0x01, 0x42}));
    Bytes truncated = mold_packet(1, {a, e});
    truncated.resize(truncated.size() - 7);
    write(dir, "malformed.bin",
          cat({raw_rec(zero_len), raw_rec(empty_block), raw_rec(overflow), raw_rec(truncated),
               raw_rec(bytes_of({0x00, 0x01})), data_rec(1, 2)}));
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--mold") {
        return write_mold_seeds(argv[2]);
    }
    if (argc != 2) {
        std::fprintf(stderr, "usage: itch_seed_corpus <dir> | --mold <dir>\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);

    const Hdr                          h{7, 1, 34200000000000ULL};
    const std::array<std::uint8_t, 14> rest{};
    const Bytes                        msgs[] = {
        make_add(h, 1, 'B', 100, "AAPL", 1500000),
        make_add_mpid(h, 2, 'S', 200, "MSFT", 3000000, "NSDQ"),
        make_executed(h, 1, 50, 99),
        make_executed_price(h, 1, 50, 100, 'Y', 1500100),
        make_cancel(h, 1, 10),
        make_delete(h, 1),
        make_replace(h, 1, 3, 80, 1500200),
        make_trade(h, 0, 'B', 300, "TSLA", 2000000, 101),
        make_cross(h, 5000, "AAPL", 1500000, 102, 'O'),
        make_broken(h, 101),
        make_system_event(h, 'O'),
        make_trading_action(h, "AAPL", 'T', ' ', "    "),
        make_stock_directory(h, "AAPL", 'Q', 'N', 100, rest),
        make_raw('L', 26, h),
    };

    Bytes stream;
    int   i = 0;
    for (const Bytes& m : msgs) {
        write(dir, "msg_" + std::to_string(i++) + ".bin", frame(m));
        append(stream, frame(m));
    }
    write(dir, "stream_mixed.bin", stream);

    // Raw single messages, for the direct decode() path.
    write(dir, "raw_add.bin", msgs[0]);
    write(dir, "raw_dir.bin", msgs[12]);

    Bytes trunc_body = stream;
    trunc_body.resize(trunc_body.size() - 5);
    write(dir, "stream_truncated_body.bin", trunc_body);

    Bytes trunc_hdr = stream;
    trunc_hdr.push_back(std::byte{0x00});
    write(dir, "stream_truncated_header.bin", trunc_hdr);

    Bytes zero_len = frame(msgs[0]);
    append(zero_len, bytes_of({0x00, 0x00}));
    append(zero_len, frame(msgs[1]));
    write(dir, "stream_zero_length.bin", zero_len);

    // Wrong length: an 'A' declared and framed at 20 bytes, and an 'E' at 40.
    write(dir, "wrong_len_short.bin", frame(make_raw('A', 20, h)));
    write(dir, "wrong_len_long.bin", frame(make_raw('E', 40, h)));
    write(dir, "unknown_type.bin", frame(make_raw('Z', 30, h)));
    write(dir, "tiny_frames.bin", bytes_of({0x00, 0x01, 'A', 0x00, 0x02, 'S', 0x00}));
    return 0;
}
