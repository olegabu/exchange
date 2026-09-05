# exchange — a worked example exchange on `sequencer`

A deterministic central limit order book, replicated by `sequencer`,
reachable over FIX 4.4. This document is the contract to build from.

**Scope, deliberately narrow.** One instrument class, one matching
engine, one FIX gateway. **No OMS**: no parent/child orders, no
allocation, no risk pre-trade checks, no position keeping. A FIX
`NewOrderSingle` becomes a book operation and nothing else stands
between them. An OMS is the obvious next layer and is explicitly out of
scope for the first version, because it is the layer that would make
every determinism question harder before the easy ones are settled.

Naming note: requested as `spec.doc`; written as `spec.md` to match
`sequencer/docs/specification.md`. Rename freely.

The *why* behind the decisions in this document — the verified facts
about `sequencer` that forced them, the alternatives weighed, and a
dated decision log — is `design.md`. Read it second.

---

## 0. Roadmap

This specification covers **v1**. The product grows in three layers,
in one repository, over one schema, composed by one state machine:

| Version | Layer | What it adds |
|---|---|---|
| **v1 — CLOB** (this document) | `src/book`, `src/state_machine`, `src/fix` | One matching engine, several books keyed by instrument, FIX 4.4 order entry (new / cancel / replace), snapshots, the determinism and replay gates. No OMS, no market data |
| v2 — spot exchange | `src/oms`, `src/marketdata` | Accounts and positions, pre-trade limits, self-trade prevention, cancel on disconnect, per-day `ClOrdID` uniqueness; a market-data gateway (output-only FIX session, the shape `sequencer` already provides) |
| v3 — perpetuals | `src/perps` | Perpetual contracts as an instrument type; margin, liquidation, funding; mark price and funding ticks as *sequenced inputs* |

The layers are inside one `apply()`, not processes on top of each
other: positions, limits and margin must see the same sequenced state
as the book and be updated by the fill in the same input. The book
layer is the bottom and depends on nothing above it (§8). Milestones are
git tags. `design.md` §1 records why this is one repository and not
three.

Some v2/v3 keys are therefore already on the v1 wire (`account`,
`instrumentId`), because SBE lets a field be appended later but not
moved — and a v1 journal that lacked them could not be replayed under
v2 with positions attributed.

---

## 1. What already exists, and what has to be built

This is the honest inventory. Everything in the first table is
finished, measured, and running today; everything in the second is
work.

### 1.1 Reused from `sequencer` as-is

| Piece | Where | State |
|---|---|---|
| Consensus + replicated log | `node/` (braft) | Done. Knee ~325-400k/s depending on gateway |
| Journal (segments, mmap, replay) | `journal/` | Done |
| FIX 4.4 session layer (hffix) | `gateway/fix/session/` | Done; conformance-tested against QuickFIX |
| FIX gateway, **journal flavour** | `gateway/fix/` | Done. `--inline_designated_outputs=false` is the default, and is the flavour this spec uses |
| Journal-as-resend-store | `gateway/fix/` | Done. A FIX `ResendRequest` is served by re-reading the journal and re-running the output codec |
| `StateMachine` embedding surface | `docs/specification.md` §4 | Done: `apply`, `snapshotSave`, `snapshotLoad`, `OutputCollector` |
| Determinism rules | `docs/specification.md` §4.1 | Done — and binding on us, see §5 |

The FIX-journal arm is also the fastest thing in the repository: zero
drops through **400k orders/sec** at a 6.7ms p50, five client machines,
3-node multi-AZ. That is the ceiling this exchange inherits before it
adds a single line of matching logic.

### 1.2 To be built here

