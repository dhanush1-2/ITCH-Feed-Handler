#!/usr/bin/env python3
"""Fill the numbers in the documents from the committed results/ files.

    gen_docs.py            rewrite every <!-- gen:NAME -->...<!-- /gen --> block
    gen_docs.py --check    exit 1 and name each block that differs; write nothing

A document holds no measured number of its own: the text between the markers belongs to this
script. Block names are the keys of BLOCKS below; an unknown name or an unclosed block is an
error (exit 2). It reads results/*.json and docs/images/ only, so it runs in CI, which has no
data files. Standard library only.
"""
import argparse
import importlib.util
import json
import re
import sys
from pathlib import Path

# The owner's GitHub "user/repo". Empty until there is a public repository: while it is empty
# the README has no CI badge and no dashboard link. Nothing derives it.
REPO_SLUG = ""

DOCS = ["README.md", "DESIGN.md", "docs/VERIFICATION.md", "docs/DESIGN-QA.md",
        "docs/OPTIMIZATION-LOG.md", "docs/METHODOLOGY.md"]
# Gitignored notes. Generated when present, and absent in CI.
PRIVATE_DOCS = ["private/RESUME.md"]
BLOCK = re.compile(r"<!-- gen:([\w.]+) -->(.*?)<!-- /gen -->", re.S)
OPEN = "<!-- gen:"
CLOSE = "<!-- /gen -->"

# Images that can only exist once the project is public. Each appears in the README when its
# file does, and not before.
PUBLIC_PROOFS = [
    ("proof-ci-green.png", "CI, every job green on the public repository."),
    ("dashboard-deployed.png", "The dashboard as deployed on GitHub Pages."),
]

CLOSING_CROSS_S = (15 * 3600 + 59 * 60, 16 * 3600 + 60)  # 15:59:00 to 16:01:00


class DocError(Exception):
    pass


def documents(root: Path) -> list:
    return DOCS + [rel for rel in PRIVATE_DOCS if (root / rel).exists()]


def clock(seconds: int) -> str:
    return f"{seconds // 3600:02d}:{seconds // 60 % 60:02d}:{seconds % 60:02d}"


def n(value) -> str:
    return f"{value:,}"


def mps(value) -> str:
    return f"{value / 1e6:.2f} M"


def mb(value) -> str:
    return f"{value / 1e6:.1f} MB"


def gb(value) -> str:
    return f"{value / 1e9:.2f} GB"


def table(head, rows) -> str:
    lines = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
    lines += ["| " + " | ".join(str(c) for c in row) + " |" for row in rows]
    return "\n\n" + "\n".join(lines) + "\n\n"


def link(name: str) -> str:
    return f"[`{name}`](results/{name})"


def lag_text(replay: dict) -> str:
    """The paced replay's worst lag. Attributes it to a time only if the file records one, and to
    the closing cross only if that time is at the close."""
    lag = replay.get("max_lag_ns", 0)
    speed = replay["speed"]
    if not lag:
        return f"At {speed}x the replay never fell behind its schedule at a clock reading."
    text = f"At {speed}x the worst sampled lag behind schedule was {lag / 1e6:.1f} ms"
    at = replay.get("max_lag_at_feed_s")
    if at is None:
        text += "; this results file does not record where in the feed it occurred"
    else:
        text += f", at feed time {clock(int(at))}"
        if CLOSING_CROSS_S[0] <= at <= CLOSING_CROSS_S[1]:
            text += ", during the closing cross"
    return text + f". {speed}x was not held at every moment."


def badges(slug: str) -> str:
    if not slug:
        return ""
    owner, repo = slug.split("/", 1)
    return (f"\n\n[![CI](https://github.com/{slug}/actions/workflows/ci.yml/badge.svg)]"
            f"(https://github.com/{slug}/actions/workflows/ci.yml) "
            f"[Live dashboard](https://{owner}.github.io/{repo}/)\n\n")


def public_proofs(root: Path) -> str:
    shown = [(f, c) for f, c in PUBLIC_PROOFS if (root / "docs" / "images" / f).is_file()]
    if not shown:
        return ""
    return "\n\n" + "\n\n".join(f"{c}\n\n![{c}](docs/images/{f})" for f, c in shown) + "\n\n"


