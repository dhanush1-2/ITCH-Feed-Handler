#!/usr/bin/env python3
"""Compare two itch_replay exports, ignoring what legitimately differs between machines.

    compare_export.py <actual.json> <expected.json>

Ignored: "meta" (commit, CPU, compiler, flags), "max_rss_bytes" and "state_dump" (a path).
Everything else must match exactly. Exits 0 when equal, 1 with the first difference.
"""
import json
import sys

IGNORED = {"meta", "max_rss_bytes", "state_dump"}


def first_diff(a, b, path="$"):
    if type(a) is not type(b):
        return f"{path}: {type(a).__name__} vs {type(b).__name__}"
    if isinstance(a, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a or k not in b:
                return f"{path}.{k}: present in only one file"
            d = first_diff(a[k], b[k], f"{path}.{k}")
            if d:
                return d
        return None
    if isinstance(a, list):
        if len(a) != len(b):
            return f"{path}: length {len(a)} vs {len(b)}"
        for i, (x, y) in enumerate(zip(a, b)):
            d = first_diff(x, y, f"{path}[{i}]")
            if d:
                return d
        return None
    return None if a == b else f"{path}: {a!r} vs {b!r}"


def load(p):
    with open(p) as f:
        d = json.load(f)
    return {k: v for k, v in d.items() if k not in IGNORED}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    d = first_diff(load(sys.argv[1]), load(sys.argv[2]))
    if d:
        print(f"exports differ at {d}", file=sys.stderr)
        return 1
    print("exports match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