| Piece | Why it is not free |
|---|---|
| `OrderBookStateMachine` wrapping liquibook | Must satisfy §4.1 determinism; liquibook was not written with replication in mind (§5) |
| Book snapshot / restore | **liquibook has no serialization.** `snapshotSave`/`snapshotLoad` must walk the book and rebuild it exactly (§6) |
| SBE schema + generated codecs | The wire format for both the braft log and the journal (§4) |
| FIX ↔ SBE codecs | `NewOrderSingle`/`OrderCancelRequest`/`OrderCancelReplaceRequest` → SBE in; SBE fills → `ExecutionReport` out (§7) |
| Liquibook vendoring + CMake | Header-only, but ships no CMake (§3.1) |
| Sequencer consumption | `sequencer` exports no CMake package yet (§3.3) |
| Apply-path benchmark | The state machine runs on the single apply thread; its cost is the throughput ceiling (§9.1) |

---

## 2. Shape

```
FIX client ──35=D──> FIX gateway (hffix, journal flavour)
                          │  InputCodec: FIX → SBE OrderRequest
                          ▼
                    Propose (braft)          payload = SBE bytes
                          │
                    consensus + replication
                          ▼
                    apply thread ──> OrderBookStateMachine (liquibook)
                          │              emits SBE ExecReport outputs
                          ▼
                       journal            same SBE bytes, durable
                          │
                    tail ──> OutputCodec: SBE → FIX 35=8
                          ▼
FIX client <──35=8── FIX gateway
```

The single most important property, inherited and not re-litigated:
**an execution report reaches the client from the journal, never from
the propose receipt** (`sequencer/docs/specification.md` §8.11). The
synchronous ack is a receipt, not a fill. This is what makes a
`ResendRequest` answerable years later and what makes two replicas
agree on what a client was told.

---

## 3. Dependencies — decided, with evidence

### 3.1 liquibook (CLOB)

- **Not in vcpkg.** Checked against a 2,548-port tree: no `liquibook`.
- **Core is header-only** — upstream: *"The core of Liquibook is a
  header-only library, so you can simply add Liquibook/src to your
  include path then `#include <book/order_book.h>`"*. C++11 or later.
- MPC (Make Project Creator) is used only for its own tests and
  examples, and Boost only for its unit tests. **We need neither.**

**Decision:** vendor the headers under `vendor/liquibook/`, pinned to a
specific commit, wrapped in a one-line INTERFACE target
`exchange::liquibook` (a `SYSTEM` include, so this repo's `-Werror`
does not apply to upstream). No MPC, no Boost, no build of upstream.

This is the same narrowest-dependency discipline
`sequencer::output_codec_api` and `sequencer::fix_session_api` follow:
depend on the declarations you need and let no object files travel that
nobody asked for.

### 3.2 SBE (encoding)

- **Not in vcpkg** either.
- SBE's C++ side is a **Java code generator**: an XML schema in,
  generated C++ headers out. **Java 21 is present on this machine**
  (verified), so generation at build time is viable.

**Decision:** schema at `schema/exchange.xml`; `make regenerate` runs
the SBE JAR over it; the JAR is vendored under `vendor/sbe/` with its
checksum.

**Also check in the generated headers** under `generated/`, refreshed
by a `make regenerate` target, and have CI diff them against a fresh
generation. Reason: a build that needs a JVM is a build that breaks on
a machine without one, and the failure would land far from its cause.
The checked-in copy is what compiles; the diff is what keeps it honest.

### 3.3 sequencer

- **`sequencer` has no `install()` or `export()` rules** (verified: the
  only match in its CMake tree is `CMAKE_EXPORT_COMPILE_COMMANDS`). It
  cannot be consumed with `find_package`.

**Decision:** sibling checkout plus `add_subdirectory`, with
`SEQUENCER_DIR` overridable — exactly the arrangement `raft-tests`
already uses — behind an `EXCHANGE_USE_SEQUENCER_PACKAGE` toggle whose
other position is `find_package(sequencer CONFIG)`. The `sequencer::*`
target names are identical either way.

`sequencer` **will be packaged**, as a change to that repository:
first `install()`/`export()` with a `sequencerConfig.cmake`, then a
vcpkg overlay port so a consumer lists `sequencer` in its own
`vcpkg.json` and vcpkg resolves braft/brpc/grpc transitively. The port
is the stage that actually decouples consumers; install/export alone
still requires the identical vcpkg baseline. It is sequenced after
build step 2 (§9), so that this repo — the first real consumer — has
shown exactly which targets and headers must be exported. A second
consumer, `ledger`, is planned; the goal is that developers build
replicated state machines on `sequencer` as a library.

