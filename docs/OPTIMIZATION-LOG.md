# Optimization log

Every figure here is from a committed file in `results/` (stamps: commit, data file, CPU, compiler, flags), except those the text marks as from an uncommitted run. Machine: Apple M4 Pro, AC power, macOS 27.2, `-O3 -DNDEBUG`, day 2019-12-30 (268,744,780 messages, 8.25 GB). This is a file replay; nothing here is network latency. Method: `docs/METHODOLOGY.md`.

Files: `results/bench_12302019_before_optimization.json` (commit ea06f30, kScan 8), `results/bench_12302019.json` (published, kScan 64), `results/screen_kscan_12302019.json` (screening rows), `results/profile_12302019.json` (profile and counters; collected by `scripts/collect_profile.py`, counters from `scripts/profile_counters.patch` applied to a scratch tree, so its stamp is not a clean commit and it is not a timing result).

## Headline

Applied (parse + book), full day, median of 5 fresh processes: **<!-- gen:apply_mps -->14.25 M<!-- /gen --> msgs/s** (<!-- gen:apply_ns -->70<!-- /gen --> ns/msg, spread <!-- gen:apply_spread -->1.0%<!-- /gen -->). Target was at least 5 M. Parse only: <!-- gen:parse_mps -->203.76 M<!-- /gen --> msgs/s. Before the one kept change: <!-- gen:apply_before_mps -->13.16 M<!-- /gen -->.

## A/B 1: IdIndex vs std::unordered_map (full day)

Same pool, same books, `unordered_map` reserved for the pool size up front. State fold identical in both (`state_checksum` equal), so both applied the same day.

<!-- gen:index_table -->

|  | msgs/s (median of 5) | spread | latency p50 / p99 / p99.9 (ns, 42 ns granularity) |
|---|---|---|---|
| IdIndex (open addressing, blocked hash) | 14.25 M | 1.0% | 83 / 375 / 625 |
| std::unordered_map | 7.11 M | 1.4% | 125 / 666 / 875 |

<!-- /gen -->

`latency.all` includes every message type in the file, the 4.25 M skipped ones (types I, J, K, L, S, V, Y: no book work, mostly 0 to 1 ticks) as well as handled ones; the per-type table (`latency_by_type_ns` in the results file) is the honest breakdown (A: p50 83, p99.9 334 ns; D: 83 / 500; U: 167 / 917). IdIndex is <!-- gen:index_speedup -->2.0x<!-- /gen --> faster applied. Table memory added after setup: IdIndex <!-- gen:tables_mb -->265.9 MB<!-- /gen --> (pool <!-- gen:pool_mb -->128.0 MB<!-- /gen --> + index <!-- gen:index_mb -->134.2 MB<!-- /gen --> + books array <!-- gen:books_mb -->3.7 MB<!-- /gen -->); unordered_map <!-- gen:map_tables_mb -->163.7 MB<!-- /gen -->. Those are setup figures; nodes are allocated as orders arrive, so the end-of-run figures are the fair ones: **<!-- gen:map_heap_end_mb -->222.1 MB<!-- /gen --> added by end of run for unordered_map vs <!-- gen:heap_end_mb -->312.6 MB<!-- /gen --> for IdIndex** (`heap_added_at_end_bytes_median`). At peak <!-- gen:peak_live -->1,924,078<!-- /gen --> live orders the unordered_map uses less; IdIndex's speed is bought with a fixed <!-- gen:index_mb -->134.2 MB<!-- /gen --> table.

## A/B 2: vector book vs Project 1 ladder, ten busiest symbols

Symbols (by message count): QQQ SPY IWM TQQQ AAPL AMD MSFT URTY TNA XLK; 15,354,044 messages, copied into one buffer in setup. The ladder is `bench/ladder_book.hpp`: `ob::PriceLadder` per side, base set once per symbol (median of the first 1,000 add prices, to give it its best case), `std::map` overflow outside the 65,536-tick window ($6.55 at this price scale). Agreement with the production book is tested (`tests/test_ladder_book.cpp`, random stream with overflow, and the synthetic day at the populated mid-point) and the two fold to the same state (`state_checksum` equal on the full run).

<!-- gen:ladder_table -->

| Book, ten busiest symbols | msgs/s | Spread |
|---|---|---|
| Sorted vector (production) | 20.38 M | 3.5% |
| Project 1 ladder | 28.67 M | 1.3% |

<!-- /gen -->

The ladder is <!-- gen:ladder_speedup -->1.41x<!-- /gen --> faster on liquid symbols, and that is the ladder's best case, for these reasons:
- The ladder queues orders by arrival; `Book` queues by reference number. The two are not the same work, and the queue order inside a level is not compared.
- `state_checksum` equality covers best bid, best ask and order count of every book, sampled every 2^20 messages. Level-by-level agreement is tested separately (random stream and the synthetic day), not in the full-day run.
- The ladder arm is pre-allocated and pre-faulted (3.16 MB per symbol, in setup) with an informed base; the vector arm allocates its level vectors inside the timed span.
- Only the ten busiest symbols, one day.

It is not usable for the market: 1,090,481 of 8,159,461 adds (13.4%) fell outside the window into the map even with a good base (URTY: 55%).

Memory, computed from real counts: ladder = 3,162,384 B per symbol (2 x (65,536 x 24 B + 8.3 KB bitmap)) x <!-- gen:symbols_with_orders -->8,892<!-- /gen --> symbols with orders = **<!-- gen:ladder_full_gb -->28.1 GB<!-- /gen -->**, more than this <!-- gen:machine_gb -->24 GiB<!-- /gen --> machine. The vector book's level vectors across all <!-- gen:symbols_with_orders -->8,892<!-- /gen --> books hold <!-- gen:vector_levels_mb -->38.9 MB<!-- /gen --> at their high-water mark (measured). The speedup is real for these ten and is the price of roughly 700x the level memory.

