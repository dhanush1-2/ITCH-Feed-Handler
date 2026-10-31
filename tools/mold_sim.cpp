// tools/mold_sim.cpp
//
// Sends an ITCH file through a simulated lossy MoldUDP64 feed (packetizer, loss, duplication,
// reordering, bursts, a lossy retransmit path, a bounded retransmit server) into the sequence
// checker and a book builder, and reports whether the books survived.
//
//   mold_sim <file> [--loss p] [--dup p] [--reorder n] [--burst p,len] [--retransmit-loss p]
//            [--seed s] [--retention n] [--dump-state path] [--dump-at-s S] [--json out]
//            [--max-messages N] [--timeout-ticks n] [--max-attempts n] [--buffer-packets n]
//            [--max-gap n] [--heartbeat-every n] [--max-live-orders n] [--check-direct]
//
//   --loss               downstream drop probability per packet (default 0)
//   --dup                duplication probability per surviving packet
//   --reorder n          reorder window in packets (0 = in order)
//   --burst p,len        probability a burst of `len` consecutive drops starts at a packet
//   --retransmit-loss    drop probability on the retransmit path
//   --seed               the one seed every random draw comes from (default 1)
//   --retention n        messages the retransmit server remembers (default 1,048,576)
//   --dump-state path    write every resting order (state_dump.hpp) at the end of the run, or
//                        with --dump-at-s S just before the first message at or after feed
//                        second S (itch_replay's semantics). A full day ends with an empty
//                        book, so only the --dump-at-s form compares anything on one. If the
//                        run does not finish the file is path + ".incomplete", never path
//   --max-messages N     use only the first N messages of the file
//   --check-direct       also replay the file straight into a second book builder and compare
//                        the state dumps (needs --dump-state)
//
// Every run also compares the SHA-256 of the delivered (seq, bytes) stream with the same hash
// computed from the file itself (see include/mold/sim.hpp).
//
// Exit codes (shared by every tool): 0 success, 2 input error, 3 the run failed: the receiver
// went Stale or never finished, the delivered stream differs from the file's, or the state
// differs from the direct replay.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <itch/mapped_file.hpp>
#include <itch/provenance.hpp>
#include <memory>
#include <mold/sim.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kExitOk     = 0;
constexpr int kExitInput  = 2;
constexpr int kExitFailed = 3;

struct InputError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Options {
    std::string     file;
    mold::SimConfig sim;
    std::string     dump_path;
    std::string     json_path;
    std::uint64_t   max_messages = 0;
    bool            check_direct = false;
};

void usage() {
    std::fprintf(stderr,
                 "usage: mold_sim <file> [--loss p] [--dup p] [--reorder n] [--burst p,len]\n"
                 "                [--retransmit-loss p] [--seed s] [--retention n]\n"
                 "                [--dump-state path] [--dump-at-s S] [--json out]\n"
                 "                [--max-messages N] [--timeout-ticks n] [--max-attempts n]\n"
                 "                [--buffer-packets n] [--max-gap n] [--heartbeat-every n]\n"
                 "                [--max-live-orders n] [--check-direct]\n");
}

std::uint64_t parse_u64(const std::string& flag, const std::string& v) {
    if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos) {
        throw InputError(flag + " needs a non-negative integer, got '" + v + "'");
    }
    return std::strtoull(v.c_str(), nullptr, 10);
}

double parse_prob(const std::string& flag, const std::string& v) {
    char*        end = nullptr;
    const double d   = std::strtod(v.c_str(), &end);
    if (v.empty() || end != v.c_str() + v.size() || !(d >= 0.0 && d <= 1.0)) {
        throw InputError(flag + " needs a probability in [0, 1], got '" + v + "'");
    }
    return d;
}

