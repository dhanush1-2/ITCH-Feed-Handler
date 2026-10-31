// bench/bench_feed.cpp
//
// Benchmark harness for the feed handler. One process measures one thing once; the median of
// several fresh processes is taken by scripts/run_bench.py. See docs/METHODOLOGY.md.
//
//   bench_feed <file> --mode frame|parse|apply|latency [--map index|unordered]
//              [--book vector|ladder|none] [--symbols A,B,...] [--max-live-orders N]
//              [--max-messages N] [--json out]
//
// Modes
//   frame    walks the length prefixes and decodes nothing
//   parse    frame + decode, with a sink that reads every field the book builder would read
//   apply    parse + BookBuilder<NullObserver>; NO per-message clock. The clock is read twice per
//            chunk of 2^20 messages, and the fold that checks the books runs between chunks,
//            outside the timed span
//   latency  apply with raw_serialized() around every decode+apply into exact histograms, with a
//            per-message-type breakdown, plus the timer's own cost measured on an empty region
//
// Everything except the timed span is setup: mapping and pre-faulting the input, allocating the
// order pool and id index (their constructors write every byte), the symbol filter and the
// ladder bases. Nothing is discarded as warm-up: the run is one pass over a trading day, and the
// first chunks are the pre-market.
//
// --symbols restricts the stream to messages whose stock_locate belongs to those symbols and
// copies it into one contiguous buffer first (setup, untimed). The ladder A/B needs it: Project
// 1's ladder is 3 MB per symbol and is tried on ten symbols only.

#include <mach/mach.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include <algorithm>
#include <array>
#include <book/builder.hpp>
#include <clock.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <histogram.hpp>
#include <itch/dispatch.hpp>
#include <itch/framer.hpp>
#include <itch/mapped_file.hpp>
#include <itch/provenance.hpp>
#include <itch/scan.hpp>
#include <ladder_book.hpp>
#include <memory>
#include <ob/id_index.hpp>
#include <ob/types.hpp>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace {

using ob::bench::Clock;
using ob::bench::do_not_optimize;
using ob::bench::Histogram;

constexpr std::size_t kChunk = std::size_t{1} << 20;

struct InputError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Options {
    std::string              file;
    std::string              mode;
    std::string              map  = "index";
    std::string              book = "vector";
    std::vector<std::string> symbols;
    std::size_t              max_live     = 4'000'000;
    std::size_t              max_messages = 0;  // 0 = whole file
    std::string              json;
};

void usage() {
    std::fprintf(stderr,
                 "usage: bench_feed <file> --mode frame|parse|apply|latency\n"
                 "                  [--map index|unordered] [--book vector|ladder|none]\n"
                 "                  [--symbols A,B,...] [--max-live-orders N]\n"
                 "                  [--max-messages N] [--json out]\n");
}

std::uint64_t parse_u64(const std::string& flag, const std::string& v) {
    try {
        std::size_t used = 0;
        const auto  n    = std::stoull(v, &used);
        if (used != v.size()) {
            throw std::invalid_argument(v);
        }
        return n;
    } catch (const std::exception&) {
        throw InputError(flag + " needs a non-negative integer, got '" + v + "'");
    }
}

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a     = argv[i];
        auto              value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw InputError(a + " needs a value");
            }
            return argv[++i];
        };
        if (a == "--mode") {
            o.mode = value();
        } else if (a == "--map") {
            o.map = value();
        } else if (a == "--book") {
            o.book = value();
        } else if (a == "--symbols") {
            std::stringstream ss(value());
            std::string       s;
            while (std::getline(ss, s, ',')) {
                if (!s.empty()) {
                    o.symbols.push_back(itch::normalize_symbol(s));
                }
            }
        } else if (a == "--max-live-orders") {
            o.max_live = static_cast<std::size_t>(parse_u64(a, value()));
        } else if (a == "--max-messages") {
            o.max_messages = static_cast<std::size_t>(parse_u64(a, value()));
        } else if (a == "--json") {
            o.json = value();
        } else if (a.rfind("--", 0) == 0) {
            throw InputError("unknown option " + a);
        } else if (o.file.empty()) {
            o.file = a;
        } else {
            throw InputError("unexpected argument " + a);
        }
    }
    if (o.file.empty() || o.mode.empty()) {
        throw InputError("a file and --mode are required");
    }
    if (o.mode != "frame" && o.mode != "parse" && o.mode != "apply" && o.mode != "latency") {
        throw InputError("--mode must be frame, parse, apply or latency");
    }
    if (o.map != "index" && o.map != "unordered") {
        throw InputError("--map must be index or unordered");
    }
    if (o.book != "vector" && o.book != "ladder" && o.book != "none") {
        throw InputError("--book must be vector, ladder or none");
    }
    if (o.book == "ladder" && o.symbols.empty()) {
        throw InputError("--book ladder needs --symbols: a ladder costs 3 MB per symbol");
    }
    if (o.max_live == 0) {
        throw InputError("--max-live-orders must be positive");
    }
    return o;
}

