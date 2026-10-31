#!/usr/bin/env python3
"""Write a valid ITCH 5.0 file from a seed. No byte comes from a real feed.

    gen_synthetic_itch.py --out day.itch [--seed 1]
    gen_synthetic_itch.py --write-fixtures       testdata/synthetic.itch.gz, testdata/
                                                 synthetic_symbols.txt and the fuzz seed

It simulates one invented trading day for eight invented symbols (TSTAA to TSTAH) and keeps its own
books, so every execution names a live order and the day ends empty. The prices and orders mean
nothing. What it does cover is what the handler has to get right:

  every order message type   A F E C X D U, plus P and Q, R S H
  opening and closing        S events in order, Q crosses of type O and C, zero-share Q (no cross),
                             non-printable C allocations at the cross
  a halt                     H state H, adds that cross the book while halted, resume, Q type H,
                             allocations that clear the cross, and a second halt with no crossing
  queue order                orders whose reference number precedes their display, added after the
                             open: the queue runs by reference, not by arrival
  match pairs                non-printable C plus printable E on one match number
  C away from display        a few C on an order behind the head of its level, as on real feeds

The random generator is written out here, so the bytes do not depend on the Python version. Standard
library only.
"""
import argparse
import bisect
import gzip
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURE = ROOT / "testdata" / "synthetic.itch.gz"
SYMBOLS_FILE = ROOT / "testdata" / "synthetic_symbols.txt"
FUZZ_SEED = ROOT / "fuzz" / "corpus" / "synthetic_first_64k.bin"
FUZZ_SEED_BYTES = 65536

NS = 1_000_000_000
TICK = 100  # raw price units per cent
# name, base price in dollars, market category. Q, G and S are Nasdaq-listed, so they await an
# opening cross; N does not.
SYMBOLS = [("TSTAA", 25, "Q"), ("TSTAB", 80, "Q"), ("TSTAC", 12, "G"), ("TSTAD", 150, "Q"),
           ("TSTAE", 40, "S"), ("TSTAF", 7, "G"), ("TSTAG", 60, "N"), ("TSTAH", 33, "N")]
LISTED = "QGS"
TARGET_LIVE = 130  # resting orders per symbol the day hovers around


class Rng:
    """64-bit LCG (Knuth's multiplier), high bits out."""

    def __init__(self, seed: int):
        self.x = (seed * 2 + 1) & (2**64 - 1)

    def u32(self) -> int:
        self.x = (self.x * 6364136223846793005 + 1442695040888963407) & (2**64 - 1)
        return self.x >> 32

    def below(self, n: int) -> int:
        return self.u32() % n

    def chance(self, percent: int) -> bool:
        return self.below(100) < percent


def be48(v: int) -> bytes:
    return v.to_bytes(6, "big")


class Side:
    """One side of a book: price -> sorted reference numbers."""

    def __init__(self, sign: int):
        self.sign = sign  # +1 bids (higher is better), -1 asks
        self.levels = {}

    def best(self):
        if not self.levels:
            return None
        return max(self.levels) if self.sign > 0 else min(self.levels)


class Book:
    def __init__(self, locate: int, name: str, base: int, category: str):
        self.locate, self.name, self.category = locate, name, category
        self.mid = base * 100 * TICK
        self.sides = {"B": Side(1), "S": Side(-1)}
        self.orders = {}  # ref -> [side, price, shares]
        self.halted = False


