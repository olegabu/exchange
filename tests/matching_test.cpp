// docs/spec.md §5, build step 3: matching semantics through apply().
// Price-time priority, partial fills, the tie case, IOC/FOK, market
// orders, cancel and replace, every admission reject, instrument
// isolation, and the one-Fill-per-input bound.
#include <gtest/gtest.h>

#include <stdexcept>

#include "harness.hpp"

namespace {

using namespace exchange;
using namespace exchange::test;

NewOrderSpec buy(std::string_view clOrdId, std::int64_t price, std::int64_t qty, std::string_view comp = "ACME") {
  NewOrderSpec s;
  s.compId = std::string(comp);
  s.clOrdId = std::string(clOrdId);
  s.side = Side::Buy;
  s.price = price;
  s.quantity = qty;
  return s;
}
NewOrderSpec sell(std::string_view clOrdId, std::int64_t price, std::int64_t qty, std::string_view comp = "ACME") {
  NewOrderSpec s = buy(clOrdId, price, qty, comp);
  s.side = Side::Sell;
  return s;
}

TEST(Instruments, AddedOnceRejectedTwice) {
  Harness h;
  auto outs = h.addAbc();
  ASSERT_EQ(outs.size(), 1u);
  EXPECT_TRUE(outs[0].is("InstrumentAdded"));
  EXPECT_EQ(outs[0].instrumentId, 1u);
  EXPECT_EQ(outs[0].symbol, "ABC");
  EXPECT_EQ(outs[0].clOrdId, "add-ABC");
  EXPECT_EQ(h.designatedCount, 1u);

  outs = h.addAbc();
  ASSERT_EQ(outs.size(), 1u);
  EXPECT_TRUE(outs[0].is("InstrumentRejected"));
  EXPECT_EQ(outs[0].reason, RejectReason::InstrumentExists);
  EXPECT_EQ(h.sm.instrumentCount(), 1u);

  outs = h.apply(addInstrument(1, "ADMIN", "x", "BAD", 0, kUnit, lots(10)));
  EXPECT_EQ(outs[0].reason, RejectReason::InvalidInstrument);
  outs = h.apply(addInstrument(1, "ADMIN", "x", "BAD", 1, kUnit, lots(10) + 1));
  EXPECT_EQ(outs[0].reason, RejectReason::InvalidInstrument) << "max qty not on lot";
  outs = h.apply(addInstrument(1, "ADMIN", "x", "BAD", 1, kUnit, lots(kMaxMatchesPerInput + 1)));
  EXPECT_EQ(outs[0].reason, RejectReason::InvalidInstrument) << "more matches than one Fill can hold";
  outs = h.apply(addInstrument(1, "ADMIN", "x", "", 1, kUnit, lots(10)));
  EXPECT_EQ(outs[0].reason, RejectReason::MalformedMessage);
  EXPECT_EQ(h.sm.instrumentCount(), 1u);
}

TEST(Matching, RestingOrderIsAcceptedAndItsIdIsTheSequenceNumber) {
  Harness h;
  h.addAbc();
  auto outs = h.apply(newOrder(buy("b1", px(10), lots(100))));
  ASSERT_EQ(outs.size(), 1u);
  EXPECT_TRUE(outs[0].is("OrderAccepted"));
  EXPECT_EQ(outs[0].orderId, h.seq);
  EXPECT_EQ(outs[0].session, 1u);
  EXPECT_EQ(outs[0].compId, "ACME");
  EXPECT_EQ(outs[0].clOrdId, "b1");
  EXPECT_EQ(outs[0].symbol, "ABC");
  EXPECT_EQ(outs[0].instrumentId, 1u);
  EXPECT_EQ(outs[0].price, px(10));
  EXPECT_EQ(outs[0].quantity, lots(100));
  EXPECT_EQ(h.sm.liveOrderCount(), 1u);
  EXPECT_EQ(h.designatedCount, 1u);
}

TEST(Matching, CrossingOrderFillsBothPartiesAtTheRestingPrice) {
  Harness h;
  h.addAbc();
  h.apply(newOrder(buy("b1", px(10), lots(100))));
  const auto bidId = h.seq;
  NewOrderSpec s = sell("s1", px(9, 50), lots(40), "OTHER");
  s.session = 2;
  s.account = "acct-2";
  auto outs = h.apply(newOrder(s));
  ASSERT_EQ(outs.size(), 2u);
  EXPECT_TRUE(outs[0].is("OrderAccepted"));
  ASSERT_TRUE(outs[1].is("Fill"));
  ASSERT_EQ(outs[1].fills.size(), 2u);
  const Exec& taker = outs[1].fills[0];
  const Exec& maker = outs[1].fills[1];
  EXPECT_TRUE(taker.aggressor);
  EXPECT_EQ(taker.session, 2u);
  EXPECT_EQ(taker.compId, "OTHER");
  EXPECT_EQ(taker.account, "acct-2");
  EXPECT_EQ(taker.clOrdId, "s1");
  EXPECT_EQ(taker.side, Side::Sell);
  EXPECT_EQ(taker.lastPx, px(10)) << "crosses at the resting price";
  EXPECT_EQ(taker.lastQty, lots(40));
  EXPECT_EQ(taker.leavesQty, 0);
  EXPECT_EQ(taker.cumQty, lots(40));
  EXPECT_EQ(taker.avgPx, px(10));
  EXPECT_EQ(taker.counterparty, bidId);
  EXPECT_FALSE(maker.aggressor);
  EXPECT_EQ(maker.session, 1u);
  EXPECT_EQ(maker.clOrdId, "b1");
  EXPECT_EQ(maker.orderId, bidId);
  EXPECT_EQ(maker.leavesQty, lots(60));
  EXPECT_EQ(maker.cumQty, lots(40));
  EXPECT_EQ(maker.counterparty, h.seq);
  EXPECT_EQ(h.sm.liveOrderCount(), 1u) << "the filled taker is retired; the bid rests with 60";
  EXPECT_EQ(h.designatedCount, 2u);
}

TEST(Matching, PriceThenTimePriority) {
  Harness h;
  h.addAbc();
  h.apply(newOrder(buy("late-best", px(10), lots(100))));   // seq 2
  const auto lateBest = h.seq;
  h.apply(newOrder(buy("worse", px(9, 99), lots(100))));    // seq 3
  h.apply(newOrder(buy("early-best", px(10), lots(100))));  // seq 4 -- same price, later
  auto outs = h.apply(newOrder(sell("s", px(9, 99), lots(250))));
  ASSERT_EQ(outs.size(), 2u);
  const auto& f = outs[1].fills;
  ASSERT_EQ(f.size(), 6u);
  EXPECT_EQ(f[1].orderId, lateBest) << "best price first";
  EXPECT_EQ(f[1].lastQty, lots(100));
  EXPECT_EQ(f[3].clOrdId, "early-best") << "then the later arrival at the same price";
  EXPECT_EQ(f[5].clOrdId, "worse") << "then the worse price";
  EXPECT_EQ(f[5].lastQty, lots(50));
  EXPECT_EQ(f[4].leavesQty, 0);
  EXPECT_EQ(f[4].cumQty, lots(250));
  // avgPx: (200 * 10.00 + 50 * 9.99) / 250 = 9.998
  EXPECT_EQ(f[4].avgPx, 999800000);
}

TEST(Matching, TiesBreakByArrivalSequenceNumber) {
  Harness h;
  h.addAbc();
  // Identical orders from identical senders: only the sequence number differs.
  for (int i = 0; i < 5; ++i) {
    h.apply(newOrder(sell("s" + std::to_string(i), px(10), lots(10))));
  }
  auto outs = h.apply(newOrder(buy("b", px(10), lots(30))));
  const auto& f = outs[1].fills;
  ASSERT_EQ(f.size(), 6u);
  EXPECT_EQ(f[1].clOrdId, "s0");
  EXPECT_EQ(f[3].clOrdId, "s1");
  EXPECT_EQ(f[5].clOrdId, "s2");
  EXPECT_LT(f[1].orderId, f[3].orderId);
  EXPECT_LT(f[3].orderId, f[5].orderId);
  EXPECT_EQ(h.sm.liveOrderCount(), 2u);
}

TEST(Matching, CancelRequestedAndTheSecondOneIsRejected) {
  Harness h;
  h.addAbc();
  h.apply(newOrder(buy("b1", px(10), lots(100))));
  const auto id = h.seq;
  auto outs = h.apply(cancelOrder(1, "ACME", "c1", "b1", "ABC", Side::Buy));
  ASSERT_EQ(outs.size(), 1u);
  EXPECT_TRUE(outs[0].is("OrderCancelled"));
  EXPECT_EQ(outs[0].clOrdId, "c1");
  EXPECT_EQ(outs[0].origClOrdId, "b1");
  EXPECT_EQ(outs[0].orderId, id);
  EXPECT_EQ(outs[0].reason, CancelReason::Requested);
  EXPECT_EQ(outs[0].quantity, lots(100));
  EXPECT_EQ(outs[0].cumQty, 0);
  EXPECT_EQ(h.sm.liveOrderCount(), 0u);

  outs = h.apply(cancelOrder(1, "ACME", "c2", "b1", "ABC", Side::Buy));
  ASSERT_EQ(outs.size(), 1u);
  EXPECT_TRUE(outs[0].is("OrderCancelRejected"));
  EXPECT_EQ(outs[0].reason, RejectReason::UnknownOrder);
  EXPECT_EQ(outs[0].clOrdId, "c2");
  EXPECT_EQ(outs[0].origClOrdId, "b1");
}

TEST(Matching, CancelChecksOwnershipSymbolAndSide) {
  Harness h;
  h.addAbc();
  h.addAbc("XYZ");
  h.apply(newOrder(buy("b1", px(10), lots(100))));
  auto outs = h.apply(cancelOrder(1, "OTHER", "c", "b1", "ABC", Side::Buy));
  EXPECT_EQ(outs[0].reason, RejectReason::UnknownOrder) << "another sender's ClOrdID is not yours";
  outs = h.apply(cancelOrder(1, "ACME", "c", "b1", "XYZ", Side::Buy));
  EXPECT_EQ(outs[0].reason, RejectReason::SymbolMismatch);
  outs = h.apply(cancelOrder(1, "ACME", "c", "b1", "ABC", Side::Sell));
  EXPECT_EQ(outs[0].reason, RejectReason::SideMismatch);
  EXPECT_EQ(h.sm.liveOrderCount(), 1u);
}

TEST(Matching, ImmediateOrCancelRemainderIsCancelled) {
  Harness h;
  h.addAbc();
  h.apply(newOrder(buy("b1", px(10), lots(40))));
  NewOrderSpec s = sell("ioc", px(10), lots(100));
  s.tif = TimeInForce::ImmediateOrCancel;
  auto outs = h.apply(newOrder(s));
  ASSERT_EQ(outs.size(), 3u);
  EXPECT_TRUE(outs[0].is("OrderAccepted"));
  EXPECT_TRUE(outs[1].is("Fill"));
  EXPECT_EQ(outs[1].fills[0].lastQty, lots(40));
  EXPECT_TRUE(outs[2].is("OrderCancelled"));
  EXPECT_EQ(outs[2].reason, CancelReason::ImmediateOrCancelRemainder);
  EXPECT_EQ(outs[2].clOrdId, "ioc");
  EXPECT_EQ(outs[2].origClOrdId, "ioc");
  EXPECT_EQ(outs[2].quantity, lots(100));
  EXPECT_EQ(outs[2].cumQty, lots(40));
  EXPECT_EQ(h.sm.liveOrderCount(), 0u);
}

TEST(Matching, FillOrKillIsAllOrNothing) {
  Harness h;
  h.addAbc();
  h.apply(newOrder(buy("b1", px(10), lots(40))));
  NewOrderSpec s = sell("fok", px(10), lots(100));
  s.tif = TimeInForce::FillOrKill;
  auto outs = h.apply(newOrder(s));
  ASSERT_EQ(outs.size(), 2u);
  EXPECT_TRUE(outs[0].is("OrderAccepted"));
  EXPECT_TRUE(outs[1].is("OrderCancelled"));
  EXPECT_EQ(outs[1].reason, CancelReason::FillOrKillUnfilled);
  EXPECT_EQ(h.sm.liveOrderCount(), 1u) << "the bid is untouched";

  s.clOrdId = "fok2";
  s.quantity = lots(40);
  outs = h.apply(newOrder(s));
  ASSERT_EQ(outs.size(), 2u);
  EXPECT_TRUE(outs[1].is("Fill"));
  EXPECT_EQ(outs[1].fills[0].leavesQty, 0);
  EXPECT_EQ(h.sm.liveOrderCount(), 0u);
}

TEST(Matching, MarketOrdersNeverRest) {
  Harness h;
  h.addAbc();
  h.apply(newOrder(sell("a1", px(10), lots(10))));
  h.apply(newOrder(sell("a2", px(10, 5), lots(10))));
  NewOrderSpec m = buy("mkt", 0, lots(15));
  m.ordType = OrdType::Market;
  auto outs = h.apply(newOrder(m));
  ASSERT_EQ(outs.size(), 2u);
  const auto& f = outs[1].fills;
  ASSERT_EQ(f.size(), 4u);
  EXPECT_EQ(f[0].lastPx, px(10));
  EXPECT_EQ(f[2].lastPx, px(10, 5));
  EXPECT_EQ(f[2].leavesQty, 0);
  EXPECT_EQ(h.sm.liveOrderCount(), 1u);

  m.clOrdId = "mkt2";
  m.quantity = lots(100);
  outs = h.apply(newOrder(m));
  ASSERT_EQ(outs.size(), 3u);
  EXPECT_TRUE(outs[2].is("OrderCancelled"));
  EXPECT_EQ(outs[2].reason, CancelReason::ImmediateOrCancelRemainder);
  EXPECT_EQ(outs[2].cumQty, lots(5));
  EXPECT_EQ(h.sm.liveOrderCount(), 0u);

  m.clOrdId = "mkt3";
  outs = h.apply(newOrder(m));
  ASSERT_EQ(outs.size(), 2u) << "an empty book: accepted, then cancelled";
  EXPECT_TRUE(outs[1].is("OrderCancelled"));

  m.clOrdId = "mkt4";
  m.price = px(10);
  outs = h.apply(newOrder(m));
  EXPECT_TRUE(outs[0].is("OrderRejected"));
  EXPECT_EQ(outs[0].reason, RejectReason::PriceNotOnTick) << "a market order carries no price";
}

TEST(Matching, ReplaceRenamesRequeuesAndCanCross) {
  Harness h;
  h.addAbc();
  h.apply(newOrder(buy("a", px(10), lots(100))));
  const auto aId = h.seq;
  h.apply(newOrder(buy("b", px(10), lots(100))));

  auto outs = h.apply(replaceOrder(1, "ACME", "a2", "a", "ABC", Side::Buy, lots(50), px(10)));
  ASSERT_EQ(outs.size(), 1u);
  EXPECT_TRUE(outs[0].is("OrderReplaced"));
  EXPECT_EQ(outs[0].clOrdId, "a2");
  EXPECT_EQ(outs[0].origClOrdId, "a");
  EXPECT_EQ(outs[0].orderId, aId) << "the order keeps its id";
  EXPECT_EQ(outs[0].quantity, lots(50));
  EXPECT_EQ(outs[0].leavesQty, lots(50));

  // liquibook re-queues on every replace, a quantity decrease included
  // (design.md §7): b is now ahead of a2.
  outs = h.apply(newOrder(sell("s", px(10), lots(10))));
  EXPECT_EQ(outs[1].fills[1].clOrdId, "b");

  // The old id is gone, the new one works.
  outs = h.apply(cancelOrder(1, "ACME", "c", "a", "ABC", Side::Buy));
  EXPECT_EQ(outs[0].reason, RejectReason::UnknownOrder);
  outs = h.apply(replaceOrder(1, "ACME", "a3", "a2", "ABC", Side::Buy, lots(50), px(11)));
  ASSERT_EQ(outs.size(), 1u);
  EXPECT_TRUE(outs[0].is("OrderReplaced"));
  EXPECT_EQ(outs[0].price, px(11));

  // A price change that crosses fills in the same input, after the replace.
  h.apply(newOrder(sell("s2", px(12), lots(20))));
  outs = h.apply(replaceOrder(1, "ACME", "a4", "a3", "ABC", Side::Buy, lots(50), px(12)));
  ASSERT_EQ(outs.size(), 2u);
  EXPECT_TRUE(outs[0].is("OrderReplaced"));
  ASSERT_TRUE(outs[1].is("Fill"));
  EXPECT_TRUE(outs[1].fills[0].aggressor);
  EXPECT_EQ(outs[1].fills[0].clOrdId, "a4");
  EXPECT_EQ(outs[1].fills[0].lastQty, lots(20));
  EXPECT_EQ(outs[1].fills[0].leavesQty, lots(30));
}

TEST(Matching, ReplaceRejects) {
  Harness h;
  h.addAbc();
  h.apply(newOrder(buy("a", px(10), lots(100))));
  h.apply(newOrder(buy("b", px(10), lots(100))));
  h.apply(newOrder(sell("s", px(10), lots(30))));  // fills a for 30
  auto outs = h.apply(replaceOrder(1, "ACME", "a2", "a", "ABC", Side::Buy, lots(30), px(10)));
  EXPECT_TRUE(outs[0].is("OrderReplaceRejected"));
  EXPECT_EQ(outs[0].reason, RejectReason::ReplaceQuantityBelowFilled);
  outs = h.apply(replaceOrder(1, "ACME", "b", "a", "ABC", Side::Buy, lots(50), px(10)));
  EXPECT_EQ(outs[0].reason, RejectReason::DuplicateClOrdId);
  outs = h.apply(replaceOrder(1, "ACME", "a2", "a", "ABC", Side::Buy, lots(50), px(10) + 1));
  EXPECT_EQ(outs[0].reason, RejectReason::PriceNotOnTick);
  outs = h.apply(replaceOrder(1, "ACME", "a2", "a", "ABC", Side::Buy, lots(50) + 1, px(10)));
  EXPECT_EQ(outs[0].reason, RejectReason::QuantityNotOnLot);
  outs = h.apply(replaceOrder(1, "ACME", "a2", "a", "ABC", Side::Buy, lots(1001), px(10)));
  EXPECT_EQ(outs[0].reason, RejectReason::QuantityTooLarge);
  outs = h.apply(replaceOrder(1, "ACME", "a2", "zz", "ABC", Side::Buy, lots(50), px(10)));
  EXPECT_EQ(outs[0].reason, RejectReason::UnknownOrder);
  outs = h.apply(replaceOrder(1, "ACME", "a2", "a", "ABC", Side::Sell, lots(50), px(10)));
  EXPECT_EQ(outs[0].reason, RejectReason::SideMismatch);
  EXPECT_EQ(h.sm.liveOrderCount(), 2u);
}

TEST(Admission, RejectsInADefinedOrder) {
  Harness h;
  h.addAbc();
  auto outs = h.apply(newOrder(buy("x", px(10), lots(1), "")));
  EXPECT_EQ(outs[0].reason, RejectReason::MalformedMessage) << "no sender";
  outs = h.apply(newOrder(buy("", px(10), lots(1))));
  EXPECT_EQ(outs[0].reason, RejectReason::MalformedMessage) << "no ClOrdID";
  NewOrderSpec s = buy("x", px(10), lots(1));
  s.sideRaw = 7;
  outs = h.apply(newOrder(s));
  EXPECT_EQ(outs[0].reason, RejectReason::MalformedMessage) << "unknown side";
  s = buy("x", px(10), lots(1));
  s.symbol = "NOPE";
  outs = h.apply(newOrder(s));
  EXPECT_EQ(outs[0].reason, RejectReason::UnknownInstrument);
  s = buy("x", px(10), lots(1));
  s.ordTypeRaw = 3;
  outs = h.apply(newOrder(s));
  EXPECT_EQ(outs[0].reason, RejectReason::UnsupportedOrdType);
  s = buy("x", px(10), lots(1));
  s.tifRaw = 6;
  outs = h.apply(newOrder(s));
  EXPECT_EQ(outs[0].reason, RejectReason::UnsupportedTimeInForce);
  outs = h.apply(newOrder(buy("x", px(10), lots(1) + 1)));
  EXPECT_EQ(outs[0].reason, RejectReason::QuantityNotOnLot);
  outs = h.apply(newOrder(buy("x", px(10), 0)));
  EXPECT_EQ(outs[0].reason, RejectReason::QuantityNotOnLot);
  outs = h.apply(newOrder(buy("x", px(10), -lots(1))));
  EXPECT_EQ(outs[0].reason, RejectReason::QuantityNotOnLot);
  outs = h.apply(newOrder(buy("x", px(10), lots(1001))));
  EXPECT_EQ(outs[0].reason, RejectReason::QuantityTooLarge);
  outs = h.apply(newOrder(buy("x", px(10) + 1, lots(1))));
  EXPECT_EQ(outs[0].reason, RejectReason::PriceNotOnTick);
  outs = h.apply(newOrder(buy("x", 0, lots(1))));
  EXPECT_EQ(outs[0].reason, RejectReason::PriceNotOnTick) << "a limit order needs a price";
  h.apply(newOrder(buy("dup", px(10), lots(1))));
  outs = h.apply(newOrder(buy("dup", px(10), lots(1))));
  EXPECT_EQ(outs[0].reason, RejectReason::DuplicateClOrdId);
  outs = h.apply(newOrder(buy("dup", px(10), lots(1), "OTHER")));
  EXPECT_TRUE(outs[0].is("OrderAccepted")) << "ClOrdID is per sender";
  EXPECT_EQ(h.sm.liveOrderCount(), 2u);
  // A reject echoes the request and touches nothing.
  outs = h.apply(newOrder(buy("echo", px(10, 5), lots(1001))));
  EXPECT_EQ(outs[0].clOrdId, "echo");
  EXPECT_EQ(outs[0].price, px(10, 5));
  EXPECT_EQ(outs[0].quantity, lots(1001));
  EXPECT_EQ(outs[0].symbol, "ABC");
  EXPECT_EQ(h.sm.liveOrderCount(), 2u);
}

TEST(Admission, UndecodableInputProducesNothingAndAFutureVersionStops) {
  Harness h;
  h.addAbc();
  Encoded junk;
  junk.bytes.assign(3, std::byte{0x42});
  EXPECT_TRUE(h.apply(junk).empty());
  junk.bytes.assign(64, std::byte{0});
  EXPECT_TRUE(h.apply(junk).empty()) << "template 0 is unknown";
  Encoded future = newOrder(buy("f", px(10), lots(1)));
  MessageHeader header;
  header.wrap(reinterpret_cast<char*>(future.bytes.data()), 0, 0, future.bytes.size());
  header.version(1);
  EXPECT_THROW(h.apply(future), std::runtime_error);
  EXPECT_EQ(h.sm.liveOrderCount(), 0u);
}

TEST(Matching, InstrumentsNeverInteract) {
  Harness h;
  h.addAbc("ABC");
  h.addAbc("XYZ");
  h.apply(newOrder(buy("b", px(10), lots(10))));
  NewOrderSpec s = sell("s", px(10), lots(10));
  s.symbol = "XYZ";
  auto outs = h.apply(newOrder(s));
  ASSERT_EQ(outs.size(), 1u);
  EXPECT_TRUE(outs[0].is("OrderAccepted"));
  EXPECT_EQ(outs[0].instrumentId, 2u);
  EXPECT_EQ(h.sm.liveOrderCount(), 2u);
}

TEST(Matching, ASweepOfFiveHundredRestingOrdersIsOneFill) {
  Harness h;
  h.addAbc();
  for (int i = 0; i < 500; ++i) {
    h.apply(newOrder(sell("s" + std::to_string(i), px(10), lots(1))));
  }
  auto outs = h.apply(newOrder(buy("sweep", px(10), lots(500))));
  ASSERT_EQ(outs.size(), 2u);
  EXPECT_TRUE(outs[0].is("OrderAccepted"));
  ASSERT_TRUE(outs[1].is("Fill"));
  EXPECT_EQ(outs[1].fills.size(), 1000u);
  EXPECT_EQ(outs[1].fills[998].leavesQty, 0) << "the taker after its last match";
  EXPECT_EQ(outs[1].fills[998].cumQty, lots(500));
  EXPECT_EQ(outs[1].fills[999].clOrdId, "s499");
  EXPECT_EQ(h.sm.liveOrderCount(), 0u);
}

}  // namespace
