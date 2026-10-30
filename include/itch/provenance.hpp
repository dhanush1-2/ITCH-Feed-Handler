// include/itch/provenance.hpp
#pragma once

// The stamp every results/ file carries: commit, data file, CPU, compiler, flags.
// Commit, compiler and flags are baked in by CMake as compile definitions at configure
// time (ITCH_GIT_COMMIT, ITCH_COMPILER, ITCH_BUILD_FLAGS); the CPU is queried at run time.
// A tree with uncommitted changes outside results/ gets a "-dirty" suffix on the commit.
// A binary built outside CMake reports "unknown" rather than inventing a value.

#include <cstdio>
#include <string>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#ifndef ITCH_GIT_COMMIT
#define ITCH_GIT_COMMIT "unknown"
#endif
#ifndef ITCH_COMPILER
#define ITCH_COMPILER "unknown"
#endif
#ifndef ITCH_BUILD_FLAGS
#define ITCH_BUILD_FLAGS "unknown"
#endif

namespace itch {

[[nodiscard]] inline std::string cpu_model() {
#if defined(__APPLE__)
    char        buf[256];
    std::size_t len = sizeof buf;
    if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0 && len > 1) {
        return std::string(buf, len - 1);
    }
#elif defined(__linux__)
    if (std::FILE* f = std::fopen("/proc/cpuinfo", "r")) {
        char        line[512];
        std::string out;
        while (std::fgets(line, sizeof line, f) != nullptr) {
            const std::string s = line;
            if (s.rfind("model name", 0) == 0) {
                const auto colon = s.find(':');
                if (colon != std::string::npos) {
                    out = s.substr(colon + 1);
                    while (!out.empty() && (out.front() == ' ' || out.front() == '\t')) {
                        out.erase(out.begin());
                    }
                    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
                        out.pop_back();
                    }
                }
                break;
            }
        }
        std::fclose(f);
        if (!out.empty()) {
            return out;
        }
    }
#endif
    return "unknown";
}

struct Provenance {
    std::string commit;
    std::string data_file;
    std::string cpu;
    std::string compiler;
    std::string flags;
};

[[nodiscard]] inline Provenance make_provenance(const std::string& data_file) {
    return {ITCH_GIT_COMMIT, data_file, cpu_model(), ITCH_COMPILER, ITCH_BUILD_FLAGS};
}

[[nodiscard]] inline std::string json_escape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

// The value of a "meta" key: an object, indented by `indent` spaces, no trailing newline.
[[nodiscard]] inline std::string provenance_json(const Provenance& p, int indent = 2) {
    const std::string in(static_cast<std::size_t>(indent), ' ');
    return "{\n" + in + "  \"commit\": \"" + json_escape(p.commit) + "\",\n" + in +
           "  \"data_file\": \"" + json_escape(p.data_file) + "\",\n" + in + "  \"cpu\": \"" +
           json_escape(p.cpu) + "\",\n" + in + "  \"compiler\": \"" + json_escape(p.compiler) +
           "\",\n" + in + "  \"flags\": \"" + json_escape(p.flags) + "\"\n" + in + "}";
}

}  // namespace itch
