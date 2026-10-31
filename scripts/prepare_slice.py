#!/usr/bin/env python3
"""Decompress testdata/synthetic.itch.gz to a plain ITCH file the tools can map.

    prepare_slice.py [--out build/testdata/synthetic.itch]

The fixture is synthetic: scripts/gen_synthetic_itch.py writes it. The tools refuse gzip input on
purpose, so it is stored compressed and expanded into the build tree before it is replayed.
"""
import argparse
import gzip
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "testdata" / "synthetic.itch.gz"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "testdata" / "synthetic.itch")
    args = ap.parse_args()
    if not SRC.exists():
        print(f"prepare_slice: {SRC} not found", file=sys.stderr)
        return 2
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with gzip.open(SRC, "rb") as src, open(args.out, "wb") as dst:
        shutil.copyfileobj(src, dst)
    print(f"{args.out}: {args.out.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
