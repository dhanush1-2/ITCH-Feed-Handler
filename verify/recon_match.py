#!/usr/bin/env python3
"""Find every match number that appears on more than one E, C, P or Q message.

    recon_match.py <file> --json out.json

Reports the message-type pairs, whether their shares and prices agree, and the volume the
pairs would double count. Stdlib only and independent of the C++ handler.
"""
import argparse
import json
import mmap
import sys
from collections import Counter
from pathlib import Path
from struct import Struct

EXEC = Struct(">QIQ")
EXEC_PRICE = Struct(">QIQcI")
TRADE = Struct(">QcI8sIQ")
CROSS = Struct(">Q8sIQc")


def scan(path):
    with open(path, "rb") as f:
        buf = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    names, seen, pairs = {}, {}, []
    pos, size = 0, len(buf)
    e, c, p, q, r = ord("E"), ord("C"), ord("P"), ord("Q"), ord("R")
    index = 0
    while pos + 2 <= size:
        length = (buf[pos] << 8) | buf[pos + 1]
        m = pos + 2
        pos = m + length
        index += 1
        kind = buf[m]
        if kind == e:
            ref, shares, match = EXEC.unpack_from(buf, m + 11)
            rec = ("E", shares, None, "Y", ref)
        elif kind == c:
            ref, shares, match, flag, price = EXEC_PRICE.unpack_from(buf, m + 11)
            rec = ("C", shares, price, flag.decode(), ref)
        elif kind == p:
            _, _, shares, _, price, match = TRADE.unpack_from(buf, m + 11)
            rec = ("P", shares, price, "Y", 0)
        elif kind == q:
            shares, _, price, match, xtype = CROSS.unpack_from(buf, m + 11)
            rec = ("Q" + xtype.decode(), shares, price, "Y", 0)
        elif kind == r:
            names[(buf[m + 1] << 8) | buf[m + 2]] = buf[m + 11:m + 19].decode().strip()
            continue
        else:
            continue
        locate = (buf[m + 1] << 8) | buf[m + 2]
        ts = int.from_bytes(buf[m + 5:m + 11], "big")
        full = rec + (locate, ts, index)
        earlier = seen.get(match)
        if earlier is None:
            seen[match] = full
        else:
            pairs.append((match, earlier, full))
    return names, seen, pairs


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("file")
    ap.add_argument("--json", type=Path, required=True)
    args = ap.parse_args()
    names, seen, pairs = scan(args.file)
    table = Counter()
    shares_by_pair = Counter()
    same_shares = Counter()
    same_locate = Counter()
    both_printable = Counter()
    gap_ns_max = Counter()
    instances = []
    for match, a, b in pairs:
        key = f"{a[0]}+{b[0]}"
        table[key] += 1
        shares_by_pair[key] += min(a[1], b[1])
        same_shares[key] += a[1] == b[1]
        same_locate[key] += a[5] == b[5]
        both_printable[key] += a[3] == "Y" and b[3] == "Y"
        gap_ns_max[key] = max(gap_ns_max[key], abs(b[6] - a[6]))
        if len(instances) < 40:
            instances.append({
                "match": match,
                "first": {"kind": a[0], "shares": a[1], "price": a[2], "printable": a[3],
                          "ref": a[4], "symbol": names.get(a[5]), "t_ns": a[6], "msg": a[7]},
                "second": {"kind": b[0], "shares": b[1], "price": b[2], "printable": b[3],
                           "ref": b[4], "symbol": names.get(b[5]), "t_ns": b[6], "msg": b[7]},
            })
    result = {
        "file": Path(args.file).name,
        "distinct_match_numbers": len(seen),
        "match_numbers_on_two_messages": len(pairs),
        "pairs": {k: {"count": table[k], "same_shares": same_shares[k],
                      "same_locate": same_locate[k], "both_printable": both_printable[k],
                      "min_shares_sum": shares_by_pair[k], "max_gap_ns": gap_ns_max[k]}
                  for k in sorted(table)},
        "first_instances": instances,
    }
    args.json.parent.mkdir(parents=True, exist_ok=True)
    with open(args.json, "w") as f:
        json.dump(result, f, indent=1, sort_keys=True)
    print(json.dumps({k: result[k] for k in ("distinct_match_numbers", "match_numbers_on_two_messages", "pairs")}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