The three repositories live together under `opensequencer/`:

```
opensequencer/
  sequencer/     the platform
  raft-tests/    its benchmark harness
  exchange/      this, an application built on it
```

They are siblings by requirement, not by convenience: `raft-tests`
resolves `SEQUENCER_DIR ?= ../../sequencer` and this repo will
`add_subdirectory` the same checkout. Grouping them cost one directory
and no edits -- the relative path was already correct and stayed
correct.

Symlinks at the old `~/workspace/{sequencer,raft-tests,exchange}`
paths point at the new locations. They exist so that the 53GB CMake
build trees, which record absolute paths in `CMakeCache.txt`, survived
the move without a full rebuild. They are a transition aid: once no
tooling refers to the old paths, delete them and reconfigure the build
trees once.

### 3.4 Dependency summary

| Dependency | Source | Build cost |
|---|---|---|
| liquibook | vendored headers, pinned | none (header-only) |
| SBE | vendored JAR + checked-in generated headers | JVM only on `make regenerate` |
| sequencer | sibling checkout via `add_subdirectory`; `find_package` once packaged | built inside this repo's tree; vcpkg manifest shared (`VCPKG_MANIFEST_DIR`) |
| hffix, braft, brpc, gtest… | inherited from sequencer's vcpkg manifest | already paid |

---

## 4. The SBE schema is the wire format for both logs

One schema serves the braft log and the journal, because they carry the
same bytes: braft replicates the proposed input payload, and the
journal stores that payload alongside the outputs `apply()` emitted.
Nothing in `sequencer` interprets either — payloads are opaque to it
(`docs/specification.md` §5), which is precisely why the application
gets to choose SBE.

Messages, minimum viable set. Every inbound message begins with the
submitting `sessionId` (the gateway's routing id), `senderCompId`,
`account` and `clOrdId`; every outbound message begins with the
addressee `sessionId`, because the output codec sees only the journal
record and must address the FIX session from it alone.

**Inbound (proposed, replicated, journaled as the input):**
- `AddInstrument` — symbol, tickSize, lotSize, maxOrderQty. Instrument
  static data is *sequenced*: it enters the state machine as an input,
  proposed by the `exchange_admin` tool, never compiled in or read from
  a file — configuration that could differ between replicas is a
  divergence waiting to happen. The state machine assigns the numeric
  `instrumentId`. A symbol is never reused.
- `NewOrder` — symbol, side, orderType, timeInForce, price, quantity
- `CancelOrder` — origClOrdId, symbol, side
- `ReplaceOrder` — origClOrdId, symbol, side, quantity, price. In v1,
  because liquibook's `replace()` makes it nearly free.

**Outbound (emitted by `apply()`, journaled as outputs):**
- `OrderAccepted`, `OrderRejected`, `OrderCancelled`,
  `OrderCancelRejected`, `OrderReplaced`, `OrderReplaceRejected`,
  `InstrumentAdded`
- `Fill` — **one per input**, carrying a repeating group of execution
  entries, one entry per party per match: `sessionId, senderCompId,
  account, clOrdId, orderId, instrumentId, symbol, side, lastPx,
  lastQty, leavesQty, cumQty, avgPx, counterpartyOrderId, aggressor`.
  The output codec maps one entry to one `ExecutionReport`.
- `BookUpdate` — deferred to v2; market data is a separate gateway
  shape (`sequencer/docs/specification.md` §8.12) and not needed to
  demonstrate matching

**Why `Fill` is a group and how large it may grow.** `sequencer`'s
`OutputCollector` allows at most 64 outputs per input and throws past
that, so one output per match would crash every replica identically on
a 64-match sweep. The binding ceiling is instead the journal's
`maxRecordBytes` (256 KiB by default, covering the input and all its
outputs): about 2,000 entries, 1,000 matches, per input. Admission
keeps it unreachable — `AddInstrument` is rejected unless
`maxOrderQty / lotSize ≤ kMaxMatchesPerInput` (the worst case is one lot
per resting order) — and `schema_test` asserts that bound times the
entry size fits the record size. A market that needs more starts its
node with a larger `--journal_max_record_bytes`.

**Identity.** `orderId` is the sequence number of the accepting
`NewOrder`: unique, deterministic, and no counter that could differ
between replicas. A match is identified by the input's sequence number
and the entry index; `ExecID(17)` is built from exactly those. There is
no separate trade id.

Rules the schema must obey, all consequences of §4.1:

- **Integers only. No floating point anywhere** — not in the schema,
  not in the matching, not in outputs. liquibook's `Price` and
  `Quantity` are `uint64_t` with no scale of their own; on the wire
  `Price` and `Qty` are `int64` in units of 10⁻⁸, declared once as the
  constant `priceExponent`. The decimal point exists only in the FIX
  codec's string conversion. Tick and lot sizes are instrument static
  data in the same units, and the state machine rejects a price or
  quantity that is not a multiple of them.
- Explicit field ordering and explicit padding; no implicit struct
  layout, no uninitialised bytes on the wire.
- Schema versioned from day one. SBE's extension rules (append-only
  fields, never renumber) are what let a journal written by an old
  binary still replay under a new one — and §8.3 requires exactly that:
  restart from any sequence number, identical output.

