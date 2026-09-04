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

Not yet measured: `snapshotSave`/`snapshotLoad` at 100k orders (build
step 4 implements them).

## 2. Loopback — pending (step 6)

## 3. Fleet p50 at 100k — pending (step 6)

## 4. Fleet sweep — pending (step 6)
