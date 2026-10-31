"""The showcase documents stay honest: generated blocks match results/, images and links exist,
no placeholder is left, every result is stamped.

Everything here needs committed files only, because CI has no data files.
"""
import importlib.util
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
GEN = ROOT / "scripts" / "gen_docs.py"
REGEN = ROOT / "scripts" / "regenerate_results.sh"
DOCS = ["README.md", "DESIGN.md", "docs/VERIFICATION.md", "docs/INTERVIEW.md", "docs/RESUME.md"]
GENERATED_IN = DOCS + ["docs/OPTIMIZATION-LOG.md", "docs/METHODOLOGY.md"]
# Stamped results have a top-level meta, except these, which have the stated shape.
NOT_RESULTS = {"expected_anomalies.json": "hand-maintained pins that --verify enforces"}
UNSTAMPED = {"ref_12302019.json": "the reference writes no stamp; compare.py pairs it with "
                                  "cpp_subset_12302019.json, which has one"}
PLACEHOLDER = re.compile(r"\[__\]|\[N\]|\[CPU\]|\bTBD\b|\bTODO\b|\bFIXME\b|\bXXX\b")
BLOCK_OPEN = "<!-- gen:"
BLOCK_CLOSE = "<!-- /gen -->"


def gen_module():
    spec = importlib.util.spec_from_file_location("gen_docs", GEN)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def text(rel):
    return (ROOT / rel).read_text()


def run_gen(*args):
    return subprocess.run([sys.executable, str(GEN), *args], capture_output=True, text=True)


def test_generated_blocks_are_fresh():
    r = run_gen("--check")
    assert r.returncode == 0, r.stdout + r.stderr


@pytest.fixture
def tree(tmp_path):
    """A copy of the docs next to the real results, for tests that damage a document."""
    (tmp_path / "docs").mkdir()
    for rel in GENERATED_IN:
        shutil.copy(ROOT / rel, tmp_path / rel)
    (tmp_path / "results").symlink_to(ROOT / "results")
    (tmp_path / "verify").symlink_to(ROOT / "verify")
    (tmp_path / "docs" / "images").symlink_to(ROOT / "docs" / "images")
    return tmp_path


def test_check_notices_an_edited_number(tree):
    assert run_gen("--check", "--root", str(tree)).returncode == 0
    readme = tree / "README.md"
    new = re.sub(r"(<!-- gen:apply_mps -->).*?(<!-- /gen -->)", r"\g<1>99.99 M\g<2>",
                 readme.read_text(), count=1)
    assert new != readme.read_text()
    readme.write_text(new)
    r = run_gen("--check", "--root", str(tree))
    assert r.returncode != 0 and "apply_mps" in r.stdout + r.stderr


def test_write_mode_repairs_what_check_found(tree):
    readme = tree / "README.md"
    readme.write_text(re.sub(r"(<!-- gen:apply_mps -->).*?(<!-- /gen -->)", r"\g<1>0\g<2>",
                             readme.read_text()))
    assert run_gen("--root", str(tree)).returncode == 0
    assert run_gen("--check", "--root", str(tree)).returncode == 0


def test_unknown_block_is_an_error(tree):
    readme = tree / "README.md"
    readme.write_text(readme.read_text() + "\nvalue <!-- gen:no_such_block -->1<!-- /gen -->\n")
    r = run_gen("--check", "--root", str(tree))
    assert r.returncode != 0 and "no_such_block" in r.stdout + r.stderr


def test_unclosed_block_is_an_error(tree):
    readme = tree / "README.md"
    readme.write_text(readme.read_text() + "\nvalue <!-- gen:apply_mps -->1\n")
    assert run_gen("--check", "--root", str(tree)).returncode != 0


@pytest.mark.parametrize("rel", DOCS)
def test_document_has_generated_numbers(rel):
    assert BLOCK_OPEN in text(rel), f"{rel} has no generated block"


def test_every_readme_image_exists():
    readme = text("README.md")
    refs = re.findall(r"!\[[^\]]*\]\(([^)]+)\)", readme) + re.findall(r'<img[^>]+src="([^"]+)"', readme)
    assert refs, "the README shows no images"
    for ref in refs:
        path = ROOT / ref
        assert path.is_file() and path.stat().st_size > 0, f"missing image {ref}"


