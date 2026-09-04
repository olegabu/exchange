#pragma once

// Adversarial input generator shared by the differential, replay and
// snapshot tests (docs/spec.md §5): fixed seeds, randomness in the
// test only, biased towards the cases where replicas diverge --
// crossing orders, exact-price ties, identical orders, cancels and
// replaces racing fills, IOC/FOK, market orders, several instruments
// and senders, and a share of invalid requests.

#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "harness.hpp"

namespace exchange::test {

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

  int instrumentCount() const { return static_cast<int>(symbols.size()); }

  Encoded instruments(int i) {
    return addInstrument(99, "ADMIN", "add" + std::to_string(i), symbols[static_cast<std::size_t>(i)],
                         kUnit / 100, kUnit, lots(1000));
  }

  // The i-th input of a run: instruments first, then traffic.
  Encoded input(int i) { return i < instrumentCount() ? instruments(i) : nextInput(); }

  Encoded nextInput() {
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

}  // namespace exchange::test
