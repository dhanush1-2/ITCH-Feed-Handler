#!/usr/bin/env python3
"""Reduce recon_book.py's rows for a day other than the pinned one to a compact findings file.

    recon_other_day.py <rows.pickle> <data file name> --out results/recon_<date>.json

Only this project's own pickle is loaded.
"""
import argparse
import json
import pickle
import subprocess
import sys
from collections import Counter
from pathlib import Path

OPEN_NS = 34_200 * 1_000_000_000
HOUR_NS = 3_600 * 1_000_000_000


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rows", type=Path)
    ap.add_argument("data_file")
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    with open(args.rows, "rb") as f:
        d = pickle.load(f)
    models, inv, nb = d["models"], d["inverted"], d["not_best"]
    crosses = {}
    for q in d["crosses"]:
        crosses.setdefault(q["symbol"], []).append(q)
    not_best_e = []
    for r in sorted((r for r in nb if r["kind"] == "E"), key=lambda r: r["t"]):
        near = min(crosses.get(r["symbol"], []), key=lambda q: abs(q["t"] - r["t"]), default=None)
        row = {k: r[k] for k in ("t", "symbol", "ref", "side", "price", "best", "opposite", "shares",
                                 "state", "crossed", "in_hours", "cont")}
        row["nearest_cross_type"] = near and near["type"]
        row["ns_from_cross"] = near and r["t"] - near["t"]
        row["at_cross_price"] = near and near["price"] == r["price"]
        not_best_e.append(row)
    late = sorted((k[1], k[0], v[0]) for k, v in inv.items() if k[1] - OPEN_NS >= 60 * 10**9)
    late_symbols = {s for _, s, _ in late}
    waiting = d["crossed_awaiting_cross"]
    commit = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    out = {
        "meta": {"commit": commit, "tool": "verify/recon_book.py, verify/recon_other_day.py",
                 "data_file": args.data_file},
        "python_oracle_counts": d["counts"],
        "E_at_best_level": sum(v for k, v in models.items() if k[0] == "E"),
        "E_lowest_ref": sum(v for k, v in models.items() if k[0] == "E" and k[3]),
        "E_first_to_arrive": sum(v for k, v in models.items() if k[0] == "E" and k[2]),
        "E_arrival_right_ref_wrong": sum(v for k, v in models.items()
                                         if k[0] == "E" and k[2] and not k[3]),
        "not_head_by_kind": Counter(r["kind"] + r["printable"] for r in d["not_head"]),
        "C_not_head_away_from_display": sum(r["kind"] == "C" and r["exec_price"] != r["price"]
                                            for r in d["not_head"]),
        "inverted_adds": sum(v[0] for v in inv.values()),
        "inverted_from_replace": sum(v[1] for v in inv.values()),
        "inverted_by_hour": dict(sorted(Counter(k[1] // HOUR_NS for k, v in inv.items()
                                                for _ in range(v[0])).items())),
        "inverted_later_than_60s_after_open": [{"t": t, "symbol": s, "orders": n} for t, s, n in late],
        "crosses_on_those_symbols": [{k: q[k] for k in ("symbol", "t", "type", "shares", "price")}
                                     for s in sorted(late_symbols) for q in crosses.get(s, [])],
        "halt_cross_on_those_symbols": [r for r in d["resume_cross"] if r[0] in late_symbols],
        "pairs": Counter(p["a"][0] + p["a"][1] + "+" + p["b"][0] + p["b"][1] for p in d["pairs"]),
        "zero_share_q": len(d["zero_q"]),
        "q_messages": len(d["crosses"]),
        "episodes_state_at_start": Counter(e["state"] for e in d["episodes"]),
        "crossed_touches_after_resume_before_cross": len(waiting),
        "max_s_after_resume": max((w[2] for w in waiting), default=0) / 1e9,
        "not_best_by_kind": Counter(r["kind"] + r["printable"] for r in nb),
        "not_best_E": not_best_e,
    }
    args.out.write_text(json.dumps(out, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
