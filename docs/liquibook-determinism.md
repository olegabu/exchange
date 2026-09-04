# liquibook under sequencer's determinism rules

`docs/spec.md` §5 lists the rules every replicated state machine must
satisfy (`sequencer/docs/specification.md` §4.1) and, for each, what to
check in liquibook. This is the check, against the vendored copy at
`vendor/liquibook` (upstream `2427613b`, plus `patches/`). Line numbers
are the vendored files'.

Reading formed these hypotheses; `tests/determinism_test.cpp` (two
instances, adversarial inputs, byte-compare; then a journal replayed
through a third instance) is the verdict, and `make check-liquibook`
keeps the grep half of this table true across upstream bumps.

| §4.1 rule | Finding | Evidence |
|---|---|---|
| No clock reads | None. No `<chrono>`, `time(`, `clock(` anywhere in `src/book/`. Time priority is insertion order, i.e. the sequence number of the accepting input. | `grep -nE 'chrono\|time\(\|clock\(' src/book/*.h` → nothing; CI runs it |
| No floating point | None. `Price`, `Quantity`, `Cost` are `uint64_t`; the cross price is the resting order's price, the fill quantity a `min` of two integers. | `types.h:7-9`; `order_book.h` `create_trade` (`cross_price = current_tracker.ptr()->price()`, `fill_qty = min(...)`) |
| No unordered iteration into outputs | The book is `std::multimap<ComparablePrice, OrderTracker>`; iteration is price order, and `std::multimap::insert` places equal keys after existing ones (C++11 [multimap.modifiers]), so a price level is FIFO by insertion. `find_on_market` walks a level in that order. No `unordered_*` anywhere. | `order_book.h:59` (typedef), `add_order` (`bids_.insert(std::make_pair(...))`), `match_regular_order` (walks `current_orders.begin()`) |
| No pointer addresses into state | Order pointers are compared for **equality** only (`result->second.ptr() == order`), never ordered; `ComparablePrice` orders by price alone. Our orders live in `std::map` nodes, keyed by `orderId` = sequence number, so identity is by id. | `order_book.h` `find_on_market`; `comparable_price.h` |
| Bounded allocation | Per call: `callbacks_` is a `std::vector` reserved to 16, so a sweep past 16 callbacks reallocates; `DeferredMatches` is a `std::list` (AON only); `try_create_deferred_trades` builds a `std::vector<int>` (AON only). Our side: one `std::map` node per live order, freed on retirement. Accepted for v1 and measured by `bench/apply_benchmark.cpp` (spec §9.1); pools are the fix if the budget is missed. | `order_book.h:327` (`reserve(16)`), `order_book.h:65`, `try_create_deferred_trades` |
| No I/O on the apply path | Three `std::cerr` sites, none on a successful path: a one-time warning inside two **deprecated** methods we never call (`move_callbacks`, `perform_callbacks`), and the two `catch` blocks in `callback_now()` that swallow an exception thrown by a listener. Our listener (`OrderBookStateMachine`, all callbacks `noexcept`) never throws; anything impossible is recorded with `fault()` and turned into a thrown error after the book call returns, so every replica stops on the same input instead of one logging and continuing. The allowlist in `vendor/liquibook/io-allowlist.txt` pins these three lines. | `order_book.h:33,1124,1135`; `src/book/book.hpp` contract; `order_book_state_machine.cpp` `fault()` / `finishInput()` |

## Hazards found while reading, and what guards them

- **`(int)` casts on the replace path** clamped any open quantity above
  2³¹ — at 10⁻⁸ units, 22 lots. `matching_test` caught it (a 100-lot
  order replaced to 50 came back as 2³³) before the differential test
  did. Patched: `vendor/liquibook/patches/0001-…`.
- **A rejected replace can leave a callback queued for the *next*
  call.** `replace()` returns early on "order is already filled" without
  `callback_now()`, so that reject would surface during a later,
  unrelated input. Unreachable here: admission rejects
  `newQty ≤ cumQty` (`ReplaceQuantityBelowFilled`) before liquibook
  sees it, and a fully filled order is never live. Recorded so nobody
  removes that check.
- **Every replace re-queues.** liquibook erases and re-inserts the
  order for any change, so a quantity decrease loses time priority
  (most venues keep it). Deterministic, so not a §4.1 concern; a
  product decision recorded in `design.md` §7 and asserted by
  `matching_test` so the behaviour cannot drift unnoticed.
- **liquibook expects the application to apply a replace to the order
  itself** (it re-inserts under the new price but keeps locating the
  order by `order->price()`). Done in `Book::Impl::on_replace` before
  anything else runs.
- **`add()` rejects a zero quantity itself**, with a callback. Admission
  never lets one through; if it did, `onReject` faults.
- **`marketPrice_` is state.** Set on every trade, read when a market
  order meets a market order (never, here: market orders are forced
  IOC and never rest) — but it is state, so the snapshot carries it.
