#!/usr/bin/env bash
# Regenerate every committed results file from a clean commit, then the dashboard data, the
# documents and the proof images. Every step runs in order; the first failure stops the script.
#
#   scripts/regenerate_results.sh [--dry-run] [--skip-fuzz] [--from JOB]
#
#   --dry-run    print each job, its command and its recorded wall time; run nothing, need no data
#   --skip-fuzz  keep the committed results/fuzz.json, for when Docker is down
#   --from JOB   start at this job, after a failure. Earlier output must still be valid
#
# The whole script re-executes itself under caffeinate -i, so the machine stays awake throughout.
#
# Fuzzing runs first, and nothing else runs alongside it. Docker is needed only for those two jobs:
# quit Docker Desktop once they finish, because an idle VM still costs load during the timed jobs.
# run_bench.py refuses a set above its load limit; the 10x job logs load to build/regen.
#
# Needs: a clean tree at the final commit, data/12302019.NASDAQ_ITCH50, data/S120825-v50.txt.gz,
# Docker for the fuzz jobs, ITCH_PLAYWRIGHT_PYTHON (a Python with Playwright, plus
# PLAYWRIGHT_BROWSERS_PATH if its browsers are not in the default place) for the dashboard
# images, about 21 GB of free disk for the decompressed 2025 day, AC power, and nothing else
# running: the benchmark and the 10x replay publish timings.
#
# Per-job wall time goes to build/regen/wall_times.log. Afterwards, review and commit results/,
# dashboard/data/, the docs and docs/images. Every stamp is checked against HEAD before the
# documents are generated.
#
# Recorded times come from earlier runs' logs and result files; a job with none shows "none".
set -euo pipefail

# Stay awake for all of it. A dry run does nothing, so it needs no assertion.
case " $* " in
  *" --dry-run "*) ;;
  *)
    if [ -z "${ITCH_REGEN_CAFFEINATED:-}" ] && command -v caffeinate >/dev/null; then
      export ITCH_REGEN_CAFFEINATED=1
      exec caffeinate -i bash "$0" "$@"
    fi
    ;;
esac

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

DRY=0; SKIP_FUZZ=0; FROM=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) DRY=1 ;;
    --skip-fuzz) SKIP_FUZZ=1 ;;
    --from) FROM="${2:?--from needs a job name}"; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

DAY=data/12302019.NASDAQ_ITCH50
GZ_2025=data/S120825-v50.txt.gz
DAY_2025=data/S120825-v50.txt
RELEASE=build/release/tools
VERIFY=build/verify/tools
WORK=build/regen
STRESS="--loss 0.01 --dup 0.005 --reorder 16 --burst 0.0005,64 --retransmit-loss 0.01"
LOG="$WORK/wall_times.log"

# What this script does not regenerate, and why.
not_regenerated() {
  cat <<'LIST'
Not regenerated, by design:
  results/expected_anomalies.json                 hand-maintained pins; every --verify job enforces them
  results/recon_12302019_arrival_queue.json       needs the old arrival-order queue (commit 0db1b32)
  results/recon_20251208_arrival_queue.json       needs the old arrival-order queue (commit 8523e7f)
  results/bench_12302019_before_optimization.json the kept baseline before the change (commit ea06f30)
  results/screen_kscan_12302019.json              screening rows from uncommitted patched binaries
  results/profile_12302019.json                   profile of the pre-optimization book, patched scratch tree
  testdata/expected_synthetic.json                    synthetic golden file; ctest compares against it, regenerated only on a reviewed change
LIST
}

# Checks what this machine can do. A dry run reports problems; a real run stops on them.
preflight() {
  local problems=0
  note() { echo "cannot run here: $1"; problems=$((problems + 1)); }
  [ -f "$DAY" ] || note "$DAY is missing (scripts/fetch_itch.py 12302019.NASDAQ_ITCH50.gz)"
  [ -f "$GZ_2025" ] || note "$GZ_2025 is missing (scripts/fetch_itch.py S120825-v50.txt.gz)"
  case "$FROM" in
    ""|build|fuzz_parser|fuzz_mold) need_docker=1 ;;
    *) need_docker=0 ;;
  esac
  if [ "$SKIP_FUZZ" = 0 ] && [ "$need_docker" = 1 ] && ! docker info >/dev/null 2>&1; then
    note "fuzz_parser, fuzz_mold: Docker is down (start it, or pass --skip-fuzz to keep results/fuzz.json)"
  fi
  [ -n "${ITCH_PLAYWRIGHT_PYTHON:-}" ] || note "dashboard images: ITCH_PLAYWRIGHT_PYTHON is not set"
  python3 -c 'import PIL' 2>/dev/null || note "proof images: Pillow is not installed for python3"
  case "$FROM" in
    # These steps write no stamps, and gen_docs has already changed the tree. check_stamps guards.
    export_dashboard|gen_demo_assets|gen_docs_check) echo "clean-tree check skipped for --from $FROM" ;;
    *)
      [ -z "$(git status --porcelain -- . ':(exclude)results' ':(exclude)data')" ] ||
        note "the tree is not clean, so stamps would say -dirty; commit first"
      ;;
  esac
  if [ "$problems" -gt 0 ] && [ "$DRY" = 0 ]; then
    echo "stopping: $problems problem(s) above" >&2
    exit 1
  fi
}

