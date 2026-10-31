# Test data

Everything here is synthetic. No byte comes from a real feed.

## synthetic.itch.gz

One invented trading day for eight invented symbols (`synthetic_symbols.txt`), written by `scripts/gen_synthetic_itch.py --write-fixtures` with seed 1. A test checks that the committed files are exactly what the generator writes, and `fuzz/corpus/synthetic_first_64k.bin` is its first 64 KB.

CI and the README's two-minute demo use it. It covers `R S H A F E C X D U P Q`, the system events in order, opening, closing and halt crosses including a zero-share one, non-printable allocations, books that cross during a halt, orders displayed after the open with an earlier reference number, match numbers on two messages, and `C` executions behind the head of a level. The day ends empty, and the books are populated at noon so the state comparisons are not vacuous.

The prices and order flow mean nothing. It tests the handler's logic and says nothing about real markets. No `B` message is generated; broken trades are covered by unit tests.

## expected_synthetic.json

The reviewed `itch_replay` export of that file:

```
itch_replay build/testdata/synthetic.itch --verify --full-check-every 1 --symbols "$(cat testdata/synthetic_symbols.txt)" --export testdata/expected_synthetic.json
```

`scripts/compare_export.py` compares a fresh export with it in CI and in `ctest`, ignoring `meta`, `max_rss_bytes` and `state_dump`. Its oracle counts are pinned under `synthetic.itch` in `results/expected_anomalies.json`. To regenerate it after an intentional change, run the command from a clean commit and review the diff.

## A real slice, for local use

`itch_slice` cuts whole order lives for chosen symbols from a real file you have downloaded. Real data does not belong in this repository, so write it under the gitignored `testdata/local/`:

```
mkdir -p testdata/local
build/release/tools/itch_slice data/12302019.NASDAQ_ITCH50 testdata/local/slice.itch --symbols AAPL,MSFT
build/release/tools/itch_replay testdata/local/slice.itch --verify --full-check-every 1
```
