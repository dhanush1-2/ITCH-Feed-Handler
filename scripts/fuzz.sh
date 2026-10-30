#!/usr/bin/env bash
# Run a libFuzzer target in Docker and write results/fuzz.json from its final stats.
#
#   scripts/fuzz.sh [target] [seconds]      defaults: fuzz_parser 300
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
SEEDS="${ITCH_FUZZ_CORPUS:-fuzz/corpus}"
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
cat > results/fuzz.json <<JSON
{
  "meta": {
    "commit": "$COMMIT",
    "data_file": "fuzz/corpus (builder seeds + first 64 KB of 12302019 partial stream)",
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
echo "wrote results/fuzz.json"