// ---- alternatives under test ------------------------------------------------------------

// std::unordered_map behind IdIndex's interface, reserved up front. The A/B baseline.
class StdMapIndex {
public:
    explicit StdMapIndex(std::size_t max_entries) : max_(max_entries) { m_.reserve(max_entries); }

    [[nodiscard]] bool insert(ob::OrderId id, ob::Slot slot) {
        if (id == 0 || m_.size() >= max_) {
            return false;
        }
        return m_.emplace(id, slot).second;
    }
    [[nodiscard]] ob::Slot find(ob::OrderId id) const {
        const auto it = m_.find(id);
        return it == m_.end() ? ob::kInvalidSlot : it->second;
    }
    bool                      erase(ob::OrderId id) { return m_.erase(id) != 0; }
    [[nodiscard]] std::size_t size() const noexcept { return m_.size(); }
    [[nodiscard]] bool        full() const noexcept { return m_.size() >= max_; }
    [[nodiscard]] std::size_t bucket_bytes() const noexcept {
        return m_.bucket_count() * sizeof(void*);
    }

private:
    std::unordered_map<ob::OrderId, ob::Slot> m_;
    std::size_t                               max_;
};

// A book that does nothing: what remains is framing, decoding, the pool and the id index. The
// difference between this and the vector book is the book's own cost.
class NullBook {
public:
    void add(ob::OrderPool&, ob::Slot) noexcept {}
    void remove(ob::OrderPool&, ob::Slot) noexcept {}
    void reduce(ob::OrderPool&, ob::Slot, std::uint32_t) noexcept {}
    [[nodiscard]] std::optional<std::int32_t> best_bid() const noexcept { return std::nullopt; }
    [[nodiscard]] std::optional<std::int32_t> best_ask() const noexcept { return std::nullopt; }
    [[nodiscard]] std::size_t                 order_count() const noexcept { return 0; }
};

// ---- parse-only sink --------------------------------------------------------------------

// Reads every field the book builder reads, so the loads cannot be elided.
struct ParseSink {
    std::uint64_t acc = 0;

    void on(const itch::AddOrderView& v) noexcept {
        acc += v.ref() + v.shares() + v.price() + v.locate() + (v.side() == 'B' ? 1U : 0U);
    }
    void on(const itch::ExecutedView& v) noexcept {
        acc += v.ref() + v.shares() + v.match() + v.locate();
    }
    void on(const itch::ExecutedPriceView& v) noexcept {
        acc += v.ref() + v.shares() + v.match() + v.price() + v.locate();
    }
    void on(const itch::CancelView& v) noexcept { acc += v.ref() + v.shares() + v.locate(); }
    void on(const itch::DeleteView& v) noexcept { acc += v.ref() + v.locate(); }
    void on(const itch::ReplaceView& v) noexcept {
        acc += v.old_ref() + v.new_ref() + v.shares() + v.price() + v.locate();
    }
    template <class V>
    void on(const V& v) noexcept {
        acc += v.locate() + v.timestamp_ns();
    }
};

// ---- measurement plumbing ---------------------------------------------------------------

struct Usage {
    std::uint64_t minflt = 0, majflt = 0, nvcsw = 0, nivcsw = 0;
};

Usage usage_now() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return {static_cast<std::uint64_t>(ru.ru_minflt), static_cast<std::uint64_t>(ru.ru_majflt),
            static_cast<std::uint64_t>(ru.ru_nvcsw), static_cast<std::uint64_t>(ru.ru_nivcsw)};
}

std::uint64_t max_rss_bytes() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<std::uint64_t>(ru.ru_maxrss);  // bytes on macOS
}

struct Prefault {
    int           passes                 = 0;
    std::uint64_t last_pass_major_faults = 0;
};
Prefault g_prefault;