class Results:
    """The committed results, read once each."""

    def __init__(self, root: Path):
        self.root = root
        self._cache = {}

    def __getitem__(self, name: str) -> dict:
        if name not in self._cache:
            path = self.root / "results" / f"{name}.json"
            if not path.exists():
                raise DocError(f"missing {path}")
            self._cache[name] = json.loads(path.read_text())
        return self._cache[name]


def build_blocks(r: Results) -> dict:
    day = lambda: r["day_12302019"]
    d25 = lambda: r["day_20251208"]
    bench = lambda: r["bench_12302019"]["results"]
    before = lambda: r["bench_12302019_before_optimization"]["results"]
    full = lambda: r["loss_full_day"]
    lossless = lambda: r["loss_full_day_lossless"]
    tbl = lambda: r["loss_table"]
    udp = lambda: r["udp_demo"]
    replay = lambda: r["replay_10x"]["replay"]
    recon = lambda: r["recon_12302019"]
    recon25 = lambda: r["recon_20251208"]

    def median(key):
        return bench()[key]["msgs_per_sec"]["median"]

    def lat(key):
        return bench()["latency"]["latency_all_ns"][key]["median"]

    def known_answer():
        k = r["known_answer_summary"]
        return {"symbols": k["symbols"], "same": k["identical_symbols"] == k["symbols"],
                "populated": k["resting_orders_per_checkpoint"]}

    def known_orders():
        pop = known_answer()["populated"]
        return next(v for k, v in sorted(pop.items()) if k != "eod")

    def closes():
        rows = recon()["crosses"]["closing_prices"]
        return sum(1 for x in rows if x["difference"] == 0), len(rows)

    def ladder_bytes():
        ladder = bench()["apply_ten_symbols_ladder"]["book_storage"]["ladder_bytes_per_symbol"]
        return ladder * bench()["apply"]["book_storage"]["books_with_levels"]

    def fuzz_run(target):
        return next(x for x in r["fuzz"]["runs"] if x["target"] == target)

    def loss_row(label):
        return next(x for x in tbl()["rows"] if x["label"] == label)

    def paced_export():
        rec = r["replay_max_compare"]
        verdict = "equals" if rec["matches"] else "DIFFERS FROM"
        return (f"Its export {verdict} a max-speed run's: the per-symbol share ledger, and top of "
                f"book each second for {len(rec['watched_symbols'])} symbols.")

    def crossed_scope():
        g = recon()["crossed"]["halt_grace"]
        return (f"with a {day()['verify']['halt_grace_s']} s grace after a resume "
                f"({n(g['crossed_touches_after_resume_before_cross'])} crossed touches at 0 s of "
                f"grace, all within {g['max_s_after_resume'] * 1000:.3f} ms of a resume)")

    def latency_caveat():
        t = bench()["latency"]
        return (f"From a separately timed run at {mps(t['msgs_per_sec']['median'])} msgs/s, "
                f"{t['ns_per_msg_median']:.1f} ns per message, with two clock reads per message; "
                "throughput comes from runs with no per-message clock")

    def proof_table():
        ka = known_answer()
        ok, total = closes()
        f = full()
        return table(["Claim", "Result", "File", "Reproduce"], [
            ["Full real day replayed and checked",
             f"{n(day()['totals']['messages'])} messages, `unknown_ref` "
             f"{day()['anomalies']['unknown_ref']}, conservation holds, "
             f"{n(day()['verify']['full_checks'])} whole-state checks, no book crossed in "
             "continuous trading " + crossed_scope(),
             link("day_12302019.json"),
             "`itch_replay data/12302019.NASDAQ_ITCH50 --verify`"],
            ["Second day, six years later",
             f"{n(d25()['totals']['messages'])} messages, `unknown_ref` "
             f"{d25()['anomalies']['unknown_ref']}, peak {n(d25()['peak_live_orders'])} live "
             "orders; no new message types",
             link("day_20251208.json"),
             "`itch_replay data/S120825-v50.txt --verify`"],
            ["Agrees with a separate Python reference",
             f"{ka['symbols']} symbols, {'identical' if ka['same'] else 'DIFFERENT'} field for "
             f"field, {n(known_orders())} resting orders at noon, "
             f"{known_orders() / f['dump']['resting_orders'] * 100:.1f}% of noon orders, 2019 only. "
             "No shared code, same author; shares the queue rule",
             link("known_answer_summary.json") + "; its inputs hold resting orders, so they are "
             "reproducible and not committed",
             "`itch_replay <day> --export-orders <symbols> results/cpp_subset_12302019.json`, "
             "`verify/reference_book.py`, then `verify/compare.py`"],
            ["Bit-identical books after injected loss",
             f"{n(f['channel']['messages_lost_downstream'])} messages lost "
             f"(`messages_lost_downstream`) and recovered; state at feed second "
             f"{n(f['dump']['at_second'])} ({n(f['dump']['resting_orders'])} resting orders) and "
             "stream hash both equal the lossless run. The end-of-day book is empty, so it "
             "proves nothing",
             link("loss_full_day.json") + ", " + link("loss_full_day_lossless.json"),
             "`mold_sim <day> --loss 0.01 --dup 0.005 --reorder 16 --burst 0.0005,64 "
             "--retransmit-loss 0.01 --seed 1 --retention 1048576 --dump-state s.bin "
             "--dump-at-s 43200`, then `cmp`"],
            ["Throughput, single thread, file replay",
             f"{mps(median('apply'))} msgs/s applied, {mps(median('parse'))} msgs/s parsed, on "
             f"{r['bench_12302019']['meta']['cpu']}",
             link("bench_12302019.json"), "`scripts/run_bench.py --day 12302019`"],
            ["Per-message latency shape",
             f"p50 {lat('p50_ns')} ns, p99 {lat('p99_ns')} ns, p99.9 {lat('p99_9_ns')} ns, "
             f"quantized at {bench()['latency']['clock']['granularity_median_ns']} ns. "
             + latency_caveat(),
             link("bench_12302019.json"), "`scripts/run_bench.py --day 12302019`"],
            ["Paced replay gives the same export as a max-speed run",
             f"{replay()['speed']}x, {replay()['paced_wall_s'] / 3600:.2f} h of wall time. "
             + paced_export() + " The 10x run takes no state dump",
             link("replay_10x.json") + ", " + link("replay_max_compare.json"),
             "`itch_replay <day> --speed 10 --symbols QQQ,SPY,AAPL,AMD,MSFT --export out.json`, "
             "then `scripts/record_replay_compare.py`"],
            ["Recovery over real sockets, loopback only",
             f"First {n(udp()['source']['messages'])} messages at "
             f"{n(udp()['config']['rate_mps'])} msgs/s, stream and state identical",
             link("udp_demo.json"),
             "`mold_udp_demo <day> --max-messages 5000000 --drop 0.01 --seed 1 --rate-mps 1000000 "
             "--dump-state s.bin --dump-at-s 25000 --check-direct`"],
            ["Parser and receiver fuzzed (libFuzzer, in Docker)",
             f"`fuzz_parser` {n(fuzz_run('fuzz_parser')['executed_units'])} executions, "
             f"`fuzz_mold` {n(fuzz_run('fuzz_mold')['executed_units'])} executions, "
             f"{fuzz_run('fuzz_parser')['crashes'] + fuzz_run('fuzz_mold')['crashes']} crashes",
             link("fuzz.json"), "`scripts/fuzz.sh fuzz_parser 300`"],
            ["Closing prices against public closes",
             f"{ok} of {total} equal on the raw 2019 basis. Rests mostly on Yahoo; the CMCSA "
             "split factor is unconfirmed",
             link("recon_12302019.json"), "`verify/recon_summary.py build/r_day.pickle build/recon_match.json "
             "verify/closing_sources.json results/halt_grace_12302019.json "
             "--out results/recon_12302019.json`"],
        ])

    def bench_table():
        b = bench()
        m = r["bench_12302019"]["machine"]
        meta = r["bench_12302019"]["meta"]
        mem = b["apply"]["memory"]
        tables = mem["order_pool_bytes"] + mem["id_index_bytes"] + mem["books_array_bytes"]
        return table(["Metric", "Value"], [
            ["Messages parsed/s (parse only)", f"{mps(median('parse'))}"],
            ["Messages applied/s (parse + book)",
             f"{mps(median('apply'))} (spread {b['apply']['msgs_per_sec']['spread'] * 100:.1f}%)"],
            ["p50 / p99 / p99.9 per-message apply latency",
             f"{lat('p50_ns')} / {lat('p99_ns')} / {lat('p99_9_ns')} ns"],
            ["How the latency was timed", latency_caveat()],
            ["Peak live orders", n(b["apply"]["peak_live_orders"])],
            ["Handler tables (order pool, id index, books array)",
             f"{mb(tables)} at setup, at a benchmark capacity of "
             f"{n(mem['order_pool_bytes'] // 32)} orders"],
            ["Heap added by the end of the run, over the input", mb(mem["heap_added_at_end_bytes_median"])],
            ["`max_rss`, which includes the mapped input file",
             f"{gb(mem['max_rss_bytes_median'])} (input {gb(mem['mapped_input_bytes'])}); not "
             "handler memory"],
            ["CPU, compiler, flags",
             f"{m['cpu']}, {meta['compiler']}, `{meta['flags']}`"],
            ["Pinning", "None. macOS has no core affinity API"],
        ])

    def latency_by_type():
        by = bench()["latency"]["latency_by_type_ns"]
        rows = []
        for kind in ("A", "F", "E", "C", "X", "D", "U"):
            x = by[kind]
            rows.append([f"`{kind}`", n(x["count"]), x["p50_ns"]["median"], x["p99_ns"]["median"],
                         x["p99_9_ns"]["median"]])
        return table(["Type", "Messages", "p50 ns", "p99 ns", "p99.9 ns"], rows)

    def ladder_table():
        b = bench()
        return table(["Book, ten busiest symbols", "msgs/s", "Spread"], [
            ["Sorted vector (production)", mps(median("apply_ten_symbols_vector")),
             f"{b['apply_ten_symbols_vector']['msgs_per_sec']['spread'] * 100:.1f}%"],
            ["Project 1 ladder", mps(median("apply_ten_symbols_ladder")),
             f"{b['apply_ten_symbols_ladder']['msgs_per_sec']['spread'] * 100:.1f}%"],
        ])

    def index_table():
        b = bench()
        rows = []
        for label, apply_key, lat_key in (
                ("IdIndex (open addressing, blocked hash)", "apply", "latency"),
                ("std::unordered_map", "apply_unordered_map", "latency_unordered_map")):
            x = b[lat_key]["latency_all_ns"]
            rows.append([label, mps(median(apply_key)),
                         f"{b[apply_key]['msgs_per_sec']['spread'] * 100:.1f}%",
                         f"{x['p50_ns']['median']} / {x['p99_ns']['median']} / "
                         f"{x['p99_9_ns']['median']}"])
        return table(["", "msgs/s (median of 5)", "spread",
                      f"latency p50 / p99 / p99.9 (ns, {b['latency']['clock']['granularity_median_ns']} ns "
                      "granularity)"], rows)

    def loss_table():
        rows = []
        for row in tbl()["rows"]:
            c, ch, rx = row["config"], row["channel"], row["rx"]
            rows.append([row["label"], n(ch["downstream_dropped"]),
                         n(ch["messages_lost_downstream"]), n(rx["gaps_detected"]),
                         "yes" if row["result"]["stream_matches_source"] else "NO",
                         "reference" if row["kind"] == "baseline" else
                         ("yes" if row["dump"]["matches_direct"] else "NO")])
        return table(["Run (first " + n(tbl()["source"]["messages"]) + " messages)",
                      "Packets dropped", "`messages_lost_downstream`", "Gaps detected",
                      "Stream hash equal", "State equal"], rows)

    def fuzz_table():
        rows = []
        for x in r["fuzz"]["runs"]:
            rows.append([f"`{x['target']}`", f"{x['duration_s']} s", n(x["executed_units"]),
                         x["crashes"], f"`{x['meta']['commit'][:7]}`"])
        return table(["Target", "Requested time", "Executions", "Crashes", "Commit"], rows)

    def provenance():
        published = ["census_12302019", "day_12302019", "day_20251208", "known_answer_summary",
                     "recon_12302019", "recon_20251208", "loss_table", "loss_full_day",
                     "loss_full_day_lossless", "udp_demo", "bench_12302019", "replay_10x",
                     "replay_max_compare", "recon_12302019_arrival_queue",
                     "recon_20251208_arrival_queue", "bench_12302019_before_optimization"]
        commits = {r[x]["meta"]["commit"][:7] for x in published}
        commits |= {x["meta"]["commit"][:7] for x in r["fuzz"]["runs"]}
        commits |= {x["meta"]["commit"][:7] for x in r["halt_grace_12302019"]["runs"]}
        return (f"The current result files come from {len(commits)} commit"
                f"{'' if len(commits) == 1 else 's'}: "
                + ", ".join(f"`{c}`" for c in sorted(commits)) + ". The kScan screening and the "
                "profile come from patched builds and say so in their own files.")

    def resume_bullets():
        meta = r["bench_12302019"]["meta"]
        f = full()
        ka = known_answer()
        return ("\n\n"
                f"- Built a C++20 NASDAQ ITCH 5.0 feed handler reconstructing full-depth books for "
                f"{n(bench()['apply']['book_storage']['books_with_levels'])} symbols at "
                f"{median('apply') / 1e6:.2f} M msgs/sec on {meta['cpu']} (separately timed run: "
                f"{lat('p99_ns')} ns p99, {bench()['latency']['clock']['granularity_median_ns']} ns "
                "clock tick), replaying one recorded day of {n(day()['totals']['messages'])} "
                "messages from a file on one thread.\n"
                "- Implemented MoldUDP64 sequence-gap detection and retransmission recovery; "
                f"under injected loss, reordering and duplication "
                f"({n(f['channel']['messages_lost_downstream'])} messages lost and recovered in "
                f"simulation), the books at feed second {n(f['dump']['at_second'])} "
                f"({n(f['dump']['resting_orders'])} resting orders) and the delivered-stream hash "
                "were bit-identical to the lossless run.\n"
                f"- Validated against an independent Python reference on {ka['symbols']} symbols "
                f"and invariant checks over the full day; fuzzed the parser with libFuzzer in "
                f"Docker ({n(fuzz_run('fuzz_parser')['executed_units'])} executions, "
                f"{fuzz_run('fuzz_parser')['crashes']} crashes).\n\n")

    return {
        # Multi-line blocks.
        "badges": lambda: badges(REPO_SLUG),
        "public_proofs": lambda: public_proofs(r.root),
        "proof_table": proof_table,
        "bench_table": bench_table,
        "latency_by_type": latency_by_type,
        "ladder_table": ladder_table,
        "index_table": index_table,
        "loss_table": loss_table,
        "fuzz_table": fuzz_table,
        "resume_bullets": resume_bullets,
        # Sentences.
        "lag_text": lambda: lag_text(replay()),
        "paced_export": paced_export,
        "crossed_scope": crossed_scope,
        "latency_caveat": latency_caveat,
        "provenance": provenance,
        # Day.
        "messages": lambda: n(day()["totals"]["messages"]),
        "messages_m": lambda: f"{day()['totals']['messages'] / 1e6:.1f} M",
        "day_bytes": lambda: gb(day()["file_bytes"]),
        "day_gz_bytes": lambda: gb(day()["meta"]["data_manifest"]["gz_bytes"]),
        "symbols_with_orders": lambda: n(day()["symbols_with_orders"]),
        "skipped": lambda: n(day()["totals"]["skipped"]),
        "full_checks": lambda: n(day()["verify"]["full_checks"]),
        "halt_grace_s": lambda: str(day()["verify"]["halt_grace_s"]),
        "peak_live": lambda: n(day()["peak_live_orders"]),
        "printed_volume": lambda: n(day()["totals"]["printed_volume_shares"]),
        "trades": lambda: n(day()["totals"]["trade_count"]),
        "unknown_ref": lambda: n(day()["anomalies"]["unknown_ref"]),
        "exec_checked": lambda: n(day()["oracle"]["exec_checked"]),
        "exec_not_best": lambda: n(day()["oracle"]["exec_not_best"]),
        "exec_not_head": lambda: n(day()["oracle"]["exec_not_head"]),
        "crossed_continuous": lambda: n(day()["oracle"]["crossed_continuous"]),
        "crossed_other": lambda: n(day()["oracle"]["crossed_other"]),
        "episodes_other": lambda: n(day()["oracle"]["episodes_other"]),
        "touches_continuous": lambda: n(day()["oracle"]["touches_continuous"]),
        "match_two": lambda: n(day()["totals"]["match_number_collisions"]["distinct"]),
        "zero_share_q": lambda: n(recon()["crosses"]["zero_share"]),
        "not_head_e": lambda: n(day()["oracle"]["exec_not_head_e"]),
        "not_head_e_before": lambda: n(
            r["recon_12302019_arrival_queue"]["exec_not_head"]["E"]["not_arrival_head"]),
        "c_through": lambda: n(recon()["exec_not_head"]["C"]["away_from_display_price"]["direction"]["inside_the_spread_side"]),
        "c_other": lambda: n(recon()["exec_not_head"]["C"]["away_from_display_price"]["direction"]["away_from_the_spread"]),
        "c_at_display": lambda: n(day()["oracle"]["exec_not_head"] - day()["oracle"]["exec_not_head_e"]
                                  - recon()["exec_not_head"]["C"]["away_from_display_price"]["not_head"]),
        "c_away": lambda: n(recon()["exec_not_head"]["C"]["away_from_display_price"]["not_head"]),
        "closes_ok": lambda: f"{closes()[0]} of {closes()[1]}",
        # Second day.
        "messages_2025": lambda: n(d25()["totals"]["messages"]),
        "unexplained_2025": lambda: n(recon25()["not_best_by_kind"]["EY"]),
        # Known answer.
        "ka_symbols": lambda: str(known_answer()["symbols"]),
        "ka_orders": lambda: n(known_orders()),
        # Benchmark.
        "cpu": lambda: r["bench_12302019"]["meta"]["cpu"],
        "apply_mps": lambda: mps(median("apply")),
        "parse_mps": lambda: mps(median("parse")),
        "apply_ns": lambda: f"{bench()['apply']['ns_per_msg_median']:.0f}",
        "apply_before_mps": lambda: mps(before()["apply"]["msgs_per_sec"]["median"]),
        "apply_gain": lambda: f"{(median('apply') / before()['apply']['msgs_per_sec']['median'] - 1) * 100:.1f}%",
        "apply_spread": lambda: f"{bench()['apply']['msgs_per_sec']['spread'] * 100:.1f}%",
        "p50": lambda: str(lat("p50_ns")),
        "p99": lambda: str(lat("p99_ns")),
        "p999": lambda: str(lat("p99_9_ns")),
        "p999_before": lambda: str(before()["latency"]["latency_all_ns"]["p99_9_ns"]["median"]),
        "unordered_mps": lambda: mps(median("apply_unordered_map")),
        "index_speedup": lambda: f"{median('apply') / median('apply_unordered_map'):.1f}x",
        "ladder_speedup": lambda: f"{median('apply_ten_symbols_ladder') / median('apply_ten_symbols_vector'):.2f}x",
        "ladder_full_gb": lambda: f"{ladder_bytes() / 1e9:.1f} GB",
        "machine_gb": lambda: f"{r['bench_12302019']['machine']['memory_bytes'] / 2**30:.0f} GiB",
        "vector_levels_mb": lambda: mb(bench()["apply"]["book_storage"]["level_vector_bytes_high_water"]),
        "index_mb": lambda: mb(bench()["apply"]["memory"]["id_index_bytes"]),
        "pool_mb": lambda: mb(bench()["apply"]["memory"]["order_pool_bytes"]),
        "books_mb": lambda: mb(bench()["apply"]["memory"]["books_array_bytes"]),
        "tables_mb": lambda: mb(bench()["apply"]["memory"]["tables_added_after_setup_bytes_median"]),
        "heap_end_mb": lambda: mb(bench()["apply"]["memory"]["heap_added_at_end_bytes_median"]),
        "peak_over_input_mb": lambda: mb(bench()["apply"]["memory"]["peak_footprint_over_input_bytes_median"]),
        "map_heap_end_mb": lambda: mb(bench()["apply_unordered_map"]["memory"]["heap_added_at_end_bytes_median"]),
        "map_tables_mb": lambda: mb(bench()["apply_unordered_map"]["memory"]["tables_added_after_setup_bytes_median"]),
        "max_rss_gb": lambda: gb(bench()["apply"]["memory"]["max_rss_bytes_median"]),
        "latency_rss_gb": lambda: gb(bench()["latency"]["memory"]["max_rss_bytes_median"]),
        "timed_mps": lambda: mps(bench()["latency"]["msgs_per_sec"]["median"]),
        "timed_ns": lambda: f"{bench()['latency']['ns_per_msg_median']:.1f}",
        "floor_mean": lambda: f"{bench()['latency']['latency_timer_floor_ns']['mean_ns']['median']:.1f}",
        "floor_p50": lambda: str(bench()["latency"]["latency_timer_floor_ns"]["p50_ns"]["median"]),
        "floor_p99": lambda: str(bench()["latency"]["latency_timer_floor_ns"]["p99_ns"]["median"]),
        "clock_granularity": lambda: str(bench()["latency"]["clock"]["granularity_median_ns"]),
        "clock_serialized": lambda: f"{bench()['latency']['clock']['overhead_serialized_ns']:.1f}",
        "clock_unserialized": lambda: f"{bench()['latency']['clock']['overhead_unserialized_ns']:.2f}",
        "zero_tick_pct": lambda: f"{bench()['latency']['latency_fraction_at_zero_ticks_median'] * 100:.1f}%",
        "apply_wall_s": lambda: f"{sorted(bench()['apply']['raw_wall_seconds_including_setup'])[2]:.0f} s",
        # Loss.
        "lost": lambda: n(full()["channel"]["messages_lost_downstream"]),
        "dropped_packets": lambda: n(full()["channel"]["downstream_dropped"]),
        "noon_second": lambda: n(full()["dump"]["at_second"]),
        "noon_orders": lambda: n(full()["dump"]["resting_orders"]),
        "noon_sha": lambda: f"{full()['dump']['sha256'][:16]}",
        "stream_sha": lambda: f"{full()['result']['stream_sha256'][:16]}",
        "loss_seconds": lambda: f"{full()['result']['seconds']:.1f} s",
        "loss_retention": lambda: n(full()["config"]["retention_messages"]),
        "max_gap": lambda: n(full()["rx"]["max_gap_len"]),
        "table_lost_1pct": lambda: n(loss_row("loss_only_0.01")["channel"]["messages_lost_downstream"]),
        "table_from_retransmit_1pct": lambda: n(loss_row("loss_only_0.01")["rx"]["messages_from_retransmit"]),
        # UDP.
        "udp_messages": lambda: n(udp()["source"]["messages"]),
        "udp_rate": lambda: n(udp()["config"]["rate_mps"]),
        # Paced.
        "paced_speed": lambda: f"{replay()['speed']}x",
        "paced_wall_h": lambda: f"{replay()['paced_wall_s'] / 3600:.2f} h",
    }


