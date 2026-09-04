#include "book/book.hpp"

#include <book/order_book.h>

namespace exchange::book {

// liquibook delivers callbacks through protected virtuals on the book
// itself, which carry more than its OrderListener interface (the
// filled quantity on accept, both "filled" flags on a fill), so the
// wrapper is a subclass rather than a listener.
struct Book::Impl final : liquibook::book::OrderBook<Order*> {
  explicit Impl(Listener& l) : listener(l) {}

  Listener& listener;

  void on_accept(Order* const& order, liquibook::book::Quantity filled) override {
    listener.onAccept(*order, filled);
  }
  void on_reject(Order* const& order, const char* reason) override { listener.onReject(*order, reason); }
  void on_fill(Order* const& inbound, Order* const& resting, liquibook::book::Quantity qty,
               liquibook::book::Price px, bool inboundDone, bool restingDone) override {
    const Notional notional = static_cast<Notional>(px) * static_cast<Notional>(qty);
    inbound->cumQty += qty;
    inbound->cumNotional += notional;
    resting->cumQty += qty;
    resting->cumNotional += notional;
    listener.onFill(*inbound, *resting, qty, px, inboundDone, restingDone);
  }
  void on_cancel(Order* const& order, liquibook::book::Quantity openQty) override {
    listener.onCancel(*order, openQty);
  }
  void on_cancel_reject(Order* const& order, const char* reason) override {
    listener.onCancelReject(*order, reason);
  }
  // liquibook re-inserts the order under the new price but keeps
  // locating it by order->price(): the application must apply the
  // change to the order itself, here, before anything else runs.
  void on_replace(Order* const& order, liquibook::book::Quantity currentBookQty,
                  liquibook::book::Quantity newBookQty, liquibook::book::Price newPx) override {
    order->qty = order->qty + newBookQty - currentBookQty;
    order->bookQty = newBookQty;
    order->px = newPx;
    listener.onReplace(*order, order->qty, newPx);
  }
  void on_replace_reject(Order* const& order, const char* reason) override {
    listener.onReplaceReject(*order, reason);
  }
};

Book::Book(Listener& listener) : impl_(std::make_unique<Impl>(listener)) {}
Book::~Book() = default;
Book::Book(Book&&) noexcept = default;
Book& Book::operator=(Book&&) noexcept = default;

bool Book::add(Order& order) {
  liquibook::book::OrderConditions conditions = liquibook::book::oc_no_conditions;
  if (order.aon) {
    conditions |= liquibook::book::oc_all_or_none;
  }
  if (order.ioc) {
    conditions |= liquibook::book::oc_immediate_or_cancel;
  }
  return impl_->add(&order, conditions);
}

void Book::cancel(Order& order) { impl_->cancel(&order); }

bool Book::replace(Order& order, Quantity newQty, Price newPx) {
  const auto delta = static_cast<std::int64_t>(newQty) - static_cast<std::int64_t>(order.qty);
  const liquibook::book::Price price =
      newPx == order.px ? liquibook::book::PRICE_UNCHANGED : static_cast<liquibook::book::Price>(newPx);
  return impl_->replace(&order, delta, price);
}

Price Book::marketPrice() const noexcept { return impl_->market_price(); }
void Book::setMarketPrice(Price price) noexcept { impl_->set_market_price(price); }

std::size_t Book::restingCount() const noexcept { return impl_->bids().size() + impl_->asks().size(); }

void Book::walk(Visitor& visitor) const {
  for (const auto& [price, tracker] : impl_->bids()) {
    visitor.visit(*tracker.ptr(), tracker.open_qty());
  }
  for (const auto& [price, tracker] : impl_->asks()) {
    visitor.visit(*tracker.ptr(), tracker.open_qty());
  }
}

}  // namespace exchange::book
