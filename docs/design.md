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

Gaps in `sequencer` are tracked as issues in that repository, because
that is where the fix has to happen and nobody working there reads this
file. This section is the context — why a test here skips — and the
issue is the work item.

- **sequencer FIX gateway, reconnect.** Catch-up re-runs the output
  codec and sends a reconnecting session *every* output in the window,
  addressee ignored (`fix_output_transport.cpp:339-381`); and
  `Fanout::toSession` ids are per-connection, so fills addressed to a
  dead id after a reconnect are dropped by `sessionFor() == nullptr`.
  For an exchange with several clients that leaks other clients' fills
  on reconnect. `tests/fix_end_to_end_test.cpp` carries a two-client
  reconnect drill asserting the correct behaviour, `GTEST_SKIP`ped until
  the fix lands. Tracked as
  **[opensequencer/sequencer#1](https://github.com/opensequencer/sequencer/issues/1)**
  — which is **closed as completed, but the defect still reproduces**
  against `origin/main` (verified 2026-09-05 with the checkout 0
  commits behind: the drill delivered 1 report addressed to ALPHA to a
  reconnecting BRAVO). `CapturingFanout::toSession` still takes its
  `SessionId` as an *unnamed* parameter and discards it
  (`fix_output_transport.cpp:24`), and `catchUp` forwards every
  captured body to the reconnecting session with no addressee filter
  (`fix_output_transport.cpp:359-372`). Note the resend path using the
  same class at line 87 is correct — it indexes `outputs` by an index
  recorded per session in a `SentRecord` — so it is plausible the
  resend fix was read as covering catch-up too. The `--gateway_id`
  ownership filter added here narrows the leak to sessions sharing one
  gateway; it does not close it.
- **sequencer FIX gateway, resends carry no `OrigSendingTime`.** FIX
  4.4 requires tag 122 on a `PossDup` retransmission.
  `SentRecord::sendingTime` is declared
  (`gateway/fix/output/include/sequencer/fix/fix_output_transport.hpp:53`)
  and passed to the session core when a resend is served
  (`fix_output_transport.cpp:99`), but **nothing ever assigns it**, so
  every resend goes out flagged `PossDup` with the field absent. Bodies
  and `MsgSeqNum` are correct, so the resend is otherwise faithful; a
  strict client engine may still reject it.
  `tests/fix_end_to_end_test.cpp` asserts the correct behaviour and
  `GTEST_SKIP`s while the gap stands. Tracked as
  **[opensequencer/sequencer#2](https://github.com/opensequencer/sequencer/issues/2)**.
- **sequencer journal, segment rollover blocks the apply thread.** At
  the default `--journal_records_per_segment=1048576` every client sees
  a ~300 ms stall at each segment boundary -- once every 20.97 s at
  50,000/s -- and it accounts for the entire p99. Quartering the flag
  takes p99 from 320 ms to 5.3 ms with p50 unmoved. Creation and
  sealing are already off the apply thread, but `roll()` still waits on
  the worker (`journal/include/sequencer/journal/writer.hpp:264-276`),
  and both of the worker's jobs scale with segment size while the
  90%-to-100% preparation window shrinks with it. Worked around here by
  setting the flag in every fleet run; tracked as
  **[opensequencer/sequencer#3](https://github.com/opensequencer/sequencer/issues/3)**,
  which also asks for a probe on the roll wait -- there is none today,
  and `SEQ_SEGMENT_OPEN_US` watches the reader, not the writer, so its
  silence misled this investigation for a while.
- **brpc cannot be run under ThreadSanitizer.** Its `bthread` is an M:N
  scheduler that switches stacks under the runtime, which tsan's
  happens-before model cannot follow, so every brpc-linked process
  reports races inside brpc/braft/glog. Verified rather than assumed:
  sequencer's own `node_integration_test` under its tsan preset reports
  eight races in the same internals. The three tests here that spawn
  `exchange_node`/`exchange_fix_gateway` therefore carry the ctest label
  `spawns-brpc`, and the tsan *test* preset excludes that label — they
  still build and still run under debug and release. Nothing in this
  repository is multi-threaded, so tsan's job here is the other 39
  tests, which run.
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
- Build step 3 findings: liquibook's replace path compared open
  quantities through `(int)`; patched (`vendor/liquibook/patches/0001`).
  Every replace re-queues (liquibook erases and re-inserts, even for a
  quantity decrease) — accepted for v1 as deterministic, asserted by
  `matching_test`, revisit if a venue rule requires priority retention.
  An undecodable or unknown input is a deterministic no-op with no
  output (there is no session to tell); an input from a newer schema
  version stops the node (`docs/liquibook-determinism.md`).
- Build step 4: a snapshot is length-prefixed SBE records
  (`src/state_machine/snapshot.hpp`), instruments by symbol then each
  book bids-then-asks in priority order. Restore re-inserts through
  `Book::add()`; a restored order is re-accepted silently, and a fill
  or cancel during restore is corruption (a live book is never crossed
  — nothing all-or-none ever rests in v1, which is the assumption that
  makes this true). Exact `cumNotional` travels as 128 bits so a
  restored replica computes the same `avgPx` on the next fill.
  `snapshotSave` at 100k resting orders measured in `measurements.md`.
- Build step 5: the FIX codecs are one library per direction (the
  chassis gflag collision, §2). The input codec rejects anything it
  cannot encode as a session-level Reject rather than proposing it, so
  a malformed order never reaches the journal. The output codec is a
  pure function of the record — asserted by encoding the same record
  twice and comparing bytes, which is what a `ResendRequest` relies on.
  `--inline_designated_outputs` is deliberately not offered by
  `exchange_fix_gateway` (§3). Instrument static data enters through
  `exchange_admin`, which proposes over brpc and prints the state
  machine's designated answer. Two more sequencer gateway gaps found by
  measurement and recorded above: catch-up addressing, and the missing
  `OrigSendingTime` on resends.
- Build step 6 (local half): `bench/exchange_fix_requester.hpp` is a
  fork of sequencer's `FixRequester` — its internals are private and its
  body is the counter's `U1`, so there is no hook to send a `35=D`
  (§4). Correlation is ClOrdID (tag 11), which every ExecutionReport
  already echoes, so no private tag and no FIFO fallback: a reply that
  cannot be correlated is one this sender did not cause. What is
  measured is NewOrderSingle to its first ExecutionReport.
- The load generator's flow sends all three order-entry messages, in a
  seven-message cycle that nets to zero: place/hit, place/cancel,
  place/replace/hit. Sending only NewOrderSingle would measure one of
  the exchange's three paths, and real venues see more cancels than
  trades. The fanout distributes whole CYCLES rather than messages,
  because a cancel or replace references the maker sent immediately
  before it and a ClOrdID is scoped to its CompID — splitting a cycle
  across sessions would reject every cancel and replace, and the sweep
  would measure rejects. The consequence is that a SINGLE-session run
  matches a client against itself, which is legal only while
  self-trade prevention is v2 (spec §11); when STP lands, this flow
  changes with it. On the fleet it is largely moot: every client quotes
  the same band on the same symbol, so a taker usually crosses somebody
  else's maker.
- `tests/load_generator_shape_test.cpp` drives the generator's OWN
  bytes through the real input codec rather than approximating them:
  "the flow is admissible" and "the flow encodes to FIX the codec
  accepts" are different claims.
- First fleet sweep (2026-09-04): clean through 25k, knee between 25k
  and 50k, against sequencer-fix's 400k on the same path. Ruled out by
  measurement: snapshots (none fired), an unbounded book (177,143
  matches, 3,830 live orders), the gateway (no thread over 22.5%), and
  the state machine (20 inputs over 2 ms in ~7 million). The apply
  thread is idle at the knee — `gap=79982us sm=13us` — so the ceiling
  is upstream, in the propose path. Numbers and next steps in
  `measurements.md` §3.
- `tools/journal_stats_main.cpp` exists because node RSS could not
  answer "is the book growing?", and reading it as book growth was
  wrong: the node mmaps its own journal. An instrument that answers the
  question directly beat another round of inference (§10.1).
- The exchange's FIX knee is ~25k against sequencer's 400k on the same
  fleet, and the investigation is in `measurements.md` §3. Exonerated by
  measurement: the platform (a counter control reached 399,899/s the
  same day), the input path and payload size (343k with every order
  rejected), the state machine (`apply` costs 1 µs at p50), snapshots,
  gateway CPU, and accumulated state. Nothing is CPU-saturated at the
  knee, which makes it a blocking path rather than a compute-bound one.
  Two of my own intermediate readings were wrong and are corrected
  there — node RSS was the journal mmap, and "the apply thread is idle"
  came from an already-collapsed run.
- Load generator: takers are immediate-or-cancel. A plain limit taker
  that finds its level already cleared by another session rests, and
  the cycle then leaks one order per occurrence — 3.8% per cycle on a
  five-client run, 53,127 orders resting and climbing. The invariant
  held for one session, which is why the single-session test missed it;
  the test now replays five phase-staggered sessions and was proven to
  fail on the defect before the fix was trusted. Fixing it did not move
  the knee: it was a defect in the measurement, not the ceiling.
- The ~25k knee was liquibook's cancel/replace being O(orders resting
  at that price), amplified by a load-generator band that packed the
  whole book into eleven prices. Widening it to a realistic band cut
  `apply()` from 131 µs to 1 µs at the same rate and moved the knee to
  ~125k, with p50 near 1 ms through 100k offered and the same match
  rate per record. The finding that mattered was methodological: the
  "apply costs 1 µs" measurement that exonerated the state machine had
  been taken at 10k with an almost empty book, and did not hold at
  scale — a probe armed in the wrong regime is as misleading as no
  probe (`measurements.md` §3).
- Session ids are namespaced by an operator-assigned `--gateway_id`
  (`gatewayId << 32 | connectionId`), because sequencer numbers a
  gateway's sessions from 1 inside each process while the journal is
  shared: two gateways both handed out session 1 and each delivered the
  other's execution reports to the wrong client. Both halves of the fix
  are ours, so sequencer is unchanged. Distinct ids are mandatory when
  more than one gateway tails a journal.
- The ladder is now run with `MAX_INFLIGHT` raised: the harness's
  default in-flight cap, not the exchange, produced every drop below
  the ceiling. Zero drops through 150k once raised.
- Retracted: 250k/375k/500k figures produced by hand-launching load
  generators sequentially and summing their rates. Twenty ssh launches
  take longer than the run, so the generators never overlapped; run
  concurrently the same configuration does ~108k. A rate summed across
  independently-timed clients is not a rate, and `sweep-gen.sh` now
  warns when generator start times are spread (`measurements.md` §3).
- Disk and network are ruled out at the ceiling: 24% disk utilisation
  and under 6% of the network link, with CPU 64% idle. The ceiling is a
  blocking path, not a saturated resource.
- The load-generator cycle is now maker → replace → taker → cancel, and
  every maker is terminated by the session that placed it: filled, or
  cancelled at the end of its own cycle. Depth is therefore bounded by
  the number of sessions rather than by run length — 6 live orders per
  1.5M records, against 52,000 with the previous shape. Cancel-rejects
  (~23% of messages) are the mechanism, not a fault.
- **The p99 departure at 50k is journal segment rollover, and it is a
  tuning property, not a floor.** It is not the state machine and not
  fills: braft reports propose-to-apply p99 of 630 µs and a 2.2 ms
  maximum, and no apply exceeded 20 ms. The per-second latency lines
  showed all generators spiking together at seconds 44 and 65 — a
  global event, **21 s apart**, which is exactly how long 1,048,576
  records take to accumulate at 50,000/s. Quartering
  `--journal_records_per_segment`, one variable, same fleet, back to
  back, took p99 from **320 ms to 5.3 ms** with p50 unmoved, and the
  full ladder to 75k now holds p99 under 1.7 ms with zero drops. The
  cost scales because a segment file is
  `records_per_segment × maxRecordBytes` (256 GiB sparse by default)
  plus a `records_per_segment × 16 B` index (16 MiB) built at every
  rollover. Fleet runs set the flag; upstream, the next segment should
  be prepared on a background thread so a rollover is never on the
  write path (`measurements.md` §3).
- Two earlier claims here were wrong and are retracted: that segment
  rollover was ruled out (the `SEQ_SEGMENT_OPEN_US` probe watches the
  *reader* opening a segment, not the *writer* creating one — a probe
  that fires correctly on the wrong side of the thing it names), and
  that head-of-line blocking in the gateway's ring-reader thread was
  the likely cause. The interval predicted the result; the hypothesis
  predicted nothing.
- **The three repos moved to the `opensequencer` GitHub org**
  (2026-09-05): `opensequencer/{sequencer,raft-tests,exchange}`, all
  public, transferred with `gh api -X POST .../transfer` so history,
  issues and stars carry over and the old URLs redirect. Locally the
  parent directory is renamed `~/workspace/total-order` →
  `~/workspace/opensequencer`, with a compatibility symlink left at the
  old name: the large CMake build trees record absolute paths, and the
  existing `~/workspace/{sequencer,raft-tests,exchange}` symlinks exist
  for exactly that reason. The compatibility symlink can go once every
  build tree has been reconfigured from scratch. `make check-generated`
  and `make preflight` were run after the rename to prove nothing
  depended on the old path; the schema's `description` attribute
  mentions the project name but does not reach generated code, so no
  wire layout moved.
