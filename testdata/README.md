# Test data

## slice.itch.gz: a real-data slice of the NASDAQ ITCH 5.0 feed

A byte-exact excerpt of `12302019.NASDAQ_ITCH50` (NASDAQ TotalView-ITCH 5.0, trading day
2019-12-30): every system event (`S`) message plus every message for eight symbols, in the
original order, each with its original length prefix. Nothing is edited, so every order's whole
life is present and the slice replays with no reference to an order outside it.

Source and attribution: NASDAQ TotalView-ITCH 5.0 sample data published by Nasdaq at
`https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/`. The data is Nasdaq's; this repository holds a
1613 KB excerpt of it solely as a regression fixture. The full file is not stored here
(`scripts/fetch_itch.py` downloads it).

| | |
|---|---|
| Messages in the slice | 44,692 of 268,744,780 |
| Uncompressed | 1613634 bytes, SHA-256 `2bf28171e8bf767733fc9fd40a17df40e8c7662389cbccd45e94bfed8e1050b6` |
| Compressed (`gzip -9 -n`) | 509838 bytes, SHA-256 `81b1b29d6d45d553a6c97b9441ef0cad7a8d96adf09cdfd9a3803d35e6311528` |

### Selection rule

Counted per symbol over the whole day (a one-pass census of the full file), then:

1. **Greedy cover.** Among symbols with 300 to 30,000 messages, repeatedly take the one that adds
   the most not-yet-covered features, ties broken by fewer messages. The features are: each of
   `A F E C X D U P Q H R`, a trading halt (`H` with state `H`), a resume to trading, an
   opening and a closing cross (`Q` types `O`, `C`), a halt/IPO cross (`Q` types `H`, `I`)
   and a non-printable `C`. Two symbols cover all of it: CETXP and SPSK.
2. **Smallest halted symbols.** ZJZZT (smallest with a halt/IPO cross).
3. **Smallest symbols with every order message type** (`A F E C X D U P` all present): CREX, OCC,
   SG, PBTS.
4. **One mid-volume symbol with several halt/IPO crosses**, so market-state handling sees
   repeated halt/resume cycles: MKD (7 halt/IPO crosses, 32,860 messages).

`testdata/slice_symbols.txt` holds the list; `itch_slice` cuts it:

```
build/release/tools/itch_slice data/12302019.NASDAQ_ITCH50 slice.itch --symbols "$(cat testdata/slice_symbols.txt)"
gzip -9 -n slice.itch
```

| Symbol | Messages | A | F | E | C | X | D | U | P | Q | H | halts (H state) | halt/IPO crosses |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| CETXP | 954 | 183 | 201 | 12 | 4 | 1 | 377 | 152 | 17 | 3 | 3 | 0 | 1 |
| CREX | 328 | 87 | 58 | 1 | 1 | 1 | 144 | 29 | 3 | 2 | 1 | 0 | 0 |
| MKD | 32860 | 12559 | 802 | 4831 | 533 | 168 | 10164 | 1576 | 2201 | 9 | 16 | 2 | 7 |
| OCC | 382 | 82 | 79 | 3 | 1 | 1 | 160 | 51 | 1 | 2 | 1 | 0 | 0 |
| PBTS | 486 | 133 | 60 | 9 | 1 | 2 | 188 | 68 | 21 | 2 | 1 | 0 | 0 |
| SG | 475 | 135 | 65 | 22 | 3 | 1 | 181 | 62 | 2 | 2 | 1 | 0 | 0 |
| SPSK | 317 | 60 | 62 | 0 | 0 | 0 | 122 | 68 | 0 | 2 | 2 | 1 | 0 |
| ZJZZT | 350 | 169 | 2 | 0 | 2 | 0 | 169 | 0 | 0 | 3 | 4 | 1 | 1 |

**Coverage.** Every in-scope message type is present in the slice (`A F E C X D U P Q H R S`),
as are non-printable `C` prints (529, excluded from printed volume), halts, resumes and all
cross types. **No broken-trade (`B`) message exists anywhere in this
trading day** (the full-day census has no `B`), so `B` handling is covered by the unit tests
(`Stats.BrokenTradeSubtracted`), not by real data.

## expected_slice.json: the golden export

The reviewed `itch_replay` export of the slice, run as

```
itch_replay slice.itch --verify --full-check-every 1 --symbols "$(cat testdata/slice_symbols.txt)" --export out.json
```

`scripts/compare_export.py` compares a fresh export with it in CI (job `verify-slice`) and in
ctest (`slice_golden`), ignoring `meta`, `max_rss_bytes` and `state_dump`. Review notes:
the replay has `unknown_ref` 0, all anomalies 0, conservation holds for every locate, and 0
crossed books in continuous trading. The oracle counts are findings, not failures: 222 executions
not at the best price (all outside continuous trading) and 9 not at the head of their queue.

To regenerate it after an intentional change, run the command above from a clean commit, using a
repo-relative slice path (`build/testdata/slice.itch`, from `scripts/prepare_slice.py`) so the
stamp holds no machine-specific path; review the diff and copy the result over this file.
