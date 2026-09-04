#pragma once

// One instrument's limit order book: price-time priority over
// liquibook (vendor/liquibook, VENDORED.md), with the quantity
// bookkeeping liquibook leaves to the application done here so that
// callers only ever see consistent Orders.
//
// Contract with the caller:
//  - Orders are owned by the caller at stable addresses for as long as
//    they rest; the book keeps Order* and compares them by identity.
//  - Callbacks arrive synchronously inside add/cancel/replace, in
//    liquibook's order: accept, fills, then any cancel. A callback may
//    not call back into the book.
//  - Callbacks must not throw. liquibook catches and logs to stderr,
//    which on the apply path is both I/O and a silent divergence
//    (spec §5); a Listener that has something impossible to report
//    records it and lets the caller fail the input afterwards.
//
// Depends on liquibook and the standard library only.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "book/order.hpp"

namespace exchange::book {

class Listener {
 public:
  virtual ~Listener() = default;
  virtual void onAccept(Order& order, Quantity filledSoFar) noexcept = 0;
  virtual void onReject(Order& order, const char* reason) noexcept = 0;
  // `inbound` is the order being added or replaced, `resting` the one
  // it matched. Both orders' cumQty/cumNotional are already updated.
  virtual void onFill(Order& inbound, Order& resting, Quantity qty, Price px, bool inboundDone,
                      bool restingDone) noexcept = 0;
  virtual void onCancel(Order& order, Quantity openQty) noexcept = 0;
  virtual void onCancelReject(Order& order, const char* reason) noexcept = 0;
  // The order's qty and px are already updated.
  virtual void onReplace(Order& order, Quantity newQty, Price newPx) noexcept = 0;
  virtual void onReplaceReject(Order& order, const char* reason) noexcept = 0;
};

class Visitor {
 public:
  virtual ~Visitor() = default;
  // `openQty` is liquibook's own count for the order; a caller that
  // keeps its own must find them equal.
  virtual void visit(const Order& order, Quantity openQty) = 0;
};

class Book {
 public:
  explicit Book(Listener& listener);
  ~Book();
  Book(Book&&) noexcept;
  Book& operator=(Book&&) noexcept;
  Book(const Book&) = delete;
  Book& operator=(const Book&) = delete;

  // Match, then rest what remains unless IOC. Returns whether anything traded.
  bool add(Order& order);
  void cancel(Order& order);
  // newQty is the new total order quantity; newPx the new limit price.
  bool replace(Order& order, Quantity newQty, Price newPx);

  // liquibook's last trade price (state: it prices market-vs-market
  // crosses and belongs in a snapshot).
  Price marketPrice() const noexcept;
  void setMarketPrice(Price price) noexcept;

  std::size_t restingCount() const noexcept;

  // Every resting order in a defined order: bids best-first then asks
  // best-first, FIFO within a price. The order a snapshot is written
  // in, and the order a restore re-inserts in (spec §6).
  void walk(Visitor& visitor) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace exchange::book
