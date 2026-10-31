# Verification

What each layer checks, how far it reaches, and where it stops. All of it is about a replay of recorded files.

| Layer | What it checks | Scope | Limit |
|---|---|---|---|
| Unit tests (`tests/*.cpp`) | Wire layouts at every alignment, book operations, receiver cases, pacer, snapshot | Pushes to main and PRs, in CI on GCC, Clang, ASan+UBSan and macOS | Synthetic streams, written by me from the spec |
| O(1) invariants | The touched book's best levels; live counts agree | After every message in `--verify` | Touched book only |
| Whole-state invariants | Every level, queue and link | Every message on the synthetic day; <!-- gen:full_checks -->265<!-- /gen --> walks on the full day | Between walks, only the O(1) checks run |
| Conservation | Share ledger per locate | End of run, every locate | A compensating pair of errors in one locate would pass |
| Feed oracle | Each of <!-- gen:exec_checked -->5,822,741<!-- /gen --> executions at best and at head; crossed books | Whole day. Exceptions are counted and pinned | Says where the model and the feed disagree, not why |
| Pins (`results/expected_anomalies.json`) | Oracle counts cannot rise; `exec_not_head_e` is pinned at <!-- gen:not_head_e -->0<!-- /gen --> | The 2019 file and the synthetic day | The 2025 counts are findings, not pins |
| Independent reference (`verify/reference_book.py`) | Message counts, shares, printed volume, trades, levels and queues | <!-- gen:ka_symbols -->20<!-- /gen --> symbols, <!-- gen:ka_orders -->121,587<!-- /gen --> resting orders at noon | 2019 only. No shared code, same author. Shares the queue rule the data taught both |
| `verify/recon_book.py` | Reproduces every oracle count from a second Python book | Both days | Independent of the C++ book, not of its market-state rule, which it mirrors |
| Closing prices | Closing cross against public closes, <!-- gen:closes_ok -->7 of 7<!-- /gen --> | Seven symbols | Mostly one unofficial source (Yahoo), raw 2019 basis, CMCSA factor unconfirmed |
| Loss injection, synthetic day | Five seeds at three loss levels, state and stream compared with a direct replay | Pushes to main and PRs, in CI | Simulated channel |
| Loss injection, full day | Stream hash, and state at feed second <!-- gen:noon_second -->43,200<!-- /gen --> (<!-- gen:noon_orders -->1,771,377<!-- /gen --> orders), under <!-- gen:lost -->11,085,799<!-- /gen --> lost messages | One stress run with one seed | End of day is empty and is not the comparison |
| Loopback UDP | Same receiver over real sockets | First <!-- gen:udp_messages -->5,000,000<!-- /gen --> messages at <!-- gen:udp_rate -->1,000,000<!-- /gen --> msgs/s | Goes Stale when unpaced. Not multicast, not a statement about real networks |
| Second day | `--verify` passes on <!-- gen:messages_2025 -->650,338,709<!-- /gen --> messages | 2025-12-08 | The reference was not run on it. <!-- gen:unexplained_2025 -->43<!-- /gen --> printable `E` not at best stay unexplained |
| Fuzzing | Parser and receiver under ASan and UBSan, via libFuzzer in Docker | See the table below; 60 s of each in CI | Apple clang has no libFuzzer runtime, so it runs only in Linux |
| ThreadSanitizer | Seqlock snapshot, pacer, viewer | CI job | Those tests only |
| Document checks (`tests/test_docs_fresh.py`) | Generated blocks, images, links, stamps | Pushes to main and PRs | Hand-written prose in `docs/DISCREPANCIES.md` is checked only for its headline counts |

## Which data

CI and the two-minute demo use the synthetic day in `testdata/`, which the Python reference also replays there. The full-day rows use the real file, downloaded from Nasdaq; those results cannot be reproduced from this repository alone. The reference's order-level output on the real day is not committed: `results/known_answer_summary.json` holds its counts and hashes.

## Loss table

Loss is `messages_lost_downstream`: messages the receiver never got on the first pass. Retransmit requests and late deliveries run higher, because reordering triggers early requests, so they are not quoted as loss. In the loss-only 1% row, <!-- gen:table_lost_1pct -->506,871<!-- /gen --> messages were lost and <!-- gen:table_from_retransmit_1pct -->506,871<!-- /gen --> came back by retransmit, which the tests assert are equal.

<!-- gen:loss_table -->

| Run (first 50,000,000 messages) | Packets dropped | `messages_lost_downstream` | Gaps detected | Stream hash equal | State equal |
|---|---|---|---|---|---|
| baseline | 0 | 0 | 0 | yes | reference |
| loss_only_0.001 | 1,207 | 51,394 | 1,169 | yes | yes |
| loss_only_0.01 | 11,990 | 506,871 | 11,495 | yes | yes |
| loss_only_0.05 | 59,043 | 2,507,812 | 54,551 | yes | yes |
| combined_stress | 49,841 | 2,110,581 | 271,027 | yes | yes |

<!-- /gen -->

Full day, combined stress: <!-- gen:dropped_packets -->257,307<!-- /gen --> downstream packets dropped carrying <!-- gen:lost -->11,085,799<!-- /gen --> messages. Noon state <!-- gen:noon_sha -->3af90ca43d96a0a6<!-- /gen -->... and stream hash <!-- gen:stream_sha -->7fe0dd6d636d2dec<!-- /gen -->... (first 16 hex digits) equal the lossless run's. Files: `results/loss_full_day.json`, `results/loss_full_day_lossless.json`.

## Fuzzing

Only what `results/fuzz.json` records.

<!-- gen:fuzz_table -->

| Target | Requested time | Executions | Crashes | Commit |
|---|---|---|---|---|
| `fuzz_mold` | 300 s | 32,295,598 | 0 | `cb6d700` |
| `fuzz_parser` | 300 s | 16,198,868 | 0 | `43d1e2f` |

<!-- /gen -->

## Known gaps

- Mostly one real day. Pins apply to that file and the synthetic day.
- No `B` message exists on the 2019 day, so broken trades have synthetic tests only.
- The <!-- gen:c_away -->82,356<!-- /gen --> `C` executions are characterised, mechanism not shown.
- The inferences about Nasdaq's engine in `docs/DISCREPANCIES.md` are marked as inferences.
- Benchmarks ran on one machine with no x86 cross-check.

## Definition of done

From the brief. Unticked items wait for the owner or for the final regeneration.

- [x] Parser and book builder pass verification on at least one full day: `results/day_12302019.json`, with a second day in `results/day_20251208.json`.
- [x] Loss-injection test suite green, locally: `ctest` entries `loss_injection` and `mold_sim_cli`.
- [ ] The same suites green in CI: the workflow has not yet run on GitHub.
- [x] Published benchmark table with full methodology: README, `docs/METHODOLOGY.md`.
- [x] Replay demo recording: `docs/images/demo-replay.gif`.
- [ ] Hosted static dashboard: it runs locally; it is not deployed.
- [x] DESIGN.md written.
- [ ] README complete: the CI and deployed-dashboard images, the badge and the dashboard link wait for a public repository.
- [ ] All results regenerated from the final commit: `scripts/regenerate_results.sh`.