def test_readme_shows_every_local_proof():
    readme = text("README.md")
    for name in ("demo-replay.gif", "dashboard-full.png", "dashboard-spread.png", "proof-ctest.png",
                 "proof-verify.png", "proof-compare.png", "proof-loss.png", "proof-cmp.png",
                 "proof-fuzz.png"):
        assert f"docs/images/{name}" in readme, name


def test_public_proofs_appear_only_when_their_file_exists(tmp_path):
    gen = gen_module()
    (tmp_path / "docs" / "images").mkdir(parents=True)
    assert "docs/images/" not in gen.public_proofs(tmp_path)
    (tmp_path / "docs" / "images" / "proof-ci-green.png").write_bytes(b"x")
    assert "docs/images/proof-ci-green.png" in gen.public_proofs(tmp_path)


def test_repo_slug_and_badge_agree():
    gen = gen_module()
    readme = text("README.md")
    assert gen.badges("") == ""
    assert ("badge.svg" in readme) == bool(gen.REPO_SLUG)
    assert ("github.io" in readme) == bool(gen.REPO_SLUG)
    if gen.REPO_SLUG:
        assert f"{gen.REPO_SLUG}/actions/workflows/ci.yml/badge.svg" in readme


def test_project_1_link_is_the_dependency_cmake_fetches():
    url = re.search(r"GIT_REPOSITORY\s+(https://github\.com/\S*orderbook\S*?)\.git",
                    text("CMakeLists.txt")).group(1)
    assert url in text("README.md")


def test_repo_slug_set_gives_badge_and_dashboard_link():
    gen = gen_module()
    out = gen.badges("someone/itch-feed-handler")
    assert "someone/itch-feed-handler/actions/workflows/ci.yml/badge.svg" in out
    assert "someone.github.io/itch-feed-handler" in out


def test_every_results_link_exists():
    for rel in GENERATED_IN + ["docs/DISCREPANCIES.md"]:
        for name in re.findall(r"results/[\w.-]+\.json", text(rel)):
            assert (ROOT / name).is_file(), f"{rel} names {name}"


def test_relative_links_exist():
    for rel in DOCS:
        base = (ROOT / rel).parent
        for link in re.findall(r"\]\((?!https?:|#|mailto:)([^)#\s]+)", text(rel)):
            assert (base / link).exists(), f"{rel} links {link}"


@pytest.mark.parametrize("planted", ["about [__] M", "[N] symbols", "[CPU]", "TBD", "TODO: x"])
def test_placeholder_check_catches_a_planted_one(planted):
    assert PLACEHOLDER.search(planted)


def test_no_unfilled_placeholders():
    for rel in DOCS:
        hits = PLACEHOLDER.findall(text(rel))
        assert not hits, f"{rel}: {hits}"


def test_every_result_has_meta_stamp():
    names = sorted(p.name for p in (ROOT / "results").glob("*.json"))
    assert len(names) > 15
    for name in names:
        if name in NOT_RESULTS or name in UNSTAMPED:
            continue
        d = json.loads((ROOT / "results" / name).read_text())
        if "runs" in d:  # fuzz.json and halt_grace: one stamp per run
            stamps = [r["meta"] for r in d["runs"]]
        elif "rows" in d and "meta" not in d:  # the kScan screening: one stamp per row
            stamps = [{"commit": r["stamp"], "data_file": "see note"} for r in d["rows"]]
        else:
            stamps = [d["meta"]]
        for s in stamps:
            assert s.get("data_file"), name
            assert s.get("commit") or s.get("bench_stamp"), name
    for name in list(NOT_RESULTS) + list(UNSTAMPED):
        assert name in names, f"exception for a file that is gone: {name}"


def test_readme_sections_are_in_the_briefs_order():
    wanted = ["Proof", "Screenshots", "Try it", "Architecture", "Benchmark", "What was found",
              "Limits"]
    heads = [line[3:] for line in text("README.md").splitlines() if line.startswith("## ")]
    at = [next(i for i, h in enumerate(heads) if h.startswith(w)) for w in wanted]
    assert at == sorted(at), heads
    first_head = text("README.md").index("\n## ")
    assert "demo-replay.gif" in text("README.md")[:first_head]


