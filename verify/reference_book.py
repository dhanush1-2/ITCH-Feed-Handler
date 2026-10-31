#!/usr/bin/env python3
"""Independent reference for the ITCH 5.0 book and trade statistics.

    reference_book.py <file> (--symbols A,B | --symbols-file path) --json out.json
                      [--checkpoint-s 43200]

Written from the field tables in the ITCH spec (docs/spec/README.md), sharing no code with the
C++ handler. Its value is that it is built differently: orders live in one dict keyed by
reference number, there are no price levels and no queues during the replay, and the book only
comes into being when a checkpoint sorts the live orders. A book bug in the C++ structure
therefore has nothing to hide behind here.

The file is a sequence of messages, each preceded by a 2-byte big-endian length.

Rules taken from the spec:
  * 1.4: the effect of modify messages is cumulative; at zero display shares the order is dead.
  * 1.4.2, 1.5.2: "N" prints are left out of volume, to avoid double counting.
  * 1.5.1: P is included in volume and never touches the book.
  * 1.5.2: a Q carries the bulk cross volume, and may show zero shares when no cross took
    place. Zero shares is no trade, so it does not count as one.
  * 1.5.3: a B undoes the E, C or P with that match number.
  * 1.3.1: the reference number is assigned at receipt, so it, not message arrival, orders a
    level (measured, not stated by the spec: docs/DISCREPANCIES.md).
  * 1.4.5: U removes the old order and the new reference number takes its place by number.
    Side and symbol carry over.

Levels: [price, total shares, order count], bids high to low, asks low to high.
FIFO: [price, [[ref, shares], ...]] with the lowest reference number first.
"""
import argparse
import json
import mmap
import sys
from pathlib import Path
from struct import Struct

# Field layouts from offset 11, spec sections 1.3 to 1.5.3.
ADD = Struct(">QcI8sI")      # ref, side, shares, stock, price (F appends a 4-byte attribution)
EXEC = Struct(">QIQ")        # ref, shares, match
EXEC_PRICE = Struct(">QIQcI")  # ref, shares, match, printable, price
CANCEL = Struct(">QI")       # ref, shares
DELETE = Struct(">Q")        # ref
REPLACE = Struct(">QQII")    # old ref, new ref, shares, price
TRADE = Struct(">QcI8sIQ")   # ref (zero), side, shares, stock, price, match
CROSS = Struct(">Q8sIQc")    # shares, stock, price, match, cross type
BREAK = Struct(">Q")         # match


class Symbol:
    __slots__ = ("name", "counts", "executed", "cancelled", "deleted", "replaced_out",
                 "replaced_in", "volume", "trades", "unknown_ref", "over_execute",
                 "over_cancel", "broken_without_print")

    def __init__(self, name):
        self.name = name
        self.counts = {}
        self.executed = 0
        self.cancelled = 0
        self.deleted = 0
        self.replaced_out = 0
        self.replaced_in = 0
        self.volume = 0
        self.trades = 0
        self.unknown_ref = 0
        self.over_execute = 0
        self.over_cancel = 0
        self.broken_without_print = 0


def snapshot(orders, names):
    """Turn the live orders into per-symbol levels and FIFO lists, sorting only here."""
    by_key = {}
    for ref, (locate, side, price, shares) in orders.items():
        by_key.setdefault((locate, side, price), []).append((ref, shares))
    out = {name: {"bids": [], "asks": [], "fifo": {"B": [], "S": []}} for name in names.values()}
    for (locate, side, price) in sorted(by_key, key=lambda k: (k[0], k[1], -k[2] if k[1] == "B" else k[2])):
        entries = sorted(by_key[(locate, side, price)])
        book = out[names[locate]]
        book["bids" if side == "B" else "asks"].append(
            [price, sum(e[1] for e in entries), len(entries)])
        book["fifo"][side].append([price, [[e[0], e[1]] for e in entries]])
    return out


