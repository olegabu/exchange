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

## 3. Fleet p50 at 100k — pending

Gated: the EC2 fleet costs ~$79/day and is started only for a
measurement that is ready to be made, then stopped
(`make stop-instances`, every instance confirmed `stopped`). The
benchmark gate above is met; the loopback says the number has to come
from here. Needs, per the plan: `APP_BIN_DIR` in
`raft-tests/sequencer/Makefile`, an `exchange_admin add-instrument`
after the gateways start, binaries checksummed on every host (§10.4),
and `/proc/<pid>/environ` checked for any env flag (§10.3).

## 4. Fleet sweep — pending (step 6)
