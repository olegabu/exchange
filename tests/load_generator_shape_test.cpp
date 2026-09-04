// What the load generator's order flow actually does, replayed through
// the state machine (docs/spec.md §9.1; §10.2, prove the instrument
// fires).
//
// A benchmark measures whatever its inputs do, so the flow is a
// designed artefact and its properties are asserted rather than
// trusted: it must exercise all three order-entry messages, it must
// match, and the depth it leaves must not grow with run length. Two
// earlier shapes failed one or the other and were caught by a loopback
// run -- see OrderShape's comment in exchange_fix_requester.hpp.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "exchange_fix_requester.hpp"
#include "fix/fix_input_codec.hpp"
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

// Replays the exact sequence of messages the load generator would put
// on one session, decoded the way the input codec decodes them.
struct Replay {
  Harness h;
  bench::OrderShape shape;
  // How many independent client sessions share the book, interleaved
  // the way the fleet's clients are. ONE is the easy case and the one
  // that hid a real defect: with a single session a taker always finds
  // the maker that session just placed, so the flow looks balanced
  // however unbalanced it is under concurrency.
  int sessions = 1;
  std::size_t matches = 0, accepts = 0, cancels = 0, replaces = 0, rejects = 0, peakResting = 0;

  explicit Replay(bench::OrderShape s, int sessionCount = 1)
      : shape(std::move(s)), sessions(sessionCount) {
    h.addAbc(shape.symbol);
  }

  // A complete FIX 4.4 message around a body, as the gateway hands the
  // codec the raw bytes it received off the socket.
  static std::string frame(const char* msgType, const std::string& body,
                            const std::string& sender = "LOADGEN") {
    const std::string inner = std::string("35=") + msgType + "\00149=" + sender +
                              "\00156=EXCHANGE\00134=7\00152=20260904-12:00:00\001" + body;
    std::string msg = "8=FIX.4.4\0019=" + std::to_string(inner.size()) + "\001" + inner;
    unsigned sum = 0;
    for (unsigned char c : msg) {
      sum += c;
    }
    char cs[8];
    snprintf(cs, sizeof cs, "%03u", sum % 256);
    return msg + "10=" + cs + "\001";
  }

  // The generator's OWN bytes, through the real input codec, into the
  // state machine. Not an approximation of them: "the flow is
  // admissible" and "the flow encodes to FIX the codec accepts" are
  // different claims, and only this checks the second.
  void run(int messages) {
    fix::ExchangeFixInputCodec codec;
    // Round-robin across sessions, one whole message at a time: every
    // session runs its own cycle, and they interleave in the book
    // exactly as separate clients do. Each carries its own CompID, so
    // ClOrdIDs stay scoped per sender as they are on the wire.
    // Sessions start at DIFFERENT points in the cycle. Round-robin from
    // a common start puts them in lockstep -- every session placing a
    // maker at the same instant, then every session taking -- so each
    // taker still finds a fresh maker and the flow looks balanced. Real
    // sessions drift; this staggers them by three steps each, which is
    // co-prime with the seven-step cycle and so spreads them across it.
    // Without this the test passed even with resting takers, which is
    // to say it tested nothing (docs/spec.md §10.2).
    std::vector<std::int64_t> next(static_cast<std::size_t>(sessions), 0);
    for (std::size_t k = 0; k < next.size(); ++k) {
      next[k] = static_cast<std::int64_t>(k) * 3;
    }
    for (int i = 0; i < messages; ++i) {
      const std::size_t who = static_cast<std::size_t>(i % sessions);
      const std::int64_t seq = next[who]++;
      const bench::PlannedStep p = bench::plan(shape, seq);
      const std::string raw = frame(bench::msgTypeOf(p.action),
                                    bench::buildBody(shape, p, seq, seq - 1),
                                    "LOADGEN" + std::to_string(who));
      sequencer::ClientRequest request;
      request.body = sequencer::Payload(reinterpret_cast<const std::byte*>(raw.data()), raw.size());
      request.sessionId = static_cast<std::uint64_t>(who) + 1;
      auto encoded = codec.toInput(request);
      ASSERT_TRUE(encoded.ok()) << "message " << i << " was not encodable: " << encoded.error();
      Encoded input;
      input.bytes.assign(encoded.value().begin(), encoded.value().end());
      const std::vector<Out> outs = h.apply(input);
      for (const Out& o : outs) {
        matches += o.is("Fill") ? o.fills.size() / 2 : 0;
        accepts += o.is("OrderAccepted");
        cancels += o.is("OrderCancelled");
        replaces += o.is("OrderReplaced");
        rejects +=
            o.is("OrderRejected") || o.is("OrderCancelRejected") || o.is("OrderReplaceRejected");
      }
      peakResting = std::max(peakResting, h.sm.liveOrderCount());
    }
  }
};

