#!/usr/bin/env python3
"""Compare the C++ per-symbol export with the Python reference, field by field.

    compare.py <cpp.json> <ref.json>

Exits 0 only on exact equality of everything but "meta", and only if at least one symbol has
resting orders at a checkpoint: a day ends with an empty book, so a comparison of nothing but
empty books would pass while proving nothing. Otherwise exits 1 and lists every difference by
symbol and field. The reference's per-symbol anomaly counts have no C++ counterpart, so they
are not compared, but any nonzero one fails: both sides have then dropped a message.
"""
import json
import sys

IGNORED = {"meta"}
MAX_LISTED = 200


def _walk(a, b, path, out):
    if type(a) is not type(b):
        out.append(f"{path}: {a!r} vs {b!r}")
    elif isinstance(a, dict):
        for key in sorted(set(a) | set(b)):
            if key not in a or key not in b:
                out.append(f"{path}.{key}: present in only one file")
            else:
                _walk(a[key], b[key], f"{path}.{key}", out)
    elif isinstance(a, list):
        if len(a) != len(b):
            out.append(f"{path}: length {len(a)} vs {len(b)}")
        for i, (x, y) in enumerate(zip(a, b)):
            _walk(x, y, f"{path}[{i}]", out)
    elif a != b:
        out.append(f"{path}: {a!r} vs {b!r}")


def _comparable(doc):
    out = {k: v for k, v in doc.items() if k not in IGNORED}
    out["symbols"] = {name: {k: v for k, v in sym.items() if k != "anomalies"}
                      for name, sym in doc["symbols"].items()}
    return out


def differences(a, b):
    out = []
    _walk(_comparable(a), _comparable(b), "", out)
    return out


def anomalies(doc):
    return [f"{name}.{key} = {n}" for name, sym in doc["symbols"].items()
            for key, n in sym.get("anomalies", {}).items() if n != 0]


def populated(doc):
    """Resting orders per checkpoint, summed over the symbols."""
    totals = {}
    for sym in doc["symbols"].values():
        for name, cp in sym["checkpoints"].items():
            totals[name] = totals.get(name, 0) + sum(lv[2] for lv in cp["bids"] + cp["asks"])
    return totals


def warnings(doc):
    totals = populated(doc)
    empty = sorted(name for name, n in totals.items() if n == 0)
    return [f"checkpoint {name} is not populated: no resting orders in any symbol"
            for name in empty]


def main_exit(a, b):
    diffs = differences(a, b)
    bad = anomalies(a) + anomalies(b)
    for d in diffs[:MAX_LISTED]:
        print(d)
    if len(diffs) > MAX_LISTED:
        print(f"... and {len(diffs) - MAX_LISTED} more")
    for line in bad:
        print(f"anomaly: {line}")
    if diffs or bad:
        print(f"DIFFERENT: {len(diffs)} differences, {len(bad)} anomalies")
        return 1
    totals = populated(a)
    for w in warnings(a):
        print(f"note: {w}")
    if not any(totals.values()):
        print("VACUOUS: the files agree, but no checkpoint holds a single resting order")
        return 1
    print(f"IDENTICAL: {len(a['symbols'])} symbols; resting orders compared per checkpoint: "
          + ", ".join(f"{k}={v}" for k, v in sorted(totals.items())))
    return 0


def main():
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    with open(sys.argv[1]) as f:
        a = json.load(f)
    with open(sys.argv[2]) as f:
        b = json.load(f)
    return main_exit(a, b)


if __name__ == "__main__":
    sys.exit(main())
