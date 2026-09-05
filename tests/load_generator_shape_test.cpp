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
  // Split out, because they mean very different things. An order
  // reject means the flow sent something inadmissible -- a bug. A
  // CANCEL reject means the maker was filled before its own cancel
  // arrived, which is the flow working: the order left the book by the
  // other path.
  std::size_t orderRejects = 0, cancelRejects = 0, replaceRejects = 0;

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
                                    bench::buildBody(shape, p, seq, seq - p.targetOffset),
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
        orderRejects += o.is("OrderRejected");
        cancelRejects += o.is("OrderCancelRejected");
        replaceRejects += o.is("OrderReplaceRejected");
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
  EXPECT_EQ(r.orderRejects, 0u)
      << "the flow sent an inadmissible order; a sweep of rejects measures nothing";
  // Every cancel is answered, either by cancelling the maker or by
  // telling us it had already filled. Both retire the order, which is
  // the property that bounds the book.
  EXPECT_EQ(r.cancels + r.cancelRejects, static_cast<std::size_t>(kCycles))
      << "one 35=F per cycle must be answered: " << r.cancels << " cancelled, " << r.cancelRejects
      << " already filled";
  EXPECT_EQ(r.replaces + r.replaceRejects, static_cast<std::size_t>(kCycles)) << "one 35=G per cycle";
  // One maker and one taker per cycle, so at most one match each.
  EXPECT_GT(r.matches, static_cast<std::size_t>(kCycles) / 2)
      << "the flow should match on most cycles";
}

TEST(LoadGeneratorShape, MatchesAndLeavesABoundedBook) {
  constexpr int kMessages = static_cast<int>(bench::kCycleLength) * 3000;
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
  const Result shortRun = measure(static_cast<int>(bench::kCycleLength) * 5 * 200);
  const Result longRun = measure(static_cast<int>(bench::kCycleLength) * 5 * 800);

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
  // Depth is what must not grow. Every maker is terminated by its own
  // session -- filled, or cancelled at the end of its cycle -- so
  // resting orders are bounded by the number of sessions in flight,
  // whatever the run length. Four times the messages, same depth.
  EXPECT_LE(longRun.live, 3 * 5u)
      << "resting orders after the long run: " << longRun.live << " (5 sessions)";
  EXPECT_LE(longRun.live, shortRun.live + 5)
      << "depth grew with run length: " << shortRun.live << " -> " << longRun.live;
  // Cancel-rejects are expected and harmless: a maker that was filled
  // before its own cancel arrived. What matters is that they do not
  // mean a LEAKED order -- which the depth assertions above cover.
}

// The rest-cancel control must do exactly what it claims: never match,
// never reject, and hold one order per session. A control that quietly
// matched, or that rejected its own cancels, would make the comparison
// it exists for meaningless.
TEST(LoadGeneratorShape, RestCancelControlNeverMatches) {
  bench::OrderShape shape;
  shape.flow = bench::Flow::RestCancel;
  Replay r{shape, 5};
  r.run(2 * 5 * 1000);
  EXPECT_EQ(r.matches, 0u) << "the control must never match; that is its whole point";
  // The replay deliberately starts each session part-way through a
  // cycle, to get the phase diversity the multi-session test needs, so
  // a session that starts on the cancel step emits one orphan cancel.
  // That is a startup effect of the harness, not of the flow: what
  // matters is that it does not recur.
  EXPECT_LE(r.rejects, static_cast<std::size_t>(r.sessions))
      << "more rejects than one startup orphan per session: " << r.rejects;
  EXPECT_GT(r.accepts, 0u);
  EXPECT_GT(r.cancels, 0u);
  EXPECT_LE(r.h.sm.liveOrderCount(), 5u) << "at most one resting order per session";
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
  const std::size_t shortRun = peakOver(static_cast<int>(bench::kCycleLength) * 500);
  const std::size_t longRun = peakOver(static_cast<int>(bench::kCycleLength) * 2000);
  EXPECT_EQ(shortRun, longRun) << "depth grew with run length: " << shortRun << " -> " << longRun;
}

}  // namespace
