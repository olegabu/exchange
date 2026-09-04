// The load generator's order flow, replayed through the state machine
// (docs/spec.md §9.1, §10.2 -- prove the instrument fires).
//
// A benchmark measures whatever its inputs actually do, so what the
// flow does is a property worth asserting: it must MATCH, and the book
// it leaves behind must stay BOUNDED however long the run is. Two
// earlier shapes failed one or the other -- see OrderShape's comment
// for both and why a loopback run, not reading, is what found them.
#include <gtest/gtest.h>

#include <string>

#include "exchange_fix_requester.hpp"
#include "harness.hpp"

namespace {

using namespace exchange;
using namespace exchange::test;

TEST(LoadGeneratorShape, TicksFormatExactly) {
  EXPECT_EQ(bench::formatTicks(10000), "100");
  EXPECT_EQ(bench::formatTicks(10025), "100.25");
  EXPECT_EQ(bench::formatTicks(10005), "100.05");
  EXPECT_EQ(bench::formatTicks(10050), "100.5");
  EXPECT_EQ(bench::formatTicks(9995), "99.95");
}

// Doubling the run must not move the peak: that is what "bounded"
// means, and a shape that merely grows slowly would pass a single
// length and fail this.
TEST(LoadGeneratorShape, DepthDoesNotGrowWithRunLength) {
  bench::OrderShape shape;
  auto peakOver = [&](int orders) {
    Harness h;
    h.addAbc(shape.symbol);
    std::size_t peak = 0;
    for (int i = 0; i < orders; ++i) {
      const bench::PlannedOrder p = bench::plan(shape, i);
      NewOrderSpec s;
      s.compId = "LOADGEN";
      s.clOrdId = std::to_string(i);
      s.symbol = shape.symbol;
      s.side = p.buy ? Side::Buy : Side::Sell;
      s.price = p.priceTicks * (kUnit / 100);
      s.quantity = lots(shape.quantityLots);
      h.apply(newOrder(s));
      peak = std::max(peak, h.sm.liveOrderCount());
    }
    return peak;
  };
  const std::size_t shortRun = peakOver(5000);
  const std::size_t longRun = peakOver(20000);
  EXPECT_EQ(shortRun, longRun) << "depth grew with run length: " << shortRun << " -> " << longRun;
}

TEST(LoadGeneratorShape, TheFlowMatchesAndLeavesABoundedBook) {
  bench::OrderShape shape;  // the defaults the load generator ships with
  Harness h;
  h.addAbc(shape.symbol);

  std::size_t matches = 0;
  std::size_t peakResting = 0;
  constexpr int kOrders = 20000;
  for (int i = 0; i < kOrders; ++i) {
    const bench::PlannedOrder p = bench::plan(shape, i);
    NewOrderSpec s;
    s.compId = "LOADGEN";
    s.clOrdId = std::to_string(i);
    s.symbol = shape.symbol;
    s.side = p.buy ? Side::Buy : Side::Sell;
    // Ticks are hundredths; the wire is 10^-8.
    s.price = p.priceTicks * (kUnit / 100);
    s.quantity = lots(shape.quantityLots);
    for (const Out& o : h.apply(newOrder(s))) {
      if (o.is("Fill")) {
        matches += o.fills.size() / 2;
      }
      EXPECT_FALSE(o.is("OrderRejected")) << "the flow must be admissible: " << int(o.reason);
    }
    peakResting = std::max(peakResting, h.sm.liveOrderCount());
  }

  // Non-vacuity: the flow actually exercises matching.
  EXPECT_GT(matches, static_cast<std::size_t>(kOrders) / 4)
      << "the benchmark's flow barely matched -- it would measure a growing book, not an exchange";
  // And depth cannot grow with the run: makers and takers alternate one
  // for one, so the pair nets out. Bounded by the band, not by kOrders
  // -- run twice as long and this number must not move.
  EXPECT_LT(peakResting, 50u) << "resting orders grew with the run: peak " << peakResting;
  EXPECT_LT(h.sm.liveOrderCount(), 50u);
}

}  // namespace
