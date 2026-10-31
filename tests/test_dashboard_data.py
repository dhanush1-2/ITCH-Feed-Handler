"""Tests for scripts/export_dashboard.py, the step between results/ and the dashboard.

The fixtures copy the committed results into a temp directory. The paced-replay export
(results/replay_10x.json) may not exist when these run, so a stand-in is made from the full-day
export with two more watched symbols; only the checks that do not depend on its pacing use it.
"""
import importlib.util
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "export_dashboard.py"
RESULTS = ROOT / "results"
NEEDED = ["day_12302019.json", "day_20251208.json", "loss_table.json", "loss_full_day.json",
          "loss_full_day_lossless.json", "bench_12302019.json", "recon_12302019.json",
          "recon_12302019_arrival_queue.json", "recon_20251208.json", "cpp_subset_12302019.json",
          "ref_12302019.json"]

spec = importlib.util.spec_from_file_location("export_dashboard", SCRIPT)
ed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ed)


@pytest.fixture
def results(tmp_path):
    d = tmp_path / "results"
    d.mkdir()
    for n in NEEDED:
        shutil.copy(RESULTS / n, d / n)
    day = json.loads((RESULTS / "day_12302019.json").read_text())
    for extra in ("X1", "X2"):
        day["watch"][extra] = day["watch"]["AAPL"]
    (d / "replay_10x.json").write_text(json.dumps(day))
    return d


def run(results, out, *args):
    return subprocess.run([sys.executable, str(SCRIPT), str(results), str(out), *args],
                          capture_output=True, text=True)


def test_export_rejects_missing_meta(results, tmp_path):
    day = json.loads((results / "day_12302019.json").read_text())
    del day["meta"]
    (results / "day_12302019.json").write_text(json.dumps(day))
    r = run(results, tmp_path / "out")
    assert r.returncode == 2
    assert "day_12302019.json" in r.stderr and "meta" in r.stderr
    assert not (tmp_path / "out" / "day.json").exists()


def test_export_rejects_a_stamp_missing_a_field(results, tmp_path):
    bench = json.loads((results / "bench_12302019.json").read_text())
    del bench["meta"]["cpu"]
    (results / "bench_12302019.json").write_text(json.dumps(bench))
    r = run(results, tmp_path / "out")
    assert r.returncode == 2 and "cpu" in r.stderr


def test_day_json_size_limit(results, tmp_path):
    r = run(results, tmp_path / "out")
    assert r.returncode == 0, r.stderr
    assert (tmp_path / "out" / "day.json").stat().st_size <= 5_000_000
    r = run(results, tmp_path / "small", "--max-day-bytes", "10000")
    assert r.returncode == 2 and "over the" in r.stderr


def test_every_section_names_its_file_and_commit(results, tmp_path):
    assert run(results, tmp_path / "out").returncode == 0
    for name in ("day", "loss", "verify", "bench"):
        j = json.loads((tmp_path / "out" / f"{name}.json").read_text())
        assert j["sources"], name
        for s in j["sources"].values():
            assert s["file"].endswith(".json") and len(s["commit"]) >= 7


def test_refuses_when_the_paced_replay_differs_from_the_max_speed_run(results, tmp_path):
    rep = json.loads((results / "replay_10x.json").read_text())
    rep["totals"]["messages"] += 1
    (results / "replay_10x.json").write_text(json.dumps(rep))
    r = run(results, tmp_path / "out")
    assert r.returncode == 2 and "totals" in r.stderr


def test_one_sided_spread_is_null():
    watch = {"locate": 1, "t": [10, 20, 30, 40],
             "bid": [1_000_000, None, 1_000_000, 1_000_000],
             "ask": [1_000_100, 1_000_100, None, 1_000_200],
             "bid_sz": [5, None, 5, 5], "ask_sz": [7, 7, None, 7]}
    g = ed.top_of_book_grid(watch, 0, 40, 10)
    # samples at 0, 10, 20, 30, 40: nothing yet, both sides, ask only, bid only, both sides
    assert g["bid"] == [None, 100.0, None, 100.0, 100.0]
    assert g["ask"] == [None, 100.01, 100.01, None, 100.02]
    assert g["spread"] == [None, 0.01, None, None, 0.02]
    assert 0 not in [x for x in g["spread"] if x is not None]
    assert g["samples_one_sided"] == 2 and g["samples_empty"] == 1
    assert g["bid_size"][2] is None and g["ask_size"][3] is None


def test_grid_carries_the_last_row_forward():
    watch = {"locate": 1, "t": [10, 50], "bid": [100, 200], "ask": [110, 210],
             "bid_sz": [1, 2], "ask_sz": [3, 4]}
    g = ed.top_of_book_grid(watch, 0, 60, 20)
    assert g["bid"] == [None, 0.01, 0.01, 0.02]
    assert g["ask"][3] == 0.021


def test_per_minute_buckets():
    ps = [0] * 86400
    ps[61] = 600
    m = ed.per_minute(ps)
    assert m["peak"][1] == 600 and m["avg"][1] == 10.0 and len(m["avg"]) == 1440
