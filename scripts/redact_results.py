#!/usr/bin/env python3
"""Move order-level and message-level records out of the committed result files.

    redact_results.py [--results results] [--local results/local]

The tools export sample records (first exceptions, cross rows) with their counts. The records are
Nasdaq's data, so the committed file keeps counts only and names what it dropped under "redacted".
The full file goes to results/local/, which is gitignored. Running it again changes nothing, and an
existing local copy is replaced only when a fresh full export arrives.
"""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RECON_2019 = ["exec_not_head.C.away_from_display_price.first_instances",
              "exec_not_head.first_E_instances", "exec_not_best.first_instances",
              "crossed.episodes_listed", "match_numbers.first_instances", "crosses.per_cross"]
RECON_2025 = ["inverted_later_than_60s_after_open", "crosses_on_those_symbols", "not_best_E"]
FIELDS = {
    "day_12302019.json": ["oracle.exceptions"],
    "day_20251208.json": ["oracle.exceptions"],
    "recon_12302019.json": RECON_2019,
    "recon_12302019_arrival_queue.json": RECON_2019,
    "recon_20251208.json": RECON_2025,
    "recon_20251208_arrival_queue.json": RECON_2025,
}


def drop(doc: dict, dotted: str) -> bool:
    parts = dotted.split(".")
    for p in parts[:-1]:
        doc = doc.get(p) if isinstance(doc, dict) else None
        if doc is None:
            return False
    return doc.pop(parts[-1], None) is not None


def redact_file(path: Path, local: Path, fields: list) -> bool:
    """True if the file held records, which are now only in the local copy."""
    text = path.read_text()
    doc = json.loads(text)
    dropped = [f for f in fields if drop(doc, f)]
    if not dropped:
        return False
    local.mkdir(parents=True, exist_ok=True)
    (local / path.name).write_text(text)
    doc["redacted"] = sorted(set(doc.get("redacted", []) + dropped))
    path.write_text(json.dumps(doc, separators=(",", ":")) + "\n")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--results", type=Path, default=ROOT / "results")
    ap.add_argument("--local", type=Path, default=ROOT / "results" / "local")
    args = ap.parse_args()
    for name, fields in FIELDS.items():
        changed = redact_file(args.results / name, args.local, fields)
        print(f"{name}: {'redacted' if changed else 'nothing to do'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
