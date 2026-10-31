#!/usr/bin/env python3
"""Build results/loss_table.json: mold_sim on a prefix of the real day at several loss levels.

    loss_table.py --sim build/release/tools/mold_sim --data data/12302019.NASDAQ_ITCH50 \
                  [--messages 50000000] [--out results/loss_table.json]

Runs mold_sim once per loss level (0 is the lossless baseline), each with --check-direct so the
books at the end of the prefix are compared with a direct replay of the same prefix, and each
writing its own JSON. The table is those JSON objects copied verbatim, no number recomputed,
plus a summary of whether every row's stream hash equals the baseline's. A row that did not
pass fails the script. The meta stamp is the one the runs share; runs that disagree on it
(a rebuild in the middle) fail the script.
"""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

LOSSES = [0.0, 0.001, 0.01, 0.05]
FIXED = ["--dup", "0.005", "--reorder", "16", "--burst", "0.0005,64",
         "--retransmit-loss", "0.01", "--seed", "1"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sim", type=Path, required=True)
    ap.add_argument("--data", type=Path, required=True)
    ap.add_argument("--messages", type=int, default=50_000_000)
    ap.add_argument("--out", type=Path, default=Path("results/loss_table.json"))
    ap.add_argument("--retention", type=int, default=1 << 20)
    args = ap.parse_args()

    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for loss in LOSSES:
            one = Path(tmp) / f"loss_{loss}.json"
            cmd = [str(args.sim), str(args.data), "--max-messages", str(args.messages),
                   "--loss", str(loss), *FIXED, "--retention", str(args.retention),
                   "--dump-state", str(Path(tmp) / "state.bin"), "--check-direct",
                   "--json", str(one)]
            print("+", " ".join(cmd), flush=True)
            r = subprocess.run(cmd, capture_output=True, text=True)
            sys.stdout.write(r.stdout)
            sys.stderr.write(r.stderr)
            if r.returncode != 0:
                print(f"loss_table: loss {loss} exited {r.returncode}", file=sys.stderr)
                return 1
            rows.append(json.loads(one.read_text()))

    meta = rows[0]["meta"]
    if any(row["meta"] != meta for row in rows):
        print("loss_table: the runs carry different meta stamps", file=sys.stderr)
        return 1
    base = rows[0]
    out_rows = []
    for row in rows:
        d = row["dump"]
        assert row["verdict"]["passed"] and d["non_empty"] and d["matches_direct"]
        out_rows.append({k: row[k] for k in ("config", "result", "rx", "channel", "dump")})
    table = {
        "meta": meta,
        "source": base["source"],
        "summary": {
            "loss_levels": LOSSES,
            "all_rows_passed": True,
            "all_streams_equal_baseline": all(
                r["result"]["stream_sha256"] == base["result"]["stream_sha256"] for r in rows),
            "all_states_equal_baseline": all(
                r["dump"]["sha256"] == base["dump"]["sha256"] for r in rows),
            "state_resting_orders": base["dump"]["resting_orders"],
        },
        "rows": out_rows,
    }
    assert table["summary"]["all_streams_equal_baseline"]
    assert table["summary"]["all_states_equal_baseline"]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(table, indent=2) + "\n")
    print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