@pytest.mark.parametrize("rel", DOCS)
def test_inline_block_never_starts_a_line(rel):
    # A line starting with an HTML comment is a raw HTML block on GitHub: its markdown is not
    # rendered. Multi-line blocks have the comment alone on its line, which is fine.
    for line in text(rel).splitlines():
        s = re.sub(r"^\s*(?:[-*]|\d+\.)\s+", "", line)
        if re.fullmatch(r"<!-- gen:[\w.]+ --><!-- /gen -->", s):
            continue  # a block that is empty right now: nothing to render
        assert not (s.startswith(BLOCK_OPEN) and BLOCK_CLOSE in s), f"{rel}: {line[:70]}"


@pytest.mark.parametrize("rel", DOCS)
def test_no_block_inside_a_code_span(rel):
    # Inside backticks the marker would show as text.
    for line in text(rel).splitlines():
        for i, part in enumerate(line.split("`")):
            assert not (i % 2 == 1 and BLOCK_OPEN in part), f"{rel}: {line[:70]}"


def test_lag_text_without_a_recorded_time():
    gen = gen_module()
    out = gen.lag_text({"speed": 10, "max_lag_ns": 157_000_000})
    assert "does not record" in out and "not held at every moment" in out
    assert "cross" not in out


def test_lag_text_at_the_close_names_the_cross():
    gen = gen_module()
    out = gen.lag_text({"speed": 10, "max_lag_ns": 157_000_000, "max_lag_at_feed_s": 57_600})
    assert "16:00:00" in out and "closing cross" in out and "not held at every moment" in out


def test_lag_text_elsewhere_names_the_time_only():
    gen = gen_module()
    out = gen.lag_text({"speed": 10, "max_lag_ns": 157_000_000, "max_lag_at_feed_s": 34_200})
    assert "09:30:00" in out and "cross" not in out


def test_content_rules():
    readme = text("README.md")
    assert "messages_lost_downstream" in readme
    assert "characterised, mechanism not shown" in readme.lower()
    assert "unexplained" in readme.lower()
    assert "feed second" in readme and "stream hash" in readme and "empty" in readme
    assert "replay" in readme.lower() and "not network" in readme.lower()
    assert "excludes" in readme.lower() and "LICENSE" in readme
    assert "exactly one printable" in readme
    joined = "\n".join(text(rel) for rel in DOCS)
    for word in ("robust", "seamless", "delve", "not just", "GraphQL", "Salesforce",
                 "tick-to-trade latency of", "blazing", "cutting-edge", "leverag"):
        assert word.lower() not in joined.lower(), word


def test_resume_names_the_cpu_and_docker_for_the_fuzzer():
    resume = text("docs/RESUME.md")
    assert "Apple M4 Pro" in resume and "Docker" in resume and "libFuzzer" in resume


def test_licence_covers_code_only():
    lic = text("LICENSE")
    assert lic.startswith("MIT License") and "Dhanush Chandra Shekar" in lic
    source = text("testdata/README.md")
    assert "Nasdaq" in source and "licen" not in source.lower()


def test_discrepancy_summary_matches_results():
    # DISCREPANCIES.md is written by hand. These are its headline counts.
    d = json.loads(text("results/day_12302019.json"))
    doc = text("docs/DISCREPANCIES.md")
    o = d["oracle"]
    for value in (d["totals"]["messages"], o["exec_not_head"], o["exec_not_best"],
                  o["crossed_other"], d["totals"]["printed_volume_shares"],
                  d["totals"]["match_number_collisions"]["distinct"]):
        assert f"{value:,}" in doc, value


def test_regenerate_script_parses():
    assert subprocess.run(["bash", "-n", str(REGEN)], capture_output=True).returncode == 0


def test_regenerate_dry_run_accounts_for_every_results_file():
    r = subprocess.run(["bash", str(REGEN), "--dry-run"], capture_output=True, text=True, cwd=ROOT)
    assert r.returncode == 0, r.stdout + r.stderr
    for p in (ROOT / "results").glob("*.json"):
        assert p.name in r.stdout, f"the script does not account for {p.name}"
    for needed in ("export_dashboard.py", "gen_docs.py", "gen_demo_assets.py"):
        assert needed in r.stdout
    assert "Expected time" in r.stdout


def test_ci_runs_the_docs_check():
    assert "scripts/gen_docs.py --check" in text(".github/workflows/ci.yml")


def test_docs_test_is_registered_with_ctest():
    assert "test_docs_fresh.py" in text("tests/CMakeLists.txt")


