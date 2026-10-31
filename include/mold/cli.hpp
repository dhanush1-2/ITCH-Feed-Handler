// include/mold/cli.hpp
#pragma once

// Small helpers shared by tools/mold_sim.cpp and tools/mold_udp_demo.cpp: option parsing,
// the data manifest and JSON strings, and the dump-file rule.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <itch/provenance.hpp>
#include <mold/sim.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace mold::cli {

// Exit codes shared by every tool.
inline constexpr int kExitOk     = 0;
inline constexpr int kExitInput  = 2;
inline constexpr int kExitFailed = 3;

struct InputError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[nodiscard]] inline std::uint64_t parse_u64(const std::string& flag, const std::string& v) {
    if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos) {
        throw InputError(flag + " needs a non-negative integer, got '" + v + "'");
    }
    return std::strtoull(v.c_str(), nullptr, 10);
}

[[nodiscard]] inline double parse_prob(const std::string& flag, const std::string& v) {
    char*        end = nullptr;
    const double d   = std::strtod(v.c_str(), &end);
    if (v.empty() || end != v.c_str() + v.size() || !(d >= 0.0 && d <= 1.0)) {
        throw InputError(flag + " needs a probability in [0, 1], got '" + v + "'");
    }
    return d;
}

[[nodiscard]] inline std::uint32_t parse_u32(const std::string& flag, const std::string& v) {
    const std::uint64_t n = parse_u64(flag, v);
    if (n > UINT32_MAX) {
        throw InputError(flag + " is too large: " + v);
    }
    return static_cast<std::uint32_t>(n);
}

// The manifest fetch_itch.py wrote beside the data, verbatim, or "null".
[[nodiscard]] inline std::string manifest_json(const std::string& data_path) {
    std::ifstream in(data_path + ".manifest.json");
    if (!in) {
        return "null";
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\r')) {
        s.pop_back();
    }
    return !s.empty() && s.front() == '{' && s.back() == '}' ? s : "null";
}

[[nodiscard]] inline std::string json_str(const std::string& s) {
    return "\"" + itch::json_escape(s) + "\"";
}

[[nodiscard]] inline const char* tf(bool b) {
    return b ? "true" : "false";
}

// Options and checks both tools share: the input file, the state dump, the JSON output, the
// message cap and the direct-replay comparison.
struct CommonOptions {
    std::string   file;
    std::string   dump_path;
    std::string   json_path;
    std::uint64_t max_messages = 0;
    bool          check_direct = false;
};

// Handles the option `a` when no tool-specific option matched it: a common option, the
// positional file, or an unknown flag (which throws). `value()` returns the next argument.
template <class ValueFn>
inline void parse_common(const std::string& a, ValueFn&& value, CommonOptions& c,
                         mold::SimConfig& sim) {
    if (a == "--dump-state") {
        c.dump_path = value();
    } else if (a == "--dump-at-s") {
        sim.dump.at_s   = parse_u64(a, value());
        sim.dump.at_set = true;
    } else if (a == "--json") {
        c.json_path = value();
    } else if (a == "--max-messages") {
        c.max_messages = parse_u64(a, value());
    } else if (a == "--max-live-orders") {
        sim.max_live_orders = static_cast<std::size_t>(parse_u64(a, value()));
        if (sim.max_live_orders == 0) {
            throw InputError("--max-live-orders must be at least 1");
        }
    } else if (a == "--check-direct") {
        c.check_direct = true;
    } else if (!a.empty() && a[0] == '-') {
        throw InputError("unknown option '" + a + "'");
    } else if (c.file.empty()) {
        c.file = a;
    } else {
        throw InputError("unexpected argument '" + a + "'");
    }
}

// Cross-validation once every argument is read.
inline void finish_common(const CommonOptions& c, mold::SimConfig& sim) {
    if (c.file.empty()) {
        throw InputError("no input file given");
    }
    if (sim.dump.at_set && c.dump_path.empty()) {
        throw InputError("--dump-at-s needs --dump-state");
    }
    if (c.check_direct && c.dump_path.empty()) {
        throw InputError("--check-direct needs --dump-state");
    }
    sim.dump.enabled = !c.dump_path.empty();
}

struct Verdict {
    bool finished              = false;
    bool stream_matches_source = false;
    int  state_matches_direct  = -1;  // -1 not checked
    bool comparison_empty      = false;
    int  exit_code             = kExitOk;
};

// The run passed when it finished, delivered the file's stream, and (if checked) its state is
// the direct replay's. A comparison of two empty books proves nothing, so it fails.
[[nodiscard]] inline Verdict judge(bool finished, const mold::ReplayOutcome& out,
                                   const std::string& source_sha, std::uint64_t source_messages,
                                   const mold::ReplayOutcome* direct) {
    Verdict v;
    v.finished = finished;
    v.stream_matches_source =
        finished && out.stream_sha256 == source_sha && out.delivered == source_messages;
    if (direct != nullptr) {
        v.comparison_empty     = direct->dump.bytes == 0 || out.dump.bytes == 0;
        v.state_matches_direct = !v.comparison_empty && out.dump.taken && direct->dump.taken &&
                                         out.dump.sha256 == direct->dump.sha256 &&
                                         out.dump.data == direct->dump.data
                                     ? 1
                                     : 0;
    }
    v.exit_code =
        finished && v.stream_matches_source && v.state_matches_direct != 0 ? kExitOk : kExitFailed;
    return v;
}

// The reason line for a failed verdict (empty when it passed and the stream/state agree).
[[nodiscard]] inline std::string failure_after_finish(const Verdict& v) {
    if (!v.stream_matches_source) {
        return "delivered stream differs from the file's";
    }
    if (v.comparison_empty) {
        return "the comparison state is empty, which proves nothing; pick a --dump-at-s where "
               "the books hold orders";
    }
    if (v.state_matches_direct == 0) {
        return "state differs from the direct replay";
    }
    return {};
}

// Writes a state dump. A book from a run that did not pass must never pass for the real one:
// it goes to path.incomplete, and an earlier file at `path` (a previous good run) is removed.
// Returns the file written.
inline std::string store_dump(const std::string& path, const std::string& data, bool run_ok) {
    std::string file = path;
    if (!run_ok) {
        file += ".incomplete";
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    const std::filesystem::path dp(file);
    if (dp.has_parent_path()) {
        std::filesystem::create_directories(dp.parent_path());
    }
    std::ofstream os(dp, std::ios::binary);
    if (!os) {
        throw InputError("cannot write '" + file + "'");
    }
    os.write(data.data(), static_cast<std::streamsize>(data.size()));
    os.close();
    if (!os) {
        throw InputError("failed writing '" + file + "'");
    }
    return file;
}

}  // namespace mold::cli