std::uint32_t parse_u32(const std::string& flag, const std::string& v) {
    const std::uint64_t n = parse_u64(flag, v);
    if (n > UINT32_MAX) {
        throw InputError(flag + " is too large: " + v);
    }
    return static_cast<std::uint32_t>(n);
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a     = argv[i];
        auto              value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw InputError(a + " needs a value");
            }
            return argv[++i];
        };
        auto& ch  = o.sim.channel;
        auto& rec = o.sim.rec;
        if (a == "--loss") {
            ch.loss = parse_prob(a, value());
        } else if (a == "--dup") {
            ch.dup = parse_prob(a, value());
        } else if (a == "--reorder") {
            ch.reorder_window = parse_u32(a, value());
        } else if (a == "--burst") {
            const std::string v     = value();
            const auto        comma = v.find(',');
            if (comma == std::string::npos) {
                throw InputError("--burst needs p,len, got '" + v + "'");
            }
            ch.burst_start = parse_prob(a, v.substr(0, comma));
            ch.burst_len   = parse_u32(a, v.substr(comma + 1));
        } else if (a == "--retransmit-loss") {
            ch.retransmit_loss = parse_prob(a, value());
        } else if (a == "--seed") {
            ch.seed = parse_u64(a, value());
        } else if (a == "--retention") {
            o.sim.retention = static_cast<std::size_t>(parse_u64(a, value()));
            if (o.sim.retention == 0) {
                throw InputError("--retention must be at least 1");
            }
        } else if (a == "--dump-state") {
            o.dump_path = value();
        } else if (a == "--dump-at-s") {
            o.sim.dump.at_s   = parse_u64(a, value());
            o.sim.dump.at_set = true;
        } else if (a == "--json") {
            o.json_path = value();
        } else if (a == "--max-messages") {
            o.max_messages = parse_u64(a, value());
        } else if (a == "--timeout-ticks") {
            rec.timeout_ticks = parse_u32(a, value());
            if (rec.timeout_ticks == 0) {
                throw InputError("--timeout-ticks must be at least 1");
            }
        } else if (a == "--max-attempts") {
            rec.max_attempts = parse_u32(a, value());
            if (rec.max_attempts == 0) {
                throw InputError("--max-attempts must be at least 1");
            }
        } else if (a == "--buffer-packets") {
            rec.max_buffered_packets = static_cast<std::size_t>(parse_u64(a, value()));
        } else if (a == "--max-gap") {
            rec.max_gap_messages = parse_u64(a, value());
            if (rec.max_gap_messages == 0) {
                throw InputError("--max-gap must be at least 1");
            }
        } else if (a == "--heartbeat-every") {
            o.sim.heartbeat_every = parse_u32(a, value());
        } else if (a == "--max-live-orders") {
            o.sim.max_live_orders = static_cast<std::size_t>(parse_u64(a, value()));
            if (o.sim.max_live_orders == 0) {
                throw InputError("--max-live-orders must be at least 1");
            }
        } else if (a == "--check-direct") {
            o.check_direct = true;
        } else if (!a.empty() && a[0] == '-') {
            throw InputError("unknown option '" + a + "'");
        } else if (o.file.empty()) {
            o.file = a;
        } else {
            throw InputError("unexpected argument '" + a + "'");
        }
    }
    if (o.file.empty()) {
        throw InputError("no input file given");
    }
    if (o.sim.dump.at_set && o.dump_path.empty()) {
        throw InputError("--dump-at-s needs --dump-state");
    }
    if (o.check_direct && o.dump_path.empty()) {
        throw InputError("--check-direct needs --dump-state");
    }
    o.sim.dump.enabled = !o.dump_path.empty();
    return o;
}