def test_replay_comparison_is_recorded_and_not_overstated():
    rec = json.loads(text("results/replay_max_compare.json"))
    assert rec["matches"] is True and rec["meta"]["commit"]
    assert "state_dump" in rec["ignored"] and "per_symbol" in rec["compared"]
    assert "watch" in rec["compared"]
    for rel in DOCS:
        assert "reproduces the max-speed" not in text(rel), rel
        assert "books it built equal" not in text(rel), rel
    assert "books it built equal" not in text("dashboard/app.js")
    assert "per-symbol share ledger" in text("README.md")


def test_c_execution_wording():
    readme = text("README.md")
    recon = json.loads(text("results/recon_12302019.json"))["exec_not_head"]["C"]
    through = recon["away_from_display_price"]["direction"]
    doc = text("docs/DISCREPANCIES.md")
    for t in (readme, doc):
        assert f"{through['inside_the_spread_side']:,}" in t
        assert "trade through their display price" in t
        assert "82,365" in t
    assert f"{through['away_from_the_spread']}" in readme


def test_crossed_headline_carries_its_grace():
    readme = text("README.md")
    grace = json.loads(text("results/recon_12302019.json"))["crossed"]["halt_grace"]
    assert f"{grace['crossed_touches_after_resume_before_cross']} crossed touches at 0 s" in readme
    assert f"{grace['max_s_after_resume'] * 1000:.3f} ms of a resume" in readme


def test_latency_is_labelled_as_a_separate_run():
    bench = json.loads(text("results/bench_12302019.json"))["results"]["latency"]
    readme = text("README.md")
    assert f"{bench['msgs_per_sec']['median'] / 1e6:.2f} M msgs/s" in readme
    assert f"{bench['ns_per_msg_median']:.1f} ns" in readme
    assert "separately timed run" in text("docs/RESUME.md")
    assert "subtracting 9 ns" not in text("docs/METHODOLOGY.md")


def test_reference_row_states_how_independent_it_is():
    readme = text("README.md")
    assert "no shared code, same author" in readme.lower() and "queue rule" in readme
    assert "of noon orders" in readme and "2019 only" in readme
    assert "before `include/itch`" not in text("docs/DISCREPANCIES.md")


def test_provenance_counts_every_stamped_commit():
    line = re.search(r"<!-- gen:provenance -->(.*?)<!-- /gen -->", text("README.md")).group(1)
    assert "82e91fc" in line and "43d1e2f" in line


def test_spec_pdfs_are_links_not_files():
    assert not list((ROOT / "docs" / "spec").glob("*.pdf"))
    spec = text("docs/spec/README.md")
    assert "NQTVITCHspecification.pdf" in spec and "moldudp64.pdf" in spec and "https://" in spec


def test_licence_exclusion_names_all_the_data():
    section = text("README.md").split("## Licence and data")[1]
    for name in ("testdata/slice.itch.gz", "fuzz/corpus", "results/", "dashboard/data/"):
        assert name in section, name


def test_readme_states_prerequisites():
    assert "CMake 3.25" in text("README.md")


def test_definition_of_done_is_honest():
    section = text("docs/VERIFICATION.md").split("## Definition of done")[1]
    assert "- [x]" in section and "- [ ]" in section
    hosted = next(x for x in section.splitlines() if "hosted" in x.lower())
    assert hosted.startswith("- [ ]")


def _dry(*args):
    return subprocess.run(["bash", str(REGEN), "--dry-run", *args], capture_output=True, text=True,
                          cwd=ROOT).stdout


def test_regenerate_runs_fuzz_before_anything_timed():
    out = _dry()
    assert out.index("[fuzz_mold]") < out.index("[bench]")
    assert out.index("[fuzz_mold]") < out.index("[replay_10x]")
    assert "replay_max_compare.json" in out


@pytest.mark.parametrize("job", ["export_dashboard", "gen_demo_assets", "gen_docs_check"])
def test_late_restart_skips_the_clean_tree_check(job):
    out = _dry("--from", job)
    assert "clean-tree check skipped" in out and "the tree is not clean" not in out


def test_early_restart_keeps_the_clean_tree_check():
    assert "clean-tree check skipped" not in _dry("--from", "bench")


def test_regenerate_reexecs_under_caffeinate_and_checks_bench_completeness():
    script = REGEN.read_text()
    assert re.search(r'exec caffeinate -i bash', script)
    assert '"complete"' in script or "complete" in script
    assert "caffeinate -i python3" not in script


def test_ci_format_check_covers_bench():
    assert "find include tests tools fuzz bench" in text(".github/workflows/ci.yml")