class Day:
    def __init__(self, seed: int):
        self.r = Rng(seed)
        self.out = bytearray()
        self.t = 0
        self.next_ref = 1000
        self.next_match = 1
        self.books = [Book(i + 1, *s) for i, s in enumerate(SYMBOLS)]
        self.count = 0

    # Wire ----------------------------------------------------------------------------------
    def emit(self, kind: str, locate: int, body: bytes) -> None:
        msg = kind.encode() + struct.pack(">HH", locate, 0) + be48(self.t) + body
        self.out += struct.pack(">H", len(msg)) + msg
        self.count += 1

    def tick(self, ns: int = 0) -> None:
        self.t += ns if ns else 1 + self.r.below(2_000_000)

    def system(self, code: str) -> None:
        self.emit("S", 0, code.encode())

    def stock(self, b: Book) -> bytes:
        return b.name.ljust(8).encode()

    def directory(self, b: Book) -> None:
        self.emit("R", b.locate, self.stock(b) + b.category.encode() + b"N" +
                  struct.pack(">I", 100) + b"N" + b"C" + b"  " + b"P" + b"N" + b" " + b"1" +
                  b"N" + struct.pack(">I", 0) + b"N")

    def action(self, b: Book, state: str) -> None:
        self.emit("H", b.locate, self.stock(b) + state.encode() + b" " + b"    ")

    def match(self) -> int:
        self.next_match += 1
        return self.next_match

    def cross(self, b: Book, shares: int, price: int, kind: str) -> None:
        self.emit("Q", b.locate, struct.pack(">Q", shares) + self.stock(b) +
                  struct.pack(">IQ", price, self.match()) + kind.encode())

    def trade(self, b: Book) -> None:
        price = b.mid + self.r.below(20) * TICK
        self.emit("P", b.locate, struct.pack(">Q", 0) + b"B" +
                  struct.pack(">I", 100 * (1 + self.r.below(5))) + self.stock(b) +
                  struct.pack(">IQ", price, self.match()))

    # Book ----------------------------------------------------------------------------------
    def place(self, b: Book, ref: int, side: str, price: int, shares: int) -> None:
        b.orders[ref] = [side, price, shares]
        bisect.insort(b.sides[side].levels.setdefault(price, []), ref)

    def lift(self, b: Book, ref: int) -> None:
        side, price, _ = b.orders.pop(ref)
        level = b.sides[side].levels[price]
        level.remove(ref)
        if not level:
            del b.sides[side].levels[price]

    def new_ref(self) -> int:
        self.next_ref += 1 + self.r.below(3)
        return self.next_ref

    def quote_price(self, b: Book, side: str) -> int:
        """A price on `side` that does not cross the other side."""
        sign = 1 if side == "B" else -1
        price = b.mid - sign * (1 + self.r.below(30)) * TICK
        other = b.sides["S" if side == "B" else "B"].best()
        if other is not None and sign * (price - other) >= 0:
            price = other - sign * (1 + self.r.below(5)) * TICK
        return max(price, TICK)

    def add(self, b: Book, ref=None, side=None, price=None, mpid=None) -> int:
        side = side or ("B" if self.r.chance(50) else "S")
        price = self.quote_price(b, side) if price is None else price
        shares = 100 * (1 + self.r.below(9)) + (self.r.below(99) if self.r.chance(10) else 0)
        ref = ref if ref is not None else self.new_ref()
        mpid = self.r.chance(8) if mpid is None else mpid
        body = struct.pack(">Q", ref) + side.encode() + struct.pack(">I", shares) + \
            self.stock(b) + struct.pack(">I", price)
        if mpid:
            self.emit("F", b.locate, body + b"TEST")
        else:
            self.emit("A", b.locate, body)
        self.place(b, ref, side, price, shares)
        return ref

    def touch(self, b: Book, side: str):
        """Head of the best level: the lowest reference there."""
        best = b.sides[side].best()
        return None if best is None else b.sides[side].levels[best][0]

    def any_order(self, b: Book):
        if not b.orders:
            return None
        keys = list(b.orders)
        return keys[self.r.below(len(keys))]

    def execute(self, b: Book, ref: int, shares=None, match=None) -> None:
        o = b.orders[ref]
        shares = min(o[2], shares or 1 + self.r.below(o[2]))
        self.emit("E", b.locate, struct.pack(">QIQ", ref, shares, match or self.match()))
        self.reduce(b, ref, shares)

    def execute_at(self, b: Book, ref: int, price: int, printable: bool, shares=None,
                   match=None) -> None:
        o = b.orders[ref]
        shares = min(o[2], shares or 1 + self.r.below(o[2]))
        self.emit("C", b.locate, struct.pack(">QIQ", ref, shares, match or self.match()) +
                  (b"Y" if printable else b"N") + struct.pack(">I", price))
        self.reduce(b, ref, shares)

    def reduce(self, b: Book, ref: int, shares: int) -> None:
        o = b.orders[ref]
        o[2] -= shares
        if o[2] == 0:
            self.lift(b, ref)

    def cancel(self, b: Book, ref: int) -> None:
        o = b.orders[ref]
        if o[2] < 2:
            return self.delete(b, ref)
        shares = 1 + self.r.below(o[2] - 1)
        self.emit("X", b.locate, struct.pack(">QI", ref, shares))
        o[2] -= shares

    def delete(self, b: Book, ref: int) -> None:
        self.emit("D", b.locate, struct.pack(">Q", ref))
        self.lift(b, ref)

    def replace(self, b: Book, ref: int) -> None:
        side = b.orders[ref][0]
        self.lift(b, ref)
        price = self.quote_price(b, side)
        shares = 100 * (1 + self.r.below(9))
        new = self.new_ref()
        self.emit("U", b.locate, struct.pack(">QQII", ref, new, shares, price))
        self.place(b, new, side, price, shares)

    # Phases --------------------------------------------------------------------------------
    def quoting(self, b: Book) -> None:
        """Book-building only: before the open, after the close."""
        ref = self.any_order(b)
        if ref is None or len(b.orders) < TARGET_LIVE or self.r.chance(30):
            self.add(b)
        elif self.r.chance(50):
            self.delete(b, ref)
        else:
            self.replace(b, ref)

    def continuous(self, b: Book) -> None:
        b.mid += (self.r.below(3) - 1) * TICK if self.r.chance(5) else 0
        roll = self.r.below(100)
        ref = self.any_order(b)
        add_below = 60 if len(b.orders) < TARGET_LIVE else 36 if len(b.orders) < 2 * TARGET_LIVE else 20
        if ref is None or roll < add_below:
            self.add(b)
        elif roll < 62 or (roll < 80 and len(b.orders) > 2 * TARGET_LIVE):
            self.delete(b, ref)
        elif roll < 80:
            self.replace(b, ref)
        elif roll < 85:
            self.cancel(b, ref)
        elif roll < 92:
            head = self.touch(b, "B" if self.r.chance(50) else "S")
            if head is not None:
                self.execute(b, head)
        elif roll < 95:
            self.execute_away(b)
        elif roll < 97:
            self.match_pair(b)
        else:
            self.trade(b)

    def execute_away(self, b: Book) -> None:
        """A C at a price better than the order's display price. Sometimes behind the head."""
        side = "B" if self.r.chance(50) else "S"
        best = b.sides[side].best()
        if best is None:
            return
        level = b.sides[side].levels[best]
        ref = level[-1] if len(level) > 1 and self.r.chance(30) else level[0]
        sign = 1 if side == "B" else -1
        self.execute_at(b, ref, best + sign * TICK, True)

    def match_pair(self, b: Book) -> None:
        """Two sides of one match: exactly one printable, so volume must count it once."""
        bid, ask = self.touch(b, "B"), self.touch(b, "S")
        if bid is None or ask is None:
            return
        shares = min(b.orders[bid][2], b.orders[ask][2], 100)
        price = b.orders[ask][1]
        match = self.match()
        self.execute_at(b, bid, price, False, shares, match)
        self.execute(b, ask, shares, match)

    def allocate(self, b: Book, price: int, rounds: int) -> int:
        """Non-printable allocations of a cross, from the head of each side."""
        total = 0
        for _ in range(rounds):
            for side in "BS":
                head = self.touch(b, side)
                if head is not None:
                    total += b.orders[head][2] // 2 + 1
                    self.execute_at(b, head, price, False, b.orders[head][2] // 2 + 1)
        return total

    def uncross(self, b: Book) -> int:
        """The halt cross clearing a crossed book. Returns the shares."""
        price = b.mid
        total = 0
        while True:
            bid, ask = b.sides["B"].best(), b.sides["S"].best()
            if bid is None or ask is None or bid < ask:
                return total
            for side in "BS":
                head = self.touch(b, side)
                total += b.orders[head][2]
                self.execute_at(b, head, price, False, b.orders[head][2])

    def halt(self, b: Book, crossing: bool) -> None:
        self.tick()
        self.action(b, "H")
        b.halted = True
        if crossing:
            ask, bid = b.sides["S"].best(), b.sides["B"].best()
            for i in range(4):
                self.tick()
                if ask is not None:
                    self.add(b, side="B", price=ask + (i + 1) * TICK, mpid=False)
                if bid is not None:
                    self.add(b, side="S", price=bid - (i + 1) * TICK, mpid=False)
        for _ in range(6):
            self.tick(NS)
            ref = self.any_order(b)
            if ref is not None and b.sides[b.orders[ref][0]].best() != b.orders[ref][1]:
                self.delete(b, ref)
        self.tick(NS)
        self.action(b, "T")
        b.halted = False
        self.tick(300_000)
        shares = self.uncross(b) + self.allocate(b, b.mid, 1)
        self.cross(b, shares, b.mid, "H")

    def run(self) -> None:
        hours = 3600 * NS
        self.t = 3 * hours
        self.system("O")
        for b in self.books:
            self.tick()
            self.directory(b)
            self.tick()
            self.action(b, "T")
        self.t = 4 * hours
        self.system("S")
        # Pre-market, and references taken now but displayed only after the open.
        held = {b.locate: [] for b in self.books}
        for _ in range(7000):
            self.t += 2_500_000
            b = self.books[self.r.below(len(self.books))]
            self.quoting(b)
            if self.r.chance(2):
                held[b.locate].append(self.new_ref())
        self.t = 9 * hours + 1800 * NS
        self.system("Q")
        for b in self.books:
            if b.category not in LISTED:
                continue
            self.tick()
            if b.locate == 3:  # no opening trade: a zero-share cross means no cross took place
                self.cross(b, 0, 0, "O")
                continue
            shares = self.allocate(b, b.mid, 2)
            self.cross(b, shares, b.mid, "O")
        # Held orders appear now with their old, lower references.
        for b in self.books:
            for ref in held[b.locate]:
                self.tick()
                side = "B" if self.r.chance(50) else "S"
                best = b.sides[side].best()
                if best is not None:
                    self.add(b, ref=ref, side=side, price=best, mpid=False)
        halts = {4000: (self.books[2], True), 9000: (self.books[6], True),
                 22000: (self.books[4], False), 30000: (self.books[2], True)}
        session = 6 * hours + 1800 * NS
        steps = 36000
        start = self.t
        for i in range(steps):
            self.t = max(self.t, start + i * (session - NS) // steps)
            if i in halts:
                self.halt(*halts[i])
            self.tick()
            self.continuous(self.books[self.r.below(len(self.books))])
        self.t = max(self.t, 16 * hours)
        self.system("M")
        for b in self.books:
            if b.category in LISTED:
                self.tick()
                shares = self.allocate(b, b.mid, 2)
                self.cross(b, shares, b.mid, "C")
        for _ in range(1000):
            self.t += 5_000_000
            self.quoting(self.books[self.r.below(len(self.books))])
        self.t = max(self.t, 20 * hours)
        self.system("E")
        for b in self.books:  # the day ends empty
            for ref in sorted(b.orders):
                self.tick()
                self.delete(b, ref)
        self.t = max(self.t, 20 * hours + 300 * NS)
        self.system("C")


def generate(seed: int) -> bytes:
    day = Day(seed)
    day.run()
    return bytes(day.out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--write-fixtures", action="store_true")
    args = ap.parse_args()
    if not args.out and not args.write_fixtures:
        ap.error("give --out or --write-fixtures")
    data = generate(args.seed)
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_bytes(data)
        print(f"{args.out}: {len(data)} bytes")
    if args.write_fixtures:
        FIXTURE.write_bytes(gzip.compress(data, 9, mtime=0))
        SYMBOLS_FILE.write_text(",".join(s[0] for s in SYMBOLS) + "\n")
        FUZZ_SEED.write_bytes(data[:FUZZ_SEED_BYTES])
        print(f"wrote {FIXTURE.name}, {SYMBOLS_FILE.name}, {FUZZ_SEED.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
