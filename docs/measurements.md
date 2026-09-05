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

**Configuration: 5 client boxes, one session each, ONE gateway on the
leader node, wide price band, `--max_inflight=50000`.**

| offered | achieved | p50 | p99 | dropped |
|---|---|---|---|---|
| 10,000 | 9,995 | 716 µs | 884 µs | 0 |
| 25,000 | 24,991 | 784 µs | 1,012 µs | 0 |
| 50,000 | 47,488 | 874 µs | 102 ms | 0 |
| 75,000 | 56,233 | 969 µs | 134 ms | 0 |
| 100,000 | 75,020 | **1,076 µs** | 177 ms | 0 |
| 150,000 | 112,478 | 9.4 ms | 301 ms | 0 |
| 200,000 | 121,888 | 11 ms | 325 ms | 936,445 |
| 250,000 | **123,408** | 753 ms | 1.27 s | 3.0M |
| 300,000–500,000 | 105k → 67k | 1.8 s → 3.1 s | — | rising |

**Zero drops through 150,000 offered**, p50 near 1 ms to 100k, ceiling
~123,000/s.

### Three corrections to what was reported earlier

- **"The knee moved to 125k" was wrong.** Hollow markers mean *achieved
  < 99% of offered*, and they start at 50k, not 125k. What moved was
  the **ceiling** (peak achieved). sequencer's README separates last
  clean rate, knee and ceiling; conflating them overstated the result.
- **The drops before the ceiling were the client, not the exchange.**
  The harness defaults `maxInflight` to `max(1000, rate/10)`; raising
  it to 50,000 removed *every* drop up to 150k. At 100k the same
  cluster went from 73,114 achieved with 75,951 drops to 75,020 with
  **zero**. Any run showing drops well below the ceiling should be
  repeated with `MAX_INFLIGHT` raised before the exchange is blamed.
- **The p99 spread is real and unexplained.** From 50k up, p50 stays
  near 1 ms while p99 is 100 ms+. That is bimodal, not a slow system,
  and nothing measured so far accounts for it.

### Controls

**A flow that never matches** (`--flow=rest-cancel`: every order a buy,
cancelled immediately, so nothing can cross), one gateway:

| offered | achieved | p50 | dropped |
|---|---|---|---|
| 100,000 | **99,965** | 1,016 µs | 0 |
| 200,000 | 142,421 | 221 ms | — |

Clean at 100k, ceiling ~142k. So matching costs the *clean* rate
(100k → ~50k) but barely moves the ceiling — the ceiling is shared and
is not the matching engine.

**Gateway count**, real flow, both gateways on the leader (a gateway
must be colocated with a node to tail its journal, and putting one on a
follower would add a cross-AZ hop to every propose):

| gateways | 100k achieved | 200k achieved | 200k p50 |
|---|---|---|---|
| 1 | **99,965** | 125,461 | 389 ms |
| 2 | 70,195 | 118,477 | **39 ms** |

A second gateway **does not raise the ceiling** — more evidence the
gateway is not the constraint — but it improves the tail markedly under
overload (389 ms → 39 ms at 200k). It is worse at 100k, and the
unanswered count roughly doubles, which is not yet explained.

### A correctness bug found on the way: session ids collide across gateways

Sequencer's `FixInputTransport::nextSessionId` is a counter starting at
1 **inside each gateway process**, while the journal is shared by every
gateway tailing it. This repository routed outputs with
`fanout.toSession(sessionId)`, so with two gateways both handing out
session 1, every output was delivered twice — once to the right client
and once to a stranger.

