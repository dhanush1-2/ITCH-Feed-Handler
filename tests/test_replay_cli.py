"""CLI tests for itch_replay, run against the built binary.

The binary comes from $ITCH_REPLAY (ctest sets it), else build/release/tools/itch_replay.
Every input is a small synthetic ITCH file written here with struct, independent of the C++
message builders.
"""
import gzip
import json
import os
import struct
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
BIN = Path(os.environ.get("ITCH_REPLAY", ROOT / "build" / "release" / "tools" / "itch_replay"))

pytestmark = pytest.mark.skipif(not BIN.exists(), reason=f"{BIN} is not built")


def hdr(kind, locate, ts):
    return kind + struct.pack(">HH", locate, 0) + ts.to_bytes(6, "big")


def sym(name):
    return name.encode().ljust(8, b" ")


def system_event(code, ts=0):
    return hdr(b"S", 0, ts) + code


def directory(locate, name, category=b"N"):
    return hdr(b"R", locate, 0) + sym(name) + category + b" " + struct.pack(">I", 100) + bytes(14)


def add(locate, ref, side, shares, name, price, ts=1_000_000_000):
    return (hdr(b"A", locate, ts) + struct.pack(">Q", ref) + side + struct.pack(">I", shares)
            + sym(name) + struct.pack(">I", price))


def executed(locate, ref, shares, match, ts=2_000_000_000):
    return hdr(b"E", locate, ts) + struct.pack(">QIQ", ref, shares, match)


def delete(locate, ref, ts=3_000_000_000):
    return hdr(b"D", locate, ts) + struct.pack(">Q", ref)


def frame(msgs):
    return b"".join(struct.pack(">H", len(m)) + m for m in msgs)


def good_day():
    return frame([
        system_event(b"O"),
        directory(1, "AAPL", b"Q"),
        directory(2, "MSFT", b"Q"),
        system_event(b"Q", 1),
        add(1, 1, b"B", 100, "AAPL", 1_500_000),
        add(1, 2, b"S", 100, "AAPL", 1_501_000, 1_100_000_000),
        add(2, 3, b"B", 50, "MSFT", 2_000_000),
        executed(1, 2, 40, 7),
        delete(1, 1),
        delete(1, 2),
        delete(2, 3),
        system_event(b"C", 4_000_000_000),
    ])


def run(path, *args):
    return subprocess.run([str(BIN), str(path), *args], capture_output=True, text=True, timeout=120)


@pytest.fixture
def day(tmp_path):
    p = tmp_path / "day.itch"
    p.write_bytes(good_day())
    return p


def test_verify_passes_on_a_good_file(day, tmp_path):
    out = tmp_path / "out.json"
    r = run(day, "--verify", "--full-check-every", "1", "--export", str(out))
    assert r.returncode == 0, r.stderr
    assert "VERIFIED" in r.stdout
    j = json.loads(out.read_text())
    assert j["verify"]["passed"] is True
    assert j["totals"]["messages"] == 12
    assert j["totals"]["live_orders_at_end"] == 0
    assert j["totals"]["printed_volume_shares"] == 40
    assert j["conservation"]["failures"] == 0
    assert len(j["per_second"]) == 86400


def test_truncated_input_fails_with_offset(day, tmp_path):
    data = day.read_bytes()
    bad = tmp_path / "cut.itch"
    bad.write_bytes(data[:-5])
    for extra in ([], ["--verify"]):
        r = run(bad, *extra)
        assert r.returncode == 2
        assert "offset" in r.stderr
        assert "VERIFIED" not in r.stdout


def test_gzip_input_refused(day, tmp_path):
    gz = tmp_path / "day.itch.gz"
    gz.write_bytes(gzip.compress(day.read_bytes()))
    r = run(gz)
    assert r.returncode == 2
    assert "fetch_itch.py" in r.stderr
    assert "VERIFIED" not in r.stdout


