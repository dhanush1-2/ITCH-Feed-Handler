#!/usr/bin/env python3
"""Replay the book in Python and record the facts around each oracle exception.

    recon_book.py <file> --out rows.pickle [--grace-s 5] [--workers 10]

Runs one process per partition of the locates, each scanning the whole file and rebuilding its
own symbols. Output is a pickle of raw rows (not committed); recon_summary.py reduces it to the
committed results. The market-state rule mirrors include/book/market_state.hpp, so the counts
can be held against the C++ oracle's as a cross-check.
"""
import argparse
import bisect
import mmap
import multiprocessing as mp
import pickle
import sys
from collections import OrderedDict
from pathlib import Path
from struct import Struct

ADD = Struct(">QcI8sI")
MPID = Struct(">4s")
EXEC = Struct(">QIQ")
EXEC_PRICE = Struct(">QIQcI")
CANCEL = Struct(">QI")
DELETE = Struct(">Q")
REPLACE = Struct(">QQII")
TRADE = Struct(">QcI8sIQ")
CROSS = Struct(">Q8sIQc")


class Sym:
    __slots__ = ("name", "category", "state", "open_seen", "await_halt", "grace_until",
                 "bids", "asks", "last_resume", "last_cross", "last_cross_type", "crossed_since",
                 "episode", "matches", "recent", "last_halt")

    def __init__(self):
        self.name = ""
        self.category = ""
        self.state = "T"
        self.open_seen = False
        self.await_halt = False
        self.grace_until = 0
        self.bids = []          # ascending prices; best is last
        self.asks = []          # ascending prices; best is first
        self.last_resume = None
        self.last_halt = None
        self.last_cross = None
        self.last_cross_type = None
        self.crossed_since = None
        self.episode = None
        self.matches = {}
        self.recent = []        # executions since the last cross, for the per-cross ledger


class Order:
    __slots__ = ("side", "price", "shares", "seq", "t", "kind", "mpid", "lineage_seq",
                 "lineage_t", "u_same_price", "u_dir", "orig_shares", "pending")

    def __init__(self):
        self.pending = None