def build(path, symbols, checkpoint_s=43_200):
    wanted = {s.encode().ljust(8, b" ") for s in symbols}
    tracked = bytearray(65536)
    state = {}
    names = {}
    orders = {}
    prints = {}
    pending_checkpoint = checkpoint_s * 1_000_000_000
    noon = None

    with open(path, "rb") as f:
        buf = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    size = len(buf)
    pos = 0
    r_kind = ord("R")
    while pos + 2 <= size:
        length = (buf[pos] << 8) | buf[pos + 1]
        m = pos + 2
        pos = m + length
        if pending_checkpoint is not None and int.from_bytes(buf[m + 5:m + 11], "big") >= pending_checkpoint:
            noon = snapshot(orders, names)
            pending_checkpoint = None
        locate = (buf[m + 1] << 8) | buf[m + 2]
        if not tracked[locate]:
            if buf[m] == r_kind and buf[m + 11:m + 19] in wanted:
                name = buf[m + 11:m + 19].decode().strip()
                tracked[locate] = 1
                names[locate] = name
                state[locate] = Symbol(name)
            else:
                continue
        s = state[locate]
        kind = chr(buf[m])
        s.counts[kind] = s.counts.get(kind, 0) + 1
        body = m + 11

        if kind == "A" or kind == "F":
            ref, side, shares, _, price = ADD.unpack_from(buf, body)
            orders[ref] = [locate, side.decode(), price, shares]
        elif kind == "E" or kind == "C":
            if kind == "E":
                ref, shares, match = EXEC.unpack_from(buf, body)
                printable = True
            else:
                ref, shares, match, flag, _ = EXEC_PRICE.unpack_from(buf, body)
                printable = flag == b"Y"
            o = orders.get(ref)
            if o is None:
                s.unknown_ref += 1
                continue
            if shares > o[3]:
                s.over_execute += 1
                continue
            s.executed += shares
            if printable:
                s.volume += shares
                s.trades += 1
                prints[match] = (locate, shares)
            o[3] -= shares
            if o[3] == 0:
                del orders[ref]
        elif kind == "X":
            ref, shares = CANCEL.unpack_from(buf, body)
            o = orders.get(ref)
            if o is None:
                s.unknown_ref += 1
                continue
            if shares > o[3]:
                s.over_cancel += 1
                continue
            s.cancelled += shares
            o[3] -= shares
            if o[3] == 0:
                del orders[ref]
        elif kind == "D":
            ref, = DELETE.unpack_from(buf, body)
            o = orders.pop(ref, None)
            if o is None:
                s.unknown_ref += 1
                continue
            s.deleted += o[3]
        elif kind == "U":
            old, new, shares, price = REPLACE.unpack_from(buf, body)
            o = orders.pop(old, None)
            if o is None:
                s.unknown_ref += 1
                continue
            s.replaced_out += o[3]
            s.replaced_in += shares
            orders[new] = [locate, o[1], price, shares]
        elif kind == "P":
            _, _, shares, _, _, match = TRADE.unpack_from(buf, body)
            s.volume += shares
            s.trades += 1
            prints[match] = (locate, shares)
        elif kind == "Q":
            shares, _, _, _, _ = CROSS.unpack_from(buf, body)
            if shares > 0:
                s.volume += shares
                s.trades += 1
        elif kind == "B":
            match, = BREAK.unpack_from(buf, body)
            hit = prints.pop(match, None)
            if hit is None:
                s.broken_without_print += 1
            else:
                s.volume -= hit[1]
                s.trades -= 1

    if noon is None:   # the file ended before the checkpoint
        noon = snapshot(orders, names)
    end = snapshot(orders, names)
    buf.close()

    out = {}
    for s in state.values():
        out[s.name] = {
            "counts": dict(sorted(s.counts.items())),
            "executed_shares": s.executed,
            "cancelled_shares": s.cancelled,
            "deleted_shares": s.deleted,
            "replaced_out_shares": s.replaced_out,
            "replaced_in_shares": s.replaced_in,
            "printed_volume": s.volume,
            "trade_count": s.trades,
            "anomalies": {
                "unknown_ref": s.unknown_ref,
                "over_execute": s.over_execute,
                "over_cancel": s.over_cancel,
                "broken_without_print": s.broken_without_print,
            },
            "checkpoints": {str(checkpoint_s): noon[s.name], "eod": end[s.name]},
        }
    return {
        "meta": {"tool": "verify/reference_book.py", "data_file": str(path),
                 "python": sys.version.split()[0]},
        "checkpoint_s": checkpoint_s,
        "symbols": dict(sorted(out.items())),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("file")
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--symbols")
    group.add_argument("--symbols-file", type=Path)
    ap.add_argument("--json", type=Path, required=True)
    ap.add_argument("--checkpoint-s", type=int, default=43_200)
    args = ap.parse_args()
    if args.symbols_file:
        symbols = args.symbols_file.read_text().split()
    else:
        symbols = args.symbols.split(",")
    result = build(args.file, symbols, args.checkpoint_s)
    missing = sorted(set(symbols) - set(result["symbols"]))
    if missing:
        print(f"reference_book: not in the directory: {missing}", file=sys.stderr)
        return 2
    args.json.parent.mkdir(parents=True, exist_ok=True)
    with open(args.json, "w") as f:
        json.dump(result, f, separators=(",", ":"), sort_keys=True)
    print(f"{args.json}: {len(result['symbols'])} symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main())
