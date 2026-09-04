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
#include <string>
#include <vector>

#include <sequencer/journal/writer.hpp>
#include <sequencer/temp_dir.hpp>

#include "generator.hpp"
#include "harness.hpp"
#include "replay_check.hpp"

namespace {

using namespace exchange;
using namespace exchange::test;

constexpr int kInputs = 30000;

TEST(Determinism, TwoInstancesAgreeByteForByteOnEveryOutput) {
  for (const std::uint64_t seed : {1u, 2u, 3u, 20260904u}) {
    Generator gen(seed);
    Harness a;
    Harness b;
    std::size_t fills = 0, accepts = 0, rejects = 0, cancels = 0, replaces = 0;
    for (int i = 0; i < kInputs; ++i) {
      const Encoded input = gen.input(i);
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
      const Encoded input = gen.input(i);
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
