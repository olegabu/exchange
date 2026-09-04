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
  std::size_t matches = 0, accepts = 0, cancels = 0, replaces = 0, rejects = 0, peakResting = 0;

  explicit Replay(bench::OrderShape s) : shape(std::move(s)) { h.addAbc(shape.symbol); }

  // A complete FIX 4.4 message around a body, as the gateway hands the
  // codec the raw bytes it received off the socket.
  static std::string frame(const char* msgType, const std::string& body) {
    const std::string inner = std::string("35=") + msgType +
                              "\00149=LOADGEN\00156=EXCHANGE\00134=7\00152=20260904-12:00:00\001" + body;
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
    for (int i = 0; i < messages; ++i) {
      const bench::PlannedStep p = bench::plan(shape, i);
      const std::string raw =
          frame(bench::msgTypeOf(p.action), bench::buildBody(shape, p, i, i - 1));
      sequencer::ClientRequest request;
      request.body = sequencer::Payload(reinterpret_cast<const std::byte*>(raw.data()), raw.size());
      request.sessionId = 1;
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
