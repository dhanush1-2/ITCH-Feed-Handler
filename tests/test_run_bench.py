"""run_bench.py's refusals: too much load, partial output, and the meaning of "complete"."""
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("run_bench", ROOT / "scripts" / "run_bench.py")
rb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rb)


def test_quiet_machine_passes_and_returns_the_load():
    assert rb.wait_for_load(4.0, 0, getload=lambda: (1.5, 1.0, 1.0)) == [1.5, 1.0, 1.0]


def test_busy_machine_is_refused():
    with pytest.raises(SystemExit, match="load"):
        rb.wait_for_load(4.0, 0, getload=lambda: (9.0, 9.0, 9.0))


def test_load_may_fall_while_waiting():
    loads = iter([(9.0, 9.0, 9.0), (2.0, 5.0, 5.0)])
    slept = []
    got = rb.wait_for_load(4.0, 100, getload=lambda: next(loads), sleep=slept.append)
    assert got[0] == 2.0 and slept


def test_complete_only_with_every_set_and_no_rejection():
    assert rb.is_complete(True, {"a": 1}, ["a"], {})
    assert not rb.is_complete(False, {"a": 1}, ["a"], {})
    assert not rb.is_complete(True, {}, ["a"], {})
    assert not rb.is_complete(True, {"a": 1}, ["a"], {"a#attempt1": 1})


def test_write_atomic_leaves_old_file_until_replaced(tmp_path):
    out = tmp_path / "x.json"
    out.write_text("old")
    rb.write_atomic(out, "new")
    assert out.read_text() == "new" and list(tmp_path.iterdir()) == [out]


def _record(tmp_path, a, b):
    pa, pb, out = tmp_path / "a.json", tmp_path / "b.json", tmp_path / "out.json"
    pa.write_text(json.dumps(a))
    pb.write_text(json.dumps(b))
    r = subprocess.run([sys.executable, str(ROOT / "scripts" / "record_replay_compare.py"), str(pa),
                        str(pb), "--out", str(out)], capture_output=True, text=True)
    return r, out


def _export(**over):
    meta = {"commit": "abc", "data_file": "d", "cpu": "c", "compiler": "x", "flags": "f"}
    return {"meta": meta, "replay": {"speed": 10}, "state_dump": {"path": "p"}, "max_rss_bytes": 1,
            "per_symbol": {"volume": [1]}, "watch": {"AAPL": {}, "SPY": {}}, **over}


def test_record_of_a_match(tmp_path):
    r, out = _record(tmp_path, _export(), _export(replay={"speed": "max"}))
    assert r.returncode == 0, r.stderr
    d = json.loads(out.read_text())
    assert d["matches"] and d["first_difference"] is None and d["watched_symbols"] == ["AAPL", "SPY"]
    assert "state_dump" in d["ignored"] and d["compared"] == ["per_symbol", "watch"]
    assert d["meta"]["commit"] == "abc"


def test_record_of_a_mismatch_fails_and_says_where(tmp_path):
    r, out = _record(tmp_path, _export(), _export(per_symbol={"volume": [2]}))
    assert r.returncode == 1
    d = json.loads(out.read_text())
    assert d["matches"] is False and "per_symbol" in d["first_difference"]
