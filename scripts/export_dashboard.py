#!/usr/bin/env python3
"""Turn the committed results/ files into the small JSON files the dashboard loads.

    export_dashboard.py results/ dashboard/data/ [--max-day-bytes N]

Writes day.json (downsampled, at most 5 MB), loss.json, verify.json, bench.json and a copy of
docs/METHODOLOGY.md as METHODOLOGY.txt. Every number comes from a results file; the only text
written here is labels, and the status words in verify.json, which are the ones in
docs/DISCREPANCIES.md. Every output section carries the file and commit it came from.

It refuses to run if an input has no "meta" stamp (commit, data_file, and for tool exports cpu,
compiler, flags), or if a number that two files must agree on does not.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MAX_DAY_BYTES = 5_000_000

DAY = "day_12302019.json"
REPLAY = "replay_10x.json"
LOSS_TABLE = "loss_table.json"
LOSS_DAY = "loss_full_day.json"
LOSS_LOSSLESS = "loss_full_day_lossless.json"
BENCH = "bench_12302019.json"
RECON = "recon_12302019.json"
RECON_ARRIVAL = "recon_12302019_arrival_queue.json"
RECON_2025 = "recon_20251208.json"
DAY_2025 = "day_20251208.json"
KNOWN_ANSWER = "known_answer_summary.json"

STAMP_FULL = ("commit", "data_file", "cpu", "compiler", "flags")
STAMP_SCRIPT = ("commit", "data_file")
# Files written by verify/*.py carry a shorter stamp.
STAMPS = {
    DAY: STAMP_FULL, REPLAY: STAMP_FULL, LOSS_TABLE: STAMP_FULL, LOSS_DAY: STAMP_FULL,
    LOSS_LOSSLESS: STAMP_FULL, BENCH: STAMP_FULL, DAY_2025: STAMP_FULL, KNOWN_ANSWER: STAMP_FULL,
    RECON: STAMP_SCRIPT, RECON_ARRIVAL: STAMP_SCRIPT, RECON_2025: STAMP_SCRIPT,
}

SESSION = (9 * 3600 + 30 * 60, 16 * 3600)  # regular session, seconds since midnight
CLOSING_CROSS_S = (15 * 3600 + 59 * 60, 16 * 3600 + 60)  # 15:59:00 to 16:01:00
GRID_S = 5  # top-of-book sample spacing
TYPE_NAMES = {
    "A": "Add order", "F": "Add order, MPID", "E": "Order executed", "C": "Executed at price",
    "X": "Order cancel", "D": "Order delete", "U": "Order replace", "P": "Trade (non-cross)",
    "Q": "Cross trade", "B": "Broken trade", "I": "Imbalance indicator", "H": "Trading action",
    "Y": "Reg SHO restriction", "L": "Participant position", "V": "MWCB decline level",
    "W": "MWCB status", "K": "IPO quoting period", "J": "LULD collar", "h": "Operational halt",
    "R": "Stock directory", "S": "System event", "N": "Retail price improvement",
}


class ExportError(Exception):
    pass


COMMIT_RE = re.compile(r"^[0-9a-f]{7,40}$")
REFUSED_SUFFIXES = ("-dirty", "-stale-build", "-unverified")
# Every 2019 input must be about the same data file; the 2025 robustness inputs about another.
DATA_2019 = {DAY, REPLAY, LOSS_TABLE, LOSS_DAY, LOSS_LOSSLESS, BENCH, RECON, RECON_ARRIVAL,
             KNOWN_ANSWER}
DATA_2025 = {DAY_2025, RECON_2025}


def check_commit(name: str, commit: str) -> None:
    """A results file must come from a clean, verified build of a known commit. A '-dirty'
    stamp means uncommitted changes, '-stale-build' a binary older than HEAD, '-unverified' that
    git could not be asked; none of them says which code produced the numbers."""
    for suffix in REFUSED_SUFFIXES:
        if commit.endswith(suffix) or suffix + "-" in commit:
            raise ExportError(f"{name}: commit stamp '{commit}' is {suffix[1:]}; regenerate it "
                              "from a clean build")
    if not COMMIT_RE.match(commit):
        raise ExportError(f"{name}: commit stamp '{commit}' is not a commit hash")


def load(results: Path, name: str) -> dict:
    """Read one results file and check its stamp."""
    path = results / name
    if not path.exists():
        raise ExportError(f"missing input {path}")
    with open(path) as f:
        d = json.load(f)
    need = STAMPS.get(name)
    if need is not None:
        meta = d.get("meta") if isinstance(d, dict) else None
        if not isinstance(meta, dict):
            raise ExportError(f"{name}: no meta stamp")
        missing = [k for k in need if not meta.get(k)]
        if missing:
            raise ExportError(f"{name}: meta stamp lacks {', '.join(missing)}")
        check_commit(name, meta["commit"])
    return d


def src(name: str, d: dict) -> dict:
    meta = d["meta"]
    return {"file": name, "commit": meta["commit"], "data_file": meta["data_file"]}


def is_closing_cross_time(second: int) -> bool:
    return CLOSING_CROSS_S[0] <= second <= CLOSING_CROSS_S[1]


def known_answer_checkpoint(populated: dict) -> str:
    """The checkpoint the known-answer comparison is quoted at: the one that is not end of day.
    Several are accepted only if they are feed seconds, which order themselves."""
    candidates = [k for k in populated if k != "eod"]
    if not candidates:
        raise ExportError("known-answer comparison has no checkpoint before end of day")
    if len(candidates) == 1:
        return candidates[0]
    if all(k.isdigit() for k in candidates):
        return min(candidates, key=int)
    raise ExportError(f"known-answer comparison has several checkpoints and no order among them: "
                      f"{', '.join(sorted(candidates))}")


def clock(seconds: int) -> str:
    return f"{seconds // 3600:02d}:{seconds // 60 % 60:02d}:{seconds % 60:02d}"


def top_of_book_grid(watch: dict, start_s: int, end_s: int, step_s: int) -> dict:
    """One symbol's top of book sampled every step_s seconds, as the state after the last
    change at or before each sample (the export has a row only for seconds with a change, and
    a reader carries the previous row forward). Prices in dollars, spread likewise.

    A side with no orders is None, and so is the spread whenever either side is None: a missing
    side is not a price of zero, and a spread across one side is not a number. The chart draws a
    gap there.
    """
    t, bid, ask = watch["t"], watch["bid"], watch["ask"]
    bid_sz, ask_sz = watch["bid_sz"], watch["ask_sz"]
    n = len(t)
    xs, b, a, sp, bs, as_ = [], [], [], [], [], []
    i = -1  # index of the last row with t <= sample time
    for s in range(start_s, end_s + 1, step_s):
        while i + 1 < n and t[i + 1] <= s:
            i += 1
        xs.append(s)
        if i < 0:
            bb = aa = None
            qb = qa = None
        else:
            bb, aa = bid[i], ask[i]
            qb, qa = bid_sz[i], ask_sz[i]
        b.append(None if bb is None else bb / 10000)
        a.append(None if aa is None else aa / 10000)
        sp.append(None if bb is None or aa is None else round((aa - bb) / 10000, 4))
        bs.append(qb if bb is not None else None)
        as_.append(qa if aa is not None else None)
    one_sided = sum(1 for x, y in zip(b, a) if (x is None) != (y is None))
    empty = sum(1 for x, y in zip(b, a) if x is None and y is None)
    return {"bid": b, "ask": a, "spread": sp, "bid_size": bs, "ask_size": as_,
            "samples_one_sided": one_sided, "samples_empty": empty}


def per_minute(per_second: list) -> dict:
    avg, peak = [], []
    for m in range(0, 1440):
        chunk = per_second[m * 60:(m + 1) * 60]
        avg.append(round(sum(chunk) / 60, 1))
        peak.append(max(chunk))
    return {"avg": avg, "peak": peak}


def check_same(a: dict, b: dict, name_a: str, name_b: str, keys) -> None:
    """The paced replay and the max-speed run must give the same export: refuse if they differ."""
    for k in keys:
        if a.get(k) != b.get(k):
            raise ExportError(f"{name_a} and {name_b} differ in '{k}'; "
                              "the paced replay must give the max-speed export")


def check_data_files(results: Path) -> dict:
    """Every input's data_file, with the rule that the 2019 inputs agree and the 2025 ones agree."""
    seen = {}
    for group in (DATA_2019, DATA_2025):
        files = {n: load(results, n)["meta"]["data_file"] for n in sorted(group)}
        if len(set(files.values())) > 1:
            raise ExportError("inputs disagree on data_file: " +
                              ", ".join(f"{n}={f}" for n, f in files.items()))
        seen.update(files)
    return seen


