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
    assert run(day, "--speed", "7").returncode == 2
    assert run(day, "--speed", "0").returncode == 2
    assert run(day, "--from", "9:30").returncode == 2
    assert run(day, "--from", "09:61:00", "--speed", "10").returncode == 2
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


ALL_PINS = {"exec_not_best": 0, "exec_not_best_continuous": 0, "exec_not_head": 1,
            "exec_not_head_continuous": 0, "exec_not_head_e": 1, "crossed_continuous": 0,
            "crossed_other": 0, "episodes_continuous": 0, "episodes_other": 0,
            "match_number_collisions": 0}


def not_head_day(path):
    """A book whose second order trades first: exec_not_head and its E count are 1 on this file."""
    path.write_bytes(frame([
        system_event(b"O"),
        directory(1, "AAA"),
        add(1, 1, b"B", 100, "AAA", 100000),
        add(1, 2, b"B", 100, "AAA", 100000),
        executed(1, 2, 100, 1),
        delete(1, 1),
        system_event(b"C", ts=5_000_000_000),
    ]))
    return path


def pinned_day(tmp_path, pins):
    path = not_head_day(tmp_path / "pin.itch")
    pin_file = tmp_path / "pins.json"
    pin_file.write_text(json.dumps({"note": "test", "pins": {"pin.itch": pins}}))
    return path, pin_file


def test_pin_at_the_count_passes(tmp_path):
    path, pins = pinned_day(tmp_path, ALL_PINS)
    r = run(path, "--verify", "--pins", str(pins))
    assert r.returncode == 0, r.stderr
    assert f"pins: {len(ALL_PINS)} applied" in r.stdout


@pytest.mark.parametrize("name", ["exec_not_head", "exec_not_head_e"])
def test_count_above_its_pin_fails_verify(tmp_path, name):
    path, pins = pinned_day(tmp_path, {**ALL_PINS, name: 0})
    r = run(path, "--verify", "--pins", str(pins))
    assert r.returncode == 3
    assert f"{name} = 1" in r.stderr


def test_unknown_pin_name_is_an_input_error(tmp_path):
    path, pins = pinned_day(tmp_path, {**ALL_PINS, "exec_not_heads": 1})
    assert run(path, "--verify", "--pins", str(pins)).returncode == 2


@pytest.mark.parametrize("missing", sorted(ALL_PINS))
def test_counter_missing_from_a_pin_object_is_an_error(tmp_path, missing):
    path, pins = pinned_day(tmp_path, {k: v for k, v in ALL_PINS.items() if k != missing})
    r = run(path, "--verify", "--pins", str(pins))
    assert r.returncode == 2
    assert missing in r.stderr


def test_file_without_pins_says_so_and_passes(tmp_path):
    path, pins = pinned_day(tmp_path, ALL_PINS)
    other = tmp_path / "other.itch"
    other.write_bytes(path.read_bytes())
    r = run(other, "--verify", "--pins", str(pins))
    assert r.returncode == 0, r.stderr
    assert "pins: none for other.itch" in r.stdout


def test_explicit_pins_file_must_exist(tmp_path):
    path, _ = pinned_day(tmp_path, ALL_PINS)
    assert run(path, "--verify", "--pins", str(tmp_path / "absent.json")).returncode == 2


def test_verify_applies_the_repo_pins_by_default(tmp_path):
    """No --pins: a file carrying a pinned name is held to results/expected_anomalies.json."""
    path = not_head_day(tmp_path / "12302019.NASDAQ_ITCH50")
    r = run(path, "--verify")
    assert r.returncode == 3      # exec_not_head_e is pinned at 0 for this name
    assert "exec_not_head_e = 1" in r.stderr
    assert "pins:" in r.stdout and "applied" in r.stdout


def test_verify_without_an_entry_runs_unpinned_by_default(day):
    r = run(day, "--verify")
    assert r.returncode == 0, r.stderr
    assert f"pins: none for {day.name}" in r.stdout


def test_pins_need_verify(tmp_path):
    path, pins = pinned_day(tmp_path, ALL_PINS)
    assert run(path, "--pins", str(pins)).returncode == 2


