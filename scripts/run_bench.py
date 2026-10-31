#!/usr/bin/env python3
"""Run the benchmark suite and write results/bench_<day>.json.

    run_bench.py --day 12302019 [--bin build/release/bench/bench_feed] [--runs 5]
                 [--only NAME[,NAME]] [--out results/bench_12302019.json] [--allow-dirty]

Protocol (docs/METHODOLOGY.md):
  * every measurement is N fresh processes (default 5), run one after another, never in parallel;
  * the published value is the MEDIAN of the N, and the spread (max - min) / median is published
    next to it;
  * a set whose spread exceeds 10% is rejected: it is retried once, and if it is rejected again
    nothing is published for it and the script exits 4;
  * a set whose runs disagree on a deterministic output (state checksum, peak live orders,
    anomaly counts) fails the script, because that is a bug, not noise;
  * the stamp (commit, CPU, compiler, flags) is read back from the binary. Without --allow-dirty
    the script refuses a stamp with a -dirty, -stale-build or -unverified suffix.

Run it under `caffeinate -i` so the machine does not idle-sleep mid-set.
"""
import argparse
import hashlib
import json
import platform
import re
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SPREAD_LIMIT = 0.10
COOLDOWN_S = 5
TOP_N_SYMBOLS = 10


def sh(cmd: list[str]) -> str:
    try:
        return subprocess.run(cmd, capture_output=True, text=True, check=False).stdout.strip()
    except OSError:
        return ""


def top_symbols(day_json: Path) -> list[str]:
    """The ten busiest symbols by message count, from the committed full-day export."""
    d = json.loads(day_json.read_text())
    ps = d["per_symbol"]
    order = sorted(range(len(ps["symbol"])), key=lambda i: (-ps["messages"][i], ps["symbol"][i]))
    return [ps["symbol"][i] for i in order[:TOP_N_SYMBOLS]]


def machine() -> dict:
    batt = sh(["pmset", "-g", "batt"])
    therm = sh(["pmset", "-g", "therm"])
    low_power = re.search(r"lowpowermode\s+(\d)", sh(["pmset", "-g"]))
    return {
        "cpu": sh(["sysctl", "-n", "machdep.cpu.brand_string"]),
        "performance_cores": sh(["sysctl", "-n", "hw.perflevel0.logicalcpu"]),
        "efficiency_cores": sh(["sysctl", "-n", "hw.perflevel1.logicalcpu"]),
        "memory_bytes": int(sh(["sysctl", "-n", "hw.memsize"]) or 0),
        "os": f"{sh(['sw_vers', '-productName'])} {sh(['sw_vers', '-productVersion'])} "
              f"(Darwin {platform.release()})",
        "power_source": "AC Power" if "AC Power" in batt else batt.splitlines()[0] if batt else "unknown",
        "pmset_batt": batt,
        "low_power_mode": low_power.group(1) if low_power else "unknown",
        "thermal_before": therm,
        "pinning": "none: macOS has no core affinity API; the thread asks for "
                   "QOS_CLASS_USER_INTERACTIVE and the scheduler decides",
    }


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(1 << 24):
            h.update(chunk)
    return h.hexdigest()


def suite(symbols: list[str]) -> dict[str, list[str]]:
    syms = ",".join(symbols)
    return {
        "frame": ["--mode", "frame"],
        "parse": ["--mode", "parse"],
        "apply": ["--mode", "apply"],
        "latency": ["--mode", "latency"],
        "apply_unordered_map": ["--mode", "apply", "--map", "unordered"],
        "latency_unordered_map": ["--mode", "latency", "--map", "unordered"],
        "apply_no_book": ["--mode", "apply", "--book", "none"],
        "apply_ten_symbols_vector": ["--mode", "apply", "--symbols", syms],
        "apply_ten_symbols_ladder": ["--mode", "apply", "--book", "ladder", "--symbols", syms],
    }


# Sets that must agree on the end-of-run state fold, and why.
AGREE = [
    (["apply", "apply_unordered_map"], "the two id indexes apply the same day to the same books"),
    (["apply_ten_symbols_vector", "apply_ten_symbols_ladder"],
     "the vector book and the ladder agree on every touch, sampled every 2^20 messages"),
]


def run_once(binary: Path, data: Path, args: list[str], tmp: Path) -> dict:
    out = tmp / "run.json"
    cmd = [str(binary), str(data), *args, "--json", str(out)]
    t = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if p.returncode != 0:
        raise SystemExit(f"bench_feed failed ({p.returncode}): {' '.join(cmd)}\n{p.stderr}")
    d = json.loads(out.read_text())
    d["wall_seconds_including_setup"] = round(time.time() - t, 1)
    return d


