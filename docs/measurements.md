# Measurements

The budgets are in `spec.md` §9.1; the order is benchmark → loopback →
fleet p50 at 100k → sweep, and nothing later runs until the earlier
number is within budget. Every number here says what produced it.

## 1. Apply path, google benchmark (build step 3)

`make bench` → `build/release/bench/apply_benchmark` (release preset,
LTO). Machine: 4 × 3.1 GHz, g++-10, 2026-09-04. Load average was still
above 6 from a preceding build, so wall time is inflated by
preemption; the **CPU** column is the one to read. Book depth is
resting orders per side, ten per price level; inputs pre-encoded;
allocations counted through a replaced `operator new`.

Per apply = CPU time ÷ applies per iteration.

| Scenario (applies/iter) | depth 10 | depth 1k | depth 100k | allocs/apply |
|---|---|---|---|---|
| rest far from touch + cancel (2) | 0.58 µs | 1.21 µs | 1.72 µs | 1.50 |
| sell filling one resting bid + replenish (2) | 1.06 µs | 1.48 µs | 2.36 µs | 2.50 |
| sell sweeping ten bids + replenish ten (11) | 1.28 µs | 1.70 µs | 2.47 µs | 2.91 |
| rest + replace (price +1 tick) + cancel (3) | 0.76 µs | 1.21 µs | 2.07 µs | 1.67 |
| off-tick reject (1) | 0.12 µs | — | 0.12 µs | ~0 |

The sweep apply itself, backed out of its row: ≈ 27.2 µs − 10 × 1.7 µs
≈ **10 µs for ten matches / twenty Fill entries**, i.e. ≈1 µs per
match.

Against the budget: every single-match scenario is under the
**10 µs @100k** bound by 4–6×, and sits at or just under the
**2.5 µs @400k** bound on a 100k-deep book. A ten-order sweep costs
one 100k-rate slot. The state machine is not the knee at 100k; at 400k
it would be close, and pooling the two `std::map` nodes per order
(the order record and the live-ClOrdID index) plus liquibook's
callback vector is the first thing to try — those are the 1.5–2.9
allocations per apply.

### Snapshot (build step 4)

Same binary, `--benchmark_filter=Snapshot`; depth is per side, so
100k is 200k resting orders. Load average was ~10 from a preceding
build; CPU column again.

