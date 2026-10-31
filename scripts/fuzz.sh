#!/usr/bin/env bash
# Run a libFuzzer target in Docker and write results/fuzz.json from its final stats.
#
#   scripts/fuzz.sh [target] [seconds]      defaults: fuzz_parser 300
#
# Targets: fuzz_parser (seed corpus fuzz/corpus) and fuzz_mold (fuzz/corpus_mold). Each run
# replaces its own entry in results/fuzz.json and keeps the other targets' entries.
#
# Docker is not a convenience here: Apple clang ships no libFuzzer runtime, so the
# target cannot be linked on the host at all.
#
# ASAN_OPTIONS=detect_leaks=0 suppresses a libFuzzer-runtime false positive on musl: a
# fuzz target whose body is `return 0;` reports the same small leak at exit.
#
# The working corpus is copied to a scratch directory so libFuzzer's additions do not
# bloat the committed seed corpus. results/fuzz.json is written only if the run finishes
# with no crash artifact.
set -euo pipefail

TARGET="${1:-fuzz_parser}"
SECS="${2:-300}"
case "$TARGET" in
  fuzz_mold) DEFAULT_SEEDS=fuzz/corpus_mold
             DATA_FILE="fuzz/corpus_mold (hand-built MoldUDP64 event sequences: gaps, overlap, malformed, tail loss)" ;;
  *)         DEFAULT_SEEDS=fuzz/corpus
             DATA_FILE="fuzz/corpus (builder seeds + first 64 KB of the synthetic day)" ;;
esac
SEEDS="${ITCH_FUZZ_CORPUS:-$DEFAULT_SEEDS}"
mkdir -p "$SEEDS"

# Provenance, taken before the run. results/ is excluded from the dirty check because
# this script writes there.
COMMIT="$(git rev-parse HEAD)"
if [ -n "$(git status --porcelain -- . ':(exclude)results')" ]; then COMMIT="$COMMIT-dirty"; fi
if [ "$(uname -s)" = Darwin ]; then
  CPU="$(sysctl -n machdep.cpu.brand_string)"
else
  CPU="$(sed -n 's/^model name[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo | head -1)"
fi

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT

docker run --rm -e ASAN_OPTIONS=detect_leaks=0 -e TARGET="$TARGET" -e SECS="$SECS" \
  -e SEEDS="$SEEDS" -v "$PWD":/w:ro -w /w alpine:3.20 sh -euc '
  apk add --no-cache git cmake ninja clang lld compiler-rt >/dev/null 2>&1
  echo "COMPILER: $(clang --version | head -1)"
  CC=clang CXX=clang++ cmake -S . -B /tmp/bf -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DITCH_BUILD_FUZZ=ON -DITCH_BUILD_TESTS=OFF -DITCH_BUILD_TOOLS=OFF >/dev/null
  cmake --build /tmp/bf --target "$TARGET" >/dev/null
  mkdir -p /tmp/work /tmp/art
  cp "/w/$SEEDS"/* /tmp/work/
  /tmp/bf/fuzz/"$TARGET" -max_total_time="$SECS" -print_final_stats=1 \
    -artifact_prefix=/tmp/art/ /tmp/work
  if ls /tmp/art/* >/dev/null 2>&1; then echo "CRASH ARTIFACTS:"; ls /tmp/art; exit 1; fi
' 2>&1 | tee "$LOG"

stat() { sed -n "s/^stat::$1:[[:space:]]*//p" "$LOG" | tail -1; }
UNITS="$(stat number_of_executed_units)"
[ -n "$UNITS" ] || { echo "fuzz.sh: no final stats in the log; not writing results/fuzz.json" >&2; exit 1; }
COMPILER="$(sed -n 's/^COMPILER: //p' "$LOG" | head -1)"
mkdir -p results
ENTRY="$(mktemp)"
trap 'rm -f "$LOG" "$ENTRY"' EXIT
cat > "$ENTRY" <<JSON
{
  "meta": {
    "commit": "$COMMIT",
    "data_file": "$DATA_FILE",
    "cpu": "$CPU (host; run inside a Docker Linux VM, alpine:3.20)",
    "compiler": "$COMPILER",
    "flags": "-O2 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer -UNDEBUG -DITCH_VERIFY=1; ASAN_OPTIONS=detect_leaks=0"
  },
  "target": "$TARGET",
  "duration_s": $SECS,
  "executed_units": $UNITS,
  "avg_exec_per_s": $(stat average_exec_per_sec),
  "new_units_added": $(stat new_units_added),
  "crashes": 0
}
JSON
python3 -I - "$ENTRY" <<'PY'
import json, os, sys
entry = json.load(open(sys.argv[1]))
runs = []
if os.path.exists("results/fuzz.json"):
    old = json.load(open("results/fuzz.json"))
    runs = old["runs"] if "runs" in old else [old]  # the first version was one flat run
runs = [r for r in runs if r.get("target") != entry["target"]] + [entry]
runs.sort(key=lambda r: r["target"])
with open("results/fuzz.json", "w") as f:
    json.dump({"runs": runs}, f, indent=2)
    f.write("\n")
PY
echo "wrote results/fuzz.json"