**Why SBE, and what is actually specific to it.**

Permanence is a property of the JOURNAL, not of SBE. Any encoding put
in front of it -- protobuf, flatbuffers, hand-rolled structs --
inherits the same "every record ever written must still decode"
constraint, because §8.3 requires replay from any sequence number to
produce identical output. That is settled by `sequencer`'s design and
is not a choice this project gets to make.

SBE is chosen for what it is good at, and it is a good fit here:
records are compact, and decoding is a cast over the buffer rather
than a parse into new objects. That matters on the apply path
specifically, where §4.1 asks for bounded allocation and the latency
budget is measured in microseconds -- a format that allocates per
record would be paying for it once per order, forever.

What IS specific to SBE is how a schema may CHANGE. Fields are located
by computed offset, not by tag, so:

- fields may be appended, never reordered, never removed;
- a field's type and width are frozen once written;
- readers resolve layout from the schema version in the header, so
  every version ever written must remain describable.

A tag-based format tolerates reordering and removal; SBE does not.
So the cost of getting the initial field layout wrong is higher than
it would be with protobuf -- not because the journal is permanent
(it would be either way), but because SBE gives fewer ways to walk a
layout mistake back. Review the layout before step 3; adding a field
later is free, moving one is not.

---

## 5. Determinism review of liquibook — the main technical risk

`sequencer/docs/specification.md` §4.1 binds every state machine.
Liquibook was written as a single-process matching engine, not as a
replicated one, so each rule below is a **claim to verify against the
vendored source**, not an assumption:

| §4.1 rule | What to check in liquibook |
|---|---|
| No clock reads | Book operations must take no timestamp of their own. If any path stamps a time, it must be replaced by a sequenced field carried in `NewOrder` |
| No floating point in state or outputs | Confirm `Price`/`Quantity` are integer typedefs on every path, including any fill-price computation |
| No unordered-container iteration into outputs | The book is price-ordered by design; verify that *order-level* structures at a price level are also deterministically ordered (FIFO by arrival), and that nothing iterates a hash map to produce fills |
| No pointer addresses into state | Check that order identity is by id, never by address, and that no comparator falls back to pointer ordering for ties |
| Bounded allocation | Per-order allocation is acceptable for v1; note it and measure. Preallocated pools are a later optimisation, not a correctness issue |
| No I/O or syscalls | Confirm no logging or callbacks perform I/O on the apply path |

**How to verify, not argue.** The decisive test is a differential one:
run the same input sequence through two independently-constructed
instances and byte-compare every emitted output — and run it with
inputs generated adversarially (crossing orders, exact-price ties,
same-price same-quantity arrivals, cancels racing fills). Reading the
code is how you form the hypothesis; the differential test is what
settles it. That ordering — hypothesis from reading, verdict from
measurement — is the single most transferable lesson from the
`sequencer` work (§10).