TEST(LoadGeneratorShape, ExercisesAllThreeOrderEntryMessages) {
  constexpr int kCycles = 300;
  Replay r{bench::OrderShape{}};
  r.run(static_cast<int>(bench::kCycleLength) * kCycles);
  // Non-vacuity: a sweep that only ever sends NewOrderSingle measures
  // one of the exchange's three paths.
  EXPECT_GT(r.accepts, 0u);
  EXPECT_GT(r.cancels, 0u) << "no 35=F in the flow";
  EXPECT_GT(r.replaces, 0u) << "no 35=G in the flow";
  EXPECT_EQ(r.rejects, 0u) << "the flow must be admissible; a sweep of rejects measures nothing";
  // Two matches per cycle (steps 1 and 6), one replace (step 5).
  EXPECT_EQ(r.matches, 2u * kCycles);
  EXPECT_EQ(r.replaces, static_cast<std::size_t>(kCycles));
  // One requested cancel per cycle (step 3); liquibook also reports the
  // replace's re-insertion, so this is a lower bound.
  EXPECT_GE(r.cancels, static_cast<std::size_t>(kCycles));
}

TEST(LoadGeneratorShape, MatchesAndLeavesABoundedBook) {
  constexpr int kMessages = 7 * 3000;
  Replay r{bench::OrderShape{}};
  r.run(kMessages);
  EXPECT_GT(r.matches, static_cast<std::size_t>(kMessages) / 8)
      << "the flow barely matched -- it would measure a growing book, not an exchange";
  // The cycle nets to zero, so depth is bounded by the band rather than
  // by the message count.
  EXPECT_LT(r.peakResting, 50u) << "resting orders grew with the run: peak " << r.peakResting;
  EXPECT_LT(r.h.sm.liveOrderCount(), 50u);
}

// The invariant that actually matters, and the one a single session
// cannot check: several clients share one book, so a taker frequently
// finds the level already cleared by somebody else's taker. If takers
// could rest, each such taker would leave an order behind that only
// leaves if a later taker happens to hit it -- 3.8% of orders per cycle
// on the five-client fleet run that found this, growing linearly with
// run length until the book, the apply cost and the latency climbed
// together and the achieved rate FELL as the offered rate rose.
//
// IOC takers make the cycle balanced whoever matches whom. This asserts
// that directly: five interleaved sessions, and depth must not grow
// with the run.
TEST(LoadGeneratorShape, SeveralSessionsSharingTheBookDoNotAccumulate) {
  struct Result {
    std::size_t rejects, live, matches;
  };
  auto measure = [](int messages) {
    Replay r{bench::OrderShape{}, 5};
    r.run(messages);
    EXPECT_GT(r.matches, 0u) << "the interleaved flow must still match";
    return Result{r.rejects, r.h.sm.liveOrderCount(), r.matches};
  };
  const Result shortRun = measure(7 * 5 * 200);
  const Result longRun = measure(7 * 5 * 800);

  // The signal is REJECTS, not depth. In this deterministic replay a
  // plain limit taker still finds a maker, so the book stays small
  // either way -- but every cycle whose maker was taken by another
  // session leaves a cancel or replace with nothing to act on, and
  // those grow one-for-one with the run. Four times the messages must
  // not mean four times the rejects.
  //
  // Without IOC takers this reads 1,001 -> 4,001, exactly linear. With
  // them it is 2 -> 2. On the five-client fleet the same defect showed
  // as 97,293 cancel/replace rejects and 53,127 orders left resting and
  // still climbing.
  EXPECT_LT(longRun.rejects, shortRun.rejects * 2 + 10)
      << "rejects grew with run length: " << shortRun.rejects << " -> " << longRun.rejects
      << " -- the flow is not self-balancing across sessions";
  EXPECT_LT(longRun.live, 50u) << "resting orders after the long run: " << longRun.live;
}

// Doubling the run must not move the peak: that is what "bounded"
// means, and a shape that merely grew slowly would pass a single length
// and fail this.
TEST(LoadGeneratorShape, DepthDoesNotGrowWithRunLength) {
  auto peakOver = [](int messages) {
    Replay r{bench::OrderShape{}};
    r.run(messages);
    return r.peakResting;
  };
  const std::size_t shortRun = peakOver(7 * 500);
  const std::size_t longRun = peakOver(7 * 2000);
  EXPECT_EQ(shortRun, longRun) << "depth grew with run length: " << shortRun << " -> " << longRun;
}

}  // namespace
