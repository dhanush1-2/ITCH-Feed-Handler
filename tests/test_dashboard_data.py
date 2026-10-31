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


def _edit(path, fn):
    d = json.loads(path.read_text())
    fn(d)
    path.write_text(json.dumps(d))


@pytest.mark.parametrize("suffix", ["-dirty", "-stale-build", "-unverified", "-stale-build-dirty"])
def test_refuses_commit_stamps_that_are_not_clean(results, tmp_path, suffix):
    _edit(results / "loss_table.json", lambda d: d["meta"].update(commit=d["meta"]["commit"] + suffix))
    r = run(results, tmp_path / "out")
    assert r.returncode == 2
    assert "loss_table.json" in r.stderr and suffix.split("-")[1] in r.stderr
    assert not (tmp_path / "out" / "loss.json").exists()


def test_refuses_an_unknown_commit(results, tmp_path):
    _edit(results / "bench_12302019.json", lambda d: d["meta"].update(commit="unknown"))
    r = run(results, tmp_path / "out")
    assert r.returncode == 2 and "bench_12302019.json" in r.stderr


def test_refuses_2019_inputs_that_disagree_on_the_data_file(results, tmp_path):
    _edit(results / "recon_12302019.json", lambda d: d["meta"].update(data_file="data/other.itch"))
    r = run(results, tmp_path / "out")
    assert r.returncode == 2 and "data_file" in r.stderr and "recon_12302019.json" in r.stderr


def test_2025_inputs_are_checked_against_each_other_not_against_2019(results, tmp_path):
    assert run(results, tmp_path / "ok").returncode == 0  # 2019 and 2025 differ, and that is fine
    _edit(results / "recon_20251208.json", lambda d: d["meta"].update(data_file="data/x.txt"))
    r = run(results, tmp_path / "out")
    assert r.returncode == 2 and "data_file" in r.stderr


def test_provenance_counts_commits(results, tmp_path):
    assert run(results, tmp_path / "out").returncode == 0
    p = json.loads((tmp_path / "out" / "provenance.json").read_text())
    assert p["commit_count"] == len(p["commits"]) >= 2
    files = [f for c in p["commits"] for f in c["files"]]
    assert "loss_table.json" in files and len(files) == len(set(files))


def test_a_failed_recovery_is_exported_as_a_failure(results, tmp_path):
    _edit(results / "loss_full_day.json", lambda d: d["result"].update(stream_matches_source=False))
    _edit(results / "loss_table.json",
          lambda d: d["rows"][2]["result"].update(delivered_messages=d["rows"][2]["result"]["delivered_messages"] - 1))
    assert run(results, tmp_path / "out").returncode == 0
    loss = json.loads((tmp_path / "out" / "loss.json").read_text())
    stress = [f for f in loss["full_day"] if f["cmp"] != "reference"][0]
    assert stress["recovered_all"] is False and stress["stream_matches_source"] is False
    assert loss["rows"][2]["recovered_all"] is False
    verify = json.loads((tmp_path / "out" / "verify.json").read_text())
    assert verify["checks"]["bit_identical"] is False and verify["checks"]["stream_hash_equal"] is False


def test_a_differing_state_is_exported_as_differs(results, tmp_path):
    _edit(results / "loss_full_day.json", lambda d: d["dump"].update(sha256="0" * 64))
    _edit(results / "loss_table.json", lambda d: d["rows"][1]["dump"].update(matches_direct=False))
    assert run(results, tmp_path / "out").returncode == 0
    loss = json.loads((tmp_path / "out" / "loss.json").read_text())
    stress = [f for f in loss["full_day"] if f["cmp"] != "reference"][0]
    assert stress["cmp"] == "differs"
    assert stress["same_as_lossless"] is False and stress["same_as_max_speed_replay"] is False
    assert loss["rows"][1]["cmp"] == "differs"
    verify = json.loads((tmp_path / "out" / "verify.json").read_text())
    assert verify["checks"]["bit_identical"] is False


def test_lossless_rows_are_the_reference_not_a_self_comparison(results, tmp_path):
    assert run(results, tmp_path / "out").returncode == 0
    loss = json.loads((tmp_path / "out" / "loss.json").read_text())
    assert [r["cmp"] for r in loss["rows"] if r["kind"] == "baseline"] == ["reference"]
    assert [f["cmp"] for f in loss["full_day"] if f["loss"] == 0] == ["reference"]
    assert all(r["checkpoint"] for r in loss["rows"])