Ties are where replicas diverge. Price-time priority must be total:
if two orders can compare equal on both price and time, the tiebreak
must be an explicit deterministic field (sequence number), never
insertion order into a container whose iteration order is incidental.

---

## 6. Snapshots

Liquibook provides no serialization, so both directions are ours.

- `snapshotSave` — walk the book in a defined order (instrument, then
  side, then price, then time priority) and write SBE records. Defined
  order matters: a snapshot is compared across replicas.
- `snapshotLoad` — rebuild by replaying those records into an empty
  book **through the same insertion path normal orders take**, so a
  restored book cannot differ structurally from a live one.

The acceptance test is the one `sequencer` already forces: restore at
sequence N, replay to M, and produce byte-identical outputs to a run
that never restarted (§8.3).

---

## 7. FIX mapping

The gateway is `sequencer`'s existing hffix one, journal flavour,
unchanged. This repo supplies only the two codecs.

| FIX in | SBE |
|---|---|
| `35=D` NewOrderSingle | `NewOrder` |
| `35=F` OrderCancelRequest | `CancelOrder` |
| `35=G` OrderCancelReplaceRequest | `ReplaceOrder` |

| SBE out | FIX out |
|---|---|
| `OrderAccepted` | `35=8` ExecutionReport, `150=0` (New) |
| `Fill`, per entry | `35=8` ExecutionReport, `150=F` (Trade) |
| `OrderRejected` | `35=8` ExecutionReport, `150=8` (Rejected) |
| `OrderCancelled` | `35=8` ExecutionReport, `150=4` (Canceled) |
| `OrderReplaced` | `35=8` ExecutionReport, `150=5` (Replaced) |
| `OrderCancelRejected` | `35=9` OrderCancelReject, `434=1` |
| `OrderReplaceRejected` | `35=9` OrderCancelReject, `434=2` |

FIX tag 1 `Account` is carried through as `account` and echoed; v1
does nothing else with it (§0).

Notes that are already settled by the platform and must not be
redesigned here:

- Execution reports arrive **from the journal**, in journal order, once
  (§8.11). The propose receipt is not turned into a FIX message.
- A `ResendRequest` is served from the journal by re-running the output
  codec, which is why the codec must be a **pure function** of the
  journal record. No clocks, no counters, no state outside the record.
- `MarketDataRequest` (`35=V`) subscription semantics, including
  `SubscriptionRequestType` (263) 0/1/2, are implemented in the
  gateway already. Market data itself is v2.

---

## 8. Layout

```
exchange/
  docs/spec.md                 this document; design.md is the why
  schema/exchange.xml          the SBE schema; append-only once written
  generated/                   checked-in SBE output, refreshed by `make regenerate`
  vendor/liquibook/            vendored headers, pinned commit
  vendor/sbe/                  vendored generator JAR
  src/
    book/                      v1 CLOB: the liquibook wrapper. Pure matching over
                               order ids, integer prices and quantities. Depends on
                               itself, liquibook and the standard library ONLY --
                               never on sessions, accounts, FIX, or sequencer
    state_machine/             the one apply(): identity, admission, instruments,
                               output encoding, snapshots
    fix/                       FIX ↔ SBE codecs, one library per direction
    main/                      exchange_node, exchange_replay, exchange_fix_gateway,
                               exchange_admin
    wire/                      SBE header helpers
    oms/ marketdata/           v2 (§0)
    perps/                     v3 (§0)
  tools/
    journal_stats_main.cpp     what a journal actually contains (§9.2)
  bench/
    apply_benchmark.cpp        google benchmark over apply() and the snapshot paths (§9.1)
  tests/
    determinism_test.cpp       differential: two instances, same inputs, byte-identical outputs
    matching_test.cpp          price-time priority, partial fills, ties, cancel/fill races
    snapshot_test.cpp          save/restore/replay produces identical outputs
    fix_end_to_end_test.cpp    order in over FIX, execution report out from the journal
  CMakeLists.txt  CMakePresets.json  Makefile
```

