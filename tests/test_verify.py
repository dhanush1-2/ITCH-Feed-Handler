"""Tests for the independent Python reference and the comparison tool.

The stream here is hand-built with struct from the field tables in docs/spec, and the expected
values were worked out by hand in the comments, so no C++ is involved in any expectation.
"""
import copy
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "verify"))

import compare  # noqa: E402
import reference_book  # noqa: E402

NOON = 43_200 * 1_000_000_000


def frame(body):
    return struct.pack(">H", len(body)) + body


def hdr(kind, locate, ts):
    return kind + struct.pack(">HH", locate, 0) + ts.to_bytes(6, "big")


def sym(name):
    return name.encode().ljust(8, b" ")


def directory(locate, name):
    return hdr(b"R", locate, 0) + sym(name) + b"N " + struct.pack(">I", 100) + bytes(14)


def add(locate, ref, side, shares, name, price, ts, mpid=None):
    body = struct.pack(">QcI", ref, side, shares) + sym(name) + struct.pack(">I", price)
    if mpid is None:
        return hdr(b"A", locate, ts) + body
    return hdr(b"F", locate, ts) + body + mpid


def execute(locate, ref, shares, match, ts):
    return hdr(b"E", locate, ts) + struct.pack(">QIQ", ref, shares, match)


def execute_price(locate, ref, shares, match, printable, price, ts):
    return hdr(b"C", locate, ts) + struct.pack(">QIQcI", ref, shares, match, printable, price)


def cancel(locate, ref, shares, ts):
    return hdr(b"X", locate, ts) + struct.pack(">QI", ref, shares)


def delete(locate, ref, ts):
    return hdr(b"D", locate, ts) + struct.pack(">Q", ref)


def replace(locate, old, new, shares, price, ts):
    return hdr(b"U", locate, ts) + struct.pack(">QQII", old, new, shares, price)


def trade(locate, shares, name, price, match, ts):
    return (hdr(b"P", locate, ts) + struct.pack(">QcI", 0, b"B", shares) + sym(name)
            + struct.pack(">IQ", price, match))


def cross(locate, shares, name, price, match, kind, ts):
    return (hdr(b"Q", locate, ts) + struct.pack(">Q", shares) + sym(name)
            + struct.pack(">IQ", price, match) + kind)


def broken(locate, match, ts):
    return hdr(b"B", locate, ts) + struct.pack(">Q", match)


def stream():
    """Two symbols; only AAA is tracked. The expected values are derived by hand below."""
    t0 = NOON - 1_000
    t1 = NOON + 1_000
    msgs = [
        directory(1, "AAA"),
        directory(2, "BBB"),
        # Resting book before noon. Bids at 10.00: 1 then 2. Bid at 9.99: 3. Ask at 10.01: 4.
        add(1, 1, b"B", 100, "AAA", 100000, t0),
        add(1, 2, b"B", 200, "AAA", 100000, t0, mpid=b"NSDQ"),
        add(1, 3, b"B", 300, "AAA", 99900, t0),
        add(1, 4, b"S", 400, "AAA", 100100, t0),
        add(2, 9, b"B", 999, "BBB", 500000, t0),            # untracked, must have no effect
        execute(1, 1, 40, 1001, t0),                        # 1: 60 left, keeps its place
        execute_price(1, 2, 50, 1002, b"Y", 100050, t0),    # 2: 150 left, printable
        execute_price(1, 3, 25, 1003, b"N", 99900, t0),     # 3: 275 left, not printable
        cancel(1, 4, 100, t0),                              # 4: 300 left
        # Noon checkpoint falls here, on the first message at or after feed second 43200,
        # and that message is for the untracked symbol.
        add(2, 10, b"S", 1, "BBB", 600000, NOON),
        replace(1, 1, 5, 70, 100000, t1),                   # 1 gone, 5 goes behind 2 at 10.00
        delete(1, 3, t1),                                   # 275 deleted
        trade(1, 500, "AAA", 100000, 1004, t1),             # no book effect
        cross(1, 1000, "AAA", 100000, 1005, b"C", t1),      # no book effect
        cross(1, 0, "AAA", 100000, 1006, b"O", t1),         # zero shares: no trade happened
        trade(1, 7, "AAA", 100000, 1007, t1),
        broken(1, 1007, t1),                                # the 7-share trade is undone
        execute(1, 4, 300, 1008, t1),                       # 4 fully executed, gone
    ]
    return b"".join(frame(m) for m in msgs)


def run_ref(tmp_path):
    path = tmp_path / "hand.itch"
    path.write_bytes(stream())
    return reference_book.build(str(path), ["AAA"], checkpoint_s=43_200)