struct Footprint {
    std::uint64_t phys_footprint      = 0;  // what Activity Monitor calls Memory
    std::uint64_t phys_footprint_peak = 0;  // peak of the above for the process lifetime
    std::uint64_t internal            = 0;  // dirty anonymous memory
    std::uint64_t external            = 0;  // file-backed pages, the mapped input among them
    std::uint64_t compressed          = 0;
    std::uint64_t resident            = 0;
};

Footprint footprint_now() {
    task_vm_info_data_t    info{};
    mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
    Footprint              f;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &cnt) ==
        KERN_SUCCESS) {
        f.phys_footprint      = info.phys_footprint;
        f.phys_footprint_peak = static_cast<std::uint64_t>(info.ledger_phys_footprint_peak);
        f.internal            = info.internal;
        f.external            = info.external;
        f.compressed          = info.compressed;
        f.resident            = info.resident_size;
    }
    return f;
}

std::string footprint_json(const Footprint& f) {
    std::ostringstream o;
    o << "{\"phys_footprint\": " << f.phys_footprint
      << ", \"phys_footprint_peak\": " << f.phys_footprint_peak
      << ", \"internal_anonymous\": " << f.internal << ", \"compressed\": " << f.compressed
      << ", \"external_file_backed\": " << f.external << ", \"resident\": " << f.resident << "}";
    return o.str();
}

// The state fold taken between chunks. It reads the touch of every book, so a run that
// silently applied something different (another index, another book) shows in the fold.
template <class Builder>
std::uint64_t fold(const Builder& b, std::uint64_t h) {
    constexpr std::uint64_t kPrime = 0x100000001B3ULL;
    for (std::size_t loc = 0; loc < Builder::kLocates; ++loc) {
        const auto& bk = b.book(static_cast<std::uint16_t>(loc));
        h              = (h ^ static_cast<std::uint64_t>(bk.best_bid().value_or(-1))) * kPrime;
        h              = (h ^ static_cast<std::uint64_t>(bk.best_ask().value_or(-1))) * kPrime;
        h              = (h ^ bk.order_count()) * kPrime;
    }
    return h;
}

struct Results {
    std::uint64_t       messages = 0, handled = 0, skipped = 0, wrong_length = 0, bytes = 0;
    std::uint64_t       timed_ticks = 0;
    std::vector<double> chunk_rate;  // messages/sec per chunk
    std::uint64_t       checksum = 0;
    Usage               use_delta;
};

// The timed loop. `Sink` is a ParseSink or a BookBuilder; `post_chunk` runs outside the timed span.
template <class Sink, class PostChunk>
Results run_throughput(std::span<const std::byte> data, std::size_t max_messages, Sink& sink,
                       bool decode_it, PostChunk post_chunk) {
    Results                    r;
    itch::Framer               fr(data);
    std::span<const std::byte> msg;
    const Clock&               clk  = Clock::instance();
    const Usage                u0   = usage_now();
    bool                       more = true;
    while (more) {
        std::size_t n  = 0;
        std::size_t hd = 0, sk = 0, wl = 0;
        std::size_t by  = 0;
        std::size_t cap = kChunk;
        if (max_messages != 0) {
            cap = std::min(cap, max_messages - static_cast<std::size_t>(r.messages));
        }
        const std::uint64_t t0 = Clock::raw_serialized();
        while (n < cap) {
            if (fr.next(msg) != itch::FrameStatus::Ok) {
                more = false;
                break;
            }
            ++n;
            by += msg.size() + 2;
            if (decode_it) {
                switch (itch::decode(msg, sink)) {
                    case itch::DecodeStatus::Handled:
                        ++hd;
                        break;
                    case itch::DecodeStatus::Skipped:
                        ++sk;
                        break;
                    case itch::DecodeStatus::WrongLength:
                        ++wl;
                        break;
                }
            }
        }
        const std::uint64_t t1 = Clock::raw_serialized();
        if (n == 0) {
            break;
        }
        r.messages += n;
        r.handled += hd;
        r.skipped += sk;
        r.wrong_length += wl;
        r.bytes += by;
        r.timed_ticks += t1 - t0;
        const double secs = clk.ticks_to_ns(t1 - t0) * 1e-9;
        if (secs > 0.0) {
            r.chunk_rate.push_back(static_cast<double>(n) / secs);
        }
        r.checksum = post_chunk(r.checksum);
        if (max_messages != 0 && r.messages >= max_messages) {
            break;
        }
    }
    const Usage u1 = usage_now();
    r.use_delta    = {u1.minflt - u0.minflt, u1.majflt - u0.majflt, u1.nvcsw - u0.nvcsw,
                      u1.nivcsw - u0.nivcsw};
    return r;
}