def spread(vals: list[float]) -> float:
    m = statistics.median(vals)
    return (max(vals) - min(vals)) / m if m else float("inf")


def pct_summary(runs: list[dict], getter) -> dict:
    vals = [getter(r) for r in runs]
    return {"median": statistics.median(vals), "min": min(vals), "max": max(vals)}


def summarize(name: str, runs: list[dict]) -> dict:
    res = [r["result"] for r in runs]
    rates = [x["msgs_per_sec"] for x in res]
    out = {
        "runs": len(runs),
        "msgs_per_sec": {"median": statistics.median(rates), "min": min(rates), "max": max(rates),
                         "all": rates, "spread": round(spread(rates), 4)},
        "ns_per_msg_median": statistics.median(x["ns_per_msg"] for x in res),
        "messages": res[0]["messages"],
        "handled": res[0]["handled"],
        "state_checksum": res[0]["state_checksum"],
        "peak_live_orders": res[0]["peak_live_orders"],
        "anomalies": res[0]["anomalies"],
        "median_of_chunk_median_msgs_per_sec": statistics.median(
            x["throughput_by_chunk"]["median_msgs_per_sec"] for x in res
            if "median_msgs_per_sec" in x["throughput_by_chunk"]) if any(
            "median_msgs_per_sec" in x["throughput_by_chunk"] for x in res) else None,
        "input_mlocked": all(r["prefault"]["mlocked"] for r in runs),
        "timed_span_major_faults_per_run": [r["timed_span"]["major_faults"] for r in runs],
        "timed_span_major_faults_max": max(r["timed_span"]["major_faults"] for r in runs),
        "timed_span_involuntary_ctx_switches_max":
            max(r["timed_span"]["involuntary_ctx_switches"] for r in runs),
        "memory": {
            "max_rss_bytes_median": statistics.median(r["memory"]["max_rss_bytes"] for r in runs),
            "max_rss_includes": runs[0]["memory"]["max_rss_includes"],
            # The input is mlocked, and the kernel then counts its pages as "internal", so the
            # absolute footprint includes the 8.25 GB file. Everything below is a DELTA over the
            # footprint taken right after the input was mapped and locked: what the handler adds.
            "tables_added_after_setup_bytes_median": statistics.median(
                r["memory"]["footprint_after_setup"]["internal_anonymous"]
                - r["memory"]["footprint_before_tables"]["internal_anonymous"] for r in runs),
            "heap_added_at_end_bytes_median": statistics.median(
                r["memory"]["footprint_end"]["internal_anonymous"]
                + r["memory"]["footprint_end"]["compressed"]
                - r["memory"]["footprint_before_tables"]["internal_anonymous"]
                - r["memory"]["footprint_before_tables"]["compressed"] for r in runs),
            "peak_footprint_over_input_bytes_median": statistics.median(
                r["memory"]["footprint_end"]["phys_footprint_peak"]
                - r["memory"]["footprint_before_tables"]["phys_footprint"] for r in runs),
            "mapped_input_bytes": runs[0]["input"]["file_bytes"],
            "order_pool_bytes": runs[0]["memory"]["order_pool_bytes"],
            "id_index_bytes": runs[0]["memory"]["id_index_bytes"],
            "books_array_bytes": runs[0]["memory"]["books_array_bytes"],
        },
    }
    if "book_storage" in runs[0]:
        out["book_storage"] = runs[0]["book_storage"]
    if "latency" in runs[0]:
        lat = [r["latency"] for r in runs]
        keys = ["p50_ns", "p90_ns", "p99_ns", "p99_9_ns", "p99_99_ns", "max_ns", "mean_ns"]
        out["latency_all_ns"] = {k: pct_summary(lat, lambda L, k=k: L["all"][k]) for k in keys}
        out["latency_fraction_at_zero_ticks_median"] = statistics.median(
            L["fraction_samples_at_zero_ticks"] for L in lat)
        out["latency_timer_floor_ns"] = {
            k: pct_summary(lat, lambda L, k=k: L["timer_floor"][k]) for k in keys}
        types = sorted(lat[0]["by_type"])
        out["latency_by_type_ns"] = {
            t: {"count": lat[0]["by_type"][t]["count"],
                **{k: pct_summary(lat, lambda L, t=t, k=k: L["by_type"][t][k])
                   for k in ["p50_ns", "p99_ns", "p99_9_ns", "max_ns", "mean_ns"]}}
            for t in types}
    out["clock"] = runs[0]["clock"]
    out["raw_wall_seconds_including_setup"] = [r["wall_seconds_including_setup"] for r in runs]
    return out