def run(args):
    path, part, parts, grace_ns = args
    with open(path, "rb") as f:
        buf = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    size = len(buf)
    pos = 0
    syms = {}
    orders = {}
    levels = {}
    in_hours = False
    seq = 0
    now = 0
    not_head, not_best, episodes, pairs, crosses, zero_q, closes = [], [], [], [], [], [], []
    counts = dict(checked=0, checked_cont=0, not_best=0, not_best_cont=0, not_head=0,
                  not_head_cont=0, crossed_cont=0, crossed_other=0, ep_cont=0, ep_other=0,
                  touches_cont=0, touches_other=0)
    # Two queue models at the best level, per kind: FIFO by message arrival, FIFO by ref number.
    # Key is (kind, printed-at-display, arrival-head, ref-head).
    models = {}
  
    late = {}
    max_ref = 0
    # Highest reference number seen by each feed second, to date a reference number.
    ref_by_second = {}
    # Adds and replaces that enter a level below a reference number already resting there,
    # grouped per symbol and timestamp: (symbol, t) -> [adds, of which replaces].
    inverted = {}
    nonprintable = []
    resume_cross = []
    crossed_awaiting_cross = []
    fates = {}

    def get(locate):
        s = syms.get(locate)
        if s is None:
            s = syms[locate] = Sym()
        return s

    def continuous(s):
        if not in_hours or s.state != "T":
            return False
        if s.category in ("Q", "G", "S") and not s.open_seen:
            return False
        return not s.await_halt or now >= s.grace_until

    def level_add(s, o, ref):
        key = (id(s), o.side, o.price)
        lv = levels.get(key)
        if lv is None:
            lv = levels[key] = OrderedDict()
            bisect.insort(s.bids if o.side == "B" else s.asks, o.price)
        lv[ref] = None

    def level_remove(s, o, ref):
        key = (id(s), o.side, o.price)
        lv = levels[key]
        del lv[ref]
        if not lv:
            del levels[key]
            arr = s.bids if o.side == "B" else s.asks
            del arr[bisect.bisect_left(arr, o.price)]

    def is_crossed(s):
        return bool(s.bids and s.asks and s.bids[-1] > s.asks[0])

    def settle(s, ref, how, t):
        """The first thing that happens to an order that was jumped."""
        for row in fates.pop(ref, ()):
            row["head_fate"] = how
            row["head_fate_dt_ns"] = t - row["t"]

    E, C, X, D, U, A, F, P, Q, H, R, S = (ord(c) for c in "ECXDUAFPQHRS")
    while pos + 2 <= size:
        length = (buf[pos] << 8) | buf[pos + 1]
        m = pos + 2
        pos = m + length
        kind = buf[m]
        locate = (buf[m + 1] << 8) | buf[m + 2]
        if kind == S:
            code = chr(buf[m + 11])
            now = int.from_bytes(buf[m + 5:m + 11], "big")
            if code == "Q":
                in_hours = True
                for s in syms.values():
                    s.open_seen = False
            elif code in "MECOS":
                in_hours = False
            continue
        if locate % parts != part:
            continue
        now = t = int.from_bytes(buf[m + 5:m + 11], "big")
        s = get(locate)
        body = m + 11
        touched = False
        ref_for_touch = 0
        if kind == A or kind == F:
            ref, side, shares, _, price = ADD.unpack_from(buf, body)
            o = Order()
            o.side, o.price, o.shares, o.seq, o.t = side.decode(), price, shares, seq, t
            o.kind = chr(kind)
            o.mpid = buf[m + 36:m + 40].decode().strip() if kind == F else ""
            o.lineage_seq, o.lineage_t, o.u_same_price, o.u_dir = seq, t, False, 0
            o.orig_shares = shares
            seq += 1
            was = is_crossed(s)
            orders[ref] = o
            lv = levels.get((id(s), o.side, price))
            if lv and ref < max(lv):
                rec = inverted.setdefault((s.name, t), [0, 0])
                rec[0] += 1
            level_add(s, o, ref)
            if ref > max_ref:
                max_ref = ref
                ref_by_second[t // 1_000_000_000] = ref
            touched = True
        elif kind == E or kind == C:
            if kind == E:
                ref, shares, match = EXEC.unpack_from(buf, body)
                flag, xprice = "Y", None
            else:
                ref, shares, match, f, xprice = EXEC_PRICE.unpack_from(buf, body)
                flag = f.decode()
            o = orders[ref]
            cont = continuous(s)
            counts["checked"] += 1
            counts["checked_cont"] += cont
            was = is_crossed(s)
            best = (s.bids[-1] if o.side == "B" else s.asks[0])
            status = "head"
            if best != o.price:
                status = "not_best"
                counts["not_best"] += 1
                counts["not_best_cont"] += cont
                not_best.append(dict(t=t, symbol=s.name, kind=chr(kind), printable=flag, ref=ref,
                                     side=o.side, price=o.price, best=best, shares=shares,
                                     exec_price=xprice, state=s.state, in_hours=in_hours,
                                     open_seen=s.open_seen, cont=cont,
                                     since_cross=None if s.last_cross is None else t - s.last_cross,
                                     last_cross_type=s.last_cross_type,
                                     since_resume=None if s.last_resume is None else t - s.last_resume,
                                     opposite=(s.asks[0] if o.side == "B" and s.asks else
                                               (s.bids[-1] if o.side == "S" and s.bids else None)),
                                     crossed=was))
            else:
                lv = levels[(id(s), o.side, o.price)]
                head = next(iter(lv))
                ref_min = min(lv)
                key = (chr(kind), xprice is None or xprice == o.price, head == ref, ref_min == ref,
                       cont)
                models[key] = models.get(key, 0) + 1
                if head != ref:
                    status = "not_head"
                    counts["not_head"] += 1
                    counts["not_head_cont"] += cont
                    h = orders[head]
                    rank, ahead, same_price_lineage_ahead, later_lineage = 0, 0, 0, 0
                    ahead_refs = []
                    for r in lv:
                        if r == ref:
                            break
                        q = orders[r]
                        rank += 1
                        ahead += q.shares
                        later_lineage += q.lineage_seq > o.lineage_seq
                        if rank <= 16:
                            ahead_refs.append(r)
                    row = dict(t=t, symbol=s.name, kind=chr(kind), printable=flag, ref=ref,
                               side=o.side, price=o.price, shares=shares, match=match,
                               exec_price=xprice, cont=cont, rank=rank, ahead_shares=ahead,
                               queue_len=len(lv), ref_min=ref_min,
                               refs_below=sum(1 for r in lv if r < ref),
                               later_lineage=later_lineage,
                               o_kind=o.kind, o_mpid=o.mpid, o_t=o.t, o_seq=o.seq,
                               o_lineage_t=o.lineage_t, o_lineage_seq=o.lineage_seq,
                               o_same_price_u=o.u_same_price, o_u_dir=o.u_dir, o_shares=o.shares,
                               o_orig=o.orig_shares,
                               h_ref=head, h_kind=h.kind, h_mpid=h.mpid, h_t=h.t, h_seq=h.seq,
                               h_lineage_t=h.lineage_t, h_lineage_seq=h.lineage_seq,
                               h_same_price_u=h.u_same_price, h_u_dir=h.u_dir, h_shares=h.shares,
                               h_orig=h.orig_shares,
                               head_fate=None, head_fate_dt_ns=None,
                               since_cross=None if s.last_cross is None else t - s.last_cross,
                               last_cross_type=s.last_cross_type,
                               since_resume=None if s.last_resume is None else t - s.last_resume,
                               opp_best=(s.asks[0] if o.side == "B" and s.asks else
                                         (s.bids[-1] if o.side == "S" and s.bids else None)),
                               crossed=was)
                    not_head.append(row)
                    fates.setdefault(head, []).append(row)
            if xprice is not None or flag == "N" or True:
                prev = s.matches.get(match)
                rec = (chr(kind), flag, ref, o.side, o.price, shares, xprice, t, status,
                       o.shares, o.t)
                if prev is None:
                    s.matches[match] = rec
                else:
                    pairs.append(dict(symbol=s.name, match=match, a=prev, b=rec))
            s.recent.append((t, chr(kind), flag, shares, o.price, xprice))
            if flag == "N":
                nonprintable.append((s.name, t, match, shares, o.side, o.price, xprice, status))
            if ref in fates:
                settle(s, ref, ("E" if kind == E else "C"), t)
            if shares >= o.shares:
                level_remove(s, o, ref)
                del orders[ref]
            else:
                o.shares -= shares
            touched = True
        elif kind == X:
            ref, shares = CANCEL.unpack_from(buf, body)
            o = orders[ref]
            was = is_crossed(s)
            if ref in fates:
                settle(s, ref, "X", t)
            if shares >= o.shares:
                level_remove(s, o, ref)
                del orders[ref]
            else:
                o.shares -= shares
            touched = True
        elif kind == D:
            ref, = DELETE.unpack_from(buf, body)
            o = orders.pop(ref)
            was = is_crossed(s)
            if ref in fates:
                settle(s, ref, "D", t)
            level_remove(s, o, ref)
            touched = True
        elif kind == U:
            old, new, shares, price = REPLACE.unpack_from(buf, body)
            o = orders.pop(old)
            was = is_crossed(s)
            if old in fates:
                settle(s, old, "U", t)
            level_remove(s, o, old)
            n = Order()
            n.side, n.price, n.shares, n.seq, n.t = o.side, price, shares, seq, t
            n.kind = "U"
            n.mpid = o.mpid
            same = price == o.price
            n.u_same_price = same
            n.u_dir = (shares > o.shares) - (shares < o.shares)
            n.lineage_seq = o.lineage_seq if same else seq
            n.lineage_t = o.lineage_t if same else t
            n.orig_shares = o.orig_shares
            seq += 1
            orders[new] = n
            lv = levels.get((id(s), n.side, price))
            if lv and new < max(lv):
                rec = inverted.setdefault((s.name, t), [0, 0])
                rec[0] += 1
                rec[1] += 1
            level_add(s, n, new)
            if new > max_ref:
                max_ref = new
                ref_by_second[t // 1_000_000_000] = new
            touched = True
        elif kind == P:
            _, _, shares, _, price, match = TRADE.unpack_from(buf, body)
            prev = s.matches.get(match)
            rec = ("P", "Y", 0, "?", price, shares, price, t, "trade", 0, 0)
            if prev is None:
                s.matches[match] = rec
            else:
                pairs.append(dict(symbol=s.name, match=match, a=prev, b=rec))
            continue
        elif kind == Q:
            shares, _, price, match, xtype = CROSS.unpack_from(buf, body)
            xt = xtype.decode()
            if shares == 0:
                zero_q.append((s.name, t, xt))
            before = [r for r in s.recent]
            crosses.append(dict(symbol=s.name, t=t, type=xt, shares=shares, price=price,
                                match=match,
                                n_shares_same_t=sum(r[3] for r in before if r[0] == t and r[2] == "N"),
                                y_shares_same_t=sum(r[3] for r in before if r[0] == t and r[2] == "Y"),
                                n_shares_since_last=sum(r[3] for r in before if r[2] == "N"),
                                y_shares_within_1ms=sum(r[3] for r in before
                                                        if t - r[0] <= 1_000_000 and r[2] == "Y"),
                                n_prints_since_last=sum(1 for r in before if r[2] == "N")))
            s.recent.clear()
            if xt in "HI" and s.last_resume is not None:
                resume_cross.append((s.name, s.last_resume, t, shares))
            s.last_cross, s.last_cross_type = t, xt
            if xt == "O":
                s.open_seen = True
            elif xt in "HI":
                s.await_halt = False
            continue
        elif kind == H:
            state = chr(buf[m + 19])
            if state == "T" and s.state != "T":
                s.await_halt = True
                s.grace_until = t + grace_ns
                s.last_resume = t
            elif state != "T" and s.state == "T":
                s.last_halt = t
            s.state = state
            continue
        elif kind == R:
            s.name = buf[m + 11:m + 19].decode().strip()
            s.category = chr(buf[m + 19])
            continue
        else:
            continue
        if len(s.recent) > 200_000:
            s.recent = s.recent[-1000:]
        if touched:
            cont = continuous(s)
            counts["touches_cont" if cont else "touches_other"] += 1
            now_crossed = is_crossed(s)
            if now_crossed:
                counts["crossed_cont" if cont else "crossed_other"] += 1
                if not was:
                    counts["ep_cont" if cont else "ep_other"] += 1
                    s.episode = dict(symbol=s.name, start=t, cont=cont, state=s.state,
                                     in_hours=in_hours, open_seen=s.open_seen,
                                     await_halt=s.await_halt,
                                     since_cross=None if s.last_cross is None else t - s.last_cross,
                                     last_cross_type=s.last_cross_type,
                                     since_resume=None if s.last_resume is None else t - s.last_resume,
                                     since_halt=None if s.last_halt is None else t - s.last_halt,
                                     bid=s.bids[-1], ask=s.asks[0], touches=0, max_spread=0,
                                     cause_kind=chr(kind), end=None, ended_by=None,
                                     hours_at_end=None)
                if s.state == "T" and s.await_halt:
                    crossed_awaiting_cross.append((s.name, t, t - s.last_resume, cont))
                ep = s.episode
                ep["touches"] += 1
                ep["max_spread"] = max(ep["max_spread"], s.bids[-1] - s.asks[0])
            elif was and s.episode is not None:
                ep = s.episode
                ep["end"] = t
                ep["ended_by"] = chr(kind)
                ep["hours_at_end"] = in_hours
                ep["state_at_end"] = s.state
                episodes.append(ep)
                s.episode = None
    for s in syms.values():
        if s.episode is not None:
            episodes.append(s.episode)
    return dict(counts=counts, not_head=not_head, not_best=not_best, episodes=episodes,
                pairs=pairs, crosses=crosses, zero_q=zero_q,
                live=len(orders), models=models, inverted=inverted, ref_by_second=ref_by_second,
                nonprintable=nonprintable, resume_cross=resume_cross,
                crossed_awaiting_cross=crossed_awaiting_cross,
                categories={s.name: s.category for s in syms.values() if s.name})


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("file")
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--grace-s", type=int, default=5)
    ap.add_argument("--workers", type=int, default=10)
    args = ap.parse_args()
    grace_ns = args.grace_s * 1_000_000_000
    jobs = [(args.file, i, args.workers, grace_ns) for i in range(args.workers)]
    with mp.Pool(args.workers) as pool:
        parts = pool.map(run, jobs, chunksize=1)
    merged = {}
    for key in ("not_head", "not_best", "episodes", "pairs", "crosses", "zero_q", "nonprintable",
                "resume_cross", "crossed_awaiting_cross"):
        merged[key] = [x for p in parts for x in p[key]]
    merged["counts"] = {k: sum(p["counts"][k] for p in parts) for k in parts[0]["counts"]}
    merged["live"] = sum(p["live"] for p in parts)
    merged["models"] = {}
    for p in parts:
        for k, v in p["models"].items():
            merged["models"][k] = merged["models"].get(k, 0) + v
    merged["inverted"] = {k: v for p in parts for k, v in p["inverted"].items()}
    merged["ref_by_second"] = {}
    for p in parts:
        for k, v in p["ref_by_second"].items():
            merged["ref_by_second"][k] = max(merged["ref_by_second"].get(k, 0), v)
    merged["categories"] = {k: v for p in parts for k, v in p["categories"].items()}
    with open(args.out, "wb") as f:
        pickle.dump(merged, f)
    print(merged["counts"], "live", merged["live"], {k: len(merged[k]) for k in ("not_head", "not_best", "episodes", "pairs", "crosses", "zero_q")})
    return 0


if __name__ == "__main__":
    sys.exit(main())
