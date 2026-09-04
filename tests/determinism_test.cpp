// docs/spec.md §5: the differential test. The same adversarial input
// sequence through two independently constructed state machines must
// produce byte-identical outputs at every step; then the same run,
// written as a journal, must survive sequencer's replay check under a
// third fresh instance (sequencer's specification.md §11).
//
// Inputs are generated from fixed seeds -- the randomness lives in the
// test, never in the state machine -- and lean on the cases where
// replicas diverge: crossing orders, exact-price ties, identical
// orders, cancels and replaces racing fills, IOC/FOK, market orders,
// several instruments and senders, and a share of invalid requests.
#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <sequencer/journal/writer.hpp>
#include <sequencer/temp_dir.hpp>

#include "harness.hpp"
#include "replay_check.hpp"

namespace {

using namespace exchange;
using namespace exchange::test;

struct Generator {
  std::mt19937_64 rng;
  std::vector<std::string> symbols{"ABC", "XYZ", "QQQ"};
  std::vector<std::string> comps{"C1", "C2", "C3", "C4", "C5"};
  // What the test believes is live per sender, maintained from the
  // outputs, so cancels and replaces mostly target real orders.
  std::map<std::string, std::set<std::string>> live;
  std::uint64_t next = 0;

  explicit Generator(std::uint64_t seed) : rng(seed) {}

  int roll(int n) { return static_cast<int>(rng() % static_cast<std::uint64_t>(n)); }
  std::string id() { return "o" + std::to_string(++next); }
  std::string liveIdFor(const std::string& comp) {
    auto& set = live[comp];
    if (set.empty() || roll(10) == 0) {
      return "missing";
    }
    auto it = set.begin();
    std::advance(it, roll(static_cast<int>(set.size())));
    return *it;
  }

  Encoded instruments(int i) {
    return addInstrument(99, "ADMIN", "add" + std::to_string(i), symbols[static_cast<std::size_t>(i)],
                         kUnit / 100, kUnit, lots(1000));
  }

  Encoded next_input() {
    const std::string comp = comps[static_cast<std::size_t>(roll(static_cast<int>(comps.size())))];
    const std::string symbol = symbols[static_cast<std::size_t>(roll(static_cast<int>(symbols.size())))];
    const int action = roll(100);
    if (action < 65) {
      NewOrderSpec s;
      s.session = static_cast<std::uint64_t>(roll(5) + 1);
      s.compId = comp;
      s.account = roll(2) ? "A" : "";
      s.clOrdId = id();
      s.symbol = symbol;
      s.side = roll(2) ? Side::Buy : Side::Sell;
      // Prices in a narrow band so most orders cross or tie.
      s.price = px(100) + (roll(7) - 3) * (kUnit / 100);
      s.quantity = lots(1 + roll(20));
      switch (roll(10)) {
        case 0:
          s.tif = TimeInForce::ImmediateOrCancel;
          break;
        case 1:
          s.tif = TimeInForce::FillOrKill;
          break;
        case 2:
          s.ordType = OrdType::Market;
          s.price = 0;
          break;
        case 3:
          s.tif = TimeInForce::GoodTillCancel;
          break;
        default:
          break;
      }
      if (roll(20) == 0) {
        // Something invalid: off-tick, off-lot, oversize, or a duplicate id.
        switch (roll(4)) {
          case 0: s.price += 1; break;
          case 1: s.quantity += 1; break;
          case 2: s.quantity = lots(5000); break;
          default: s.clOrdId = liveIdFor(comp); break;
        }
      }
      return newOrder(s);
    }
    if (action < 85) {
      const std::string target = liveIdFor(comp);
      return cancelOrder(1, comp, id(), target, symbol, roll(2) ? Side::Buy : Side::Sell);
    }
    const std::string target = liveIdFor(comp);
    return replaceOrder(1, comp, id(), target, symbol, roll(2) ? Side::Buy : Side::Sell, lots(1 + roll(30)),
                        px(100) + (roll(7) - 3) * (kUnit / 100));
  }

