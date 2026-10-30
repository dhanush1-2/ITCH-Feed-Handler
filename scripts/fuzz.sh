#!/usr/bin/env bash
# Run a libFuzzer target in Docker.
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
# bloat the committed seed corpus. The final stats go to /tmp/fuzz_<target>.log in the
# container and are echoed here; results/fuzz.json is written by the caller from them.
set -euo pipefail

TARGET="${1:-fuzz_parser}"
SECS="${2:-300}"
SEEDS="${ITCH_FUZZ_CORPUS:-fuzz/corpus}"
mkdir -p "$SEEDS"

docker run --rm -e ASAN_OPTIONS=detect_leaks=0 -v "$PWD":/w:ro -w /w alpine:3.20 sh -euc '
  apk add --no-cache git cmake ninja clang lld compiler-rt >/dev/null 2>&1
  CC=clang CXX=clang++ cmake -S . -B /tmp/bf -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DITCH_BUILD_FUZZ=ON -DITCH_BUILD_TESTS=OFF -DITCH_BUILD_TOOLS=OFF >/dev/null
  cmake --build /tmp/bf --target '"$TARGET"' >/dev/null
  mkdir -p /tmp/work /tmp/art
  cp /w/'"$SEEDS"'/* /tmp/work/
  /tmp/bf/fuzz/'"$TARGET"' -max_total_time='"$SECS"' -print_final_stats=1 \
    -artifact_prefix=/tmp/art/ /tmp/work
  if ls /tmp/art/* >/dev/null 2>&1; then echo "CRASH ARTIFACTS:"; ls /tmp/art; exit 1; fi
'