def test_reference_matches_handbuilt_stream(tmp_path):
    out = run_ref(tmp_path)
    aaa = out["symbols"]["AAA"]
    # R A F A A E C C X U D P Q Q P B E
    assert aaa["counts"] == {"A": 3, "B": 1, "C": 2, "D": 1, "E": 2, "F": 1, "P": 2, "Q": 2,
                             "R": 1, "U": 1, "X": 1}
    assert aaa["executed_shares"] == 40 + 50 + 25 + 300
    assert aaa["cancelled_shares"] == 100
    assert aaa["deleted_shares"] == 275
    assert aaa["replaced_out_shares"] == 60
    assert aaa["replaced_in_shares"] == 70
    # 40 E + 50 printable C + 500 P + 1000 Q + 300 E. Non-printable C, zero Q, broken P are out.
    assert aaa["printed_volume"] == 1890
    assert aaa["trade_count"] == 5

    noon = aaa["checkpoints"]["43200"]
    assert noon["bids"] == [[100000, 210, 2], [99900, 275, 1]]
    assert noon["asks"] == [[100100, 300, 1]]
    assert noon["fifo"]["B"] == [[100000, [[1, 60], [2, 150]]], [99900, [[3, 275]]]]
    assert noon["fifo"]["S"] == [[100100, [[4, 300]]]]

    end = aaa["checkpoints"]["eod"]
    assert end["bids"] == [[100000, 220, 2]]
    assert end["asks"] == []
    assert end["fifo"]["B"] == [[100000, [[2, 150], [5, 70]]]]
    assert end["fifo"]["S"] == []


def test_reference_rejects_a_modify_of_an_unknown_order(tmp_path):
    path = tmp_path / "bad.itch"
    path.write_bytes(frame(directory(1, "AAA")) + frame(execute(1, 77, 1, 1, 5)))
    out = reference_book.build(str(path), ["AAA"], checkpoint_s=43_200)
    assert out["symbols"]["AAA"]["anomalies"]["unknown_ref"] == 1


def test_reference_needs_no_third_party_imports():
    src = (ROOT / "verify" / "reference_book.py").read_text()
    imported = {line.split()[1].split(".")[0] for line in src.splitlines()
                if line.startswith(("import ", "from "))}
    assert imported <= set(sys.stdlib_module_names)


def test_compare_passes_on_equal_and_ignores_meta(tmp_path):
    a = run_ref(tmp_path)
    b = copy.deepcopy(a)
    b["meta"] = {"other": 1}
    assert compare.differences(a, b) == []


def test_compare_detects_one_share_difference(tmp_path):
    a = run_ref(tmp_path)
    b = copy.deepcopy(a)
    b["symbols"]["AAA"]["executed_shares"] += 1
    diffs = compare.differences(a, b)
    assert len(diffs) == 1
    assert "AAA" in diffs[0] and "executed_shares" in diffs[0]


def test_compare_detects_fifo_order_difference(tmp_path):
    a = run_ref(tmp_path)
    b = copy.deepcopy(a)
    b["symbols"]["AAA"]["checkpoints"]["43200"]["fifo"]["B"][0][1].reverse()
    diffs = compare.differences(a, b)
    assert diffs and "43200" in diffs[0] and "fifo" in diffs[0]


def test_compare_exit_codes(tmp_path):
    a = run_ref(tmp_path)
    b = copy.deepcopy(a)
    pa, pb = tmp_path / "a.json", tmp_path / "b.json"
    pa.write_text(json.dumps(a))
    pb.write_text(json.dumps(b))
    tool = [sys.executable, str(ROOT / "verify" / "compare.py"), str(pa), str(pb)]
    assert subprocess.run(tool, capture_output=True).returncode == 0
    b["symbols"]["AAA"]["trade_count"] += 1
    pb.write_text(json.dumps(b))
    r = subprocess.run(tool, capture_output=True, text=True)
    assert r.returncode == 1
    assert "AAA" in r.stdout + r.stderr


def test_compare_refuses_a_vacuous_comparison(tmp_path):
    """An empty book at both checkpoints must not read as agreement on the book."""
    a = run_ref(tmp_path)
    for side in ("bids", "asks"):
        a["symbols"]["AAA"]["checkpoints"]["43200"][side] = []
    a["symbols"]["AAA"]["checkpoints"]["43200"]["fifo"] = {"B": [], "S": []}
    assert any("populated" in w for w in compare.warnings(a))


def test_cpp_export_matches_reference_on_handbuilt_stream(tmp_path):
    """The whole loop: both implementations, one hand-built file, exact equality."""
    binary = Path(os.environ.get("ITCH_REPLAY", ROOT / "build" / "release" / "tools" / "itch_replay"))
    if not binary.exists():
        pytest.skip(f"{binary} is not built")
    path = tmp_path / "hand.itch"
    path.write_bytes(stream())
    out = tmp_path / "cpp.json"
    r = subprocess.run([str(binary), str(path), "--export-orders", "AAA", str(out)],
                       capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    ref = reference_book.build(str(path), ["AAA"], checkpoint_s=43_200)
    assert compare.differences(json.loads(out.read_text()), ref) == []
    assert compare.main_exit(json.loads(out.read_text()), ref) == 0


def test_compare_fails_on_a_reference_anomaly(tmp_path):
    a = run_ref(tmp_path)
    b = copy.deepcopy(a)
    b["symbols"]["AAA"]["anomalies"]["unknown_ref"] = 1
    assert compare.main_exit(a, b) == 1


def test_compare_exit_on_empty_books_is_failure(tmp_path):
    a = run_ref(tmp_path)
    for cp in a["symbols"]["AAA"]["checkpoints"].values():
        cp["bids"], cp["asks"] = [], []
        cp["fifo"] = {"B": [], "S": []}
    assert compare.main_exit(a, copy.deepcopy(a)) == 1
