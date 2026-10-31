"""mold_sim as a command: exit codes, the dump files and the JSON, on the synthetic day.

Environment (set by ctest): MOLD_SIM, ITCH_REPLAY, ITCH_SLICE.
"""
import json
import os
import subprocess
from pathlib import Path

import pytest

SIM = os.environ.get("MOLD_SIM")
REPLAY = os.environ.get("ITCH_REPLAY")
SLICE = os.environ.get("ITCH_SLICE")

pytestmark = pytest.mark.skipif(not (SIM and REPLAY and SLICE), reason="run through ctest")

HOSTILE = ["--loss", "0.01", "--dup", "0.005", "--reorder", "16", "--burst", "0.0005,64",
           "--retransmit-loss", "0.01", "--seed", "1"]


def run(*args):
    return subprocess.run([SIM, *map(str, args)], capture_output=True, text=True)


def test_hostile_run_matches_a_direct_file_replay(tmp_path):
    state, out = tmp_path / "state.bin", tmp_path / "loss.json"
    r = run(SLICE, *HOSTILE, "--dump-state", state, "--dump-at-s", 43200, "--json", out)
    assert r.returncode == 0, r.stderr
    # The reference: itch_replay's own dump of the same file at the same feed second.
    ref = tmp_path / "ref.bin"
    d = subprocess.run([REPLAY, SLICE, "--dump-state", str(ref), "--dump-at-s", "43200"],
                       capture_output=True, text=True)
    assert d.returncode == 0, d.stderr
    assert state.stat().st_size > 0, "the comparison state must not be empty"
    assert state.read_bytes() == ref.read_bytes()

    j = json.loads(out.read_text())
    assert j["verdict"] == {"passed": True, "exit_code": 0}
    assert j["result"]["finished"] is True and j["result"]["state"] == "Finished"
    assert j["result"]["stream_matches_source"] is True
    assert j["result"]["stream_sha256"] == j["source"]["stream_sha256"]
    assert j["result"]["delivered_messages"] == j["source"]["messages"] == 46930
    assert j["dump"]["non_empty"] is True and j["dump"]["complete"] is True
    assert j["dump"]["resting_orders"] == state.stat().st_size // 19
    assert j["rx"]["gaps_detected"] > 0 and j["rx"]["messages_from_retransmit"] > 0
    assert j["channel"]["downstream_dropped"] > 0
    # Every counter and the whole configuration are recorded.
    assert set(j["rx"]) == {"gaps_detected", "dup_packets", "partial_overlaps", "wrong_session",
                            "malformed", "retransmit_requests", "messages_delivered_late",
                            "messages_from_retransmit",
                            "messages_buffered_peak", "buffer_overflow_drops", "max_gap_len"}
    assert set(j["channel"]) == {"sent", "dropped", "duplicated", "reordered", "delivered",
                                 "in_flight", "downstream_dropped", "retransmit_dropped",
                                 "messages_lost_downstream"}
    c = j["channel"]
    assert c["sent"] - c["dropped"] + c["duplicated"] == c["delivered"] + c["in_flight"]
    assert c["dropped"] == c["downstream_dropped"] + c["retransmit_dropped"]
    assert j["config"]["seed"] == 1 and j["config"]["loss"] == 0.01
    assert j["config"]["reorder_window"] == 16 and j["config"]["burst_len"] == 64
    assert {"commit", "data_file", "cpu", "compiler", "flags"} <= set(j["meta"])


def test_check_direct_compares_two_builders(tmp_path):
    r = run(SLICE, *HOSTILE, "--dump-state", tmp_path / "s.bin", "--dump-at-s", 43200,
            "--check-direct", "--json", tmp_path / "j.json")
    assert r.returncode == 0, r.stderr
    assert "IDENTICAL" in r.stdout
    j = json.loads((tmp_path / "j.json").read_text())
    assert j["dump"]["matches_direct"] is True
    assert j["dump"]["sha256"] == j["dump"]["direct_sha256"]


