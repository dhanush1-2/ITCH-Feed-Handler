"""mold_udp_demo as a command: real loopback UDP against a direct file replay, on the synthetic day.

Environment (set by ctest): MOLD_UDP_DEMO, ITCH_REPLAY, ITCH_SLICE.
"""
import json
import os
import subprocess

import pytest

DEMO = os.environ.get("MOLD_UDP_DEMO")
REPLAY = os.environ.get("ITCH_REPLAY")
SLICE = os.environ.get("ITCH_SLICE")

pytestmark = pytest.mark.skipif(not (DEMO and REPLAY and SLICE), reason="run through ctest")

DUMP_AT_S = 43200
SLICE_MESSAGES = 46930


def run(*args, timeout=120):
    return subprocess.run([DEMO, *map(str, args)], capture_output=True, text=True, timeout=timeout)


def test_udp_replay_of_slice_matches_file_replay(tmp_path):
    state, out = tmp_path / "state.bin", tmp_path / "udp.json"
    r = run(SLICE, "--drop", "0.01", "--dump-state", state, "--dump-at-s", DUMP_AT_S, "--json", out)
    assert r.returncode == 0, (r.stdout, r.stderr)
    # The reference: itch_replay's own dump of the same file at the same feed second.
    ref = tmp_path / "ref.bin"
    d = subprocess.run([REPLAY, SLICE, "--dump-state", str(ref), "--dump-at-s", str(DUMP_AT_S)],
                       capture_output=True, text=True)
    assert d.returncode == 0, d.stderr
    # The end-of-day book is empty; the chosen second must hold orders or this proves nothing.
    assert state.stat().st_size > 0, "the comparison state must not be empty"
    assert ref.stat().st_size > 0
    assert state.read_bytes() == ref.read_bytes()

    j = json.loads(out.read_text())
    assert j["verdict"] == {"passed": True, "exit_code": 0}
    assert j["result"]["state"] == "Finished" and j["result"]["finished"] is True
    assert j["result"]["stream_matches_source"] is True
    assert j["result"]["stream_sha256"] == j["source"]["stream_sha256"]
    assert j["result"]["delivered_messages"] == j["source"]["messages"] == SLICE_MESSAGES
    assert j["dump"]["non_empty"] is True and j["dump"]["complete"] is True
    assert j["dump"]["resting_orders"] == state.stat().st_size // 19
    # Injected drops happened and were recovered over the retransmit socket.
    assert j["injected"]["downstream_dropped"] > 0
    assert j["rx"]["gaps_detected"] > 0 and j["rx"]["messages_from_retransmit"] > 0
    assert {"commit", "data_file", "cpu", "compiler", "flags"} <= set(j["meta"])
    assert j["config"]["drop"] == 0.01


def test_kernel_drops_and_injected_drops_are_reported_separately(tmp_path):
    out = tmp_path / "udp.json"
    r = run(SLICE, "--drop", "0.02", "--seed", 3, "--json", out)
    assert r.returncode == 0, (r.stdout, r.stderr)
    j = json.loads(out.read_text())
    k, inj, sock = j["kernel"], j["injected"], j["sockets"]
    assert set(k) == {"feed_datagrams_lost", "retransmit_datagrams_lost", "request_datagrams_lost",
                      "send_errors"}
    assert inj["downstream_dropped"] > 0
    # What the sender wrote to the socket is the packets the injector let through, no more.
    assert sock["feed_sent"] + k["send_errors"] <= j["injected"]["delivered_to_socket"]
    assert sock["feed_received"] == sock["feed_sent"] - k["feed_datagrams_lost"]
    assert sock["feed_received"] >= 0 and k["feed_datagrams_lost"] >= 0
    assert sock["rcvbuf_bytes"] > 0
    assert "latency" not in json.dumps(j).lower().replace("not a latency claim", "")


def test_lossless_run_has_no_injected_drops_and_check_direct_agrees(tmp_path):
    out = tmp_path / "udp.json"
    r = run(SLICE, "--dump-state", tmp_path / "s.bin", "--dump-at-s", DUMP_AT_S, "--check-direct",
            "--json", out)
    assert r.returncode == 0, (r.stdout, r.stderr)
    assert "IDENTICAL" in r.stdout
    j = json.loads(out.read_text())
    assert j["injected"]["downstream_dropped"] == 0
    assert j["dump"]["matches_direct"] is True