def check_deterministic(name: str, runs: list[dict]) -> None:
    keys = [lambda r: r["result"]["state_checksum"], lambda r: r["result"]["peak_live_orders"],
            lambda r: json.dumps(r["result"]["anomalies"], sort_keys=True),
            lambda r: r["result"]["messages"]]
    for k in keys:
        if len({k(r) for r in runs}) != 1:
            raise SystemExit(f"{name}: the runs disagree on a deterministic output: "
                             f"{[k(r) for r in runs]}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--day", required=True, help="e.g. 12302019")
    ap.add_argument("--bin", type=Path, default=Path("build/release/bench/bench_feed"))
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--only", default="", help="comma separated suite names")
    ap.add_argument("--out", type=Path, default=None)
    ap.add_argument("--allow-dirty", action="store_true")
    ap.add_argument("--label", default="", help="free text stored in the output, e.g. a candidate name")
    args = ap.parse_args()

    data = Path(f"data/{args.day}.NASDAQ_ITCH50")
    out_path = args.out or Path(f"results/bench_{args.day}.json")
    day_json = Path(f"results/day_{args.day}.json")
    if not data.exists() or not args.bin.exists() or not day_json.exists():
        raise SystemExit(f"missing input: {data}, {args.bin} or {day_json}")

    symbols = top_symbols(day_json)
    every = suite(symbols)
    chosen = [n for n in args.only.split(",") if n] or list(every)
    for n in chosen:
        if n not in every:
            raise SystemExit(f"unknown suite entry {n}; known: {', '.join(every)}")

    m = machine()
    sha = sha256_file(data)
    results: dict[str, dict] = {}
    rejected: dict[str, dict] = {}
    stamp = None

    def write(final: bool) -> None:
        m["thermal_after"] = sh(["pmset", "-g", "therm"])
        doc = {
            "meta": {**(stamp or {}), "os": m["os"], "power_source": m["power_source"],
                     "low_power_mode": m["low_power_mode"], "pinning": m["pinning"],
                     "data_sha256": sha, "data_bytes": data.stat().st_size,
                     "label": args.label, "complete": final},
            "machine": m,
            "protocol": {
                "processes_per_measurement": args.runs, "published": "median",
                "spread_definition": "(max - min) / median of msgs_per_sec",
                "spread_limit": SPREAD_LIMIT, "cooldown_seconds_between_runs": COOLDOWN_S,
                "timer": "mach counter via ob::bench::Clock; granularity in each result's clock block",
                "ten_symbols": symbols,
            },
            "results": results,
            "rejected_sets": rejected,
        }
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(json.dumps(doc, indent=2) + "\n")

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        for name in chosen:
            for attempt in (1, 2):
                runs = []
                for i in range(args.runs):
                    print(f"[{name}] attempt {attempt} run {i + 1}/{args.runs}", flush=True)
                    runs.append(run_once(args.bin, data, every[name], tmp))
                    if i + 1 < args.runs:
                        time.sleep(COOLDOWN_S)
                check_deterministic(name, runs)
                stamps = {json.dumps(r["meta"], sort_keys=True) for r in runs}
                if len(stamps) != 1:
                    raise SystemExit(f"{name}: runs carry different stamps")
                stamp = runs[0]["meta"]
                s = summarize(name, runs)
                rate = s["msgs_per_sec"]
                print(f"[{name}] median {rate['median']:.4g} msgs/s spread "
                      f"{rate['spread'] * 100:.1f}%", flush=True)
                if rate["spread"] <= SPREAD_LIMIT:
                    results[name] = s
                    break
                rejected[f"{name}#attempt{attempt}"] = s
                print(f"[{name}] spread over {SPREAD_LIMIT * 100:.0f}%: rejected", flush=True)
                time.sleep(COOLDOWN_S * 3)
            else:
                print(f"[{name}] rejected twice; nothing published for it", file=sys.stderr)
            write(False)
            time.sleep(COOLDOWN_S)

    for names, why in AGREE:
        have = [n for n in names if n in results]
        if len(have) == len(names) and len({results[n]["state_checksum"] for n in have}) != 1:
            raise SystemExit(f"state checksums differ between {names}: {why}")

    commit = stamp["commit"] if stamp else "unknown"
    clean = re.fullmatch(r"[0-9a-f]{40}", commit) is not None
    if not clean and not args.allow_dirty:
        raise SystemExit(f"the binary's stamp is '{commit}': rebuild from a clean commit "
                         "(or pass --allow-dirty for an experiment)")

    write(True)
    print(f"wrote {out_path}")
    return 4 if any(n not in results for n in chosen) else 0


if __name__ == "__main__":
    sys.exit(main())