  // Keep the live set honest from what the state machine said.
  void observe(const std::vector<Out>& outs) {
    for (const Out& o : outs) {
      if (o.is("OrderAccepted")) {
        live[o.compId].insert(o.clOrdId);
      } else if (o.is("OrderCancelled")) {
        live[o.compId].erase(o.origClOrdId);
      } else if (o.is("OrderReplaced")) {
        live[o.compId].erase(o.origClOrdId);
        live[o.compId].insert(o.clOrdId);
      } else if (o.is("Fill")) {
        for (const Exec& e : o.fills) {
          if (e.leavesQty == 0) {
            live[e.compId].erase(e.clOrdId);
          }
        }
      }
    }
  }
};

constexpr int kInputs = 30000;

TEST(Determinism, TwoInstancesAgreeByteForByteOnEveryOutput) {
  for (const std::uint64_t seed : {1u, 2u, 3u, 20260904u}) {
    Generator gen(seed);
    Harness a;
    Harness b;
    std::vector<Encoded> inputs;
    for (int i = 0; i < 3; ++i) {
      inputs.push_back(gen.instruments(i));
    }
    std::size_t fills = 0, accepts = 0, rejects = 0, cancels = 0, replaces = 0;
    for (int i = 0; i < kInputs; ++i) {
      const Encoded input = i < 3 ? inputs[static_cast<std::size_t>(i)] : gen.next_input();
      const auto outsA = a.apply(input);
      const auto outsB = b.apply(input);
      ASSERT_EQ(outsA.size(), outsB.size()) << "seed " << seed << " input " << i;
      for (std::size_t k = 0; k < outsA.size(); ++k) {
        ASSERT_EQ(outsA[k].raw, outsB[k].raw) << "seed " << seed << " input " << i << " output " << k << " ("
                                              << outsA[k].name << ")";
      }
      ASSERT_EQ(a.designatedCount, b.designatedCount);
      for (const Out& o : outsA) {
        fills += o.is("Fill") ? o.fills.size() / 2 : 0;
        accepts += o.is("OrderAccepted");
        rejects += o.is("OrderRejected") || o.is("OrderCancelRejected") || o.is("OrderReplaceRejected");
        cancels += o.is("OrderCancelled");
        replaces += o.is("OrderReplaced");
      }
      gen.observe(outsA);
    }
    EXPECT_EQ(a.sm.liveOrderCount(), b.sm.liveOrderCount());
    // Non-vacuity (spec §10.2): the generator actually exercised every path.
    EXPECT_GT(fills, 1000u) << "seed " << seed;
    EXPECT_GT(accepts, 1000u);
    EXPECT_GT(rejects, 100u);
    EXPECT_GT(cancels, 100u);
    EXPECT_GT(replaces, 100u);
  }
}

TEST(Determinism, AJournalOfTheRunReplaysIdenticallyThroughAFreshInstance) {
  const auto dir = sequencer::makeTempDir("exchange-replay");
  {
    Generator gen(7);
    Harness a;
    sequencer::journal::JournalOptions options;
    options.recordsPerSegment = 4096;  // several segments, cheaply
    sequencer::journal::JournalWriter writer(dir / "journal", options);
    for (int i = 0; i < 10000; ++i) {
      const Encoded input = i < 3 ? gen.instruments(i) : gen.next_input();
      const auto outs = a.apply(input);
      writer.append(writer.nextSequenceNumber(), input.payload(), a.collector.outputs());
      gen.observe(outs);
    }
    writer.flush(false);
  }
  OrderBookStateMachine fresh;
  sequencer::replay::detail::ReplayConfig config;
  config.dataDir = dir;
  const auto result = sequencer::replay::detail::runReplayCheck(config, fresh);
  EXPECT_TRUE(result.ok) << result.message;
  EXPECT_EQ(result.recordsChecked, 10000u);
  EXPECT_FALSE(result.firstDivergentSequence.has_value());
  std::filesystem::remove_all(dir);
}

}  // namespace
