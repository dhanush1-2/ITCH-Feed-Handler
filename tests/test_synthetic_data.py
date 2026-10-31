"""The public tree holds synthetic market data only, and order-level results stay local."""
import gzip
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gen = module("gen_synthetic_itch")
redact = module("redact_results")


def test_fixtures_are_exactly_what_the_generator_writes():
    data = gen.generate(1)
    assert gzip.decompress((ROOT / "testdata" / "synthetic.itch.gz").read_bytes()) == data
    assert (ROOT / "fuzz" / "corpus" / "synthetic_first_64k.bin").read_bytes() == data[:65536]


def test_generator_is_deterministic_and_seeded():
    assert gen.generate(1) == gen.generate(1)
    assert gen.generate(1) != gen.generate(2)


def test_every_required_message_type_is_present():
    data = gen.generate(1)
    seen, i = set(), 0
    while i < len(data):
        n = int.from_bytes(data[i:i + 2], "big")
        seen.add(chr(data[i + 2]))
        i += 2 + n
    assert set("RSHAFECXDUPQ") <= seen


def test_real_data_files_are_gone():
    for rel in ("testdata/slice.itch.gz", "testdata/expected_slice.json",
                "testdata/slice_symbols.txt", "fuzz/corpus/real_first_64k.bin"):
        assert not (ROOT / rel).exists(), rel


def walk(o, path=""):
    if isinstance(o, dict):
        yield path, o
        for k, v in o.items():
            yield from walk(v, f"{path}.{k}")
    elif isinstance(o, list):
        for v in o:
            yield from walk(v, f"{path}[]")


def tracked(*pathspecs):
    r = subprocess.run(["git", "ls-files", *pathspecs], cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        pytest.skip("not a git checkout")
    return [ROOT / x for x in r.stdout.splitlines()]


def test_no_tracked_result_holds_order_or_message_records():
    files = [p for p in tracked("results", "dashboard/data") if p.suffix == ".json" and p.exists()]
    assert files
    for p in files:
        for path, d in walk(json.loads(p.read_text())):
            assert "ref" not in d, f"{p.name}{path}"
            assert not ("symbol" in d and ("t" in d or "t_ns" in d)), f"{p.name}{path}"
            assert "fifo" not in d, f"{p.name}{path}"


@pytest.mark.parametrize("name", ["cpp_subset_12302019.json", "ref_12302019.json"])
def test_order_level_files_are_not_tracked(name):
    assert not tracked(f"results/{name}")
    assert name in (ROOT / ".gitignore").read_text()


def test_known_answer_summary_has_counts_and_hashes_only():
    d = json.loads((ROOT / "results" / "known_answer_summary.json").read_text())
    assert d["meta"]["commit"] and d["identical_symbols"] == d["symbols"]
    assert len(d["cpp_sha256"]) == 64 and len(d["ref_sha256"]) == 64
    assert d["resting_orders_per_checkpoint"]["43200"] > 0


def test_redaction_moves_records_aside_and_is_repeatable(tmp_path):
    (tmp_path / "local").mkdir()
    f = tmp_path / "day_x.json"
    f.write_text(json.dumps({"meta": {"commit": "c"}, "oracle": {"exec_not_best": 3,
                 "exceptions": {"exec_not_best": [{"ref": 1, "symbol": "A", "t_ns": 2}]}}}))
    assert redact.redact_file(f, tmp_path / "local", ["oracle.exceptions"]) is True
    d = json.loads(f.read_text())
    assert "exceptions" not in d["oracle"] and d["oracle"]["exec_not_best"] == 3
    assert d["redacted"] == ["oracle.exceptions"]
    assert "exceptions" in json.loads((tmp_path / "local" / "day_x.json").read_text())["oracle"]
    keep = (tmp_path / "local" / "day_x.json").read_text()
    assert redact.redact_file(f, tmp_path / "local", ["oracle.exceptions"]) is False
    assert (tmp_path / "local" / "day_x.json").read_text() == keep


def test_summary_tool_reports_a_difference(tmp_path):
    doc = {"meta": {"commit": "c", "data_file": "d", "cpu": "x", "compiler": "y", "flags": "z"},
           "checkpoint_s": 43200,
           "symbols": {"A": {"counts": {"A": 1}, "checkpoints": {
               "43200": {"bids": [[1, 1, 1]], "asks": [], "fifo": {}}}}}}
    cpp, ref, out = tmp_path / "c.json", tmp_path / "r.json", tmp_path / "s.json"
    cpp.write_text(json.dumps(doc))
    ref.write_text(json.dumps({**doc, "symbols": {"A": {**doc["symbols"]["A"], "counts": {"A": 2}}}}))
    r = subprocess.run([sys.executable, str(ROOT / "scripts" / "known_answer_summary.py"), str(cpp),
                        str(ref), "--out", str(out)], capture_output=True, text=True)
    assert r.returncode == 1
    assert json.loads(out.read_text())["identical_symbols"] == 0
