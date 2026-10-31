#!/usr/bin/env python3
"""Write the committed summary of the known-answer comparison.

    known_answer_summary.py results/cpp_subset_12302019.json results/ref_12302019.json \
        --out results/known_answer_summary.json

Both inputs list resting orders, so they stay local and gitignored. They are reproducible with the
commands in docs/DISCREPANCIES.md. The summary holds what verify/compare.py concluded, with counts,
and the SHA-256 of each input so that a local run can be matched to it. Exit 1 if they differ.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "verify"))
import compare  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cpp", type=Path)
    ap.add_argument("ref", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    cpp, ref = json.loads(args.cpp.read_text()), json.loads(args.ref.read_text())
    if sorted(cpp["symbols"]) != sorted(ref["symbols"]):
        print("known_answer_summary: the inputs cover different symbols", file=sys.stderr)
        return 2

    def one(doc, name):
        return {**doc, "symbols": {name: doc["symbols"][name]}}

    names = sorted(cpp["symbols"])
    same = sum(1 for n in names if not compare.differences(one(cpp, n), one(ref, n)))
    summary = {
        "meta": cpp["meta"],
        "note": "Summary only. The inputs hold resting orders; they are reproducible and not "
                "committed.",
        "symbols": len(names),
        "identical_symbols": same,
        "checkpoint_s": cpp["checkpoint_s"],
        "resting_orders_per_checkpoint": compare.populated(cpp),
        "cpp_file": args.cpp.name, "cpp_sha256": hashlib.sha256(args.cpp.read_bytes()).hexdigest(),
        "ref_file": args.ref.name, "ref_sha256": hashlib.sha256(args.ref.read_bytes()).hexdigest(),
    }
    args.out.write_text(json.dumps(summary, indent=2) + "\n")
    print(f"{same} of {len(names)} symbols identical")
    return 0 if same == len(names) else 1


if __name__ == "__main__":
    sys.exit(main())
