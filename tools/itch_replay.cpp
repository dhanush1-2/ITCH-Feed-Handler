// tools/itch_replay.cpp
//
// Replays one decompressed ITCH 5.0 file through the full-depth book builder and gathers
// statistics.
//
//   itch_replay <file> [--symbols A,B] [--max-live-orders N] [--verify]
//               [--full-check-every N] [--halt-grace-s S] [--dump-state path] [--dump-at-s S]
//               [--export path] [--speed max]
//
//   --symbols            symbols whose top of book is recorded once per second (case is
//                        normalised; a name not in the file's stock directory is an error)
//   --max-live-orders    order capacity, default 16,000,000; exhausting it is an error
//   --verify             run the structural checks, the feed oracle and the conservation
//                        check; exit 3 on any hard failure. Oracle exception counts are
//                        findings, not failures
//   --full-check-every   with --verify, a whole-state check every N messages (default
//                        1,000,000; the end-of-run check always runs)
//   --halt-grace-s       seconds of feed time after a halt resumes before the symbol counts
//                        as continuous trading (default 5)
//   --dump-state         write every resting order (see state_dump.hpp), at the end of the
//                        file unless --dump-at-s says otherwise. A full day ends with no
//                        resting orders, so its end-of-file dump is empty
//   --dump-at-s S        dump just before the first message at or after feed second S
//   --export             write the JSON export (results/day_<date>.json for a full day)
//
// Exit codes (shared by every tool): 0 success, 2 input error (bad option, unreadable or
// truncated file, unknown symbol, capacity exhausted), 3 verification failure.

#include <sys/resource.h>

#include <book/builder.hpp>
#include <book/invariants.hpp>
#include <book/state_dump.hpp>
#include <book/stats.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <itch/dispatch.hpp>
#include <itch/framer.hpp>
#include <itch/mapped_file.hpp>
#include <itch/provenance.hpp>
#include <itch/scan.hpp>
#include <itch/sha256.hpp>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace {

constexpr int kExitOk     = 0;
constexpr int kExitInput  = 2;
constexpr int kExitVerify = 3;

struct InputError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Options {
    std::string              file;
    std::vector<std::string> symbols;
    std::size_t              max_live       = 16'000'000;
    bool                     verify         = false;
    std::uint64_t            full_every     = 0;
    bool                     full_every_set = false;
    std::uint64_t            grace_s        = 5;
    std::string              dump_path;
    bool                     dump_at_set = false;
    std::uint64_t            dump_at_s   = 0;
    std::string              export_path;
};

void usage() {
    std::fprintf(stderr,
                 "usage: itch_replay <file> [--symbols A,B] [--max-live-orders N] [--verify]\n"
                 "                   [--full-check-every N] [--halt-grace-s S]\n"
                 "                   [--dump-state path] [--export path] [--speed max]\n");
}

std::uint64_t parse_u64(const std::string& flag, const std::string& v) {
    if (v.empty() || v.find_first_not_of("0123456789") != std::string::npos) {
        throw InputError(flag + " needs a non-negative integer, got '" + v + "'");
    }
    return std::strtoull(v.c_str(), nullptr, 10);
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
        if (a == "--symbols") {
            std::stringstream     ss(value());
            std::string           item;
            std::set<std::string> seen;
            while (std::getline(ss, item, ',')) {
                const std::string n = itch::normalize_symbol(item);
                if (!n.empty() && seen.insert(n).second) {
                    o.symbols.push_back(n);
                }
            }
        } else if (a == "--max-live-orders") {
            o.max_live = static_cast<std::size_t>(parse_u64(a, value()));
            if (o.max_live == 0) {
                throw InputError("--max-live-orders must be at least 1");
            }
        } else if (a == "--verify") {
            o.verify = true;
        } else if (a == "--full-check-every") {
            o.full_every     = parse_u64(a, value());
            o.full_every_set = true;
        } else if (a == "--halt-grace-s") {
            o.grace_s = parse_u64(a, value());
        } else if (a == "--dump-state") {
            o.dump_path = value();
        } else if (a == "--dump-at-s") {
            o.dump_at_s   = parse_u64(a, value());
            o.dump_at_set = true;
        } else if (a == "--export") {
            o.export_path = value();
        } else if (a == "--speed") {
            const std::string v = value();
            if (v != "max") {
                throw InputError("--speed " + v + " is not available yet; only 'max' is");
            }
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
    if (o.dump_at_set && o.dump_path.empty()) {
        throw InputError("--dump-at-s needs --dump-state");
    }
    if (o.verify && !o.full_every_set) {
        o.full_every = 1'000'000;
    }
    return o;
}

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

std::string json_str(const std::string& s) {
    return "\"" + itch::json_escape(s) + "\"";
}

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

std::uint64_t max_rss_bytes() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(ru.ru_maxrss);  // bytes
#else
    return static_cast<std::uint64_t>(ru.ru_maxrss) * 1024;  // kilobytes
#endif
}