// ---- JSON -------------------------------------------------------------------------------

std::string num(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6g", v);
    return buf;
}

struct Percentiles {
    double        p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0, min = 0, mean = 0;
    std::uint64_t count = 0, zero_ticks = 0, saturated = 0;
};

Percentiles summarize(Histogram& h) {
    Percentiles p;
    if (h.empty()) {
        return p;
    }
    const Clock& c = Clock::instance();
    p.count        = h.count();
    p.p50          = c.ticks_to_ns(h.percentile(50));
    p.p90          = c.ticks_to_ns(h.percentile(90));
    p.p99          = c.ticks_to_ns(h.percentile(99));
    p.p999         = c.ticks_to_ns(h.percentile(99.9));
    p.p9999        = c.ticks_to_ns(h.percentile(99.99));
    p.max          = c.ticks_to_ns(h.max());
    p.min          = c.ticks_to_ns(h.min());
    p.mean         = c.ticks_to_ns(1) * h.mean();
    p.saturated    = h.saturated();
    for (const std::uint32_t s : h.samples()) {
        if (s != 0) {
            break;  // sorted by now
        }
        ++p.zero_ticks;
    }
    return p;
}

std::string pct_json(const Percentiles& p) {
    std::ostringstream o;
    o << "{\"count\": " << p.count << ", \"p50_ns\": " << num(p.p50)
      << ", \"p90_ns\": " << num(p.p90) << ", \"p99_ns\": " << num(p.p99)
      << ", \"p99_9_ns\": " << num(p.p999) << ", \"p99_99_ns\": " << num(p.p9999)
      << ", \"max_ns\": " << num(p.max) << ", \"min_ns\": " << num(p.min)
      << ", \"mean_ns\": " << num(p.mean) << ", \"samples_at_zero_ticks\": " << p.zero_ticks
      << ", \"saturated\": " << p.saturated << "}";
    return o.str();
}

std::string clock_json() {
    const Clock&       c = Clock::instance();
    std::ostringstream o;
    o << "{\"counter_hz\": " << c.counter_hz() << ", \"ns_per_tick\": " << num(c.ns_per_tick())
      << ", \"granularity_median_ns\": " << num(c.resolution_ns())
      << ", \"granularity_min_ns\": " << num(c.resolution_min_ns())
      << ", \"overhead_unserialized_ns\": " << num(c.overhead_ns_raw())
      << ", \"overhead_serialized_ns\": " << num(c.overhead_ns_serialized()) << "}";
    return o.str();
}

std::string chunks_json(const std::vector<double>& rates) {
    std::vector<double> v = rates;
    std::sort(v.begin(), v.end());
    std::ostringstream o;
    o << "{\"chunk_messages\": " << kChunk << ", \"count\": " << v.size();
    if (!v.empty()) {
        o << ", \"min_msgs_per_sec\": " << num(v.front())
          << ", \"p10_msgs_per_sec\": " << num(v[v.size() / 10])
          << ", \"median_msgs_per_sec\": " << num(v[v.size() / 2])
          << ", \"p90_msgs_per_sec\": " << num(v[v.size() * 9 / 10])
          << ", \"max_msgs_per_sec\": " << num(v.back());
    }
    o << "}";
    return o.str();
}

// ---- driver -----------------------------------------------------------------------------

// The input as the timed loop sees it: the whole mapped file, or the symbol-filtered copy.
struct Input {
    std::span<const std::byte> data;
    std::vector<std::byte>     filtered;  // owns `data` when a filter is in force
    std::vector<std::uint16_t> locates;   // the filtered symbols' locates
    std::vector<std::string>   names;
    std::uint64_t              source_messages = 0;
    std::uint64_t              kept_messages   = 0;
};

