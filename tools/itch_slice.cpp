// tools/itch_slice.cpp
//
// Cuts a small, self-consistent piece out of a full ITCH 5.0 file for the CI fixture.
//
//   itch_slice <in> <out> --symbols A,B
//
// Keeps every 'S' (system event) message and every message whose stock_locate belongs to one of
// the symbols, byte for byte and in the original order, each with its 2-byte length prefix.
// Because an order's whole life (add, execute, cancel, replace, delete) carries its book's
// locate, the slice replays with no reference to an order that is not in it.
//
// Exit codes (shared by every tool): 0 success, 2 input error.

#include <algorithm>
#include <bitset>
#include <cstdio>
#include <exception>
#include <itch/framer.hpp>
#include <itch/mapped_file.hpp>
#include <itch/messages.hpp>
#include <itch/scan.hpp>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::string              in, out;
    std::vector<std::string> names;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--symbols" && i + 1 < argc) {
            std::stringstream ss(argv[++i]);
            std::string       item;
            while (std::getline(ss, item, ',')) {
                const std::string n = itch::normalize_symbol(item);
                if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end()) {
                    names.push_back(n);
                }
            }
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "itch_slice: unknown option '%s'\n", a.c_str());
            return 2;
        } else if (in.empty()) {
            in = a;
        } else if (out.empty()) {
            out = a;
        } else {
            std::fprintf(stderr, "itch_slice: unexpected argument '%s'\n", a.c_str());
            return 2;
        }
    }
    if (in.empty() || out.empty() || names.empty()) {
        std::fprintf(stderr, "usage: itch_slice <in> <out> --symbols A,B\n");
        return 2;
    }
    try {
        const auto mf  = itch::MappedFile::open(in);
        const auto res = itch::resolve_symbols(mf.bytes(), names);
        if (!res.unresolved.empty()) {
            std::fprintf(stderr, "itch_slice: unknown symbol '%s'\n", res.unresolved[0].c_str());
            return 2;
        }
        std::bitset<65536> keep;
        for (const auto& kv : res.locate) {
            keep.set(kv.second);
        }
        std::FILE* os = std::fopen(out.c_str(), "wb");
        if (os == nullptr) {
            std::fprintf(stderr, "itch_slice: cannot write '%s'\n", out.c_str());
            return 2;
        }
        const auto                 data = mf.bytes();
        itch::Framer               fr(data);
        std::span<const std::byte> msg;
        std::size_t                kept = 0, total = 0;
        while (fr.next(msg) == itch::FrameStatus::Ok) {
            ++total;
            bool take = false;
            if (static_cast<char>(msg[0]) == 'S') {
                take = true;
            } else if (msg.size() >= 11) {
                take = keep.test(itch::HeaderView(msg.data()).locate());
            }
            if (take) {
                // The frame is the 2 length bytes immediately before the message.
                const std::byte* p = msg.data() - 2;
                if (std::fwrite(p, 1, msg.size() + 2, os) != msg.size() + 2) {
                    std::fclose(os);
                    std::fprintf(stderr, "itch_slice: write failed for '%s'\n", out.c_str());
                    return 2;
                }
                ++kept;
            }
        }
        if (std::fclose(os) != 0) {
            std::fprintf(stderr, "itch_slice: write failed for '%s'\n", out.c_str());
            return 2;
        }
        std::printf("kept %zu of %zu messages for %zu symbols -> %s\n", kept, total,
                    res.locate.size(), out.c_str());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "itch_slice: %s\n", e.what());
        return 2;
    }
}