| | depth 1k (2k orders) | depth 100k (200k orders) |
|---|---|---|
| `snapshotSave` | 0.80 ms, 270 KB | **83 ms, 27 MB** (≈135 B/order) |
| `snapshotLoad` into a fresh instance | 2.5 ms | **400 ms** (≈3 allocations/order: the order record, the live index, liquibook's tracker) |

Save is what stalls the apply thread: at 200k resting orders, 83 ms
once per braft snapshot interval. Load happens on a restarting
replica, off the critical path. Both scale linearly with resting
orders, not with history.

## 2. Loopback (build step 6) — a rig check, not a measurement

One 4-core, 3.1 GHz development box running all three roles:
`exchange_node` (whose apply thread pure-spins a whole core by
design), `exchange_fix_gateway`, and `exchange_load_generator` over the
loopback interface. Release preset, 3 s warm-up, 10 s measured.

| offered | p50 | p99 | drops | unanswered | rig schedule lag p99 |
|---|---|---|---|---|---|
| 1,000/s | 4.8 ms | 25.5 ms | 0 | 0 | 17.5 ms |
| 10,000/s | 13.7 ms | 83.8 ms | 0 | 0 | 17.4 ms |

**What this establishes.** The whole path works under continuous load:
nothing dropped, nothing went unanswered, the gateway logged no errors,
and the flow matched (`load_generator_shape_test` pins that). That is
what a loopback run is for.

**What it does not establish, and cannot.** Any latency number. The
harness's own rig criterion rejects it: schedule lag sits near 17 ms at
p99 at *every* rate including 1,000/s, which means the load generator's
own thread is being descheduled, and at 10,000/s the harness prints
"the offered rate was not actually achieved. This run cannot be
reported as such" and declines the row. Four cores cannot host a
pure-spinning apply thread, brpc's worker pool, a gateway and a sender
at once. This is exactly the case the fleet exists for, and exactly the
reason to run the loopback first: it costs nothing and it says plainly
that the next number has to come from real hardware.

**One thing the loopback caught that no reading would have.** The first
two order-flow shapes tried in `exchange_fix_requester.hpp` produced,
respectively, almost no matching and a book that grew with run length.
Both would have made a fleet sweep measure a growing book rather than
an exchange, at $79/day. `tests/load_generator_shape_test.cpp` now
asserts that the flow matches and that peak depth does not move when
the run doubles.

## 3. Fleet — the first sweep (2026-09-04)

Fleet: 3 × `c7a.4xlarge` nodes, multi-AZ (us-east-1a/b/c); 5 ×
`c7a.2xlarge` clients. **One** FIX gateway, on NODE1 — the same shape
`sequencer-fix` was measured in, so the two are comparable and the
intended difference between them is a matching engine on the apply
thread instead of an eight-byte counter. Release preset, binaries
checksum-verified on every host (§10.4), 10 s warm-up, 30 s measured,
offered rate split across the five clients.

| offered | achieved | p50 | p99 | dropped by rig |
|---|---|---|---|---|
Final ladder, after the load-generator fix below:

| offered | achieved | p50 | p99 | dropped by rig |
|---|---|---|---|---|
| 10,000 | 9,995 | **786 µs** | 1,005 µs | 0 |
| 25,000 | 24,991 | **899 µs** | 2,332 µs | 0 |
| 50,000 | 25,043 | 164 ms | 222 ms | 659,726 |
| 75,000 | 15,234 | 276 ms | 340 ms | 2,155,903 |
| 100,000 | 12,022 | 512 ms | 591 ms | 3,339,154 |
| 150,000–500,000 | 10.1k → 6.1k | 1.0 s → 7.7 s | — | rising |

Chart: `raft-tests/knee-exchange-fix.svg`.

**The result.** Clean through **25k** at 1.2 ms p50 with zero drops.
The knee is between 25k and 50k. `sequencer-fix` — the same gateway,
journal and delivery path, carrying a counter — runs clean to **400k**.
So the exchange reaches roughly **1/16th** of it, and the achieved rate
*declines* past the knee rather than plateauing.

### The investigation: what the ceiling is not

Each ruled out by measurement, not by argument (§10.1). The sweep was
re-run after each change; the knee did not move.

- **Not the platform.** sequencer's own counter, run on this same
  fleet on the same day through the same gateway shape, reached
  **399,899/s at 4.5 ms p50 with zero drops**. braft, the journal, the
  gateway chassis and the hardware are all fine at 400k. This control
  is what makes the rest of the comparison meaningful.
- **Not the input path, the payload size, the FIX input codec, or
  propose.** Running the identical flow against an unknown symbol, so
  every order is *rejected* — same 97-byte input, same codec, same
  propose path, one small output, no matching — reached **99,955/s
  with zero drops** and 343,696/s at 400k offered.
- **Not the state machine.** With `SEQ_APPLY_STALL_US=25` armed and
  verified in `/proc/<pid>/environ` (§10.3), `apply()` costs **1 µs at
  p50 and 46 µs at maximum**. That is a ceiling near a million per
  second, forty times above the observed knee.
- **Not snapshots.** None ever fired: braft's default interval is an
  hour and the runs were minutes.
- **Not gateway CPU.** Under a five-client 50k run its busiest thread
  was **13.7%** of one core, and a `perf` profile showed nothing above
  2.6%, with `__sched_yield` on top — a thread spinning on empty.
- **Not accumulated journal or book state.** 50k collapses identically
  on a freshly wiped cluster (24,258 achieved) as on one that had
  already run the lower rates (21,011).

Two of my own earlier readings were wrong and are corrected here: node
RSS growth was the node mmapping its own journal, not the book growing;
and "the apply thread is idle so the ceiling is upstream" came from an
already-collapsed run, where idleness is as much effect as cause. The
`sm=1 µs` figure above is the direct measurement that replaces it.

### What was found and fixed

The five-client run left **53,127 orders resting and climbing**, with
97,293 cancel/replace rejects, and the arithmetic closed exactly:
1,403,953 accepted − 1,123,246 retired by fills − 227,595 cancelled =
53,112. The load generator's cycle balances only if each taker consumes
the maker its *own* session placed; with five sessions interleaved a
taker often finds the level already cleared, so it rested instead,
leaking 3.8% of orders per cycle — linear in run length. **The sweep
was partly measuring a book growing without bound.**

Fixed by making takers immediate-or-cancel, which is also what an
aggressive order usually is. `load_generator_shape_test` now replays
five phase-staggered sessions and asserts rejects do not grow with run
length; it reads 1,001 → 4,001 with the defect and 2 → 2 without, and
that failure was demonstrated before the fix was trusted (§10.2). An
earlier version of the same test passed *with the bug reintroduced*,
because round-robin put every session in lockstep — a vacuous probe,
caught only by trying to make it fail.

**This did not move the knee**, so it was a real defect in the
measurement, not the cause of the ceiling.

### What the ceiling actually was

liquibook locates an order for `cancel` and `replace` by **scanning its
price level** (`find_on_market` walks the multimap comparing pointers),
so both are O(orders resting at that price). The load generator's band
was five ticks either side of the mid, which put the entire book into
eleven prices — nothing like a real venue — and made that scan the
dominant cost of `apply()`.

The probe that found it was the same one that had said "1 µs": the
earlier figure was measured at 10k with a nearly empty book, and does
not hold at scale. Re-armed at the knee it read very differently, and
the fix is visible in one number:

| at 45,000/s | `apply()` p50 | p50 latency | achieved |
|---|---|---|---|
| 11 price levels | **131 µs** | 2.46 ms | 43,813 |
| 1,001 price levels | **1 µs** | 1.06 ms | 44,712 |

131× less work per input from spreading the same book over more
prices, with the **same match rate per record** (0.29 either way) and a
book of ~5.5k orders instead of ~22k — so this is not throughput bought
by matching less.

### The ladder after the fix

Same fleet, same gateway, band widened to what a venue looks like:

| offered | achieved | p50 | dropped |
|---|---|---|---|
| 10,000 | 9,995 | 871 µs | 0 |
| 25,000 | 24,994 | 921 µs | 0 |
| 50,000 | 42,133 | 1,025 µs | 12,724 |
| 75,000 | 55,339 | 1,136 µs | 35,701 |
| 100,000 | 73,114 | **1,311 µs** | 75,951 |
| 150,000 | 106,890 | 8.2 ms | 259,874 |
| 200,000 | **124,899** | 86 ms | 1.2M |
| 250,000–500,000 | 109k → 64k | 157 ms → 550 ms | rising |

**p50 stays near 1 ms through 100,000 offered, and the peak achieved
rate is ~125,000/s** — five times the first sweep's ~25k. `spec.md`
§9.1's latency target (interface p50 + ≤100 µs) is met at 100k: 1,311 µs
against the counter's 1,005 µs on the same fleet, so the matching
engine is costing about 300 µs of round trip, not milliseconds.

The remaining gap to `sequencer-fix`'s 400k is what an exchange
actually buys: 288 B records against 16 B, 1.57 FIX messages out per
input against 1, and a book to maintain.

### Still open

- **Cancel/replace stays O(depth at a price).** Harmless with a wide
  book, dominant with a narrow one, so an instrument whose liquidity
  sits at one or two prices would hit it. The fix is an index from
  order identity to book position, which is a change to the vendored
  matcher: `liquibook-determinism.md` records it as v2 work.
- Between 50k and 150k the rig drops rise while p50 stays near 1 ms,
  which is the client's in-flight cap binding rather than the exchange
  slowing; a run with a larger `--max_inflight` would separate them.

## 4. Fleet p50 at 100k — superseded

Folded into §3: 100k is well past the knee, so the §9.1 latency target
is not meaningful there yet. The comparable figure is 25k at 1.2 ms.

Gated: the EC2 fleet costs ~$79/day and is started only for a
measurement that is ready to be made, then stopped
(`make stop-instances`, every instance confirmed `stopped`). The
benchmark gate above is met; the loopback says the number has to come
from here. Needs, per the plan: `APP_BIN_DIR` in
`raft-tests/sequencer/Makefile`, an `exchange_admin add-instrument`
after the gateways start, binaries checksummed on every host (§10.4),
and `/proc/<pid>/environ` checked for any env flag (§10.3).