The layer rule is enforced by the linker, not by convention:
`exchange::book` is its own target with no `sequencer::*` dependency.

---

## 9. Build order

Each step ends in something runnable or measurable. No step depends on
a later one being right.

1. **Skeleton + dependencies.** Vendor liquibook, vendor SBE, wire
   `add_subdirectory` onto sequencer, get an empty `StateMachine` that
   compiles and runs a node. Nothing matches yet.
2. **Schema.** `exchange.xml`, generation, checked-in headers, CI diff.
   Review the field LAYOUT here: appending a field later is free,
   moving or removing one is not (§4).
3. **Matching.** `OrderBookStateMachine` over liquibook, with the
   determinism review of §5 done as it is written, the differential
   test, and the apply-path benchmark of §9.1. (In parallel, as its own
   change to `sequencer`: packaging, §3.3.)
4. **Snapshots.** Save/restore plus the replay-identity test.
5. **FIX codecs.** Both directions; end-to-end test through the real
   gateway binary.
6. **Measure.** Reuse `raft-tests`' harness shape, in the order §9.1
   prescribes: benchmark gate, loopback, fleet p50 at 100k, then the
   knee sweep with the `merge-hdr.py` percentile merge and `mkcharts.py`
   for the plot. The FIX journal arm's 400k is the reference to beat or
   explain.

### 9.1 Latency and throughput budget

The interfaces are already measured (`raft-tests/sequencer/README.md`):
roughly 800–900 µs p50 at 100k orders/s, and a 400k knee for the
FIX-journal arm. `apply()` runs on the single pinned pure-spin apply
thread (`sequencer/docs/specification.md` §5.4) and adds to both
numbers directly. Two budgets follow, and the second is the tighter:

| Budget | Bound | Why |
|---|---|---|
| Latency | apply p50 ≤ **100 µs** | Keeps the 1 ms p50 target at 100k on top of the measured interface p50 |
| Throughput | apply cost per input **< 1/rate: 10 µs at 100k, 2.5 µs at 400k** | One thread applies everything; above this it is the knee |
| Allocation | zero heap allocations per apply after warm-up is the target; v1's per-order allocation is permitted, **measured and recorded** | §4.1's bounded-allocation rule, and allocation is the usual tail source |
| Snapshot | save and load cost at 100k resting orders bounded and measured | A snapshot stalls the apply thread |

The order of measurement is fixed so that no fleet time (≈$79/day) is
spent on an unmeasured state machine:

1. `bench/apply_benchmark.cpp` (google benchmark, release preset):
   resting add, add crossing one level, add sweeping ten levels, cancel,
   replace, reject, and the snapshot paths, at books of 10, 1k and 100k
   resting orders — reporting ns/op and allocations per apply. Inputs
   are pre-encoded outside the timed loop so only the state machine is
   measured. **Gate:** within budget, or fix before going further.
2. Loopback on the development machine at 100k, for rig problems and a
   first p50.
3. Fleet, p50 at 100k only.
4. Fleet, the full sweep.

Numbers, and the explanation of every difference from the interface
reference, go to `measurements.md`.

---

### 9.2 Asking the journal what actually happened

`exchange_journal_stats --data_dir=<node data dir>` replays a journal
through a fresh state machine — the same path `exchange_replay` uses —
and reports what went in, what came out, and the book at the end:

```
records            2000000 of 25000021
inputs:   NewOrder 1428571   CancelOrder 285714   ReplaceOrder 285714
outputs:  OrderAccepted 1428571   Fill 571425   OrderCancelRejected 51944
          (Fill entries) 1142850  -> 571425 matches
book at the end:  live orders 51947   instruments 1
```

`--replay_through=N` bounds it, since replaying tens of millions of
records takes minutes.

**It exists because nothing else could answer "is this measurement
measuring what I think".** A load generator reports its own throughput
and latency and nothing about what the exchange did with the traffic:
a run where every order is rejected looks *fast*. This tool is what
established that the flow matched at the designed rate, that the book
was bounded (and later, that it was not), and that a suspiciously good
result was real work rather than a stream of rejects.

