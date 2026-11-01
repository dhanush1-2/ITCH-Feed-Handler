# Design questions and answers

How this handler deals with the hard parts of a market-data path, with the evidence from this repository. A/B arbitration and kernel bypass were not built, so those two answers are designs.

## How are gaps detected and repaired, and what happens to messages arriving meanwhile?

`Receiver` tracks `expected`, the next sequence to deliver, and `hi`, the next one after the highest it knows exists. A packet starting past `expected` means a hole. A heartbeat past `hi` finds tail loss. A hole is requested at once, split at 65,534 messages, and re-requested every `timeout_ticks` without progress. After `max_attempts` at the same head position the receiver goes Stale and the run exits nonzero, since the protocol has no negative acknowledgement to say a range has expired.

Messages arriving during recovery are copied into a bounded buffer keyed by start sequence, then drained once the head fills, so the book only ever sees strict sequence order. If the buffer is full the packet is dropped and counted, the later hole gets requested, and retransmit heals it.

On the full day under combined stress, <!-- gen:lost -->11,085,799<!-- /gen --> messages were lost, and the state at feed second <!-- gen:noon_second -->43,200<!-- /gen --> plus the stream hash equal the lossless run. Loss here means `messages_lost_downstream`. Retransmit counters run far higher, because reordering opens holes that are requested before the delayed original arrives. A reorder-tolerance delay would reduce that; the immediate request was kept.

## How would two redundant feeds fit in?

Not built. Arbitration would sit at the same `Receiver` boundary, ahead of the book. First arrival wins per sequence, and the second copy is already handled as a counted duplicate. Each feed keeps its own health: last sequence seen, gap count, time since last packet. A retransmit is requested only when both feeds have missed a sequence, and only after a short wait for the other feed. The book, oracle and state comparison would not change, and the existing check carries over: the delivered stream hash must equal the file's, whichever feed supplied each message.

## Why does the replace message need care, and what happens to queue priority?

`U` carries the old reference, a new reference, shares and price, but no symbol or side, which come from the order being replaced. It acts as a delete plus an add with a new reference number, so the order loses its priority.

Three things need care here. The new reference can collide with a live order, which is counted as `duplicate_add` and leaves the live order intact. The replaced order's remaining shares go out through `replaced_out` and the new ones in through `replaced_in`, or conservation fails. And position in the new level comes from the reference number, like any other add, not from arrival.

## How is the order map laid out, and why?

An open-addressed `IdIndex` from Project 1 over a pre-reserved `OrderPool`. Live orders are slots in one pool, and levels link them by slot. Reserving up front means no rehash mid-day, and running out is a fatal error, because a dropped order corrupts every later message for that symbol.

The comparison on the full day, with the same pool, books and final state:

<!-- gen:index_table -->

|  | msgs/s (median of 5) | spread | latency p50 / p99 / p99.9 (ns, 42 ns granularity) |
|---|---|---|---|
| IdIndex (open addressing, blocked hash) | 14.41 M | 0.6% | 83 / 375 / 583 |
| std::unordered_map | 7.25 M | 1.1% | 125 / 625 / 833 |

<!-- /gen -->

The index is <!-- gen:index_speedup -->2.0x<!-- /gen --> faster, and that speed costs memory: a fixed <!-- gen:index_mb -->134.2 MB<!-- /gen --> table, where `std::unordered_map` ended with less total memory at this order count (<!-- gen:map_heap_end_mb -->222.1 MB<!-- /gen --> added against <!-- gen:heap_end_mb -->312.6 MB<!-- /gen -->). Peak live orders were <!-- gen:peak_live -->1,924,078<!-- /gen -->. Handler tables are the figure to use. `max_rss` includes the mapped input.

Hardware counters were not available. The map is measurably slower, but why is a hypothesis.

## What would kernel bypass change?

Not built. Only the receive boundary changes. Today `tools/mold_udp_demo.cpp` calls `recvfrom`, and that loopback demo is not a performance claim. With DPDK or Onload:

- The receive thread busy-polls a NIC ring on a pinned, isolated core, instead of blocking in the kernel.
- Packets land in pre-registered hugepage buffers. Parsing is already zero-copy views over bytes, so a view over a ring buffer works, provided the buffer stays valid until the message is applied. The receiver's out-of-order copy stays as it is.
- Time comes from NIC hardware timestamps rather than a logical tick.
- Multicast group joins and A/B handling move to the new layer, and the retransmit request gets its own path.

`Receiver`, the framer, decode, the builder and every verification layer stay the same. So does the check: hash the delivered stream. The benchmark would not carry over, since it measures a replay on an <!-- gen:cpu -->Apple M4 Pro<!-- /gen -->. Bypass would need Linux on x86 and a new measurement.

## What does the book look like during opening and closing crosses?

Not a clean two-sided market. On this feed:

- The cross itself is a `Q` message, which never touches a book. <!-- gen:zero_share_q -->12,189<!-- /gen --> `Q` messages have zero shares, which spec 1.5.2 says means no cross took place, so they are not trades.
- Allocations come as non-printable `C` messages at the cross price, in an order that does not walk the book. That is why <!-- gen:exec_not_best -->273<!-- /gen --> executions are not at the best price. All of them are outside continuous trading.
- Books can cross during a halt. Every one of the <!-- gen:episodes_other -->30<!-- /gen --> episodes starts in a halted or paused symbol. That the engine accepts orders without matching during a halt is an inference, though the correlation is complete. `docs/DISCREPANCIES.md` quotes an example.

So the "never crossed" invariant is scoped: <!-- gen:crossed_continuous -->0<!-- /gen --> crossed touches out of <!-- gen:touches_continuous -->255,010,681<!-- /gen --> in continuous trading, <!-- gen:crossed_scope -->with a 5 s grace after a resume (634 crossed touches at 0 s of grace, all within 1.214 ms of a resume)<!-- /gen -->. NOII and auction logic are out of scope, so this reports what the feed prints during a cross and does not model the auction.
