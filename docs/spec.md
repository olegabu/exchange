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
| FIX ↔ SBE codecs | `NewOrderSingle`/`OrderCancelRequest` → SBE in; SBE fills → `ExecutionReport` out (§7) |
| Liquibook vendoring + CMake | Header-only, but ships no CMake (§3.1) |
| Sequencer consumption | `sequencer` exports no CMake package (§3.3) |

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

**Decision:** vendor the headers under `third_party/liquibook/`, pinned
to a specific commit, wrapped in a one-line INTERFACE target
`exchange::liquibook`. No MPC, no Boost, no build of upstream.

This is the same narrowest-dependency discipline
`sequencer::output_codec_api` and `sequencer::fix_session_api` follow:
depend on the declarations you need and let no object files travel that
nobody asked for.

### 3.2 SBE (encoding)

- **Not in vcpkg** either.
- SBE's C++ side is a **Java code generator**: an XML schema in,
  generated C++ headers out. **Java 21 is present on this machine**
  (verified), so generation at build time is viable.

**Decision:** schema at `schema/exchange.xml`; a CMake custom command
runs the SBE JAR to generate headers into the build tree; the JAR is
vendored under `third_party/sbe/`.

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
already uses. Adding install/export rules to `sequencer` is the
cleaner long-run answer and is a change to *that* repo, not this one;
not required for v1.

### 3.4 Dependency summary

| Dependency | Source | Build cost |
|---|---|---|
| liquibook | vendored headers, pinned | none (header-only) |
| SBE | vendored JAR + checked-in generated headers | JVM only on `make regenerate` |
| sequencer | sibling checkout | builds via its own release preset |
| hffix, braft, brpc, gtest… | inherited from sequencer's vcpkg manifest | already paid |

---

## 4. The SBE schema is the wire format for both logs

One schema serves the braft log and the journal, because they carry the
same bytes: braft replicates the proposed input payload, and the
journal stores that payload alongside the outputs `apply()` emitted.
Nothing in `sequencer` interprets either — payloads are opaque to it
(`docs/specification.md` §5), which is precisely why the application
gets to choose SBE.

Messages, minimum viable set:

**Inbound (proposed, replicated, journaled as the input):**
- `NewOrder` — clientOrderId, instrumentId, side, price (int64, fixed
  point), quantity, timeInForce, orderType
- `CancelOrder` — clientOrderId, originalClientOrderId, instrumentId
- `ReplaceOrder` — deferred to v2 unless it falls out of liquibook free

**Outbound (emitted by `apply()`, journaled as outputs):**
- `OrderAccepted`, `OrderRejected`, `OrderCancelled`,
  `OrderCancelRejected`
- `Fill` — makerOrderId, takerOrderId, price, quantity, aggressor side
- `BookUpdate` — deferred to v2; market data is a separate gateway
  shape (`sequencer/docs/specification.md` §8.12) and not needed to
  demonstrate matching

Rules the schema must obey, all consequences of §4.1:

- **Fixed-point integers only. No floating point anywhere** — not in
  the schema, not in the matching, not in outputs. Prices are int64 in
  minimum ticks; the tick size is instrument static data, not a
  runtime float.
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

| SBE out | FIX out |
|---|---|
| `OrderAccepted` | `35=8` ExecutionReport, `150=0` (New) |
| `Fill` | `35=8` ExecutionReport, `150=F` (Trade) |
| `OrderRejected` | `35=8` ExecutionReport, `150=8` (Rejected) |
| `OrderCancelled` | `35=8` ExecutionReport, `150=4` (Canceled) |
| `OrderCancelRejected` | `35=9` OrderCancelReject |

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
  docs/spec.md                 this document
  schema/exchange.xml          the SBE schema; append-only once written
  generated/                   checked-in SBE output, refreshed by `make regenerate`
  src/
    order_book_state_machine.{hpp,cpp}
    fix_codecs.{hpp,cpp}       FIX ↔ SBE, both directions
    exchange_node_main.cpp     RunNode(argc, argv, make_unique<OrderBookStateMachine>())
  tests/
    determinism_test.cpp       differential: two instances, same inputs, byte-identical outputs
    matching_test.cpp          price-time priority, partial fills, ties, cancel/fill races
    snapshot_test.cpp          save/restore/replay produces identical outputs
    fix_end_to_end_test.cpp    order in over FIX, execution report out from the journal
  third_party/liquibook/       vendored headers, pinned commit
  third_party/sbe/             vendored generator JAR
  CMakeLists.txt
```

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
   determinism review of §5 done as it is written, plus the
   differential test.
4. **Snapshots.** Save/restore plus the replay-identity test.
5. **FIX codecs.** Both directions; end-to-end test through the real
   gateway binary.
6. **Measure.** Reuse `raft-tests`' harness shape: knee sweep, the
   `merge-hdr.py` percentile merge, `mkcharts.py` for the plot. The FIX
   journal arm's 400k is the reference to beat or explain.

---

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

## 11. Open questions

- **Instrument static data.** Tick size, lot size, price bands must be
  deterministic state. Loaded from a sequenced input at startup, or
  compiled in for v1? Recommend: sequenced input, because config that
  differs between replicas is a divergence waiting to happen.
- **Self-trade prevention.** Not in liquibook. Needed for a credible
  exchange, but it is a policy layer; v2.
- **Multi-instrument.** One book per instrument in one state machine,
  or one instrument per raft group? The former is simpler and matches
  the single-apply-thread model; the latter scales further. v1: single
  state machine, several books, keyed by instrument id.
- **Order id space.** Client order ids are client-scoped and not
  unique across sessions. The exchange must mint its own ids
  deterministically — sequence number plus an index, never a counter
  that could differ between replicas.