struct Dump {
    std::string   path;
    std::uint64_t bytes = 0;
    std::string   sha256;
    bool          at_end     = true;
    std::uint64_t at_second  = 0;  // feed second the dump was taken before (when !at_end)
    std::uint64_t at_message = 0;  // frames read before the dump
};

std::string file_sha256(const std::string& path, std::uint64_t& bytes) {
    std::ifstream     in(path, std::ios::binary);
    itch::Sha256      h;
    std::vector<char> buf(1 << 20);
    bytes = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<std::size_t>(in.gcount());
        h.update(buf.data(), n);
        bytes += n;
    }
    return h.hex();
}

book::StatsObserver& stats_of(book::StatsObserver& o) {
    return o;
}
book::StatsObserver& stats_of(book::BasicVerifyObserver<book::StatsObserver>& o) {
    return o.extra();
}

const char* kOracleNames[] = {"exec_not_best", "exec_not_head", "crossed_continuous",
                              "crossed_other"};

template <class Builder>
void write_export(const Options& o, const itch::MappedFile& mf, const Builder& b,
                  const book::StatsObserver& st, const book::VerifyObserver::Options& vopt,
                  const book::Oracle* oracle, std::uint64_t full_checks, itch::FrameStatus status,
                  std::uint64_t handled, std::uint64_t skipped, std::uint64_t wrong_length,
                  const std::vector<book::ConservationFailure>& cons,
                  const std::vector<std::string>& failures, const Dump* dump) {
    std::filesystem::path out(o.export_path);
    if (out.has_parent_path()) {
        std::filesystem::create_directories(out.parent_path());
    }
    std::ofstream js(out);
    if (!js) {
        throw InputError("cannot write '" + o.export_path + "'");
    }
    const std::string manifest = "\"data_manifest\": " + manifest_json(o.file);
    js << "{\n  \"meta\": " << itch::provenance_json(itch::make_provenance(o.file), 2, manifest)
       << ",\n";
    js << "  \"file\": " << json_str(std::filesystem::path(o.file).filename().string()) << ",\n";
    js << "  \"file_bytes\": " << mf.bytes().size() << ",\n";
    js << "  \"price_scale\": 10000,\n";

    js << "  \"verify\": {\"enabled\": " << (o.verify ? "true" : "false")
       << ", \"passed\": " << (o.verify ? (failures.empty() ? "true" : "false") : "null")
       << ", \"full_check_every\": " << o.full_every << ", \"full_checks\": " << full_checks
       << ", \"halt_grace_s\": " << vopt.halt_grace_ns / 1'000'000'000ULL << ", \"failures\": [";
    for (std::size_t i = 0; i < failures.size(); ++i) {
        js << (i != 0 ? ", " : "") << json_str(failures[i]);
    }
    js << "]},\n";

    std::uint64_t locates = 0, with_orders = 0;
    std::uint64_t tot_added = 0, tot_in = 0, tot_exec = 0, tot_canc = 0, tot_del = 0, tot_out = 0;
    for (std::size_t i = 0; i < Builder::kLocates; ++i) {
        const auto& s = st.symbol(static_cast<std::uint16_t>(i));
        locates += s.messages != 0;
        with_orders += s.orders_added != 0;
        tot_added += s.added;
        tot_in += s.replaced_in;
        tot_exec += s.executed;
        tot_canc += s.cancelled;
        tot_del += s.deleted;
        tot_out += s.replaced_out;
    }
    js << "  \"totals\": {\n";
    js << "    \"messages\": " << st.frames() << ",\n";
    js << "    \"handled\": " << handled << ",\n";
    js << "    \"skipped\": " << skipped << ",\n";
    js << "    \"wrong_length\": " << wrong_length << ",\n";
    js << "    \"short_frames\": " << st.short_frames() << ",\n";
    js << "    \"frame_status\": \"" << status_name(status) << "\",\n";
    js << "    \"unique_locates\": " << locates << ",\n";
    js << "    \"first_timestamp_ns\": " << st.first_ts() << ",\n";
    js << "    \"last_timestamp_ns\": " << st.last_ts() << ",\n";
    js << "    \"system_events\": " << st.system_events() << ",\n";
    js << "    \"last_system_event\": " << json_char(st.last_system_event()) << ",\n";
    js << "    \"per_second_out_of_range\": " << st.out_of_range_seconds() << ",\n";
    js << "    \"shares\": {\"added\": " << tot_added << ", \"replaced_in\": " << tot_in
       << ", \"executed\": " << tot_exec << ", \"cancelled\": " << tot_canc
       << ", \"deleted\": " << tot_del << ", \"replaced_out\": " << tot_out << "},\n";
    js << "    \"live_orders_at_end\": " << b.live_orders() << ",\n";
    js << "    \"printed_volume_shares\": " << st.printed_volume() << ",\n";
    js << "    \"trade_count\": " << st.trade_count() << ",\n";
    js << "    \"volume_by_kind\": {\"E\": " << st.volume_of('E')
       << ", \"C\": " << st.volume_of('C') << ", \"P\": " << st.volume_of('P')
       << ", \"Q\": " << st.volume_of('Q') << "},\n";
    js << "    \"nonprintable_prints\": " << st.nonprintable_prints() << ",\n";
    js << "    \"broken\": {\"messages\": " << st.broken_messages()
       << ", \"prints_excluded\": " << st.broken_prints()
       << ", \"shares_excluded\": " << st.broken_shares()
       << ", \"without_print\": " << st.broken_without_print() << "},\n";
    js << "    \"match_number_collisions\": {\"distinct\": " << st.match_collisions()
       << ", \"extra_prints\": " << st.match_extra_prints()
       << ", \"cross_kind\": " << st.match_collisions_cross_kind() << "}\n";
    js << "  },\n";

    js << "  \"by_type\": {";
    bool first = true;
    st.for_each_type([&](char t, std::uint64_t n) {
        js << (first ? "\n    " : ",\n    ") << json_char(t) << ": " << n;
        first = false;
    });
    js << "\n  },\n";

    js << "  \"per_second\": [";
    const auto& ps = st.per_second();
    for (std::size_t i = 0; i < ps.size(); ++i) {
        js << (i != 0 ? "," : "") << ps[i];
    }
    js << "],\n";

    js << "  \"watch\": ";
    book::write_watch_json(js, st, "  ");
    js << ",\n";

    const auto& an = b.anomalies();
    js << "  \"anomalies\": {\"unknown_ref\": " << an.unknown_ref
       << ", \"duplicate_add\": " << an.duplicate_add
       << ", \"locate_mismatch\": " << an.locate_mismatch
       << ", \"over_execute\": " << an.over_execute << ", \"over_cancel\": " << an.over_cancel
       << ", \"zero_shares\": " << an.zero_shares << "},\n";

    if (oracle == nullptr) {
        js << "  \"oracle\": null,\n";
    } else {
        const auto& c = oracle->counts();
        js << "  \"oracle\": {\n";
        js << "    \"exec_checked\": " << c.exec_checked << ",\n";
        js << "    \"exec_checked_continuous\": " << c.exec_checked_continuous << ",\n";
        js << "    \"exec_not_best\": " << c.exec_not_best << ",\n";
        js << "    \"exec_not_best_continuous\": " << c.exec_not_best_continuous << ",\n";
        js << "    \"exec_not_head\": " << c.exec_not_head << ",\n";
        js << "    \"exec_not_head_continuous\": " << c.exec_not_head_continuous << ",\n";
        js << "    \"touches_continuous\": " << c.touches_continuous << ",\n";
        js << "    \"touches_other\": " << c.touches_other << ",\n";
        js << "    \"crossed_continuous\": " << c.crossed_continuous << ",\n";
        js << "    \"crossed_other\": " << c.crossed_other << ",\n";
        js << "    \"episodes_continuous\": " << c.episodes_continuous << ",\n";
        js << "    \"episodes_other\": " << c.episodes_other << ",\n";
        js << "    \"kept_per_kind\": " << book::Oracle::kKeep << ",\n";
        js << "    \"exceptions\": {";
        for (std::size_t k = 0; k < 4; ++k) {
            js << (k != 0 ? ",\n      " : "\n      ") << "\"" << kOracleNames[k] << "\": [";
            const auto& v = oracle->exceptions(static_cast<book::OracleKind>(k));
            for (std::size_t i = 0; i < v.size(); ++i) {
                js << (i != 0 ? ", " : "") << "{\"t_ns\": " << v[i].timestamp_ns
                   << ", \"locate\": " << v[i].locate
                   << ", \"symbol\": " << json_str(st.name(v[i].locate))
                   << ", \"ref\": " << v[i].ref << "}";
            }
            js << "]";
        }
        js << "\n    }\n  },\n";
    }

    js << "  \"conservation\": {\"failures\": " << cons.size() << ", \"first\": [";
    for (std::size_t i = 0; i < cons.size() && i < 20; ++i) {
        js << (i != 0 ? ", " : "") << "{\"locate\": " << cons[i].locate
           << ", \"symbol\": " << json_str(st.name(cons[i].locate))
           << ", \"imbalance\": " << cons[i].imbalance << "}";
    }
    js << "]},\n";

    js << "  \"peak_live_orders\": " << b.peak_live_orders() << ",\n";
    js << "  \"max_rss_bytes\": " << max_rss_bytes() << ",\n";
    js << "  \"symbols_with_orders\": " << with_orders << ",\n";
    if (dump != nullptr) {
        js << "  \"state_dump\": {\"path\": " << json_str(dump->path)
           << ", \"at\": " << (dump->at_end ? "\"end\"" : "\"before_feed_second\"")
           << ", \"feed_second\": " << (dump->at_end ? "null" : std::to_string(dump->at_second))
           << ", \"after_messages\": " << dump->at_message << ", \"bytes\": " << dump->bytes
           << ", \"sha256\": \"" << dump->sha256 << "\"},\n";
    } else {
        js << "  \"state_dump\": null,\n";
    }

    // Per-symbol ledger, columnar: row i of every array is the same symbol.
    std::vector<std::uint16_t> rows;
    for (std::size_t i = 0; i < Builder::kLocates; ++i) {
        if (st.symbol(static_cast<std::uint16_t>(i)).messages != 0) {
            rows.push_back(static_cast<std::uint16_t>(i));
        }
    }
    js << "  \"per_symbol\": {\n";
    const auto col = [&](const char* name, auto&& f, bool last = false) {
        js << "    \"" << name << "\": [";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            js << (i != 0 ? "," : "");
            f(rows[i]);
        }
        js << (last ? "]\n" : "],\n");
    };
    col("locate", [&](std::uint16_t l) { js << l; });
    col("symbol", [&](std::uint16_t l) { js << json_str(st.name(l)); });
    col("messages", [&](std::uint16_t l) { js << st.symbol(l).messages; });
    col("added", [&](std::uint16_t l) { js << st.symbol(l).added; });
    col("replaced_in", [&](std::uint16_t l) { js << st.symbol(l).replaced_in; });
    col("executed", [&](std::uint16_t l) { js << st.symbol(l).executed; });
    col("cancelled", [&](std::uint16_t l) { js << st.symbol(l).cancelled; });
    col("deleted", [&](std::uint16_t l) { js << st.symbol(l).deleted; });
    col("replaced_out", [&](std::uint16_t l) { js << st.symbol(l).replaced_out; });
    col("live", [&](std::uint16_t l) { js << book::live_shares(b, l); });
    col("volume", [&](std::uint16_t l) { js << st.symbol(l).volume; });
    col("trades", [&](std::uint16_t l) { js << st.symbol(l).trades; }, true);
    js << "  }\n}\n";
    js.close();
    if (!js) {
        throw InputError("failed writing '" + o.export_path + "'");
    }
}

