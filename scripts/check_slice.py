#!/usr/bin/env python3
"""Replay the CI slice in full-verify mode and compare the export with the golden file.

    check_slice.py --replay <itch_replay> --slice <slice.itch> [--work dir]

Runs exactly the command the verify-slice CI job runs.
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--replay", type=Path, required=True)
    ap.add_argument("--slice", type=Path, required=True)
    ap.add_argument("--work", type=Path, default=ROOT / "build" / "testdata")
    args = ap.parse_args()
    symbols = (ROOT / "testdata" / "slice_symbols.txt").read_text().strip()
    args.work.mkdir(parents=True, exist_ok=True)
    out = args.work / "slice_export.json"
    r = subprocess.run([str(args.replay), str(args.slice), "--verify", "--full-check-every", "1",
                        "--symbols", symbols, "--export", str(out)],
                       capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    if r.returncode != 0 or "VERIFIED" not in r.stdout:
        print(f"check_slice: replay exited {r.returncode}", file=sys.stderr)
        return 1
    c = subprocess.run([sys.executable, str(ROOT / "scripts" / "compare_export.py"), str(out),
                        str(ROOT / "testdata" / "expected_slice.json")])
    return c.returncode


if __name__ == "__main__":
    sys.exit(main())