def test_symbol_filter_normalizes_case(day, tmp_path):
    out = tmp_path / "out.json"
    r = run(day, "--symbols", "aapl, msft", "--export", str(out))
    assert r.returncode == 0, r.stderr
    assert "watching AAPL" in r.stdout and "watching MSFT" in r.stdout
    watch = json.loads(out.read_text())["watch"]
    assert set(watch) == {"AAPL", "MSFT"}
    aapl = watch["AAPL"]
    # Second 1: bid and ask rest. Second 2: 40 of the ask's 100 shares execute, it still rests.
    # Second 3: both orders are deleted, so neither side has a price.
    assert aapl["t"] == [1, 2, 3]
    assert aapl["bid"] == [1_500_000, 1_500_000, None]
    assert aapl["ask"] == [1_501_000, 1_501_000, None]
    assert aapl["spread"] == [1000, 1000, None]


def test_unknown_symbol_reported(day):
    r = run(day, "--symbols", "AAPL,AAPLL")
    assert r.returncode == 2
    assert "AAPLL" in r.stderr
    assert "VERIFIED" not in r.stdout


def test_missing_end_of_messages_fails_verify(tmp_path):
    p = tmp_path / "noend.itch"
    p.write_bytes(frame([
        directory(1, "AAPL"),
        add(1, 1, b"B", 100, "AAPL", 1_500_000),
        delete(1, 1),
    ]))
    r = run(p, "--verify")
    assert r.returncode == 3
    assert "end-of-messages" in r.stderr
    assert "VERIFIED" not in r.stdout
    assert run(p).returncode == 0  # without --verify the file is merely replayed


def test_unknown_ref_fails_verify(tmp_path):
    p = tmp_path / "orphan.itch"
    p.write_bytes(frame([
        directory(1, "AAPL"),
        delete(1, 99),
        system_event(b"C", 4_000_000_000),
    ]))
    r = run(p, "--verify")
    assert r.returncode == 3
    assert "unknown_ref" in r.stderr


def test_capacity_error_is_exit_2(day):
    r = run(day, "--max-live-orders", "2")
    assert r.returncode == 2
    assert "--max-live-orders" in r.stderr
    assert "VERIFIED" not in r.stdout


def test_bad_usage_is_exit_2(day):
    assert run(day, "--no-such-flag").returncode == 2
    assert run(day, "--speed", "10").returncode == 2
    assert run(day.with_name("missing.itch")).returncode == 2


def test_dump_state_is_stamped_in_the_export(day, tmp_path):
    out, dump = tmp_path / "out.json", tmp_path / "state.bin"
    # Stop before the deletes so orders rest at the end.
    data = day.read_bytes()
    short = tmp_path / "short.itch"
    cut = len(frame([
        system_event(b"O"), directory(1, "AAPL", b"Q"), directory(2, "MSFT", b"Q"),
        system_event(b"Q", 1), add(1, 1, b"B", 100, "AAPL", 1_500_000),
        add(1, 2, b"S", 100, "AAPL", 1_501_000, 1_100_000_000),
        add(2, 3, b"B", 50, "MSFT", 2_000_000),
    ]))
    short.write_bytes(data[:cut])
    r = run(short, "--dump-state", str(dump), "--export", str(out))
    assert r.returncode == 0, r.stderr
    import hashlib
    j = json.loads(out.read_text())["state_dump"]
    assert j["bytes"] == dump.stat().st_size == 3 * 19
    assert j["sha256"] == hashlib.sha256(dump.read_bytes()).hexdigest()


def test_dump_at_second_captures_midday_state(day, tmp_path):
    out, dump = tmp_path / "out.json", tmp_path / "mid.bin"
    # The adds are at second 1, the execute at 2, the deletes at 3, so just before second 2
    # three orders rest; the end-of-file state is empty.
    r = run(day, "--dump-state", str(dump), "--dump-at-s", "2", "--export", str(out))
    assert r.returncode == 0, r.stderr
    j = json.loads(out.read_text())["state_dump"]
    assert j["at"] == "before_feed_second" and j["feed_second"] == 2
    assert j["bytes"] == dump.stat().st_size == 3 * 19
    r = run(day, "--dump-state", str(tmp_path / "end.bin"))
    assert r.returncode == 0 and (tmp_path / "end.bin").stat().st_size == 0
    r = run(day, "--dump-state", str(tmp_path / "never.bin"), "--dump-at-s", "99")
    assert r.returncode == 2 and "99" in r.stderr
    assert run(day, "--dump-at-s", "2").returncode == 2