Fixed here rather than in sequencer, since both halves are ours: the
input codec composes an operator-assigned `--gateway_id` into the wire
session id (`gatewayId << 32 | connectionId`), and the output codec
delivers only what its own id addresses, stripping the half back off
before handing the id to the transport.
`FixCodecs.TwoGatewaysDoNotDeliverEachOthersReports` asserts it and was
shown to fail without the fix. It does **not** fix the reconnect gap
(olegabu/sequencer#1): the low half is still a per-connection counter.

Fixing it did not change the two-gateway throughput, so the collision
was a correctness bug, not the cause of the shortfall.

### A retraction: 250k / 375k / 500k were not real

While testing whether the rig was the ceiling, I launched 20 load
generators by hand, one ssh at a time, and summed their reported
achieved rates to 250,000, then 375,000, then 499,810/s at
sub-millisecond p50 — apparently beating sequencer's own counter.

**That was an artifact and those numbers are withdrawn.** Twenty
sequential ssh launches take 60–100 s against a 55 s run, so the first
generator finished before the last one started. Each measured a
lightly-loaded cluster on its own, and summing rates that were never
concurrent produces a total that was never offered.

Run properly — all 20 launched in parallel, log timestamps confirming
they started within one second of each other — the same configuration
achieves **107,569/s with 17M drops and a 333 ms p50**. That is
consistent with every parallel measurement here, and with the ~123k
ceiling the single-generator ladders showed.

`sweep-gen.sh` now checks the spread of generator start times and warns
when a row is not a concurrent measurement, so this cannot recur
silently.

The lesson generalises past this bug: **a rate summed across
independently-timed clients is not a rate.** The existing
`sweep-multi.sh` avoids it by launching in parallel and waiting on all
of them before reading any result; my hand-rolled loop did not, and the
number it produced was flattering enough that only its implausibility
gave it away.

### The ladder with a bounded flow, 20 sessions, two gateways

Configuration: 5 client boxes × 4 generators = **20 sessions**, split
across **two gateways on the leader** (a gateway must be colocated with
a node to tail its journal, so a follower would add a cross-AZ propose
hop). `--max_inflight=50000`, 1,001 price levels, gateways confirmed
accepting connections before any client starts.

| offered | achieved | p50 | p90 | p99 | dropped |
|---|---|---|---|---|---|
| 25,000 | 25,000 | 809 µs | 932 µs | 1,284 µs | 0 |
| 50,000 | 49,980 | 906 µs | 1,081 µs | 233 ms | 0 |
| 75,000 | **74,980** | 1,022 µs | 1,288 µs | 387 ms | 0 |
| 100,000 | 72,154 | 1,146 µs | 1,705 µs | 225 ms | 0 |
| 125,000 | 89,022 | 1,274 µs | 2,104 µs | 214 ms | 0 |
| 150,000 | 102,665 | 1,432 µs | 2,820 µs | 236 ms | 0 |
| 200,000 | 128,895 | 2,258 µs | 36 ms | 214 ms | 0 |

Full offered rate through **75,000**, p50 near 1 ms throughout, and
**zero rig drops at every rate**. Ceiling ~129,000.

The book is now bounded by construction: **6 live orders** after 1.5M
records, against ~52,000 before, with matching healthy (349,033 matches
per 1.5M records). The 349,003 cancel-rejects are the mechanism, not a
fault: a maker filled before its own cancel arrived. Every maker leaves
the book on one path or the other, so depth is bounded by the number of
sessions rather than the length of the run.

### Why p99 departs at 50k, and what it is not

p50 stays near 1 ms while p99 jumps to ~230 ms from 50k up. The shape
says what kind of thing it is: at 50,000/s over 30 s that is 1.5M
requests, so p99 = 233 ms means roughly **15,000 slow requests** — far
too many to be "fills cost more". But a single ~300 ms stall at that
rate catches ~15,000 in flight, so one or two brief stalls per run
reproduce the number exactly. At 25k a 30 s window usually contains
none, which is why p99 is clean there.

Ruled out by direct measurement, with every probe verified in
`/proc/<pid>/environ` first:

| suspect | probe | result |
|---|---|---|
| The state machine / fills | `SEQ_APPLY_STALL_US=20000` | **no apply exceeded 20 ms** in a 40 s run at 50k |
| Journal segment rollover | `SEQ_SEGMENT_OPEN_US=1000` | **zero** segment opens over 1 ms |
| Journal tail and output codec | `SEQ_TAIL_STALL_US=20000` | **no tail stall** over 20 ms |
| The node: propose → commit → apply | braft's own `node_propose_batch_apply_wait_us` | p50 492 µs, **p99 630 µs**, max 2,206 µs |

So the answer to "is it the book, or fills taking longer" is **no**: the
whole node-side path, matching included, is tight to a 2.2 ms maximum.
The 230 ms tail happens entirely outside the node.

What is left is the gateway's session I/O and the client. The strongest
remaining hypothesis, untested: `FixOutputTransport` delivers to every
session from **one ring-reader thread**, so a single session whose
socket blocks — a client slow to read, a full TCP window — stalls
delivery for all of them. That is consistent with everything measured:
nothing CPU-saturated, the node clean, and a second gateway (halving
the sessions per delivery thread) markedly improving the tail at high
rates. Testing it needs a probe around the send path, or a run with one
deliberately slow client.

### Still open

- **What the ~110–125k ceiling is — still open, but no longer disk or
  network.** Measured on the leader at the ceiling: disk 52.7 MB/s,
  263 IOPS, **24% utilisation** (of a gp3 volume rated 125 MB/s and
  3,000 IOPS, `w_await` 3.7 ms); network **rx 20.5 MB/s, tx 57.8 MB/s**
  against a link of roughly 1,560 MB/s; CPU 64% idle. None of them is
  close to saturation.
  Also not the gateway (a second one does not raise the ceiling), not
  the state machine (1 µs with a wide book), not the output codec
  (307k), not the input path (343k). Everything measured has headroom
  and yet the rate is capped, which is the signature of a *blocking*
  path rather than a saturated resource. The untested surface left is
  the propose→commit round trip inside braft and the handoff between
  the gateway's session threads and the ring.
- **The bimodal p99**, and why two gateways lose more replies than one.

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


