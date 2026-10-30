// tools/seed_corpus.cpp
//
// Writes seed inputs for fuzz_parser into a directory: one framed message per supported
// type, a mixed stream, and truncated and wrong-length variants. Built from the test
// builders, which are independent of the views.
//
//   itch_seed_corpus <dir>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <support/itch_builder.hpp>

namespace {
using namespace itch::test;

void write(const std::filesystem::path& dir, const std::string& name, const Bytes& b) {
    std::ofstream f(dir / name, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: itch_seed_corpus <dir>\n");
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
