# Design

A replay of recorded NASDAQ TotalView-ITCH 5.0 files. Figures are generated from `results/`, on <!-- gen:cpu -->Apple M4 Pro<!-- /gen -->, unless the text says otherwise.

## Data path

1. **Input.** `MappedFile` maps the file and rejects a missing, empty or gzipped path with a clear error. Or `Receiver` delivers MoldUDP64 messages in strict sequence order.
2. **Framing.** `Framer` walks the 2-byte big-endian length prefixes and reports truncation with its offset.
3. **Decode.** `decode` dispatches on the type byte to a view that holds one pointer. Fields are read with `memcpy` plus a byte swap, so there is no `reinterpret_cast` of wire bytes and no unaligned read. Out-of-scope types are skipped by length and counted (<!-- gen:skipped -->4,248,527<!-- /gen --> on the 2019 day).
4. **Apply.** `BookBuilder` routes by `stock_locate`. Only `E`, `C`, `X`, `D` and `U` remove displayed liquidity. `P`, `Q` and `B` never touch a book. The handler never matches.
5. **Observe.** An observer template parameter receives each message and hook. The benchmark uses a null observer. `--verify` adds invariants, the feed oracle, market state and statistics.
6. **Publish.** JSON exports go to `results/`, then `scripts/export_dashboard.py` writes the dashboard's data. With `--tui`, watched symbols publish snapshots through a seqlock.

Prices stay as raw integers (1/10000 dollars) everywhere except display and JSON output.

## Decisions and evidence

| Decision | Evidence |
|---|---|
| Sorted-vector levels instead of Project 1's ladder | The ladder is <!-- gen:ladder_speedup -->1.41x<!-- /gen --> faster on ten symbols, but a full-market ladder needs <!-- gen:ladder_full_gb -->28.1 GB<!-- /gen --> on a <!-- gen:machine_gb -->24 GiB<!-- /gen --> machine, against <!-- gen:vector_levels_mb -->38.9 MB<!-- /gen --> of level vectors. `results/bench_12302019.json`, `docs/OPTIMIZATION-LOG.md` |
| Check relationships that hold, not `E`/`C` against `P` | `P` prints non-displayed executions and `E`/`C` displayed ones, so they do not sum to each other. The checks became per-symbol conservation, executions at best and at head, and printed volume against the reference. The plan's further assumption that match numbers are disjoint was wrong: <!-- gen:match_two -->4,324<!-- /gen --> appear on two messages, two sides of one match, exactly one printable, no double count |
| `stock_locate` is a 2-byte index | 65,536 books in a vector, so the index cannot go out of bounds |
| Primary data is the 2019-12-30 file, second day is 2025-12-08 | <!-- gen:messages -->268,744,780<!-- /gen --> and <!-- gen:messages_2025 -->650,338,709<!-- /gen --> messages. `results/census_12302019.json`, `results/day_20251208.json` |
| Integrity comes from three checks, not the `.md5sum` files | Those files returned 404. `fetch_itch.py` checks Content-Length, gzip's CRC and size trailer, and a final `S`/`C` message, and records its own SHA-256 |
| Invariants are split by cost | O(1) checks after every message in the verify build. A whole-state walk after every message on the slice, and every 1,000,000 messages on the full day (<!-- gen:full_checks -->265<!-- /gen --> walks) |
| A crossed book is reported, not assumed impossible | <!-- gen:crossed_continuous -->0<!-- /gen --> in continuous trading, <!-- gen:crossed_other -->8,579<!-- /gen --> touches outside it. Explained counts are pinned in `results/expected_anomalies.json`, so a new one fails `--verify` |
| One global order table on one thread | A full-market replay needs it, and nothing here needs a parser and book thread split |
| Committed test data is a filtered real slice | A head-of-file cut would be pre-market with almost no executions. A symbol filter keeps whole order lives. `testdata/README.md` |
| Levels queue by reference number | All `E` at a best level hit the lowest reference; arrival order was wrong <!-- gen:not_head_e_before -->10,565<!-- /gen --> times and never right where reference order was wrong. `docs/DISCREPANCIES.md` |
| `RetransmitServer` stores pointers into the mapped file | The source map outlives the server. It keeps the ring small |
| Bursts apply downstream only, with their own retransmit-loss setting | The simplest model that still tests recovery. A lost answer is covered by `retransmit_loss` |
| Halt grace stays at <!-- gen:halt_grace_s -->5<!-- /gen --> s | Sensitivity run at 0, 5 and 30 s, `results/halt_grace_12302019.json`. Longer grace discards real trading |
| Level scan bound of 64 | Raised applied throughput from <!-- gen:apply_before_mps -->13.16 M<!-- /gen --> to <!-- gen:apply_mps -->14.25 M<!-- /gen --> with no p99.9 regression. Chosen in-sample on one day. `docs/OPTIMIZATION-LOG.md` |