// The manifest fetch_itch.py wrote beside the data, verbatim, or "null".
std::string manifest_json(const std::string& data_path) {
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

std::string json_str(const std::string& s) {
    return "\"" + itch::json_escape(s) + "\"";
}

const char* tf(bool b) {
    return b ? "true" : "false";
}

struct Verdict {
    bool finished              = false;
    bool stream_matches_source = false;
    int  state_matches_direct  = -1;  // -1 not checked
    bool direct_nonempty       = false;
    int  exit_code             = kExitOk;
};

void write_json(const Options& o, const itch::MappedFile& mf, std::size_t used_bytes,
                std::uint64_t source_messages, const std::string& source_sha,
                const mold::SimResult& r, const mold::ReplayOutcome* direct, const Verdict& v,
                const std::string& dump_file, double seconds) {
    std::filesystem::path out(o.json_path);
    if (out.has_parent_path()) {
        std::filesystem::create_directories(out.parent_path());
    }
    std::ofstream js(out);
    if (!js) {
        throw InputError("cannot write '" + o.json_path + "'");
    }
    const auto& ch  = o.sim.channel;
    const auto& rec = o.sim.rec;
    const auto& rx  = r.rx;
    const auto& cc  = r.channel;
    js << std::setprecision(10);
    const std::string manifest = "\"data_manifest\": " + manifest_json(o.file);
    js << "{\n  \"meta\": " << itch::provenance_json(itch::make_provenance(o.file), 2, manifest)
       << ",\n";
    js << "  \"config\": {\n";
    js << "    \"seed\": " << ch.seed << ",\n";
    js << "    \"loss\": " << ch.loss << ",\n";
    js << "    \"dup\": " << ch.dup << ",\n";
    js << "    \"reorder_window\": " << ch.reorder_window << ",\n";
    js << "    \"burst_start\": " << ch.burst_start << ",\n";
    js << "    \"burst_len\": " << ch.burst_len << ",\n";
    js << "    \"retransmit_loss\": " << ch.retransmit_loss << ",\n";
    js << "    \"retention_messages\": " << o.sim.retention << ",\n";
    js << "    \"timeout_ticks\": " << rec.timeout_ticks << ",\n";
    js << "    \"max_attempts\": " << rec.max_attempts << ",\n";
    js << "    \"max_buffered_packets\": " << rec.max_buffered_packets << ",\n";
    js << "    \"max_gap_messages\": " << rec.max_gap_messages << ",\n";
    js << "    \"heartbeat_every_packets\": " << o.sim.heartbeat_every << ",\n";
    js << "    \"mtu\": " << o.sim.mtu << ",\n";
    js << "    \"max_messages\": " << o.max_messages << ",\n";
    js << "    \"dump_at_s\": " << (o.sim.dump.at_set ? std::to_string(o.sim.dump.at_s) : "null")
       << ",\n";
    js << "    \"check_direct\": " << tf(o.check_direct) << "\n  },\n";
    js << "  \"source\": {\"file\": " << json_str(std::filesystem::path(o.file).filename().string())
       << ", \"file_bytes\": " << mf.bytes().size() << ", \"bytes_used\": " << used_bytes
       << ", \"messages\": " << source_messages << ", \"packets\": " << r.source_packets
       << ", \"stream_sha256\": " << json_str(source_sha) << "},\n";
    js << "  \"result\": {\"state\": " << json_str(mold::to_string(r.state))
       << ", \"stale_reason\": " << json_str(mold::to_string(r.stale))
       << ", \"finished\": " << tf(v.finished) << ", \"delivered_messages\": " << r.out.delivered
       << ", \"stream_sha256\": " << json_str(r.out.stream_sha256)
       << ", \"stream_matches_source\": " << tf(v.stream_matches_source)
       << ", \"end_of_session_ticks\": " << r.end_ticks << ", \"handled\": " << r.out.handled
       << ", \"skipped\": " << r.out.skipped << ", \"wrong_length\": " << r.out.wrong_length
       << ", \"live_orders_at_end\": " << r.out.live_orders << ", \"seconds\": " << std::fixed
       << std::setprecision(1) << seconds << std::defaultfloat << std::setprecision(10) << "},\n";
    js << "  \"rx\": {\"gaps_detected\": " << rx.gaps_detected
       << ", \"dup_packets\": " << rx.dup_packets
       << ", \"partial_overlaps\": " << rx.partial_overlaps
       << ", \"wrong_session\": " << rx.wrong_session << ", \"malformed\": " << rx.malformed
       << ", \"retransmit_requests\": " << rx.retransmit_requests
       << ", \"messages_recovered\": " << rx.messages_recovered
       << ", \"messages_buffered_peak\": " << rx.messages_buffered_peak
       << ", \"buffer_overflow_drops\": " << rx.buffer_overflow_drops
       << ", \"max_gap_len\": " << rx.max_gap_len << "},\n";
    js << "  \"channel\": {\"sent\": " << cc.sent << ", \"dropped\": " << cc.dropped
       << ", \"duplicated\": " << cc.duplicated << ", \"reordered\": " << cc.reordered
       << ", \"delivered\": " << cc.delivered << "},\n";
    js << "  \"dump\": ";
    if (!o.sim.dump.enabled) {
        js << "null";
    } else {
        const auto& d = r.out.dump;
        js << "{\"path\": " << json_str(dump_file) << ", \"at_end\": " << tf(d.at_end)
           << ", \"at_second\": " << d.at_second << ", \"at_message\": " << d.at_message
           << ", \"bytes\": " << d.bytes << ", \"resting_orders\": " << d.orders()
           << ", \"non_empty\": " << tf(d.bytes > 0) << ", \"complete\": " << tf(d.complete)
           << ", \"sha256\": " << json_str(d.sha256);
        if (direct != nullptr) {
            js << ", \"direct_sha256\": " << json_str(direct->dump.sha256)
               << ", \"direct_bytes\": " << direct->dump.bytes
               << ", \"matches_direct\": " << tf(v.state_matches_direct == 1);
        }
        js << "}";
    }
    js << ",\n  \"verdict\": {\"passed\": " << tf(v.exit_code == kExitOk)
       << ", \"exit_code\": " << v.exit_code << "}\n}\n";
    js.close();
    if (!js) {
        throw InputError("failed writing '" + o.json_path + "'");
    }
}

int run(const Options& o) {
    const auto                 mf     = itch::MappedFile::open(o.file);
    std::span<const std::byte> source = mf.bytes();
    if (o.max_messages != 0) {
        source = source.first(mold::prefix_bytes(source, o.max_messages));
    }

    // The file's own stream hash and message count, with no network and no book.
    mold::StreamHash src_hash;
    std::uint64_t    source_messages = 0;
    {
        itch::Framer               framer(source);
        std::span<const std::byte> msg;
        itch::FrameStatus          st;
        while ((st = framer.next(msg)) == itch::FrameStatus::Ok) {
            src_hash.add(++source_messages, msg);
        }
        if (st != itch::FrameStatus::End) {
            throw InputError("framing error at offset " + std::to_string(framer.offset()) + " of " +
                             std::to_string(source.size()) +
                             " bytes; the file is truncated or corrupt");
        }
    }
    const std::string source_sha = src_hash.hex();

    std::unique_ptr<mold::ReplayOutcome> direct;
    if (o.check_direct) {
        direct = std::make_unique<mold::ReplayOutcome>(
            mold::direct_replay(source, o.sim.max_live_orders, o.sim.dump));
    }

    const auto      t0 = std::chrono::steady_clock::now();
    mold::SimResult r;
    try {
        r = mold::run_sim(source, o.sim);
    } catch (const std::length_error& e) {
        throw InputError(e.what());
    } catch (const book::CapacityExceeded& e) {
        throw InputError(std::string("order capacity exhausted: ") + e.what());
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    Verdict v;
    v.finished = r.finished();
    v.stream_matches_source =
        v.finished && r.out.stream_sha256 == source_sha && r.out.delivered == source_messages;
    if (direct) {
        v.direct_nonempty      = direct->dump.bytes > 0;
        v.state_matches_direct = r.out.dump.taken && direct->dump.taken &&
                                         r.out.dump.sha256 == direct->dump.sha256 &&
                                         r.out.dump.data == direct->dump.data
                                     ? 1
                                     : 0;
    }
    const bool ok = v.finished && v.stream_matches_source && v.state_matches_direct != 0;
    v.exit_code   = ok ? kExitOk : kExitFailed;

    std::string dump_file = o.dump_path;
    if (o.sim.dump.enabled) {
        if (!v.finished) {
            dump_file += ".incomplete";  // a partial book must never pass for the real one
        }
        std::filesystem::path dp(dump_file);
        if (dp.has_parent_path()) {
            std::filesystem::create_directories(dp.parent_path());
        }
        std::ofstream os(dp, std::ios::binary);
        if (!os) {
            throw InputError("cannot write '" + dump_file + "'");
        }
        os.write(r.out.dump.data.data(), static_cast<std::streamsize>(r.out.dump.data.size()));
        os.close();
        if (!os) {
            throw InputError("failed writing '" + dump_file + "'");
        }
    }
    if (!o.json_path.empty()) {
        write_json(o, mf, source.size(), source_messages, source_sha, r, direct.get(), v, dump_file,
                   secs);
    }

    const auto& rx = r.rx;
    std::printf(
        "%s: %llu messages in %llu packets, %.1f s\n"
        "channel: sent %llu dropped %llu duplicated %llu reordered %llu\n"
        "receiver: %s (%s), gaps %llu, requests %llu, recovered %llu, dup %llu, overlap %llu, "
        "buffer peak %llu msgs, overflow drops %llu, max gap %llu\n"
        "stream: %s (%llu delivered)\n",
        o.file.c_str(), static_cast<unsigned long long>(source_messages),
        static_cast<unsigned long long>(r.source_packets), secs,
        static_cast<unsigned long long>(r.channel.sent),
        static_cast<unsigned long long>(r.channel.dropped),
        static_cast<unsigned long long>(r.channel.duplicated),
        static_cast<unsigned long long>(r.channel.reordered), mold::to_string(r.state),
        mold::to_string(r.stale), static_cast<unsigned long long>(rx.gaps_detected),
        static_cast<unsigned long long>(rx.retransmit_requests),
        static_cast<unsigned long long>(rx.messages_recovered),
        static_cast<unsigned long long>(rx.dup_packets),
        static_cast<unsigned long long>(rx.partial_overlaps),
        static_cast<unsigned long long>(rx.messages_buffered_peak),
        static_cast<unsigned long long>(rx.buffer_overflow_drops),
        static_cast<unsigned long long>(rx.max_gap_len),
        v.stream_matches_source ? "IDENTICAL to the file's" : "DIFFERS from the file's",
        static_cast<unsigned long long>(r.out.delivered));
    if (o.sim.dump.enabled) {
        std::printf("state: %llu resting orders (%llu bytes) at %s -> %s%s\n",
                    static_cast<unsigned long long>(r.out.dump.orders()),
                    static_cast<unsigned long long>(r.out.dump.bytes),
                    r.out.dump.at_end
                        ? "end of run"
                        : ("feed second " + std::to_string(r.out.dump.at_second)).c_str(),
                    dump_file.c_str(), v.finished ? "" : " (INCOMPLETE)");
        if (direct) {
            std::printf("state vs direct replay: %s (%llu resting orders direct)\n",
                        v.state_matches_direct == 1 ? "IDENTICAL" : "DIFFERS",
                        static_cast<unsigned long long>(direct->dump.orders()));
        }
    }
    if (!v.finished) {
        std::fprintf(stderr, "mold_sim: FAILED: receiver ended %s (%s); books are incomplete\n",
                     mold::to_string(r.state), mold::to_string(r.stale));
    } else if (!v.stream_matches_source) {
        std::fprintf(stderr, "mold_sim: FAILED: delivered stream differs from the file's\n");
    } else if (v.state_matches_direct == 0) {
        std::fprintf(stderr, "mold_sim: FAILED: state differs from the direct replay\n");
    }
    return v.exit_code;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    try {
        o = parse(argc, argv);
    } catch (const InputError& e) {
        std::fprintf(stderr, "mold_sim: %s\n", e.what());
        usage();
        return kExitInput;
    }
    try {
        return run(o);
    } catch (const InputError& e) {
        std::fprintf(stderr, "mold_sim: %s\n", e.what());
        return kExitInput;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "mold_sim: %s\n", e.what());
        return kExitInput;
    }
}