TOTAL=0; UNRECORDED=""; STARTED=0
[ -n "$FROM" ] || STARTED=1

# job NAME RECORDED_SECONDS|none DESCRIPTION -- command words. A step is a shell function or a tool.
job() {
  local name="$1" recorded="$2" what="$3"; shift 4
  if [ "$recorded" = none ]; then
    UNRECORDED="$UNRECORDED $name"
  else
    TOTAL=$(python3 -c "print($TOTAL + $recorded)")
  fi
  if [ "$DRY" = 1 ]; then
    printf '[%s] recorded: %s s. %s\n    ' "$name" "$recorded" "$what"
    if [ "$(type -t "$1")" = function ]; then
      declare -f "$1" | sed '1,2d;$d;s/^/   /'
    else
      printf '%q ' "$@"
      printf '\n'
    fi
    return 0
  fi
  [ "$name" = "$FROM" ] && STARTED=1
  if [ "$STARTED" = 0 ]; then
    echo "[$name] skipped by --from"
    return 0
  fi
  echo "=== [$name] $what"
  # In a subshell with -e on, and not under || or if, so a failure inside a step stops that step.
  local t0=$SECONDS rc=0
  set +e
  ( set -e; "$@" )
  rc=$?
  set -e
  printf '%s\t%d s\trecorded %s\texit %d\t%s\n' "$name" $((SECONDS - t0)) "$recorded" "$rc" \
    "$(date -u +%FT%TZ)" >> "$LOG"
  if [ "$rc" -ne 0 ]; then
    echo "FAILED: [$name] exit $rc. Fix it, then rerun with --from $name" >&2
    exit "$rc"
  fi
}

build_all() {
  cmake --preset release
  cmake --build --preset release -j
  cmake --preset verify
  cmake --build --preset verify -j
}

census() { "$RELEASE/itch_stat" "$DAY" --json results/census_12302019.json; }

day_2019() {
  "$VERIFY/itch_replay" "$DAY" --verify --symbols AAPL,MSFT,SPY \
    --dump-state results/state_12302019.bin --dump-at-s 43200 --export results/day_12302019.json
}

halt_grace() {
  local g
  echo '{"note": "no pins", "pins": {}}' > "$WORK/no_pins.json"
  for g in 0 5 30; do
    # A pins file with no entry for this day: grace 0 is meant to exceed the day's pins.
    "$VERIFY/itch_replay" "$DAY" --verify --halt-grace-s "$g" --pins "$WORK/no_pins.json" \
      --export "$WORK/grace_$g.json"
  done
  python3 -I - "$WORK" <<'PY'
import json, sys
keys = ("crossed_continuous", "crossed_other", "touches_continuous", "touches_other",
        "episodes_continuous", "episodes_other", "exec_checked", "exec_checked_continuous",
        "exec_not_best", "exec_not_best_continuous", "exec_not_head", "exec_not_head_continuous")
runs = []
for g in (0, 5, 30):
    d = json.load(open(f"{sys.argv[1]}/grace_{g}.json"))
    runs.append({"halt_grace_s": g, "meta": {k: d["meta"][k] for k in
                 ("commit", "data_file", "cpu", "compiler", "flags")},
                 "oracle": {k: d["oracle"][k] for k in keys}})
note = json.load(open("results/halt_grace_12302019.json"))["note"]
json.dump({"note": note, "runs": runs}, open("results/halt_grace_12302019.json", "w"), indent=2)
PY
}

cpp_subset() {
  "$RELEASE/itch_replay" "$DAY" --export-orders "$(paste -sd, verify/subset.txt)" \
    results/cpp_subset_12302019.json
}