def test_export_orders_unknown_symbol_is_an_input_error(day, tmp_path):
    r = run(day, "--export-orders", "NOPE", str(tmp_path / "o.json"))
    assert r.returncode == 2


def test_paced_run_has_the_same_books_as_max_and_reports_its_pacing(day, tmp_path):
    fast, slow = tmp_path / "fast.json", tmp_path / "slow.json"
    assert run(day, "--export", str(fast)).returncode == 0
    r = run(day, "--speed", "10", "--export", str(slow))
    assert r.returncode == 0, r.stderr
    f, s = json.loads(fast.read_text()), json.loads(slow.read_text())
    assert f["replay"]["speed"] == "max" and f["replay"]["max_lag_ns"] is None
    assert s["replay"]["speed"] == 10
    # The file spans 4 s of feed time, so at least 0.4 s of wall time at 10x (a lower bound that
    # a run which did not pace could not meet), and generous headroom above it for a loaded runner.
    assert 0.35 < s["replay"]["paced_wall_s"] < 15.0
    assert s["replay"]["max_lag_ns"] >= 0
    assert "max_lag_at_feed_s" in s["replay"] and "max_lag_at_feed_s" in f["replay"]
    if s["replay"]["max_lag_ns"] == 0:
        assert s["replay"]["max_lag_at_feed_s"] is None
    else:
        assert 0 <= s["replay"]["max_lag_at_feed_s"] <= 4.0
    assert "progress:" in r.stdout
    for k in ("totals", "by_type", "per_second", "anomalies", "per_symbol"):
        assert f[k] == s[k], k


def test_from_replays_the_early_part_at_max_speed(day, tmp_path):
    # The feed ends at 4 s; --from 00:00:03 paces only the last second, so 0.1 s at 10x.
    out = tmp_path / "out.json"
    r = run(day, "--speed", "1", "--from", "00:00:03", "--export", str(out))
    assert r.returncode == 0, r.stderr
    j = json.loads(out.read_text())
    assert j["replay"]["from_feed_s"] == 3
    assert j["replay"]["paced_feed_s"] <= 1.001
    # One paced feed second takes at least a wall second at 1x; the early part is not paced, so
    # the whole run is far below the 3 s it would take if everything were. Wide upper bound.
    assert j["replay"]["paced_wall_s"] >= 0.9
    assert j["replay"]["wall_s"] < 30


def test_tui_needs_a_terminal(day):
    r = run(day, "--tui", "AAPL")
    assert r.returncode == 2
    assert "terminal" in r.stderr


def _run_tui_and_quit(day, extra, tmp_path):
    """Start --tui under a pty, press q after the first frames, return (rc, output)."""
    pty = pytest.importorskip("pty")
    import select
    import time
    m, s = pty.openpty()
    p = subprocess.Popen([str(BIN), str(day), "--speed", "1", "--tui", "AAPL", *extra],
                         stdin=s, stdout=s, stderr=s, close_fds=True)
    out = b""
    end = time.time() + 1.0
    while time.time() < end:
        r, _, _ = select.select([m], [], [], 0.1)
        if r:
            out += os.read(m, 65536)
    os.write(m, b"q")
    end = time.time() + 20
    while p.poll() is None and time.time() < end:
        r, _, _ = select.select([m], [], [], 0.1)
        if r:
            try:
                out += os.read(m, 65536)
            except OSError:
                break
    rc = p.poll()
    if rc is None:
        p.kill()
        pytest.fail("itch_replay did not stop after q")
    try:
        while select.select([m], [], [], 0.1)[0]:
            out += os.read(m, 65536)
    except OSError:
        pass
    return rc, out.decode(errors="replace")


def test_interrupted_run_with_export_exits_4_and_writes_nothing(day, tmp_path):
    out = tmp_path / "out.json"
    rc, text = _run_tui_and_quit(day, ["--export", str(out)], tmp_path)
    assert rc == 4, text
    assert "no export was written" in text
    assert not out.exists()


def test_interrupted_run_without_output_exits_0(day, tmp_path):
    rc, text = _run_tui_and_quit(day, [], tmp_path)
    assert rc == 0, text
    assert "no export was written" in text