## Memory reporting

`max_rss` is <!-- gen:max_rss_gb -->8.57 GB<!-- /gen --> because it includes the mapped, mlocked <!-- gen:day_bytes -->8.25 GB<!-- /gen --> input. The latency set's RSS (<!-- gen:latency_rss_gb -->10.49 GB<!-- /gen -->) is harness memory: it holds about 2.2 GB of raw histogram samples and is not handler memory. Handler memory is the delta over the footprint taken after the input was locked: **<!-- gen:tables_mb -->265.9 MB<!-- /gen -->** of tables at setup, **<!-- gen:heap_end_mb -->312.6 MB<!-- /gen -->** added by end of run (levels, compressed pages), <!-- gen:peak_over_input_mb -->312.8 MB<!-- /gen --> peak over input.

## Profile

Figures in this section are in `results/profile_12302019.json`, collected from a rerun of the pre-optimization book (kScan 8) after the first draft of this log, so they differ slightly from my first quick look. Sampling and counters slow the run; none of it is a timing result.

`xctrace` is unavailable (Command Line Tools only, no Xcode), as in Project 1. Used `/usr/bin/sample` (6 s, 1 ms interval) on a full-day apply run, plus temporary counters in `book.hpp`, kept as `scripts/profile_counters.patch` and not applied to the tree. Top of stack, 4,559 samples: `Book::lower_bound` 2,558 (56%), `decode` 554, `add_order` 455, `retire` 226 + `IdIndex::erase` 198 (9%), `memmove` 180 (4%), `replace` 145, `Book::add` 133. Level search dominates.

Counters (full day): 284.9 M `lower_bound` calls; 40% end at the best level, 58% within 2 levels of the back; 22.9% (65.2 M) fall through the 8-level scan to binary search over typically 32 to 4,096 levels.

The two items the plan asked about:
- Reference-number insertion in `Book::add`: the walk past a higher reference ran on 360,450 of 140.3 M adds (0.26%), 3.28 M steps in total. It does not matter.
- Order counter (`orders_`): one increment per add or remove, not visible in the profile.

Attribution by ablation (baseline binary): parse only 197.6 M msgs/s (5 ns); with pool and id index but a no-op book 30.3 M (33 ns); full 13.2 M (76 ns). The book itself is about 43 ns of the 76.

## Candidates

Rule: keep only if over 5% throughput and no p99.9 regression on the 5-process protocol. Apply mode, 5 fresh processes per row, bracketed by baseline repeats (13.05, 12.96, 12.99 M).

| Variant | msgs/s | vs baseline (13.0 M) | Decision |
|---|---|---|---|
| kScan 2 | 12.37 M | -4.8% | rejected |
| kScan 4 | 12.81 M | -1.5% | rejected |
| kScan 16 | 13.33 M | +2.5% | rejected, under 5% |
| kScan 32 | 13.94 / 13.96 M | +7.3% | works, 64 is slightly better |
| **kScan 64** | **14.03 M** | **+8.0%** | **kept** |
| kScan 128 | 13.88 M | +6.8% | rejected, no better than 64 |
| kScan 8 then galloping search | 13.55 M | +4.2% | rejected, under 5% |

kScan 64 was chosen in-sample: screened and published on the same single day and machine, with no held-out day. The gain may be smaller on a day with a different level distribution.

Kept: `Book::kScan` 8 to 64. Published protocol result, same binary configuration, committed stamps: <!-- gen:apply_before_mps -->13.16 M<!-- /gen --> to <!-- gen:apply_mps -->14.25 M<!-- /gen --> (+<!-- gen:apply_gain -->8.2%<!-- /gen -->). p99.9 per-message apply latency: <!-- gen:p999_before -->666<!-- /gen --> ns before, <!-- gen:p999 -->625<!-- /gen --> ns after. No regression at 42 ns granularity. The unordered_map arm moved the same way (875 vs 917 ns p99.9). State fold unchanged, and the full verify and the noon `cmp` are silent (below).

Why it works (hypothesis the numbers fit, not proven without hardware counters): a short linear scan from the back is predictable and touches adjacent cache lines, while `partition_point` over a few hundred levels is a chain of dependent loads with unpredictable branches.

Not tried: lookahead prefetch of the next message's index bucket, and a different hash mix. The profile gave them little to work on (`IdIndex::erase` plus `retire` about 9%; the hash already spreads blocks), `IdIndex` is Project 1's read-only code (a prefetch would mean copying it), and the level search was the larger target. They are the next step, not a finding that they fail.

## Verification after the change

`itch_replay data/12302019.NASDAQ_ITCH50 --verify --symbols <watch list> --dump-state ... --dump-at-s 43200` (release build, pins applied by default): exit 0, VERIFIED, 265 whole-state checks, conservation holds for all 65,536 locates. `cmp results/state_12302019.bin <dump>` silent. Export `verify`, `totals`, `by_type`, `anomalies`, `oracle`, `conservation`, `peak_live_orders`, `state_dump` (sans path) and `per_symbol` equal the committed `results/day_12302019.json`. All 142 ctest tests pass.

## Things that went wrong and were fixed

- First timing runs (discarded, not committed) had 75% to 97% spread. The mapped input was being paged in during the timed span (about 1,300 major faults per run, 20% slower; seen in uncommitted runs), because `MADV_SEQUENTIAL` plus memory pressure on a 24 GB machine kept evicting pages. The harness now `mlock`s the input; major faults in every timed span are 0 and are published per run.
- `frame` mode runs 0.45 s per pass. It is stable only because of the lock (5.8% spread in the published set); treat it as a floor, not a headline.