reference() {
  python3 -I verify/reference_book.py "$DAY" --symbols-file verify/subset.txt \
    --json results/ref_12302019.json
}

# Both inputs list resting orders: gitignored, local only. The committed record is the summary.
compare_subset() {
  python3 -I verify/compare.py results/cpp_subset_12302019.json results/ref_12302019.json
  python3 -I scripts/known_answer_summary.py results/cpp_subset_12302019.json \
    results/ref_12302019.json --out results/known_answer_summary.json
}

recon_2019() {
  python3 -I verify/recon_book.py "$DAY" --out build/r_day.pickle
  python3 -I verify/recon_match.py "$DAY" --json build/recon_match.json
  python3 -I verify/recon_summary.py build/r_day.pickle build/recon_match.json \
    verify/closing_sources.json results/halt_grace_12302019.json --out results/recon_12302019.json
}

# shellcheck disable=SC2086  # STRESS is a list of words
loss_stress() {
  "$RELEASE/mold_sim" "$DAY" $STRESS --seed 1 --retention 1048576 \
    --dump-state results/state_loss.bin --dump-at-s 43200 --json results/loss_full_day.json
  cmp results/state_12302019.bin results/state_loss.bin
}

loss_lossless() {
  "$RELEASE/mold_sim" "$DAY" --seed 1 --retention 1048576 \
    --dump-state results/state_lossless.bin --dump-at-s 43200 --json results/loss_full_day_lossless.json
  cmp results/state_12302019.bin results/state_lossless.bin
}

loss_table() {
  python3 -I scripts/loss_table.py --sim "$RELEASE/mold_sim" --data "$DAY" --messages 50000000 \
    --out results/loss_table.json
}

udp_demo() {
  "$RELEASE/mold_udp_demo" "$DAY" --max-messages 5000000 --drop 0.01 --seed 1 --rate-mps 1000000 \
    --dump-state results/state_udp_demo.bin --dump-at-s 25000 --check-direct --json results/udp_demo.json
}

bench() { python3 -I scripts/run_bench.py --day 12302019 --label "final, kScan=64"; }

replay_10x() {
  local load_log="$WORK/replay_10x_load.log" sampler
  ( while :; do echo "$(date -u +%FT%TZ) $(uptime)"; sleep 300; done ) >> "$load_log" &
  sampler=$!
  trap 'kill "$sampler" 2>/dev/null || true' EXIT
  "$RELEASE/itch_replay" "$DAY" --speed 10 --symbols QQQ,SPY,AAPL,AMD,MSFT --export results/replay_10x.json
}

replay_max_compare() {
  "$RELEASE/itch_replay" "$DAY" --symbols QQQ,SPY,AAPL,AMD,MSFT --export "$WORK/replay_max.json"
  python3 -I scripts/record_replay_compare.py results/replay_10x.json "$WORK/replay_max.json" \
    --out results/replay_max_compare.json
}

# The 2025 file's decompressed copy was removed to save disk, so it comes from the kept .gz.
day_2025() {
  trap 'rm -f "$DAY_2025"' EXIT
  gunzip -c "$GZ_2025" > "$DAY_2025"
  "$VERIFY/itch_replay" "$DAY_2025" --verify --export results/day_20251208.json
  python3 -I verify/recon_book.py "$DAY_2025" --out build/r_2025.pickle
  python3 -I verify/recon_other_day.py build/r_2025.pickle "$DAY_2025" --out results/recon_20251208.json
  rm -f "$DAY_2025"
  trap - EXIT
}

fuzz_parser() { scripts/fuzz.sh fuzz_parser 300; }
fuzz_mold() { scripts/fuzz.sh fuzz_mold 300; }

# Everything this script regenerated must carry today's commit with no suffix.
check_stamps() {
  python3 -I - "$(git rev-parse HEAD)" "$SKIP_FUZZ" <<'PY'
import json, sys
head, skip_fuzz = sys.argv[1], sys.argv[2] == "1"
names = ["census_12302019", "day_12302019", "day_20251208", "known_answer_summary", "recon_12302019",
         "recon_20251208", "loss_table", "loss_full_day", "loss_full_day_lossless", "udp_demo",
         "bench_12302019", "replay_10x", "replay_max_compare"]
bad = []
for n in names:
    c = json.load(open(f"results/{n}.json"))["meta"]["commit"]
    if c != head:
        bad.append(f"{n}: {c}")
if not json.load(open("results/bench_12302019.json"))["meta"]["complete"]:
    bad.append("bench_12302019: complete is false, a set was rejected; rerun with --from bench")
for r in json.load(open("results/halt_grace_12302019.json"))["runs"]:
    if r["meta"]["commit"] != head:
        bad.append(f"halt_grace: {r['meta']['commit']}")
if not skip_fuzz:
    for r in json.load(open("results/fuzz.json"))["runs"]:
        if r["meta"]["commit"] != head:
            bad.append(f"fuzz {r['target']}: {r['meta']['commit']}")
if bad:
    sys.exit("stamps differ from HEAD " + head + ":\n  " + "\n  ".join(bad))
print("all stamps are", head)
PY
}