def build_provenance(results: Path) -> dict:
    data = check_data_files(results)
    by_commit = {}
    for name in sorted(data):
        c = load(results, name)["meta"]["commit"]
        by_commit.setdefault(c, []).append(name)
    return {"commit_count": len(by_commit),
            "commits": [{"commit": c, "files": f} for c, f in by_commit.items()],
            "data_files": sorted(set(data.values()))}


def build_day(results: Path, max_bytes: int) -> dict:
    day, rep = load(results, DAY), load(results, REPLAY)
    check_same(day, rep, DAY, REPLAY,
               ("totals", "by_type", "per_second", "anomalies", "per_symbol", "peak_live_orders"))
    ps = day["per_second"]
    if len(ps) != 86400:
        raise ExportError(f"{DAY}: per_second has {len(ps)} entries, expected 86400")
    peak = max(ps)
    peak_s = ps.index(peak)

    open_close = ps[SESSION[0]:SESSION[1] + 1]
    watch = rep.get("watch") or {}
    if len(watch) < 5:
        raise ExportError(f"{REPLAY}: needs five watched symbols, has {len(watch)}")
    symbols = {}
    for name, w in watch.items():
        g = top_of_book_grid(w, SESSION[0], SESSION[1], GRID_S)
        g["locate"] = w["locate"]
        symbols[name] = g
    totals = day["totals"]
    mix = [{"type": k, "name": TYPE_NAMES.get(k, k), "count": v}
           for k, v in sorted(day["by_type"].items(), key=lambda kv: -kv[1])]
    out = {
        "sources": {"day": src(DAY, day), "top_of_book": src(REPLAY, rep)},
        "price_unit": "dollars (the feed's integer prices divided by 10,000, for display)",
        "kpi": {
            "messages": totals["messages"],
            "symbols": day["by_type"]["R"],
            "symbols_note": "stock directory entries (type R)",
            "peak_msgs_per_sec": peak,
            "peak_second": clock(peak_s),
            "peak_second_s": peak_s,
            "peak_is_closing_cross": is_closing_cross_time(peak_s),
            "unknown_refs": day["anomalies"]["unknown_ref"],
            "first_message": clock(totals["first_timestamp_ns"] // 10**9),
            "last_message": clock(totals["last_timestamp_ns"] // 10**9),
            "printed_volume_shares": totals["printed_volume_shares"],
            "trades": totals["trade_count"],
            "peak_live_orders": day["peak_live_orders"],
        },
        "minute": {"start_s": 0, **per_minute(ps)},
        "session": {"start_s": SESSION[0], "step_s": 1, "msgs_per_sec": open_close},
        "book": {"start_s": SESSION[0], "end_s": SESSION[1], "step_s": GRID_S,
                 "symbols": symbols,
                 "note": f"The state after the last change at or before each {GRID_S}-second "
                         "mark. A side with no orders is a gap, not zero."},
        "mix": mix,
    }
    size = len(json.dumps(out, separators=(",", ":")).encode())
    if size > max_bytes:
        raise ExportError(f"day.json would be {size:,} bytes, over the {max_bytes:,} limit")
    return out


def checkpoint_text(d: dict) -> str:
    return f"after {d['at_message']:,} messages" if d["at_end"] \
        else f"at feed second {d['at_second']:,}"


def build_loss(results: Path) -> dict:
    table = load(results, LOSS_TABLE)
    full, lossless = load(results, LOSS_DAY), load(results, LOSS_LOSSLESS)
    day = load(results, DAY)
    rows = []
    for r in table["rows"]:
        c, res, ch, rx, d = r["config"], r["result"], r["channel"], r["rx"], r["dump"]
        lossless_row = r["kind"] == "baseline"
        # Every lost message is recovered exactly when the delivered stream is the source stream:
        # same count, same hash, nothing skipped.
        recovered_all = bool(res["stream_matches_source"] and res["finished"] and
                             res["delivered_messages"] == table["source"]["messages"])
        rows.append({
            "label": r["label"], "kind": r["kind"],
            "loss": c["loss"], "dup": c["dup"], "reorder_window": c["reorder_window"],
            "burst_start": c["burst_start"], "retransmit_loss": c["retransmit_loss"],
            "messages": table["source"]["messages"], "delivered": res["delivered_messages"],
            "lost": ch["messages_lost_downstream"],
            "gaps_detected": rx["gaps_detected"],
            "stream_matches_source": res["stream_matches_source"],
            "recovered_all": recovered_all,
            # The recorded comparison of the receiver's state with a direct replay's (cmp).
            "cmp": "reference" if lossless_row else
                   ("identical" if d["matches_direct"] else "differs"),
            "checkpoint": checkpoint_text(d),
            "resting_orders": d["resting_orders"],
        })
    full_rows = []
    ref_sha = lossless["dump"]["sha256"]
    for name, f in ((LOSS_LOSSLESS, lossless), (LOSS_DAY, full)):
        c, res, ch, rx, d = f["config"], f["result"], f["channel"], f["rx"], f["dump"]
        is_ref = name == LOSS_LOSSLESS
        # Stress is compared with the lossless run AND with the dump the max-speed replay took
        # at the same second (day_12302019.json). The lossless run is the reference, so it is
        # compared with the max-speed dump only.
        same_as_direct = d["sha256"] == day["state_dump"]["sha256"] and \
            d["at_second"] == day["state_dump"]["feed_second"]
        same_as_ref = d["sha256"] == ref_sha
        if is_ref:
            cmp = "reference"
        else:
            cmp = "identical" if (same_as_ref and same_as_direct) else "differs"
        full_rows.append({
            "file": name, "loss": c["loss"], "dup": c["dup"], "reorder_window": c["reorder_window"],
            "lost": ch["messages_lost_downstream"], "gaps_detected": rx["gaps_detected"],
            "messages": f["source"]["messages"], "delivered": res["delivered_messages"],
            "stream_matches_source": res["stream_matches_source"],
            "recovered_all": bool(res["stream_matches_source"] and res["finished"] and
                                  res["delivered_messages"] == f["source"]["messages"]),
            "cmp": cmp, "checkpoint": checkpoint_text(d),
            "same_as_lossless": same_as_ref, "same_as_max_speed_replay": same_as_direct,
            "stream_sha256": res["stream_sha256"],
            "noon_resting_orders": d["resting_orders"], "noon_sha256": d["sha256"],
            "passed": f["verdict"]["passed"],
        })
    return {
        "sources": {"table": src(LOSS_TABLE, table), "full_day": src(LOSS_DAY, full),
                    "full_day_lossless": src(LOSS_LOSSLESS, lossless),
                    "max_speed_replay": src(DAY, day)},
        "table_messages": table["source"]["messages"],
        "rows": rows,
        "full_day": full_rows,
        "note": ("Loss is messages_lost_downstream: messages the receiver never got on the "
                 "first pass. Retransmit requests and late deliveries are not loss; reordering "
                 "triggers early requests, so they run higher."),
    }


def build_verify(results: Path) -> dict:
    day = load(results, DAY)
    rep = load(results, REPLAY)
    recon, arrival = load(results, RECON), load(results, RECON_ARRIVAL)
    d25, recon25 = load(results, DAY_2025), load(results, RECON_2025)
    full, lossless = load(results, LOSS_DAY), load(results, LOSS_LOSSLESS)
    known = load(results, KNOWN_ANSWER)  # the comparison itself needs the local order-level files
    populated = known["resting_orders_per_checkpoint"]
    noon_orders = full["dump"]["resting_orders"]

    o = day["oracle"]
    away = recon["exec_not_head"]["C"]["away_from_display_price"]["not_head"]
    c_at_display = o["exec_not_head"] - o["exec_not_head_e"] - away
    e_before = arrival["exec_not_head"]["E"]["not_arrival_head"]
    unexplained_2025 = recon25["not_best_by_kind"]["EY"]
    rows = [
        {"what": "E executions not at the head of their level",
         "count": o["exec_not_head_e"], "before": e_before, "status": "Found, fixed, re-verified",
         "detail": "Levels queue by reference number, as the exchange does, not by arrival."},
        {"what": "C executions away from the display price, not at the head",
         "count": away, "status": "Characterised, mechanism not shown",
         "detail": "Most trade through their display price, so the displayed level does not show "
                   "their real limit and the head test does not apply. Order types are not visible."},
        {"what": "C executions at the display price, not at the head",
         "count": c_at_display, "status": "Explained",
         "detail": "Non-printable, outside continuous trading, at a cross."},
        {"what": "Executions not at the best price", "count": o["exec_not_best"],
         "status": "Explained", "detail": "Halt-cross allocations at the cross price."},
        {"what": "Crossed-book touches outside continuous trading", "count": o["crossed_other"],
         "extra": f'{o["episodes_other"]} episodes', "status": "Explained",
         "detail": "Every episode starts in a halted or paused symbol."},
        {"what": "Match numbers on two messages",
         "count": day["totals"]["match_number_collisions"]["distinct"], "status": "Explained",
         "detail": "Two sides of one match, exactly one printable. Volume is not double counted."},
        {"what": "Crossed-book touches in continuous trading", "count": o["crossed_continuous"],
         "status": "Agrees",
         "detail": f"Zero, with a {day['verify']['halt_grace_s']} s halt grace."},
        {"what": "2025-12-08: printable E not at the best price", "count": unexplained_2025,
         "status": "Unexplained",
         "detail": "Outside continuous trading, in a crossed book near a halt cross; not shown."},
    ]
    return {
        "sources": {"day": src(DAY, day), "recon": src(RECON, recon),
                    "recon_arrival_queue": src(RECON_ARRIVAL, arrival),
                    "day_2025": src(DAY_2025, d25), "recon_2025": src(RECON_2025, recon25),
                    "loss_full_day": src(LOSS_DAY, full), "known_answer": src(KNOWN_ANSWER, known),
                    "replay": src(REPLAY, rep)},
        "checks": {
            "verify_passed": day["verify"]["passed"],
            "whole_state_checks": day["verify"]["full_checks"],
            "unknown_refs": day["anomalies"]["unknown_ref"],
            "conservation_failures": day["conservation"]["failures"],
            "known_answer_symbols": known["symbols"],
            "known_answer_identical": known["identical_symbols"],
            "known_answer_resting_orders": populated,
            "known_answer_checkpoint": known_answer_checkpoint(populated),
            "noon_resting_orders": noon_orders,
            "noon_sha256": full["dump"]["sha256"],
            "checkpoint_second": full["dump"]["at_second"],
            "noon_loss_equals_lossless": full["dump"]["sha256"] == lossless["dump"]["sha256"],
            "noon_equals_max_speed_dump": full["dump"]["sha256"] == day["state_dump"]["sha256"],
            "stream_hash_equal": full["result"]["stream_matches_source"] and
            lossless["result"]["stream_matches_source"],
            "stream_sha256": full["result"]["stream_sha256"],
            "end_of_day_resting_orders": day["totals"]["live_orders_at_end"],
            "bit_identical": bool(
                full["dump"]["sha256"] == lossless["dump"]["sha256"] == day["state_dump"]["sha256"]
                and full["result"]["stream_matches_source"]
                and lossless["result"]["stream_matches_source"]),
            "bit_identical_note": (
                f"The state at feed second {full['dump']['at_second']:,} ({noon_orders:,} resting "
                "orders) and the whole-stream hash. The end-of-day book is empty, so it proves "
                "nothing."),
        },
        "robustness": {"messages": d25["totals"]["messages"], "verify_passed": d25["verify"]["passed"],
                       "unknown_refs": d25["anomalies"]["unknown_ref"],
                       "peak_live_orders": d25["peak_live_orders"]},
        "discrepancies": rows,
    }


def build_bench(results: Path) -> dict:
    b = load(results, BENCH)
    rep = load(results, REPLAY)
    r = b["results"]

    def row(key, label, note=""):
        x = r[key]
        return {"key": key, "label": label, "note": note,
                "msgs_per_sec": x["msgs_per_sec"]["median"], "spread": x["msgs_per_sec"]["spread"],
                "ns_per_msg": x["ns_per_msg_median"], "messages": x["messages"], "runs": x["runs"]}

    table = [
        row("frame", "Frame only", "length-prefix walk"),
        row("parse", "Parse", "frame and decode"),
        row("apply", "Apply, full day", "parse and book"),
        row("apply_unordered_map", "Apply, std::unordered_map index", "A/B baseline"),
        row("apply_no_book", "Apply, no-op book", "ablation"),
        row("apply_ten_symbols_vector", "Ten busiest symbols, vector book", "production book"),
        row("apply_ten_symbols_ladder", "Ten busiest symbols, Project 1 ladder",
            "pre-allocated, informed base"),
    ]
    lat = r["latency"]["latency_all_ns"]
    a = r["apply"]
    ladder, vec = r["apply_ten_symbols_ladder"], r["apply_ten_symbols_vector"]
    speedup = ladder["msgs_per_sec"]["median"] / vec["msgs_per_sec"]["median"]
    per_symbol = ladder["book_storage"]["ladder_bytes_per_symbol"]
    symbols = a["book_storage"]["books_with_levels"]
    mem = a["memory"]
    replay = rep.get("replay") or {}
    return {
        "sources": {"bench": src(BENCH, b), "replay": src(REPLAY, rep)},
        "machine": {"cpu": b["machine"]["cpu"], "os": b["machine"]["os"],
                    "memory_bytes": b["machine"]["memory_bytes"]},
        "protocol": {"processes": b["protocol"]["processes_per_measurement"],
                     "published": b["protocol"]["published"]},
        "table": table,
        "clock_granularity_ns": r["latency"]["clock"]["granularity_median_ns"],
        "latency_ns": {k: lat[k]["median"] for k in ("p50_ns", "p90_ns", "p99_ns", "p99_9_ns")},
        "handler_memory": {
            "order_pool_bytes": mem["order_pool_bytes"], "id_index_bytes": mem["id_index_bytes"],
            "books_array_bytes": mem["books_array_bytes"],
            "level_vectors_bytes": a["book_storage"]["level_vector_bytes_high_water"],
            "books_with_levels": symbols,
            "note": "Handler tables. Not max_rss, which includes the mapped input file.",
        },
        "ladder": {
            "speedup": round(speedup, 2),
            "ladder_msgs_per_sec": ladder["msgs_per_sec"]["median"],
            "vector_msgs_per_sec": vec["msgs_per_sec"]["median"],
            "bytes_per_symbol": per_symbol, "symbols": symbols,
            "full_market_bytes": per_symbol * symbols,
            "machine_memory_bytes": b["machine"]["memory_bytes"],
            "vector_level_bytes": a["book_storage"]["level_vector_bytes_high_water"],
        },
        "paced_replay": replay if replay.get("speed") != "max" else None,
        "methodology": "METHODOLOGY.txt",
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("results")
    ap.add_argument("out")
    ap.add_argument("--max-day-bytes", type=int, default=MAX_DAY_BYTES)
    args = ap.parse_args(argv)
    results, out = Path(args.results), Path(args.out)
    try:
        parts = {
            "day.json": build_day(results, args.max_day_bytes),
            "loss.json": build_loss(results),
            "verify.json": build_verify(results),
            "bench.json": build_bench(results),
            "provenance.json": build_provenance(results),
        }
    except ExportError as e:
        print(f"export_dashboard: {e}", file=sys.stderr)
        return 2
    except (KeyError, IndexError, TypeError, json.JSONDecodeError) as e:
        print(f"export_dashboard: unexpected shape in a results file: {type(e).__name__}: {e}",
              file=sys.stderr)
        return 2
    out.mkdir(parents=True, exist_ok=True)
    for name, d in parts.items():
        (out / name).write_text(json.dumps(d, separators=(",", ":")) + "\n")
    meth = ROOT / "docs" / "METHODOLOGY.md"
    if meth.exists():
        # The page shows it as text, so gen_docs.py's markers come out and the numbers stay.
        (out / "METHODOLOGY.txt").write_text(re.sub(r"<!-- /?gen[^>]*-->", "", meth.read_text()))
    for name in parts:
        print(f"wrote {out / name} ({(out / name).stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
