# exchange — design record

`spec.md` is the contract: what v1 must do. This file is the *why*: the
facts about `sequencer` that forced each design choice, the decisions
that followed, the gaps we know about, and a dated log so that a later
reader — or a later assistant session, which starts with none of the
conversation — does not re-derive or silently reverse any of it.

Append to §7 whenever a decision is made. Never rewrite history there.

---

## 1. One repo, layered — not three repos inheriting

The product grows CLOB → spot exchange → perpetuals (spec §0). The
progression is real; the repository boundary is the wrong place to cut
it, for four reasons:

1. **The journal is permanent and the SBE schema is append-only.**
   Every record ever written must decode forever, and fields can only
   be appended. Three repos would mean three schemas that must compose;
   the wire format is precisely the thing that must evolve as one.
2. **An OMS is not a layer on top of the CLOB — it is inside the same
   `apply()`.** Position keeping, pre-trade limits and margin must see
   the same sequenced state as the book and be updated by the fill in
   the same input, atomically. `sequencer/docs/specification.md` §4.2
   names the shape: an admission layer in front of an execution core.
   So "exchange reuses CLOB" is a library dependency between two
   directories compiled into one state machine.
3. **Cross-repo reuse in this ecosystem is the expensive kind.**
   `sequencer` is consumed by sibling checkout + `add_subdirectory` with
   a shared vcpkg baseline (until §5 lands). A chain
   `perps → exchange → clob → sequencer` would be four sibling checkouts
   whose toolchain and baselines must all agree, for one consumer each.