## Recovery state machine

`Receiver<Sink, Requester>` (`include/mold/receiver.hpp`) holds `expected`, the next sequence to deliver, and `hi`, the next sequence after the highest message it knows exists from data, a heartbeat or end of session.

```
          packet leaves a hole, or heartbeat past hi
  Live  -------------------------------------------->  Recovering
   ^                                                    |    |
   |            buffer drained, expected == hi          |    | max_attempts without progress,
   +----------------------------------------------------+    | or a gap past max_gap_messages
   |                                                         v
   | end of session reached                                Stale (final, nothing more delivered)
   v
 Finished
```

For each packet, once it has been validated whole:

| Case | Action |
|---|---|
| `end <= expected` | Duplicate: counted, discarded |
| `start < expected < end` | Partial overlap: only the new tail is delivered |
| `start <= expected` | Delivered, then the buffer drains while contiguous |
| `start > expected` | Copied into the buffer. If past `hi`, the hole `[hi, start)` is requested at once |
| Malformed, or another session | Counted, nothing applied. A malformed packet behaves like a lost one |

Messages that arrive during recovery wait in the buffer, so a book never sees a message out of order. The buffer is bounded. A packet that does not fit is dropped and counted, and `hi` does not advance past it, so the next packet or heartbeat reveals the hole. Time is a logical tick in the simulator, which makes the tests deterministic.

The protocol has no negative acknowledgement, so a range the server no longer holds looks like requests that are never answered. That ends in `Stale`, a nonzero exit, and a dump written only to `<path>.incomplete`.

## Invariant scoping

Hard invariants throw, because a violation means the book or the checker is wrong: level totals and counts equal their queues, links are consistent, prices are strictly ordered, each order's locate, side and price match its level, and live counts agree across the pool, index and books.

Feed properties are counted, because a mismatch is a finding about the data or the model. An execution not at best or not at head, and a crossed book, go to the oracle.

"Continuous trading" (`include/book/market_state.hpp`) requires all of: the system is between the `Q` and `M` events, the symbol's state is `T`, and the symbol is not awaiting a cross. A Nasdaq-listed symbol awaits its opening cross after `Q`. A symbol resuming from a halt awaits a halt or IPO cross, or <!-- gen:halt_grace_s -->5<!-- /gen --> s of feed time. The "never crossed" check applies only there.

Conservation holds per locate: `added + replaced_in == executed + cancelled + deleted + replaced_out + live`, where `live` is summed from the books' level totals, so two independent tallies are compared.

## Failure scenarios

| Scenario | Behaviour | Test |
|---|---|---|
| `.gz`, truncated, empty or missing input | Clear error with the offset, nonzero exit, no success summary | `test_mapped_file.cpp`, `test_replay_cli.py` |
| Live orders exceed capacity | Stops at once, names the count and the flag to raise | `CapacityIsFatal` |
| Retention expired, or retransmits keep failing | Receiver goes Stale and exits 3; no "identical" report | `ExpiredRetentionFailsVisibly`, `test_mold_sim_cli.py` |
| One-sided, stub-only or halted book | Export and dashboard show no spread, not a zero or a huge number | `ExportOneSidedBook`, `test_one_sided_spread_is_null` |
| Typo, lowercase or unlisted symbol | Case normalized, unknown names reported, exit 2 | `test_replay_cli.py` |
| Oracle count above its pin | `--verify` exits 3 | `test_replay_cli.py` |
| Result stamped `-dirty` or `-stale-build` | The dashboard exporter refuses it | `test_dashboard_data.py` |
| `--tui` interrupted while asked to write output | Exit 4, says nothing was written, terminal restored | `test_replay_cli.py` |