It also corrected a wrong inference. Node RSS climbing during a run
was read as the book growing; the tool showed the book bounded at 3,830
orders, and the memory was the node mmapping its own journal. **Reach
for it before drawing conclusions from process metrics** — §10.1's
lesson, in the one place this repository keeps tripping over it.

## 10. What this project must not relearn

The `sequencer` work paid for these. They are written down here because
a new contributor — or a new assistant session — starts with none of
the conversation that produced them, and each one cost hours.

1. **Hypotheses from reading code were wrong far more often than
   right.** Every real fix in the sequencer work came from
   instrumentation, not inspection: six-plus code-reading theories were
   rejected by measurement before the actual cause was found, more than
   once. Instrument first, then form the theory.
2. **Verify the instrument fires.** A probe that reports nothing is
   indistinguishable from an absent probe. Prove non-vacuity — run it
   at a threshold that must trigger, or delete the fix and watch the
   test fail.
3. **Verify flags reach the process.** `make` does not carry
   environment over `ssh`. Read `/proc/<pid>/environ` before trusting
   any A/B.
4. **Verify binaries by checksum on every host.** A stale remote binary
   produces real-looking numbers for code that is not under test — the
   most expensive failure mode available in this kind of work.
5. **Build what CI builds before pushing.** `sequencer`'s release
   preset uses LTO, which folds duplicate symbols that its debug preset
   rejects; two link failures reached CI an hour after their pushes
   because local release builds were green. `make preflight` exists for
   this.
6. **Some bugs cannot reproduce locally at all.** QuickFIX's
   `QUICKFIX_THROW` becomes `noexcept` under C++17, turning "config key
   absent" into `std::terminate` — and whether it does depends on the
   compiler *vcpkg* used, not the one building the project. When a bug
   is environment-dependent, add an assertion that fails locally on the
   same condition.
7. **Fix the whole class, not the instance.** That same QuickFIX bug
   was fixed once for the first of four optional-config probes; CI
   simply failed on the second one an hour later.
8. **Search by behaviour, not by name.** A deduplication pass that
   grepped for identifiers missed five more copies that did the same
   thing under different names. Grep for `mkdtemp`, not `makeTempDir`.
9. **Charts are output too.** Render them and look. A palette entry
   that was never validated failed a colour-vision check; a legend
   silently drew two of seven series outside the canvas; a subset
   regeneration silently replaced a seven-series comparison with a
   three-series one. All three looked fine in a thumbnail.
10. **`braft` holds one fd per log segment and truncates only on
    snapshot.** Past ~1000 segments it hits `EMFILE`, latches the node
    into `ERROR`, and **never recovers — while still running and
    listening**. Raise `ulimit -n` at every launch site. This silently
    produced a whole sweep of zeros.

Read alongside this: `sequencer/docs/specification.md` (§4.1
determinism, §8.11 delivery path, §8.12 FIX shape),
`sequencer/gateway/quickfix/README.md` (the `QUICKFIX_THROW` section),
`sequencer/gateway/fix/README.md`, and
`raft-tests/sequencer/README.md` (knees, the residual tail, the fd
trap).

---

## 11. Open questions — and the ones now closed

Closed (2026-09-04; the reasoning is in `design.md` §7):

- **Instrument static data** — a sequenced `AddInstrument` input (§4),
  proposed by `exchange_admin`. Config that differs between replicas is
  a divergence waiting to happen.
- **Multi-instrument** — one state machine, several books, keyed by
  instrument id. One instrument per raft group scales further and is
  not needed to demonstrate matching.
- **Order id space** — `orderId` is the accepting input's sequence
  number (§4). Never a counter that could differ between replicas.

Open:

- **Self-trade prevention.** Not in liquibook. Needed for a credible
  exchange, but it is a policy layer; v2, in `src/oms`.
- **Reconnect routing in the FIX gateway.** `sequencer`'s catch-up
  ignores the addressee and its session ids are per-connection
  (`design.md` §4). A `sequencer` change; documented and skip-tested
  here until it lands.
