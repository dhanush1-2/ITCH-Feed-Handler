#!/usr/bin/env python3
"""Collect the profile behind docs/OPTIMIZATION-LOG.md into results/profile_<day>.json.

    collect_profile.py --day 12302019 --bench build/release/bench/bench_feed \
                       --counters-bench <bench_feed built with scripts/profile_counters.patch>

Two separate runs, neither a timing result:
  1. /usr/bin/sample (6 s, 1 ms) of a full-day apply run of --bench; top-of-stack table kept.
  2. a full-day apply run of --counters-bench, whose stderr carries lower_bound and
     reference-walk counters. That binary is built from a temporary patch (book.hpp with kScan 8,
     counters); it is stamped by its own run output and is never a published number.
The profile describes the Book with kScan = 8, the code the optimization started from, so
--bench must be a build with kScan 8 (commit 93efea9).
"""
import argparse, json, re, subprocess, sys, tempfile, time
from pathlib import Path


def stamp(bench: str, data: str) -> dict:
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "s.json"
        subprocess.run([bench, data, "--mode", "frame", "--max-messages", "1000", "--json", str(out)],
                       check=True, capture_output=True)
        return json.loads(out.read_text())["meta"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--day", required=True)
    ap.add_argument("--bench", required=True)
    ap.add_argument("--counters-bench", required=True)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    data = f"data/{a.day}.NASDAQ_ITCH50"
    out = Path(a.out or f"results/profile_{a.day}.json")
    with tempfile.TemporaryDirectory() as td:
        sfile = Path(td) / "sample.txt"
        p = subprocess.Popen([a.bench, data, "--mode", "apply", "--json", str(Path(td) / "r.json")],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(14)  # past setup, well inside the ~22 s timed pass
        subprocess.run(["/usr/bin/sample", str(p.pid), "6", "-file", str(sfile)],
                       check=True, capture_output=True)
        p.wait()
        text = sfile.read_text()
        top = text.split("Sort by top of stack, same collapsed (when >= 5):")[1].split("Binary Images:")[0]
        rows = []
        for line in top.strip().splitlines():
            m = re.match(r"\s*(.*?)\s{2,}(\d+)\s*$", line)
            if m:
                name = re.sub(r"\s*\(in [^)]*\)", "", m.group(1))
                rows.append({"symbol": name[:120], "samples": int(m.group(2))})
        total = int(re.search(r"^\s*(\d+) Thread_", text, re.M).group(1))
        c = subprocess.run([a.counters_bench, data, "--mode", "apply", "--json", str(Path(td) / "c.json")],
                           check=True, capture_output=True, text=True).stderr
    head = dict(re.findall(r"(\w+)=(\d+)", c.splitlines()[0]))
    head = {k: int(v) for k, v in head.items()}
    steps = [int(re.search(rf"^steps\[{i}\]=(\d+)", c, re.M).group(1)) for i in range(8)]
    pp = {int(k): int(v) for k, v in re.findall(r"^pp_len_log2\[(\d+)\]=(\d+)", c, re.M)}
    sz = {int(k): int(v) for k, v in re.findall(r"^vecsize_log2\[(\d+)\]=(\d+)", c, re.M)}
    doc = {
        "meta": {"data_file": data, "bench_stamp": stamp(a.bench, data),
                 "counters_bench_stamp": stamp(a.counters_bench, data),
                 "note": "Profile of the pre-optimization book (kScan 8). Not a timing result: "
                         "sampling and counters slow the run. The counters binary comes from "
                         "scripts/profile_counters.patch applied to a scratch tree, so its "
                         "stamp is not a clean commit."},
        "sample": {"tool": "/usr/bin/sample, 6 s, 1 ms interval, full-day apply",
                   "thread_samples": total, "top_of_stack": rows},
        "counters": {**head,
                     "scan_steps_from_back_0_to_7": steps,
                     "calls_reaching_binary_search_by_log2_len": pp,
                     "calls_by_log2_vector_size": sz},
    }
    out.write_text(json.dumps(doc, indent=2) + "\n")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