void build_filtered(const itch::MappedFile& mf, const std::vector<std::string>& names, Input& in) {
    const itch::Resolved res = itch::resolve_symbols(mf.bytes(), names);
    if (!res.unresolved.empty()) {
        throw InputError("symbol not in this file's stock directory: " + res.unresolved.front());
    }
    std::vector<bool> want(65536, false);
    for (const auto& [name, loc] : res.locate) {
        want[loc] = true;
        in.locates.push_back(loc);
        in.names.push_back(name);
    }
    itch::Framer               fr(mf.bytes());
    std::span<const std::byte> msg;
    in.filtered.reserve(std::size_t{1} << 28);
    while (fr.next(msg) == itch::FrameStatus::Ok) {
        ++in.source_messages;
        if (msg.size() >= 11 && want[itch::HeaderView(msg.data()).locate()]) {
            const std::byte* start = msg.data() - 2;  // the length prefix is part of the frame
            in.filtered.insert(in.filtered.end(), start, start + msg.size() + 2);
            ++in.kept_messages;
        }
    }
    in.data = std::span<const std::byte>(in.filtered.data(), in.filtered.size());
}

// Median of the first 1000 add prices per locate: the "reference price" a ladder is centred
// on. It gives the ladder the best chance; the first order of the day is often a stub quote.
std::vector<std::pair<std::uint16_t, std::int32_t>> ladder_bases(const Input& in) {
    std::vector<std::vector<std::int32_t>> px(65536);
    itch::Framer                           fr(in.data);
    std::span<const std::byte>             msg;
    while (fr.next(msg) == itch::FrameStatus::Ok) {
        if (msg.size() == itch::AddOrderView::kSize && static_cast<char>(msg[0]) == 'A') {
            const itch::AddOrderView v(msg.data());
            auto&                    vec = px[v.locate()];
            if (vec.size() < 1000) {
                vec.push_back(static_cast<std::int32_t>(v.price()));
            }
        }
    }
    std::vector<std::pair<std::uint16_t, std::int32_t>> out;
    for (const std::uint16_t loc : in.locates) {
        auto& v = px[loc];
        if (!v.empty()) {
            std::sort(v.begin(), v.end());
            out.emplace_back(loc, v[v.size() / 2]);
        }
    }
    return out;
}

std::uint64_t count_type(const std::vector<std::uint64_t>& counts, char c) {
    return counts[static_cast<unsigned char>(c)];
}

struct Report {
    const Options&          o;
    const Input&            in;
    const itch::MappedFile* mf;
    Footprint               fp_before_tables, fp_after_setup, fp_end;
    std::size_t             table_pool_bytes = 0, table_index_bytes = 0, books_array_bytes = 0;
    Results                 r;
    std::string             extra;  // mode-specific members, already formatted
    std::uint64_t           peak_live = 0, live_end = 0;
    std::string             anomalies_json;
    std::uint64_t           prefault_ns = 0;
};

void write_report(const Report& rp) {
    const Options&         o = rp.o;
    std::ostringstream     js;
    const itch::Provenance prov = itch::make_provenance(o.file);
    const double           secs = Clock::instance().ticks_to_ns(rp.r.timed_ticks) * 1e-9;
    js << "{\n  \"meta\": " << itch::provenance_json(prov, 2) << ",\n";
    js << "  \"config\": {\"mode\": \"" << o.mode << "\", \"map\": \"" << o.map
       << "\", \"book\": \"" << o.book << "\", \"max_live_orders\": " << o.max_live
       << ", \"max_messages\": " << o.max_messages << ", \"symbols\": [";
    for (std::size_t i = 0; i < rp.in.names.size(); ++i) {
        js << (i != 0 ? ", " : "") << "\"" << rp.in.names[i] << "\"";
    }
    js << "], \"chunk_messages\": " << kChunk << "},\n";
    js << "  \"input\": {\"file_bytes\": " << rp.mf->bytes().size()
       << ", \"source_messages\": " << rp.in.source_messages
       << ", \"kept_messages\": " << rp.in.kept_messages << ", \"timed_bytes\": " << rp.r.bytes
       << "},\n";
    js << "  \"clock\": " << clock_json() << ",\n";
    js << "  \"result\": {\"messages\": " << rp.r.messages << ", \"handled\": " << rp.r.handled
       << ", \"skipped\": " << rp.r.skipped << ", \"wrong_length\": " << rp.r.wrong_length
       << ", \"timed_seconds\": " << num(secs)
       << ", \"msgs_per_sec\": " << num(secs > 0 ? static_cast<double>(rp.r.messages) / secs : 0)
       << ", \"ns_per_msg\": "
       << num(rp.r.messages != 0 ? secs * 1e9 / static_cast<double>(rp.r.messages) : 0)
       << ", \"mbytes_per_sec\": "
       << num(secs > 0 ? static_cast<double>(rp.r.bytes) / secs / 1e6 : 0)
       << ", \"state_checksum\": \"" << std::hex << rp.r.checksum << std::dec << "\""
       << ", \"peak_live_orders\": " << rp.peak_live << ", \"live_orders_end\": " << rp.live_end
       << ", \"anomalies\": " << (rp.anomalies_json.empty() ? "null" : rp.anomalies_json)
       << ", \"throughput_by_chunk\": " << chunks_json(rp.r.chunk_rate) << "},\n";
    js << "  \"prefault\": {\"passes\": " << g_prefault.passes
       << ", \"last_pass_major_faults\": " << g_prefault.last_pass_major_faults << "},\n";
    js << "  \"timed_span\": {\"minor_faults\": " << rp.r.use_delta.minflt
       << ", \"major_faults\": " << rp.r.use_delta.majflt
       << ", \"voluntary_ctx_switches\": " << rp.r.use_delta.nvcsw
       << ", \"involuntary_ctx_switches\": " << rp.r.use_delta.nivcsw << "},\n";
    js << "  \"memory\": {\"max_rss_bytes\": " << max_rss_bytes()
       << ", \"max_rss_includes\": \"the mapped input file's resident pages as well as the tables\""
       << ",\n    \"footprint_before_tables\": " << footprint_json(rp.fp_before_tables)
       << ",\n    \"footprint_after_setup\": " << footprint_json(rp.fp_after_setup)
       << ",\n    \"footprint_end\": " << footprint_json(rp.fp_end)
       << ",\n    \"order_pool_bytes\": " << rp.table_pool_bytes
       << ", \"id_index_bytes\": " << rp.table_index_bytes
       << ", \"books_array_bytes\": " << rp.books_array_bytes << "}";
    if (!rp.extra.empty()) {
        js << ",\n  " << rp.extra;
    }
    js << "\n}\n";
    if (o.json.empty()) {
        std::fputs(js.str().c_str(), stdout);
    } else {
        std::ofstream f(o.json);
        if (!f) {
            throw InputError("cannot write " + o.json);
        }
        f << js.str();
    }
}

