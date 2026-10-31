#!/usr/bin/env python3
"""Record whether a paced replay's export equals a max-speed run's.

    record_replay_compare.py <paced.json> <max_speed.json> --out results/replay_max_compare.json

Uses compare_export.py's rule. It compares the exports: the per-symbol share ledger, counts and
top of book per second for the watched symbols. It does not compare book state, because
"state_dump" is ignored and the paced run takes no dump. The stamp is the max-speed run's, the run
this script's caller just made; the paced run's commit is recorded beside it. Exit 0 on a match,
1 on a difference. The record is written either way.
"""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compare_export  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("paced", type=Path)
    ap.add_argument("max_speed", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    paced_doc = json.loads(args.paced.read_text())
    max_doc = json.loads(args.max_speed.read_text())
    a, b = compare_export.load(args.paced), compare_export.load(args.max_speed)
    diff = compare_export.first_diff(a, b)
    record = {
        "meta": max_doc["meta"],
        "paced_file": args.paced.name,
        "paced_commit": paced_doc["meta"]["commit"],
        "paced_speed": paced_doc.get("replay", {}).get("speed"),
        "compared": sorted(set(a) | set(b)),
        "ignored": sorted(compare_export.IGNORED),
        "watched_symbols": sorted(a.get("watch", {})),
        "matches": diff is None,
        "first_difference": diff,
        "note": "Compares exports, not book state: no state dump is taken at the paced speed.",
    }
    args.out.write_text(json.dumps(record, indent=2) + "\n")
    print("exports match" if diff is None else f"exports differ at {diff}")
    return 0 if diff is None else 1


if __name__ == "__main__":
    sys.exit(main())