def test_a_tiny_receive_buffer_still_ends_identical(tmp_path):
    # An unpaced sender into a 64 KiB socket buffer: whatever the kernel discards is recovered
    # by re-request, and the accounting identities hold whether or not anything was discarded.
    state, out = tmp_path / "state.bin", tmp_path / "udp.json"
    r = run(SLICE, "--rate-mps", 0, "--rcvbuf", 65536, "--dump-state", state, "--dump-at-s",
            DUMP_AT_S, "--check-direct", "--json", out)
    assert r.returncode == 0, (r.stdout, r.stderr)
    j = json.loads(out.read_text())
    assert j["injected"]["downstream_dropped"] == 0
    assert j["sockets"]["rcvbuf_bytes"] > 0
    assert j["kernel"]["feed_datagrams_lost"] >= 0
    if j["kernel"]["feed_datagrams_lost"] > 0:
        # Something the kernel discarded had to come back by re-request. Reordering cannot occur
        # here, so a lost datagram can only have been delivered as a retransmit.
        assert j["rx"]["gaps_detected"] > 0
        assert j["rx"]["messages_from_retransmit"] > 0
    assert j["dump"]["matches_direct"] is True and j["dump"]["non_empty"] is True


def test_rate_limited_run_finishes(tmp_path):
    out = tmp_path / "udp.json"
    r = run(SLICE, "--rate-mps", 2_000_000, "--max-messages", 20000, "--json", out)
    assert r.returncode == 0, (r.stdout, r.stderr)
    j = json.loads(out.read_text())
    assert j["result"]["delivered_messages"] == 20000
    assert j["config"]["rate_mps"] == 2_000_000


def test_expired_retention_goes_stale_exits_3_and_terminates(tmp_path):
    state, out = tmp_path / "state.bin", tmp_path / "udp.json"
    r = run(SLICE, "--drop", "0.2", "--retention", 1, "--tick-ms", 1, "--dump-state", state,
            "--dump-at-s", DUMP_AT_S, "--json", out, timeout=60)
    assert r.returncode == 3, (r.stdout, r.stderr)
    assert "FAILED" in r.stderr
    assert not state.exists(), "a partial book must not be written under the real name"
    assert (tmp_path / "state.bin.incomplete").exists()
    j = json.loads(out.read_text())
    assert j["result"]["state"] == "Stale" and j["result"]["finished"] is False
    assert j["result"]["stale_reason"] == "retries_exhausted"
    assert j["verdict"] == {"passed": False, "exit_code": 3}


def test_a_feed_that_never_arrives_terminates_with_exit_3(tmp_path):
    out = tmp_path / "udp.json"
    r = run(SLICE, "--drop", "1", "--max-messages", 500, "--idle-timeout-ms", 300,
            "--end-wait-ms", 600, "--json", out, timeout=30)
    assert r.returncode == 3, (r.stdout, r.stderr)
    j = json.loads(out.read_text())
    assert j["result"]["finished"] is False
    assert j["result"]["state"] in ("Live", "Stale")
    assert j["result"]["gave_up"] == "idle_timeout"


@pytest.mark.parametrize("args", [
    ["--bogus"],
    ["--drop", "1.5"],
    ["--rate-mps", "fast"],
    ["--dump-at-s", "5"],
    ["--check-direct"],
    ["--retention", "0"],
    ["--tick-ms", "0"],
])
def test_bad_options_exit_2(args):
    r = run(SLICE, *args)
    assert r.returncode == 2, (args, r.stderr)


def test_missing_file_and_unreached_second_exit_2(tmp_path):
    assert run(tmp_path / "nope.itch").returncode == 2
    r = run(SLICE, "--dump-state", tmp_path / "s.bin", "--dump-at-s", 99999)
    assert r.returncode == 2
    assert "never reaches" in r.stderr


def test_check_direct_on_an_empty_state_fails(tmp_path):
    r = run(SLICE, "--dump-state", tmp_path / "s.bin", "--dump-at-s", 1, "--check-direct")
    assert r.returncode == 3, (r.stdout, r.stderr)
    assert "empty" in r.stderr