// Runs one throughput or latency pass with a concrete Index and BookT.
template <class Index, class BookT>
int run_builder(const Options& o, const itch::MappedFile& mf, Input& in) {
    Report rp{o, in, &mf, {}, {}, {}, 0, 0, 0, {}, {}, 0, 0, {}, 0};
    rp.fp_before_tables = footprint_now();

    book::NullObserver                                  obs;
    book::BookBuilder<book::NullObserver, Index, BookT> b(o.max_live, obs);

    // Ladder bases and allocation (setup): the lazy first-order allocation would otherwise be
    // paid inside the timed span, and the 3 MB arrays pre-faulted in the other arm.
    if constexpr (std::is_same_v<BookT, bench::LadderBook>) {
        for (const auto& [loc, px] : ladder_bases(in)) {
            b.book_for_setup(loc).set_base(px);
        }
        for (const std::uint16_t loc : in.locates) {
            b.book_for_setup(loc).allocate();
        }
    }
    rp.fp_after_setup   = footprint_now();
    rp.table_pool_bytes = o.max_live * sizeof(ob::Order);
    rp.books_array_bytes =
        book::BookBuilder<book::NullObserver, Index, BookT>::kLocates * sizeof(BookT);
    if constexpr (std::is_same_v<Index, ob::IdIndex>) {
        std::size_t cap = 1;
        while (cap < o.max_live * 2) {
            cap <<= 1;
        }
        rp.table_index_bytes = cap * 16;
    }

    const auto post = [&](std::uint64_t h) { return fold(b, h); };

    if (o.mode == "apply") {
        rp.r = run_throughput(in.data, o.max_messages, b, true, post);
    } else {  // latency
        // Count per type first (setup) so every histogram is reserved exactly.
        std::vector<std::uint64_t> counts(256, 0);
        std::uint64_t              total = 0;
        {
            itch::Framer               fr(in.data);
            std::span<const std::byte> msg;
            while (fr.next(msg) == itch::FrameStatus::Ok &&
                   (o.max_messages == 0 || total < o.max_messages)) {
                ++counts[static_cast<unsigned char>(msg[0])];
                ++total;
            }
        }
        Histogram                                   all(total);
        std::array<std::unique_ptr<Histogram>, 256> per;
        for (std::size_t t = 0; t < 256; ++t) {
            if (counts[t] != 0) {
                per[t] = std::make_unique<Histogram>(counts[t]);
            }
        }
        Histogram floor_h(5'000'000);
        rp.fp_after_setup = footprint_now();

        itch::Framer               fr(in.data);
        std::span<const std::byte> msg;
        const Clock&               clk = Clock::instance();
        const Usage                u0  = usage_now();
        std::uint64_t              n = 0, hd = 0, sk = 0, wl = 0, by = 0, sum_ticks = 0;
        std::uint64_t              h = 0;
        while (n < total && fr.next(msg) == itch::FrameStatus::Ok) {
            const auto          type = static_cast<unsigned char>(msg[0]);
            const std::uint64_t t0   = Clock::raw_serialized();
            const auto          st   = itch::decode(msg, b);
            const std::uint64_t t1   = Clock::raw_serialized();
            const std::uint64_t d    = t1 - t0;
            sum_ticks += d;
            all.record(d);
            per[type]->record(d);
            ++n;
            by += msg.size() + 2;
            hd += st == itch::DecodeStatus::Handled;
            sk += st == itch::DecodeStatus::Skipped;
            wl += st == itch::DecodeStatus::WrongLength;
            if ((n & (kChunk - 1)) == 0) {
                h = fold(b, h);
            }
        }
        const Usage u1 = usage_now();
        // The timer on an empty region, same instructions, nothing between them.
        for (std::size_t i = 0; i < 5'000'000; ++i) {
            const std::uint64_t a = Clock::raw_serialized();
            const std::uint64_t c = Clock::raw_serialized();
            floor_h.record(c - a);
        }
        rp.r.messages     = n;
        rp.r.handled      = hd;
        rp.r.skipped      = sk;
        rp.r.wrong_length = wl;
        rp.r.bytes        = by;
        rp.r.timed_ticks  = sum_ticks;  // sum of per-message windows, NOT wall time of the loop
        rp.r.checksum     = fold(b, h);
        rp.r.use_delta    = {u1.minflt - u0.minflt, u1.majflt - u0.majflt, u1.nvcsw - u0.nvcsw,
                             u1.nivcsw - u0.nivcsw};
        (void)clk;

        std::ostringstream ex;
        const Percentiles  pa = summarize(all);
        const Percentiles  pf = summarize(floor_h);
        ex << "\"latency\": {\n    \"definition\": \"ticks between two raw_serialized() reads "
              "around decode+apply of one framed message; framing is outside the window; "
              "timer cost is NOT subtracted (see timer_floor)\",\n";
        ex << "    \"all\": " << pct_json(pa) << ",\n";
        ex << "    \"fraction_samples_at_zero_ticks\": "
           << num(pa.count != 0 ? static_cast<double>(pa.zero_ticks) / static_cast<double>(pa.count)
                                : 0)
           << ",\n    \"timer_floor\": " << pct_json(pf) << ",\n    \"by_type\": {";
        bool first = true;
        for (std::size_t t = 0; t < 256; ++t) {
            if (per[t]) {
                ex << (first ? "\n      " : ",\n      ") << "\"" << static_cast<char>(t)
                   << "\": " << pct_json(summarize(*per[t]));
                first = false;
            }
        }
        ex << "\n    }\n  }";
        rp.extra = ex.str();
        (void)count_type;
    }

    rp.fp_end    = footprint_now();
    rp.peak_live = b.peak_live_orders();
    rp.live_end  = b.live_orders();
    {
        const book::Anomalies& a = b.anomalies();
        std::ostringstream     an;
        an << "{\"unknown_ref\": " << a.unknown_ref << ", \"duplicate_add\": " << a.duplicate_add
           << ", \"locate_mismatch\": " << a.locate_mismatch
           << ", \"over_execute\": " << a.over_execute << ", \"over_cancel\": " << a.over_cancel
           << ", \"zero_shares\": " << a.zero_shares << "}";
        rp.anomalies_json = an.str();
    }

    // What the books and the index hold at the end, per variant.
    std::ostringstream st;
    if constexpr (std::is_same_v<BookT, book::Book>) {
        std::size_t level_bytes = 0, with_levels = 0;
        for (std::size_t loc = 0; loc < 65536; ++loc) {
            const std::size_t rb = b.book(static_cast<std::uint16_t>(loc)).reserved_bytes();
            level_bytes += rb;
            with_levels += rb != 0;
        }
        st << "\"book_storage\": {\"kind\": \"vector\", \"level_vector_bytes_high_water\": "
           << level_bytes << ", \"books_with_levels\": " << with_levels << "}";
    } else if constexpr (std::is_same_v<BookT, bench::LadderBook>) {
        std::uint64_t      in_w = 0, over = 0, alloc = 0;
        std::ostringstream per_sym;
        bool               first = true;
        for (const std::uint16_t loc : in.locates) {
            const auto& bk = b.book(loc);
            in_w += bk.in_window_adds();
            over += bk.overflow_adds();
            alloc += bk.allocated() ? 1 : 0;
            per_sym << (first ? "" : ", ") << "{\"locate\": " << loc
                    << ", \"in_window_adds\": " << bk.in_window_adds()
                    << ", \"overflow_adds\": " << bk.overflow_adds() << "}";
            first = false;
        }
        st << "\"book_storage\": {\"kind\": \"ladder\", \"ladder_bytes_per_symbol\": "
           << bench::LadderBook::kLadderBytes << ", \"symbols_allocated\": " << alloc
           << ", \"adds_in_window\": " << in_w << ", \"adds_in_overflow_map\": " << over
           << ", \"per_symbol\": [" << per_sym.str() << "]}";
    } else {
        st << "\"book_storage\": {\"kind\": \"none\"}";
    }
    if constexpr (std::is_same_v<Index, StdMapIndex>) {
        // libc++ unordered_map: bucket array is exact; nodes are malloc'd one by one and their
        // size is not exposed, so it is left to the footprint figures.
        st << ",\n  \"index_storage\": {\"kind\": \"std::unordered_map\", \"reserved_buckets\": "
           << "\"see footprint\"}";
    }
    rp.extra = rp.extra.empty() ? st.str() : rp.extra + ",\n  " + st.str();

    write_report(rp);
    do_not_optimize(rp.r.checksum);
    return 0;
}