def test_expired_retention_exits_3_and_marks_the_dump_incomplete(tmp_path):
    state, out = tmp_path / "state.bin", tmp_path / "loss.json"
    r = run(SLICE, "--seed", 1, "--burst", "0.01,1000", "--retention", 100,
            "--dump-state", state, "--dump-at-s", 43200, "--json", out)
    assert r.returncode == 3, (r.stdout, r.stderr)
    assert "FAILED" in r.stderr
    assert not state.exists(), "a partial book must not be written under the real name"
    assert Path(str(state) + ".incomplete").exists()
    j = json.loads(out.read_text())
    assert j["result"]["state"] == "Stale" and j["result"]["finished"] is False
    assert j["result"]["stale_reason"] == "retries_exhausted"
    assert j["dump"]["complete"] is False and j["dump"]["path"].endswith(".incomplete")
    assert j["verdict"] == {"passed": False, "exit_code": 3}


def test_a_lossless_run_is_clean(tmp_path):
    out = tmp_path / "j.json"
    r = run(SLICE, "--json", out)
    assert r.returncode == 0, r.stderr
    j = json.loads(out.read_text())
    assert j["rx"]["gaps_detected"] == 0 and j["rx"]["retransmit_requests"] == 0
    assert j["channel"]["dropped"] == 0


def test_max_messages_uses_a_prefix(tmp_path):
    out = tmp_path / "j.json"
    r = run(SLICE, "--max-messages", 1000, "--loss", "0.02", "--json", out)
    assert r.returncode == 0, r.stderr
    j = json.loads(out.read_text())
    assert j["source"]["messages"] == 1000 and j["result"]["delivered_messages"] == 1000


@pytest.mark.parametrize("args", [
    ["--bogus"],
    ["--loss", "1.5"],
    ["--burst", "0.1"],
    ["--dump-at-s", "5"],
    ["--check-direct"],
    ["--retention", "0"],
])
def test_bad_options_exit_2(args):
    r = run(SLICE, *args)
    assert r.returncode == 2, (args, r.stderr)


def test_missing_file_and_unreached_second_exit_2(tmp_path):
    assert run(tmp_path / "nope.itch").returncode == 2
    r = run(SLICE, "--dump-state", tmp_path / "s.bin", "--dump-at-s", 99999)
    assert r.returncode == 2
    assert "never reaches" in r.stderr


def test_loss_only_run_recovers_exactly_what_was_lost(tmp_path):
    out = tmp_path / "j.json"
    r = run(SLICE, "--loss", "0.05", "--seed", 2, "--retransmit-loss", "0", "--json", out)
    assert r.returncode == 0, r.stderr
    j = json.loads(out.read_text())
    lost = j["channel"]["messages_lost_downstream"]
    assert lost > 0
    assert j["rx"]["messages_from_retransmit"] == lost == j["rx"]["messages_delivered_late"]


def test_check_direct_on_an_empty_state_fails(tmp_path):
    # Feed second 1 is before the slice's first message: the books are empty, so the comparison
    # proves nothing and must not pass.
    r = run(SLICE, "--dump-state", tmp_path / "s.bin", "--dump-at-s", 1, "--check-direct")
    assert r.returncode == 3, (r.stdout, r.stderr)
    assert "empty" in r.stderr


def test_a_failed_run_removes_the_earlier_dump(tmp_path):
    state = tmp_path / "state.bin"
    ok = run(SLICE, "--dump-state", state, "--dump-at-s", 43200)
    assert ok.returncode == 0 and state.exists()
    bad = run(SLICE, "--seed", 1, "--burst", "0.01,1000", "--retention", 100,
              "--dump-state", state, "--dump-at-s", 43200)
    assert bad.returncode == 3
    assert not state.exists(), "the previous run's file must not survive a failed run"
    assert Path(str(state) + ".incomplete").exists()
