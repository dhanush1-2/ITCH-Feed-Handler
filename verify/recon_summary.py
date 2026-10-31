#!/usr/bin/env python3
"""Reduce recon_book.py's raw rows to the committed evidence for docs/DISCREPANCIES.md.

    recon_summary.py <rows.pickle> <recon_match.json> <closing_sources.json> <halt_grace.json>
                     --out results/recon_12302019.json

Every number in the document comes from this output. Only this project's own pickle is loaded.
"""
import argparse
import bisect
import json
import pickle
import subprocess
import sys
from collections import Counter
from pathlib import Path

OPEN_NS = 34_200 * 1_000_000_000


def tally(rows, key, top=None):
    c = Counter(str(key(r)) for r in rows)
    return dict(c.most_common(top))


def pct(sorted_values, q):
    return sorted_values[min(int(len(sorted_values) * q), len(sorted_values) - 1)]


def not_head_section(d):
    nh = d["not_head"]
    models = d["models"]
    E = [r for r in nh if r["kind"] == "E"]
    C = [r for r in nh if r["kind"] == "C"]
    away = [r for r in C if r["exec_price"] != r["price"]]
    at_display = [r for r in C if r["exec_price"] == r["price"]]

    def direction(r):
        better = (r["side"] == "B") == (r["exec_price"] > r["price"])
        return "inside_the_spread_side" if better else "away_from_the_spread"

    def where(r):
        if r["opp_best"] is None:
            return "no_opposite"
        if 2 * r["exec_price"] == r["price"] + r["opp_best"]:
            return "midpoint_of_display_and_opposite_best"
        if r["exec_price"] == r["opp_best"]:
            return "at_opposite_best"
        lo, hi = sorted((r["price"], r["opp_best"]))
        return "between" if lo < r["exec_price"] < hi else "outside"

    secs = []
    highest = []
    top = 0
    for sec, ref in sorted(d["ref_by_second"].items()):
        top = max(top, ref)
        secs.append(sec)
        highest.append(top)

    def dated(ref):
        return secs[min(bisect.bisect_left(highest, ref), len(secs) - 1)]

    delays = sorted(r["o_t"] // 10**9 - dated(r["ref"]) for r in E) or [None]
    table = [{"kind": k[0], "at_display_price": k[1], "arrival_head": k[2], "lowest_ref": k[3],
              "continuous": k[4], "count": v} for k, v in sorted(models.items())]
    e_total = sum(v for k, v in models.items() if k[0] == "E")
    e_ref_head = sum(v for k, v in models.items() if k[0] == "E" and k[3])
    c_total = sum(v for k, v in models.items() if k[0] == "C")
    inv = d["inverted"]
    inv_t = [k[1] for k in inv]
    pairs = {p["match"] for p in d["pairs"]}
    return {
        "total": len(nh),
        "continuous": sum(r["cont"] for r in nh),
        "by_kind": tally(nh, lambda r: r["kind"] + r["printable"]),
        "queue_model_table": table,
        "E": {
            "at_best_level": e_total, "not_queue_head": len(E),
            "not_first_to_arrive": sum(v for k, v in models.items() if k[0] == "E" and not k[2]),
            "lowest_ref_at_level": e_ref_head,
            "not_lowest_ref_at_level": e_total - e_ref_head,
            "order_displayed_before_open": sum(r["o_t"] < OPEN_NS for r in E),
            "order_displayed_max_s_after_open": max((r["o_t"] - OPEN_NS for r in E), default=0) / 1e9,
            "order_ref_dated_before_open": sum(dated(r["ref"]) < 34_200 for r in E),
            "ref_to_display_delay_s": {"min": delays[0], "median": pct(delays, .5),
                                       "max": delays[-1]},
            "head_arrived_before_open": sum(r["h_t"] < OPEN_NS for r in E),
            "exec_hour": dict(sorted(Counter(r["t"] // (3600 * 10**9) for r in E).items())),
            "rejected_hypotheses": {
                "same_timestamp_batch_head_fills_at_same_t": sum(
                    r["head_fate"] in ("E", "C") and r["head_fate_dt_ns"] == 0 for r in E),
                "same_mpid_as_head": sum(bool(r["o_mpid"]) and r["o_mpid"] == r["h_mpid"] for r in E),
                "either_order_has_mpid": sum(bool(r["o_mpid"] or r["h_mpid"]) for r in E),
                "replaced_at_same_price_keeping_priority": sum(r["o_same_price_u"] for r in E),
                "head_is_odd_lot": sum(r["h_shares"] < 100 for r in E),
                "head_fate": tally(E, lambda r: r["head_fate"]),
            },
        },
        "inverted_adds": {
            "orders": sum(v[0] for v in inv.values()),
            "of_which_replaces": sum(v[1] for v in inv.values()),
            "symbol_timestamp_groups": len(inv),
            "first_s_after_open": (min(inv_t) - OPEN_NS) / 1e9,
            "last_s_after_open": (max(inv_t) - OPEN_NS) / 1e9,
        },
        "C": {
            "total_at_best_level": c_total,
            "not_queue_head": len(C),
            "away_from_display_price": {
                "not_head": len(away),
                "all_at_best_level": sum(v for k, v in models.items() if k[0] == "C" and not k[1]),
                "direction": tally(away, direction),
                "where": tally(away, where),
                "where_printable": tally([r for r in away if r["printable"] == "Y"], where),
                "where_nonprintable": tally([r for r in away if r["printable"] == "N"], where),
                "first_instances": [{k: r[k] for k in ("t", "symbol", "side", "price", "exec_price",
                                                       "opp_best", "shares", "rank", "ref", "h_ref")}
                                    for r in sorted(away, key=lambda r: r["t"])[:10]],
            },
            "at_display_price": {
                "not_head": len(at_display),
                "printable": tally(at_display, lambda r: r["printable"]),
                "continuous": sum(r["cont"] for r in at_display),
                "lowest_ref": sum(r["ref"] == r["ref_min"] for r in at_display),
                "paired_match": sum(r["match"] in pairs for r in at_display),
            },
        },
        "first_E_instances": [{k: r[k] for k in ("t", "symbol", "side", "price", "shares", "ref",
                                                 "o_t", "rank", "h_ref", "h_t", "head_fate")}
                              for r in sorted(E, key=lambda r: r["t"])[:10]],
        "top_symbols": tally(nh, lambda r: r["symbol"], 10),
    }


def not_best_section(d):
    nb = d["not_best"]
    crosses = {}
    for q in d["crosses"]:
        crosses.setdefault(q["symbol"], []).append(q)

    def nearest(r):
        return min(crosses[r["symbol"]], key=lambda q: abs(q["t"] - r["t"]))

    near = [nearest(r) for r in nb]
    dist = [abs(q["t"] - r["t"]) for r, q in zip(nb, near)]
    return {
        "total": len(nb),
        "continuous": sum(r["cont"] for r in nb),
        "by_kind": tally(nb, lambda r: r["kind"] + r["printable"]),
        "book_crossed_before": sum(r["crossed"] for r in nb),
        "nearest_cross_type": tally(near, lambda q: q["type"]),
        "at_nearest_cross_price": sum((r["exec_price"] if r["exec_price"] is not None else r["price"])
                                      == q["price"] for r, q in zip(nb, near)),
        "max_ns_from_nearest_cross": max(dist),
        "symbols": tally(nb, lambda r: r["symbol"]),
        "first_instances": [{k: r[k] for k in ("t", "symbol", "side", "price", "best", "exec_price",
                                               "shares", "state", "crossed")}
                            for r in sorted(nb, key=lambda r: r["t"])[:10]],
    }


def crossed_section(d, grace):
    ep = sorted(d["episodes"], key=lambda e: e["start"])
    waiting = d["crossed_awaiting_cross"]
    return {
        "touches_outside_continuous": d["counts"]["crossed_other"],
        "touches_continuous": d["counts"]["crossed_cont"],
        "episodes": len(ep),
        "touches_in_episodes": sum(e["touches"] for e in ep),
        "state_at_start": tally(ep, lambda e: e["state"]),
        "started_by": tally(ep, lambda e: e["cause_kind"]),
        "ended_by": tally(ep, lambda e: e["ended_by"]),
        "ended_outside_hours": sum(e["hours_at_end"] is False for e in ep),
        "symbols": tally(ep, lambda e: e["symbol"]),
        "episodes_listed": [{k: e.get(k) for k in ("symbol", "start", "end", "state", "bid", "ask",
                                                   "touches", "cause_kind", "ended_by")} for e in ep],
        "halt_grace": {
            "crossed_touches_after_resume_before_cross": len(waiting),
            "max_s_after_resume": max(w[2] for w in waiting) / 1e9,
            "continuous_at_5s_grace": sum(w[3] for w in waiting),
            "symbols": tally(waiting, lambda w: w[0]),
            "measured_runs": [{"halt_grace_s": r["halt_grace_s"],
                               "crossed_continuous": r["oracle"]["crossed_continuous"],
                               "episodes_continuous": r["oracle"]["episodes_continuous"],
                               "exec_checked_continuous": r["oracle"]["exec_checked_continuous"]}
                              for r in grace["runs"]],
        },
    }


def match_section(d, match):
    pairs = d["pairs"]
    paired = {p["match"] for p in pairs}
    crosses = {}
    for q in d["crosses"]:
        if q["shares"] > 0:
            crosses.setdefault(q["symbol"], []).append(q)
    unpaired = [n for n in d["nonprintable"] if n[2] not in paired]
    near = [min(crosses[n[0]], key=lambda q: abs(q["t"] - n[1])) for n in unpaired]
    gaps = [abs(q["t"] - n[1]) for n, q in zip(unpaired, near)]
    return {
        "distinct_match_numbers": match["distinct_match_numbers"],
        "on_two_messages": match["match_numbers_on_two_messages"],
        "by_full_scan": match["pairs"],
        "by_flags": tally(pairs, lambda p: p["a"][0] + p["a"][1] + "+" + p["b"][0] + p["b"][1]),
        "same_timestamp": sum(p["a"][7] == p["b"][7] for p in pairs),
        "same_shares": sum(p["a"][5] == p["b"][5] for p in pairs),
        "opposite_sides_where_both_known": sum(p["a"][3] != p["b"][3] for p in pairs
                                               if p["b"][3] != "?"),
        "both_sides_known": sum(p["b"][3] != "?" for p in pairs),
        "with_a_Q": sum(p["a"][0] == "Q" or p["b"][0] == "Q" for p in pairs),
        "printable_messages_per_pair": tally(pairs, lambda p: (p["a"][1] == "Y") + (p["b"][1] == "Y")),
        "nonprintable_total": len(d["nonprintable"]),
        "nonprintable_in_pairs": len(d["nonprintable"]) - len(unpaired),
        "nonprintable_elsewhere": {
            "count": len(unpaired),
            "nearest_nonzero_cross_type": tally(near, lambda q: q["type"]),
            "at_that_cross_price": sum(n[6] == q["price"] for n, q in zip(unpaired, near)),
            "max_ns_from_that_cross": max(gaps),
        },
        "first_instances": match["first_instances"][:5],
    }


def crosses_section(d, sources):
    cr = d["crosses"]
    nz = [q for q in cr if q["shares"] > 0]
    by_symbol_close = {q["symbol"]: q for q in cr if q["type"] == "C"}
    closes = []
    for src in sources["sources"]:
        factor = 1.0
        for sp in src["splits"]:
            factor *= sp["num"] / sp["den"]
        raw = round(src["yahoo_close"] * factor, 2)
        q = by_symbol_close[src["symbol"]]
        closes.append({"symbol": src["symbol"], "closing_cross_price": q["price"] / 10000,
                       "closing_cross_shares": q["shares"], "yahoo_close": src["yahoo_close"],
                       "split_factor": factor, "public_close_raw_basis": raw,
                       "difference": round(q["price"] / 10000 - raw, 4), "url": src["url"],
                       "splits_url": src["splits_url"], "splits": src["splits"]})
    return {
        "messages": len(cr),
        "zero_share": sum(q["shares"] == 0 for q in cr),
        "by_type_nonzero": tally(nz, lambda q: q["type"]),
        "by_type_zero": tally([q for q in cr if q["shares"] == 0], lambda q: q["type"]),
        "q_shares": sum(q["shares"] for q in nz),
        "nonprintable_shares_same_timestamp": sum(q["n_shares_same_t"] for q in nz),
        "crosses_where_same_timestamp_nonprintable_exceeds_q": sum(
            q["n_shares_same_t"] > q["shares"] for q in nz),
        "crosses_where_same_timestamp_nonprintable_equals_q": sum(
            q["n_shares_same_t"] == q["shares"] for q in nz),
        "printable_exec_shares_within_1ms_before": sum(q["y_shares_within_1ms"] for q in nz),
        "per_cross": [{"symbol": q["symbol"], "t": q["t"], "type": q["type"], "q_shares": q["shares"],
                       "nonprintable_same_t": q["n_shares_same_t"]}
                      for q in sorted(nz, key=lambda q: -q["shares"])[:25]],
        "closing_prices": closes,
        "closing_rounding": sources["rounding"],
        "closing_note": sources["note"],
        "closing_secondary": sources["secondary"],
        "closing_unreachable": sources["unreachable"],
        "closing_retrieved": sources["retrieved"],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rows", type=Path)
    ap.add_argument("match", type=Path)
    ap.add_argument("sources", type=Path)
    ap.add_argument("grace", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    with open(args.rows, "rb") as f:
        d = pickle.load(f)
    match = json.loads(args.match.read_text())
    sources = json.loads(args.sources.read_text())
    grace = json.loads(args.grace.read_text())
    commit = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    out = {
        "meta": {"commit": commit, "tools": "verify/recon_book.py, verify/recon_match.py, verify/recon_summary.py",
                 "data_file": "data/12302019.NASDAQ_ITCH50"},
        "python_oracle_counts": d["counts"],
        "exec_not_head": not_head_section(d),
        "exec_not_best": not_best_section(d),
        "crossed": crossed_section(d, grace),
        "match_numbers": match_section(d, match),
        "crosses": crosses_section(d, sources),
    }
    args.out.write_text(json.dumps(out, indent=1) + "\n")
    print(json.dumps({k: v for k, v in out.items() if k != "crosses"}, indent=0)[:100])
    return 0


if __name__ == "__main__":
    sys.exit(main())