int run(const Options& o) {
    // QOS is the only scheduling hint macOS offers. There is no core affinity API.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    const itch::MappedFile mf = itch::MappedFile::open(o.file);
    Input                  in;
    in.data = mf.bytes();

    // MappedFile::open advises MADV_SEQUENTIAL, which on macOS releases pages behind the read
    // position, so a pre-fault pass would be undone before the timed span begins (measured: tens
    // of thousands of major faults inside the timed loop). Reset the advice, then pre-fault, and
    // report the fault counts of the timed span so the claim can be checked.
    (void)::madvise(const_cast<std::byte*>(mf.bytes().data()), mf.bytes().size(), MADV_NORMAL);
    // One pass is not enough when the file is a large share of RAM: the kernel can evict the head
    // while the tail is being touched. Repeat until a pass takes few major faults.
    Prefault pf;
    for (; pf.passes < 8;) {
        const Usage a = usage_now();
        mf.prefault();
        ++pf.passes;
        pf.last_pass_major_faults = usage_now().majflt - a.majflt;
        if (pf.last_pass_major_faults < 64) {
            break;
        }
    }
    g_prefault = pf;
    if (!o.symbols.empty()) {
        build_filtered(mf, o.symbols, in);
    }

    if (o.mode == "frame" || o.mode == "parse") {
        Report rp{o, in, &mf, {}, {}, {}, 0, 0, 0, {}, {}, 0, 0, {}, 0};
        rp.fp_before_tables = rp.fp_after_setup = footprint_now();
        ParseSink sink;
        rp.r          = run_throughput(in.data, o.max_messages, sink, o.mode == "parse",
                                       [](std::uint64_t h) { return h; });
        rp.r.checksum = sink.acc;
        do_not_optimize(sink.acc);
        rp.fp_end = footprint_now();
        write_report(rp);
        return 0;
    }

    using book::Book;
    if (o.map == "index") {
        if (o.book == "vector") {
            return run_builder<ob::IdIndex, Book>(o, mf, in);
        }
        if (o.book == "ladder") {
            return run_builder<ob::IdIndex, bench::LadderBook>(o, mf, in);
        }
        return run_builder<ob::IdIndex, NullBook>(o, mf, in);
    }
    if (o.book == "vector") {
        return run_builder<StdMapIndex, Book>(o, mf, in);
    }
    if (o.book == "ladder") {
        return run_builder<StdMapIndex, bench::LadderBook>(o, mf, in);
    }
    return run_builder<StdMapIndex, NullBook>(o, mf, in);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options o = parse_args(argc, argv);
        return run(o);
    } catch (const InputError& e) {
        std::fprintf(stderr, "bench_feed: %s\n", e.what());
        usage();
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bench_feed: %s\n", e.what());
        return 3;
    }
}
