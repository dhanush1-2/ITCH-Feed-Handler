// tools/itch_stat.cpp
//
// Census of one decompressed ITCH 5.0 file: message counts by type, unique stock locates,
// first and last timestamp, the last system event, and how the framer ended.
//
//   itch_stat <file> [--json out]
//
// Exit codes (shared by every tool): 0 success, 2 input error, 3 verification failure.
// The census is a verification: a full-day file must end with FrameStatus::End, no
// wrong-length frame, and a last system event of 'C' (end of messages). Anything else
// exits 3, and the JSON is still written so the failure can be inspected.

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <itch/dispatch.hpp>
#include <itch/framer.hpp>
#include <itch/mapped_file.hpp>
#include <itch/messages.hpp>
#include <itch/provenance.hpp>
#include <string>
#include <type_traits>

namespace {

constexpr int kExitOk     = 0;
constexpr int kExitInput  = 2;
constexpr int kExitVerify = 3;

const char* status_name(itch::FrameStatus s) {
    switch (s) {
        case itch::FrameStatus::Ok:
            return "Ok";
        case itch::FrameStatus::End:
            return "End";
        case itch::FrameStatus::TruncatedHeader:
            return "TruncatedHeader";
        case itch::FrameStatus::TruncatedBody:
            return "TruncatedBody";
        case itch::FrameStatus::ZeroLength:
            return "ZeroLength";
    }
    return "Unknown";
}

struct Census {
    std::array<std::uint64_t, 256> by_type{};
    std::bitset<65536>             locates;
    std::uint64_t                  messages      = 0;
    std::uint64_t                  handled       = 0;
    std::uint64_t                  skipped       = 0;
    std::uint64_t                  wrong_length  = 0;
    std::uint64_t                  short_frames  = 0;  // under the 11-byte common header
    std::uint64_t                  first_ts      = 0;
    std::uint64_t                  last_ts       = 0;
    bool                           have_ts       = false;
    char                           last_event    = 0;  // 0 = none seen
    std::uint64_t                  system_events = 0;

    // Only called by decode() for a correctly sized, in-scope message.
    template <class V>
    void on(const V& v) {
        if constexpr (std::is_same_v<V, itch::SystemEventView>) {
            last_event = v.event_code();
            ++system_events;
        }
    }
};

std::string json_char(char c) {
    if (c == 0) {
        return "null";
    }
    if (c == '"' || c == '\\') {
        return std::string("\"\\") + c + "\"";
    }
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) {
        return "\"?\"";
    }
    return std::string("\"") + c + "\"";
}

std::string json_str(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

// "12302019.NASDAQ_ITCH50" -> "12302019"
std::string day_of(const std::filesystem::path& p) {
    const std::string name = p.filename().string();
    return name.substr(0, name.find('.'));
}

void usage() {
    std::fprintf(stderr, "usage: itch_stat <file> [--json out]\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string file;
    std::string json_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--json") {
            if (i + 1 >= argc) {
                usage();
                return kExitInput;
            }
            json_path = argv[++i];
        } else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "itch_stat: unknown option '%s'\n", a.c_str());
            usage();
            return kExitInput;
        } else if (file.empty()) {
            file = a;
        } else {
            usage();
            return kExitInput;
        }
    }
    if (file.empty()) {
        usage();
        return kExitInput;
    }
    if (json_path.empty()) {
        json_path = "results/census_" + day_of(file) + ".json";
    }

    try {
        const auto mf = itch::MappedFile::open(file);
        mf.prefault();

        Census                     c;
        itch::Framer               framer(mf.bytes());
        itch::FrameStatus          status;
        std::span<const std::byte> msg;
        while ((status = framer.next(msg)) == itch::FrameStatus::Ok) {
            ++c.messages;
            ++c.by_type[std::to_integer<unsigned char>(msg[0])];

            if (msg.size() >= 11) {
                const itch::HeaderView h(msg.data());
                c.locates.set(h.locate());
                const std::uint64_t ts = h.timestamp_ns();
                if (!c.have_ts) {
                    c.first_ts = ts;
                    c.have_ts  = true;
                }
                c.last_ts = ts;
            } else {
                ++c.short_frames;
            }

            switch (itch::decode(msg, c)) {
                case itch::DecodeStatus::Handled:
                    ++c.handled;
                    break;
                case itch::DecodeStatus::Skipped:
                    ++c.skipped;
                    break;
                case itch::DecodeStatus::WrongLength:
                    ++c.wrong_length;
                    break;
            }
        }

        const bool ok =
            status == itch::FrameStatus::End && c.wrong_length == 0 && c.last_event == 'C';

        std::filesystem::path out(json_path);
        if (out.has_parent_path()) {
            std::filesystem::create_directories(out.parent_path());
        }
        std::ofstream js(out);
        if (!js) {
            std::fprintf(stderr, "itch_stat: cannot write '%s'\n", json_path.c_str());
            return kExitInput;
        }
        js << "{\n";
        js << "  \"meta\": " << itch::provenance_json(itch::make_provenance(file), 2) << ",\n";
        js << "  \"file\": " << json_str(std::filesystem::path(file).filename().string()) << ",\n";
        js << "  \"file_bytes\": " << mf.bytes().size() << ",\n";
        js << "  \"frame_status\": \"" << status_name(status) << "\",\n";
        js << "  \"stopped_at_offset\": " << framer.offset() << ",\n";
        js << "  \"messages\": " << c.messages << ",\n";
        js << "  \"handled\": " << c.handled << ",\n";
        js << "  \"skipped\": " << c.skipped << ",\n";
        js << "  \"wrong_length\": " << c.wrong_length << ",\n";
        js << "  \"short_frames\": " << c.short_frames << ",\n";
        js << "  \"unique_locates\": " << c.locates.count() << ",\n";
        js << "  \"first_timestamp_ns\": " << c.first_ts << ",\n";
        js << "  \"last_timestamp_ns\": " << c.last_ts << ",\n";
        js << "  \"system_events\": " << c.system_events << ",\n";
        js << "  \"last_system_event\": " << json_char(c.last_event) << ",\n";
        js << "  \"verified\": " << (ok ? "true" : "false") << ",\n";
        js << "  \"counts_by_type\": {";
        bool first = true;
        for (std::size_t t = 0; t < c.by_type.size(); ++t) {
            if (c.by_type[t] == 0) {
                continue;
            }
            js << (first ? "\n    " : ",\n    ") << json_char(static_cast<char>(t)) << ": "
               << c.by_type[t];
            first = false;
        }
        js << "\n  }\n}\n";
        js.close();

        std::printf(
            "%s: %llu messages, frame status %s at offset %zu, %zu locates, wrong_length "
            "%llu, last system event %c -> %s\n",
            file.c_str(), static_cast<unsigned long long>(c.messages), status_name(status),
            framer.offset(), c.locates.count(), static_cast<unsigned long long>(c.wrong_length),
            c.last_event == 0 ? '-' : c.last_event, ok ? "OK" : "VERIFICATION FAILED");
        std::printf("census written to %s\n", json_path.c_str());
        return ok ? kExitOk : kExitVerify;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "itch_stat: %s\n", e.what());
        return kExitInput;
    }
}
