#pragma once

// The matcher's view of an order (docs/spec.md §8, the layer rule):
// an id, a side, integer price and quantities, and the two liquibook
// conditions. Nothing about who sent it. The state machine derives
// from this to attach identity; the book only ever sees this part.

#include <cstdint>

namespace exchange::book {

using OrderId = std::uint64_t;
using Price = std::uint64_t;     // liquibook's Price; 0 is a market order
using Quantity = std::uint64_t;  // liquibook's Quantity
__extension__ typedef __int128 Notional;  // Price * Quantity does not fit 64 bits

struct Order {
  OrderId id = 0;
  bool buy = false;
  Price px = 0;
  Quantity qty = 0;         // current order quantity: initial, then as replaced
  Quantity cumQty = 0;      // filled so far
  Notional cumNotional = 0; // exact sum of px * qty over fills; avgPx derives from it
  bool aon = false;         // all-or-none
  bool ioc = false;         // immediate-or-cancel; aon && ioc is fill-or-kill

  // What liquibook was told the quantity was when the order entered
  // the book: the open quantity at insertion (the whole order for a
  // live add, the leaves for a snapshot restore), then adjusted by
  // replaces. liquibook's tracker is built from it and its accounting
  // is relative to it; ours (qty/cumQty) is absolute.
  Quantity bookQty = 0;

  // liquibook's Order concept (vendor/liquibook/src/book/order.h).
  bool is_buy() const noexcept { return buy; }
  Price price() const noexcept { return px; }
  Price stop_price() const noexcept { return 0; }
  Quantity order_qty() const noexcept { return bookQty; }
  bool all_or_none() const noexcept { return aon; }
  bool immediate_or_cancel() const noexcept { return ioc; }

  Quantity leavesQty() const noexcept { return qty - cumQty; }
  Price avgPx() const noexcept {
    return cumQty == 0 ? 0 : static_cast<Price>(cumNotional / static_cast<Notional>(cumQty));
  }
};

}  // namespace exchange::book
