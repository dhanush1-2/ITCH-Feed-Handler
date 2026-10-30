#!/usr/bin/env python3
"""Decompress testdata/slice.itch.gz to a plain ITCH file the tools can map.

    prepare_slice.py [--out build/testdata/slice.itch]

The tools refuse gzip input on purpose, so the CI fixture is stored compressed and
expanded into the build tree (never into the source tree) before it is replayed.
"""
import argparse
import gzip
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "testdata" / "slice.itch.gz"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=ROOT / "build" / "testdata" / "slice.itch")
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