template <bool Verify>
int run(const Options& o, const itch::MappedFile& mf, const itch::Resolved& resolved,
        std::unordered_set<std::uint64_t> broken) {
    using Obs = std::conditional_t<Verify, book::BasicVerifyObserver<book::StatsObserver>,
                                   book::StatsObserver>;
    book::VerifyObserver::Options vopt;
    vopt.full_every    = o.full_every;
    vopt.halt_grace_ns = o.grace_s * 1'000'000'000ULL;

    auto make = [&]() {
        if constexpr (Verify) {
            return Obs(vopt);
        } else {
            return Obs();
        }
    };
    Obs                    obs = make();
    book::StatsObserver&   st  = stats_of(obs);
    book::BookBuilder<Obs> b(o.max_live, obs);
    if constexpr (Verify) {
        obs.bind(b);
    }
    st.bind(b);
    st.set_broken_matches(std::move(broken));
    for (const auto& [name, loc] : resolved.locate) {
        st.watch(name, loc);
        std::printf("watching %s (locate %u)\n", name.c_str(), static_cast<unsigned>(loc));
    }

    const auto                 t0 = std::chrono::steady_clock::now();
    itch::Framer               framer(mf.bytes());
    itch::FrameStatus          status;
    std::span<const std::byte> msg;
    std::uint64_t              handled = 0, skipped = 0, wrong_length = 0;
    Dump                       dump;
    bool                       dumped    = false;
    const auto                 take_dump = [&](bool at_end, std::uint64_t second) {
        std::filesystem::path dp(o.dump_path);
        if (dp.has_parent_path()) {
            std::filesystem::create_directories(dp.parent_path());
        }
        {
            std::ofstream os(dp, std::ios::binary);
            if (!os) {
                throw InputError("cannot write '" + o.dump_path + "'");
            }
            book::write_state(os, b);
        }
        dump.path       = o.dump_path;
        dump.at_end     = at_end;
        dump.at_second  = second;
        dump.at_message = st.frames();
        dump.sha256     = file_sha256(o.dump_path, dump.bytes);
        dumped          = true;
    };
    const std::uint64_t dump_at_ns = o.dump_at_s * 1'000'000'000ULL;
    while ((status = framer.next(msg)) == itch::FrameStatus::Ok) {
        if (o.dump_at_set && !dumped && msg.size() >= 11 &&
            itch::HeaderView(msg.data()).timestamp_ns() >= dump_at_ns) {
            take_dump(false, o.dump_at_s);
        }
        st.on_frame(msg);
        switch (itch::decode(msg, b)) {
            case itch::DecodeStatus::Handled:
                ++handled;
                break;
            case itch::DecodeStatus::Skipped:
                ++skipped;
                break;
            case itch::DecodeStatus::WrongLength:
                ++wrong_length;
                break;
        }
    }
    if (status != itch::FrameStatus::End) {
        throw InputError(std::string("framing error: ") + status_name(status) + " at offset " +
                         std::to_string(framer.offset()) + " of " +
                         std::to_string(mf.bytes().size()) +
                         " bytes; the file is truncated or "
                         "corrupt");
    }
    if constexpr (Verify) {
        obs.finish();  // the end-of-run whole-state check; may throw VerificationFailure
    }
    st.finish();
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const auto               cons = book::conservation_failures(b, st);
    std::vector<std::string> failures;
    if constexpr (Verify) {
        if (b.anomalies().unknown_ref != 0) {
            failures.push_back("unknown_ref = " + std::to_string(b.anomalies().unknown_ref));
        }
        if (!cons.empty()) {
            failures.push_back("conservation fails for " + std::to_string(cons.size()) +
                               " symbol(s)");
        }
        if (wrong_length != 0) {
            failures.push_back("wrong_length = " + std::to_string(wrong_length));
        }
        if (st.last_system_event() != 'C') {
            failures.push_back(
                std::string("missing end-of-messages event (last system event ") +
                (st.last_system_event() == 0 ? "none" : std::string(1, st.last_system_event())) +
                ")");
        }
    }

    if (!o.dump_path.empty() && !dumped) {
        if (o.dump_at_set) {
            throw InputError("the file never reaches feed second " + std::to_string(o.dump_at_s) +
                             ", so there is no state to dump there");
        }
        take_dump(true, 0);
    }
    const Dump* dump_ptr = dumped ? &dump : nullptr;

    if (!o.export_path.empty()) {
        const book::Oracle* oracle = nullptr;
        std::uint64_t       fulls  = 0;
        if constexpr (Verify) {
            oracle = &obs.oracle();
            fulls  = obs.full_checks();
        }
        write_export(o, mf, b, st, vopt, oracle, fulls, status, handled, skipped, wrong_length,
                     cons, failures, dump_ptr);
    }

    std::printf("%s: %llu messages in %.1f s, live at end %zu, peak live %zu, unknown_ref %llu\n",
                o.file.c_str(), static_cast<unsigned long long>(st.frames()), secs, b.live_orders(),
                b.peak_live_orders(), static_cast<unsigned long long>(b.anomalies().unknown_ref));
    if constexpr (Verify) {
        const auto& c = obs.counts();
        std::printf(
            "oracle: exec_checked %llu, not_best %llu (%llu continuous), not_head %llu "
            "(%llu continuous), crossed_continuous %llu of %llu touches, episodes %llu\n",
            static_cast<unsigned long long>(c.exec_checked),
            static_cast<unsigned long long>(c.exec_not_best),
            static_cast<unsigned long long>(c.exec_not_best_continuous),
            static_cast<unsigned long long>(c.exec_not_head),
            static_cast<unsigned long long>(c.exec_not_head_continuous),
            static_cast<unsigned long long>(c.crossed_continuous),
            static_cast<unsigned long long>(c.touches_continuous),
            static_cast<unsigned long long>(c.episodes_continuous));
        if (!failures.empty()) {
            for (const std::string& f : failures) {
                std::fprintf(stderr, "itch_replay: VERIFICATION FAILED: %s\n", f.c_str());
            }
            return kExitVerify;
        }
        std::printf(
            "VERIFIED (%llu whole-state checks, conservation holds for all %zu "
            "locates)\n",
            static_cast<unsigned long long>(obs.full_checks()), Obs::Builder::kLocates);
    }
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    try {
        o = parse(argc, argv);
    } catch (const InputError& e) {
        std::fprintf(stderr, "itch_replay: %s\n", e.what());
        usage();
        return kExitInput;
    }
    try {
        const auto mf = itch::MappedFile::open(o.file);

        itch::Resolved resolved;
        if (!o.symbols.empty()) {
            resolved = itch::resolve_symbols(mf.bytes(), o.symbols);
            if (!resolved.unresolved.empty()) {
                std::string names;
                for (const std::string& n : resolved.unresolved) {
                    names += (names.empty() ? "" : ", ") + n;
                }
                throw InputError("unknown symbol(s) " + names +
                                 ": not in the stock directory of '" + o.file + "'");
            }
        }
        // Broken trades arrive after their trade, so the volume rule needs them first.
        auto broken = itch::scan_broken(mf.bytes());

        return o.verify ? run<true>(o, mf, resolved, std::move(broken))
                        : run<false>(o, mf, resolved, std::move(broken));
    } catch (const book::CapacityExceeded& e) {
        std::fprintf(stderr, "itch_replay: %s\n", e.what());
        return kExitInput;
    } catch (const book::VerificationFailure& e) {
        std::fprintf(stderr, "itch_replay: VERIFICATION FAILED: %s\n", e.what());
        return kExitVerify;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "itch_replay: %s\n", e.what());
        return kExitInput;
    }
}