export_dash() { python3 -I scripts/export_dashboard.py results/ dashboard/data/; }

mkdir -p "$WORK"
preflight
not_regenerated
[ "$DRY" = 1 ] || printf '# run started %s at %s\n' "$(date -u +%FT%TZ)" "$(git rev-parse HEAD)" >> "$LOG"

# Sources of the recorded times: build/logs/regen.log (51.8, 27.8, 201.3 s), the mold_sim "seconds"
# fields in results/ (73.8, 70.8, 58.8 s, 5.0 s), results/replay_10x.json (6122.9 s),
# results/bench_12302019.json (raw_wall_seconds 1022.3 s plus 45 runs x 5 s cooldown), and
# results/fuzz.json (300 s each requested; the Docker build is not recorded). "halt_grace" is three
# runs of the 51.8 s replay; "replay_max_compare" one of the 27.8 s replay.
job build              none    "configure and build the release and verify presets" -- build_all
if [ "$SKIP_FUZZ" = 0 ]; then
  job fuzz_parser      300     "results/fuzz.json, parser entry (Docker)" -- fuzz_parser
  job fuzz_mold        300     "results/fuzz.json, receiver entry (Docker)" -- fuzz_mold
fi
job census             none    "results/census_12302019.json" -- census
job day_2019           51.8    "results/day_12302019.json, state_12302019.bin" -- day_2019
job halt_grace         155.4   "results/halt_grace_12302019.json" -- halt_grace
job cpp_subset         27.8    "results/cpp_subset_12302019.json (local, gitignored)" -- cpp_subset
job reference          none    "results/ref_12302019.json (local, gitignored)" -- reference
job compare            none    "compare with the reference; results/known_answer_summary.json" -- compare_subset
job recon_2019         none    "results/recon_12302019.json" -- recon_2019
job loss_stress        73.8    "results/loss_full_day.json, then cmp the noon state" -- loss_stress
job loss_lossless      70.8    "results/loss_full_day_lossless.json, then cmp the noon state" -- loss_lossless
job loss_table         58.8    "results/loss_table.json" -- loss_table
job udp_demo           5.0     "results/udp_demo.json" -- udp_demo
job bench              1247.3  "results/bench_12302019.json" -- bench
job replay_10x         6122.9  "results/replay_10x.json" -- replay_10x
job replay_max_compare 27.8    "results/replay_max_compare.json: exports at max speed and 10x" -- replay_max_compare
job day_2025           201.3   "results/day_20251208.json, results/recon_20251208.json (from the .gz)" -- day_2025
job redact             none    "order-level records move from results/ to results/local/" -- python3 -I scripts/redact_results.py
job check_stamps       none    "every regenerated stamp equals HEAD" -- check_stamps
# gen_docs runs before the export because the dashboard copies docs/METHODOLOGY.md.
job gen_docs           none    "scripts/gen_docs.py" -- python3 -I scripts/gen_docs.py
job export_dashboard   none    "scripts/export_dashboard.py results/ dashboard/data/" -- export_dash
job gen_demo_assets    none    "scripts/gen_demo_assets.py: real commands to docs/images" -- python3 scripts/gen_demo_assets.py
job gen_docs_check     none    "scripts/gen_docs.py --check" -- python3 -I scripts/gen_docs.py --check

cat <<'NOTE'
Re-check by hand, these are typed: DESIGN.md "no p99.9 regression" and include/book/book.hpp "+8%"
against docs/OPTIMIZATION-LOG.md's generated figures; other figures in docs/DISCREPANCIES.md.
NOTE
printf 'Expected time: %s s (%s h) from recorded wall times only.\n' "$TOTAL" \
  "$(python3 -c "print(round($TOTAL / 3600, 2))")"
echo "No recorded wall time, so not in that total:$UNRECORDED"
