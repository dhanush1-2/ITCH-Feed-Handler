// include/itch/provenance.hpp
#pragma once

// The stamp every results/ file carries: commit, data file, CPU, compiler, flags.
// Compiler, flags and the BUILD commit are baked in by CMake as compile definitions
// (ITCH_COMPILER, ITCH_BUILD_FLAGS, ITCH_GIT_COMMIT); the CPU, the current HEAD and the
// dirty state are queried at run time from ITCH_SOURCE_DIR and compared (classify_commit):
//   build == HEAD, tree clean     <hash>
//   build == HEAD, tree dirty     <hash>-dirty        (changes outside results/, data/)
//   build != HEAD                 <build>-stale-build [-dirty]
//   git unavailable at run time   <build>-unverified
// A binary built outside CMake reports "unknown" rather than inventing a value.

#include <cstdio>
#include <string>
#include <utility>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#ifndef ITCH_GIT_COMMIT
#define ITCH_GIT_COMMIT "unknown"
#endif
#ifndef ITCH_SOURCE_DIR
#define ITCH_SOURCE_DIR "."
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

// Pure classification of the commit string. `head` is empty when git could not be asked.
[[nodiscard]] inline std::string classify_commit(const std::string& build, const std::string& head,
                                                 bool dirty) {
    if (build == "unknown") {
        return "unknown";
    }
    if (head.empty()) {
        return build + "-unverified";
    }
    std::string out = build == head ? head : build + "-stale-build";
    if (dirty) {
        out += "-dirty";
    }
    return out;
}

namespace detail {

// Runs `cmd`, returns false if it cannot start or exits nonzero; stdout goes to `out`.
[[nodiscard]] inline bool run_capture(const std::string& cmd, std::string& out) {
    std::FILE* f = ::popen(cmd.c_str(), "r");
    if (f == nullptr) {
        return false;
    }
    char buf[256];
    out.clear();
    while (std::fgets(buf, sizeof buf, f) != nullptr) {
        out += buf;
    }
    return ::pclose(f) == 0;
}

[[nodiscard]] inline std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (const char c : s) {
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return out + "'";
}

}  // namespace detail

// HEAD and dirty state of the source tree, resolved now, not at configure time.
[[nodiscard]] inline std::string resolve_commit() {
    const std::string git = "git -C " + detail::shell_quote(ITCH_SOURCE_DIR) + " ";
    std::string       head;
    std::string       status;
    if (!detail::run_capture(git + "rev-parse HEAD 2>/dev/null", head)) {
        return classify_commit(ITCH_GIT_COMMIT, "", false);
    }
    while (!head.empty() && (head.back() == '\n' || head.back() == '\r')) {
        head.pop_back();
    }
    if (!detail::run_capture(
            git + "status --porcelain -- . ':(exclude)results' ':(exclude)data' 2>/dev/null",
            status)) {
        return classify_commit(ITCH_GIT_COMMIT, "", false);
    }
    return classify_commit(ITCH_GIT_COMMIT, head, !status.empty());
}

[[nodiscard]] inline Provenance make_provenance(const std::string& data_file) {
    return {resolve_commit(), data_file, cpu_model(), ITCH_COMPILER, ITCH_BUILD_FLAGS};
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
// `extra` is already-formatted members ("key": value, comma separated, no leading comma)
// appended after the stamp, e.g. the manifest of the data file.
[[nodiscard]] inline std::string provenance_json(const Provenance& p, int indent = 2,
                                                 const std::string& extra = {}) {
    const std::string in(static_cast<std::size_t>(indent), ' ');
    return "{\n" + in + "  \"commit\": \"" + json_escape(p.commit) + "\",\n" + in +
           "  \"data_file\": \"" + json_escape(p.data_file) + "\",\n" + in + "  \"cpu\": \"" +
           json_escape(p.cpu) + "\",\n" + in + "  \"compiler\": \"" + json_escape(p.compiler) +
           "\",\n" + in + "  \"flags\": \"" + json_escape(p.flags) + "\"" +
           (extra.empty() ? std::string() : ",\n" + in + "  " + extra) + "\n" + in + "}";
}

}  // namespace itch