def rewrite(text: str, blocks: dict, rel: str, stale: list) -> str:
    if text.count(OPEN) != text.count(CLOSE):
        raise DocError(f"{rel}: {text.count(OPEN)} blocks opened, {text.count(CLOSE)} closed")

    def one(m):
        name, body = m.group(1), m.group(2)
        if OPEN in body:
            raise DocError(f"{rel}: block '{name}' is not closed before the next one opens")
        if name not in blocks:
            raise DocError(f"{rel}: unknown block '{name}'")
        new = blocks[name]()
        if new != body:
            stale.append(f"{rel}: {name}")
        return f"<!-- gen:{name} -->{new}{CLOSE}"

    return BLOCK.sub(one, text)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="report stale blocks, write nothing")
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    args = ap.parse_args(argv)
    try:
        blocks = build_blocks(Results(args.root))
        stale, out = [], {}
        for rel in documents(args.root):
            path = args.root / rel
            if not path.exists():
                raise DocError(f"missing document {rel}")
            out[path] = rewrite(path.read_text(), blocks, rel, stale)
    except DocError as e:
        print(f"gen_docs: {e}", file=sys.stderr)
        return 2
    except (KeyError, IndexError, TypeError, ValueError, StopIteration) as e:
        print(f"gen_docs: unexpected shape in a results file: {type(e).__name__}: {e}",
              file=sys.stderr)
        return 2
    if args.check:
        for line in stale:
            print(f"stale: {line}")
        print(f"gen_docs: {len(stale)} stale block(s)" if stale else "gen_docs: all blocks fresh")
        return 1 if stale else 0
    for path, text in out.items():
        if path.read_text() != text:
            path.write_text(text)
    print(f"gen_docs: {len(stale)} block(s) updated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
