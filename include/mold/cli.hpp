// include/mold/cli.hpp
#pragma once

// Small helpers shared by tools/mold_sim.cpp and tools/mold_udp_demo.cpp: option parsing,
// the data manifest and JSON strings, and the dump-file rule.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <itch/provenance.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>

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