4. **Perps is the same exchange, not a new one.** A perpetual is an
   instrument type; margin and liquidation are the account layer gating
   admission; funding and mark price are periodic *sequenced inputs*
   (the sequencer's externalised-non-determinism pattern). A spot
   exchange with position keeping is most of the perps account model,
   so that layer must be designed to be extended, not replaced.

Consequences: `src/book/` may depend on nothing but itself, liquibook
and the standard library — enforced by the linker (`exchange::book` has
no sequencer dependency), not by convention. Milestones are git tags;
`git subtree split src/book` extracts a standalone CLOB later if one is
ever wanted. Splitting later is one command; merging three repos later
is not.

---

## 2. Facts about `sequencer` that shaped v1 (verified, with locations)

Paths are relative to `../sequencer`.

| Fact | Where | Consequence here |
|---|---|---|
| `OutputCollector` caps at **64 outputs per input** and throws past it | `journal/include/sequencer/state_machine.hpp:38-42` | One `Fill` output per input carrying a repeating group, never one output per match |
| `outputIndex` in resend/catch-up counts **codec fan-out calls**, not state-machine outputs | `gateway/fix/output/src/fix_output_transport.cpp:22-31,87-92,364-378` | One SBE output may become N FIX messages: a `Fill` group → one ExecutionReport per entry |
| `OutputCodec` sees only `RecordView` (sequence, input, outputs); routing is `Fanout::toSession(uint64)` | `gateway/output/include/sequencer/output_codec.hpp:20-65` | Every outbound message carries its addressee `sessionId` |
| `ClientRequest::sessionId` is a per-connection counter from 1; the CompID pair is what survives a reconnect | `gateway/fix/input/src/fix_input_transport.cpp:320,344`; `gateway/fix/session/include/sequencer/fix/fix_session.hpp:294` | Journal both `sessionId` (routing) and `senderCompId` (ownership) |
| Catch-up re-sends **every** captured output to a reconnecting session | `fix_output_transport.cpp:339-381` (`CapturingFanout`) | Known gap, §4 |
| `RunFixSessionGateway(config, inputCodec, outputCodec)`; the app writes its own `main` and gflags | `gateway/fix/include/sequencer/fix/fix_session_gateway.hpp:38-70`; `examples/counter/fix_gateway_main.cpp` | Our gateway main is the counter's with the codecs swapped |
| Input and output codec libraries must be **separate** targets linking only the `*_codec_api` INTERFACE targets | `examples/counter/CMakeLists.txt:16-38` | Otherwise two chassis gflags (`--listen_port`, `--data_dir`) collide at link — and only under the debug preset, since release LTO folds them (spec §10.5) |
| `runReplayCheck` is all-or-nothing: no "restore at N, replay to M" | `tools/replay/src/replay_check.hpp:17-40`, `replay_check.cpp:19-92` | Our snapshot identity test is new code over `JournalWriter`/`JournalReader` and `SnapshotWriter(path)`/`SnapshotReader(path)` |
| Journal record = `u64 seq · u32 len · input · u16 n · {u32 len · output}×n`; `maxRecordBytes` default 256 KiB; `maxInputSize` 64 KiB | `journal/include/sequencer/journal/record_view.hpp:36-52`, `writer.hpp:67-68`, `node/src/node_impl.hpp` | The real per-input ceiling on fills, §3 |
| The apply thread is single, pinned, pure-spin, and "performs no allocation after warm-up" | `docs/specification.md` §5.1, §5.4 | The throughput budget in spec §9.1 |
| liquibook `master` at `2427613b` (BSD-3, Object Computing Inc.): `Price`, `Quantity`, `Cost` are `uint64_t`; books are `std::multimap<ComparablePrice, OrderTracker>` (equal keys keep insertion order); orders are located by pointer **equality**; callbacks are queued in a `std::vector`; `std::cerr` appears only in the exception handler of `callback_now()`; `<cmath>` is included; no clock, no `rand`, no unordered container | `vendor/liquibook/src/book/{types,order_book,order_tracker}.h` | Hypotheses for spec §5, settled by the differential test, not by reading |
| SBE 1.40.1 (2026-08-21) `sbe-all` jar on Maven Central; Java 21 present | `https://repo1.maven.org/maven2/uk/co/real-logic/sbe-all/1.40.1/` | Vendored with sha256 |
| vcpkg has `hffix` and `quickfix` ports; no `sbe`, no `liquibook` | `$VCPKG_ROOT/ports` | Both vendored |
| `raft-tests/sequencer/Makefile` hardcodes `build/release/examples/$(APP)/`, and its `FixRequester` sends the counter's `U1` with no payload hook | `Makefile:12,289-319,551`; `bench/load_generator/include/sequencer/bench/fix_requester.hpp:228-237` | Step 6 needs an `APP_BIN_DIR` override there and an exchange requester here |

---

## 3. Decisions beyond the spec's text

- **Identity.** `orderId` = the sequence number of the accepting
  `NewOrder`: unique, deterministic, no counter to disagree about. A
  match is named by the input's sequence number plus its entry index,
  which the codec already has: `ExecID(17)` = `"<seq>-<outputIndex>-<entry>"`.
  No separate trade id.
- **Keys that v2/v3 hang off, fixed now.** SBE cannot walk a layout
  back, so v1 already carries what later layers key on: `symbol`
  (`char[8]`, the input codec is stateless and has no journal to learn
  a table from), `account` (`char[16]`, FIX tag 1 — a CompID identifies
  a *connection*, one firm trades many accounts, positions and margin
  are per account; v1 only echoes it), and a numeric `instrumentId`
  assigned by `AddInstrument` and echoed in every output. Symbols are
  never reused; a second `AddInstrument` for a known symbol is rejected.
- **Prices are integers everywhere.** liquibook has no scale. On the
  wire `Price` and `Qty` are `int64` in units of 10⁻⁸ (a constant
  `priceExponent` in the schema). The decimal point exists only in the
  FIX codec's string conversion — no `double` anywhere.
- **Outputs per input.** `OrderAccepted` or `OrderRejected`, then at most
  one `Fill` (a group of execution entries, one per party per match),
  then `OrderCancelled` for an IOC/FOK remainder. The 64-output cap
  forbids one output per match; the binding ceiling is the journal's
  `maxRecordBytes` (input + all outputs), ≈2,000 entries at the default
  256 KiB. Admission makes it unreachable: `AddInstrument` is rejected
  unless `maxOrderQty / lotSize ≤ kMaxMatchesPerInput` (worst case one
  lot per resting order), and `schema_test` asserts the entry size times
  that bound fits the record size. A market needing more raises
  `--journal_max_record_bytes` at journal creation.
- **Dedup** is against *live* `ClOrdID`s per `senderCompId` only. A
  history table is unbounded state; live-only keeps state bounded and
  fully snapshot-able. FIX's per-day uniqueness is v2's concern.
- **Books.** One state machine, `std::map<Symbol, Book>`; `Book` wraps
  `liquibook::OrderBook<Order*>`. `Order`s live in a `std::map` keyed by
  `orderId` for node-stable addresses. liquibook compares order pointers
  for equality only, never orders by them; `make check-liquibook` keeps
  the vendored copy free of the constructs spec §5 forbids.
- **Time in force.** Day/GTC rest; IOC/FOK map to liquibook conditions;
  Market orders are forced IOC; stops are rejected (v2).
- **Replace** keeps `orderId`, adopts the new `clOrdId`, and loses
  priority on a price change (liquibook re-inserts) — standard.
- **Snapshot** is SBE, walked instrument → side → price priority → time
  priority straight off the multimap; load rebuilds through `Book::add()`
  with `order_qty() == leavesQty` and refuses a snapshot that fills on
  load (a crossed book is corruption).
- **Designation.** The submitter's own outputs are designated; the
  journal-flavour FIX gateway ignores designation (§8.11), request/
  response transports get it for free.
- **`--inline_designated_outputs` is not offered.** It is sound only
  when every output a session receives originates in its own inputs
  (`gateway/input/src/input_gateway_impl.hpp:51-68`); passive fills
  violate that.

---

## 4. Known gaps (recorded, not fixed here)

- **sequencer FIX gateway, reconnect.** Catch-up re-runs the output
  codec and sends a reconnecting session *every* output in the window,
  addressee ignored (`fix_output_transport.cpp:339-381`); and
  `Fanout::toSession` ids are per-connection, so fills addressed to a
  dead id after a reconnect are dropped by `sessionFor() == nullptr`.
  For an exchange with several clients that leaks other clients' fills
  on reconnect. `tests/fix_end_to_end_test.cpp` carries a two-client
  reconnect drill asserting the correct behaviour, `GTEST_SKIP`ped with
  a pointer here until the fix lands in `sequencer/gateway/fix` as its
  own change.
- **`FixRequester` has no payload hook** (`fix_requester.hpp:228-237`);
  the exchange load generator forks it. Upstream generalisation
  (virtual body builder, configurable MsgType) is a sequencer follow-up.
- **`raft-tests` hardcodes `examples/$(APP)`**; step 6 adds
  `APP_BIN_DIR`.
- **Per-order heap allocation** — our `std::map` node and liquibook's
  callback vector — is accepted for v1 and measured by
  `bench/apply_benchmark.cpp`; pools are the fix if the §9.1 budget is
  missed.
- v2: cancel-on-disconnect, self-trade prevention, stops, market data,
  price bands, per-day `ClOrdID` uniqueness.

---

## 5. Sequencer as a library

`exchange` is the second link consumer after `examples/counter`; a
`ledger` (TigerBeetle-style general ledger) is planned; the goal is that
developers build replicated state machines on `sequencer`. So it will be
packaged, in two stages, each a change to *that* repository:

1. `install()`/`export()` producing `sequencerConfig.cmake` with
   `find_dependency()` for braft, brpc, gflags, glog, Protobuf, OpenSSL,
   Boost and hffix, plus a consumer smoke test in its CI.
2. An overlay vcpkg port, so a consumer lists `sequencer` in its own
   `vcpkg.json` and vcpkg resolves the transitive dependencies. This is
   the stage that actually decouples consumers; stage 1 alone still
   requires the identical vcpkg baseline.

Sequenced after exchange step 2, so the first real consumer defines what
must be exported. Until then this repo uses `add_subdirectory` behind
`EXCHANGE_USE_SEQUENCER_PACKAGE`; the `sequencer::*` target names are the
same either way, so nothing else changes when the toggle flips.

---

## 6. Performance discipline

The interfaces were measured (`../raft-tests/sequencer/README.md`):
~800–900 µs p50 at 100k orders/s, a 400k knee for the FIX-journal arm.
The state machine's `apply()` runs on the single pure-spin apply thread
and adds to both:

- **Latency**: ≤100 µs of apply cost keeps the 1 ms p50 target at 100k.
- **Throughput** (the tighter one): apply cost per input must be
  < 1/rate — **10 µs at 100k, 2.5 µs at 400k** — or the apply thread is
  the knee.

Order of measurement, so no fleet time (~$79/day) is spent on an
unmeasured state machine: google benchmark on `apply()` and the
snapshot paths → local loopback → fleet p50 at 100k → full sweep. The
numbers live in `measurements.md`.

---

## 7. Decision log

**2026-09-04**
- Single repository, layered `src/{book,state_machine,fix}` now,
  `src/{oms,marketdata,perps}` later; milestones are tags. (§1)
- Vendored code under `vendor/` (single-word directory names).
- `ReplaceOrder` (35=G) is in v1: liquibook has `replace()`.
- Instrument static data is a sequenced `AddInstrument` input proposed
  by `exchange_admin`, not compiled in.
- The FIX gateway reconnect gap stays in `sequencer`; documented and
  skip-tested here. (§4)
- `sequencer` will be packaged (install/export, then a vcpkg overlay
  port), after exchange step 2. (§5)
- Microbenchmark the apply path with google benchmark before any fleet
  run; budgets in spec §9.1. (§6)
- No trade id field; `account` is FIX tag 1 as a string; prices are
  `int64` at 10⁻⁸; one `Fill` group per input bounded by admission, not
  chunked. (§3)
