// docs/spec.md §6, build step 4: restore at N, replay to M, byte-identical
// outputs to a run that never restarted; save→load→save is
// byte-identical; a crossed or truncated snapshot is refused.
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include <sequencer/temp_dir.hpp>

#include <exchange/InstrumentSnapshot.h>
#include <exchange/OrderSnapshot.h>
#include <exchange/SnapshotEnd.h>
#include <exchange/SnapshotHeader.h>

#include "generator.hpp"
#include "harness.hpp"
#include "state_machine/snapshot.hpp"

namespace {

using namespace exchange;
using namespace exchange::test;

std::vector<std::byte> fileBytes(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::vector<char> chars((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return {reinterpret_cast<const std::byte*>(chars.data()),
          reinterpret_cast<const std::byte*>(chars.data()) + chars.size()};
}

void save(OrderBookStateMachine& sm, const std::filesystem::path& p) {
  sequencer::SnapshotWriter writer(p);
  sm.snapshotSave(writer);
}

void load(OrderBookStateMachine& sm, const std::filesystem::path& p) {
  sequencer::SnapshotReader reader(p);
  sm.snapshotLoad(reader);
}

struct TempDir {
  std::filesystem::path path = sequencer::makeTempDir("exchange-snapshot");
  ~TempDir() { std::filesystem::remove_all(path); }
};

TEST(Snapshot, RestoreAtNThenReplayToMMatchesTheUninterruptedRun) {
  TempDir dir;
  for (const std::uint64_t seed : {11u, 12u, 13u}) {
    Generator gen(seed);
    Harness uninterrupted;
    constexpr int kN = 6000, kM = 12000;
    std::vector<Encoded> inputs;
    for (int i = 0; i < kM; ++i) {
      inputs.push_back(gen.input(i));
      // The generator's live set is fed from the uninterrupted run; the
      // restored run must produce the same outputs anyway.
      gen.observe(uninterrupted.apply(inputs.back()));
    }
    // A second uninterrupted run to N, saved there.
    Harness a;
    for (int i = 0; i < kN; ++i) {
      a.apply(inputs[static_cast<std::size_t>(i)]);
    }
    const auto snap = dir.path / ("snap-" + std::to_string(seed));
    save(a.sm, snap);
    ASSERT_GT(a.sm.liveOrderCount(), 20u) << "the book at N is not trivial";

    Harness b;
    load(b.sm, snap);
    b.seq = a.seq;
    EXPECT_EQ(b.sm.liveOrderCount(), a.sm.liveOrderCount());
    EXPECT_EQ(b.sm.instrumentCount(), a.sm.instrumentCount());
    EXPECT_EQ(b.sm.lastAppliedSequence(), a.sm.lastAppliedSequence());

    // save -> load -> save is byte-identical.
    const auto again = dir.path / ("again-" + std::to_string(seed));
    save(b.sm, again);
    EXPECT_EQ(fileBytes(snap), fileBytes(again));

    // Replay N+1..M on both; every output byte-identical to the run
    // that never stopped.
    Harness c;
    for (int i = 0; i < kN; ++i) {
      c.apply(inputs[static_cast<std::size_t>(i)]);
    }
    for (int i = kN; i < kM; ++i) {
      const auto outsB = b.apply(inputs[static_cast<std::size_t>(i)]);
      const auto outsC = c.apply(inputs[static_cast<std::size_t>(i)]);
      ASSERT_EQ(outsB.size(), outsC.size()) << "seed " << seed << " input " << i;
      for (std::size_t k = 0; k < outsB.size(); ++k) {
        ASSERT_EQ(outsB[k].raw, outsC[k].raw) << "seed " << seed << " input " << i << " output " << k;
      }
    }
    EXPECT_EQ(b.sm.liveOrderCount(), c.sm.liveOrderCount());
    const auto endB = dir.path / ("endB-" + std::to_string(seed));
    const auto endC = dir.path / ("endC-" + std::to_string(seed));
    save(b.sm, endB);
    save(c.sm, endC);
    EXPECT_EQ(fileBytes(endB), fileBytes(endC));
  }
}

TEST(Snapshot, EmptyStateRoundTrips) {
  TempDir dir;
  OrderBookStateMachine sm;
  save(sm, dir.path / "empty");
  OrderBookStateMachine restored;
  load(restored, dir.path / "empty");
  EXPECT_EQ(restored.liveOrderCount(), 0u);
  EXPECT_EQ(restored.instrumentCount(), 0u);
  save(restored, dir.path / "empty2");
  EXPECT_EQ(fileBytes(dir.path / "empty"), fileBytes(dir.path / "empty2"));
}

TEST(Snapshot, RestoredOrdersKeepPriorityAndPartialFills) {
  TempDir dir;
  Harness h;
  h.addAbc();
  h.apply(newOrder([] { NewOrderSpec s; s.clOrdId = "first"; s.price = px(10); s.quantity = lots(10); return s; }()));
  h.apply(newOrder([] { NewOrderSpec s; s.clOrdId = "second"; s.price = px(10); s.quantity = lots(10); return s; }()));
  h.apply(newOrder([] { NewOrderSpec s; s.clOrdId = "s"; s.side = Side::Sell; s.price = px(10); s.quantity = lots(4); return s; }()));
  save(h.sm, dir.path / "s");
  Harness r;
  load(r.sm, dir.path / "s");
  r.seq = h.seq;
  auto outs = r.apply(newOrder([] { NewOrderSpec s; s.clOrdId = "s2"; s.side = Side::Sell; s.price = px(10); s.quantity = lots(10); return s; }()));
  ASSERT_EQ(outs.size(), 2u);
  const auto& f = outs[1].fills;
  ASSERT_EQ(f.size(), 4u);
  EXPECT_EQ(f[1].clOrdId, "first") << "time priority survives a restore";
  EXPECT_EQ(f[1].lastQty, lots(6)) << "and so does the partial fill";
  EXPECT_EQ(f[1].cumQty, lots(10));
  EXPECT_EQ(f[1].avgPx, px(10));
  EXPECT_EQ(f[3].clOrdId, "second");
  EXPECT_EQ(f[3].lastQty, lots(4));
}

// Hand-built snapshot files, to prove the refusals.
struct SnapshotBuilder {
  std::filesystem::path path;
  std::vector<std::byte> buf = std::vector<std::byte>(512);
  sequencer::SnapshotWriter writer;
  explicit SnapshotBuilder(std::filesystem::path p) : path(std::move(p)), writer(path) {}

  template <class Msg, class Fn>
  void record(Fn&& fill) {
    auto m = wire::encode<Msg>(buf);
    fill(m);
    snapshot::writeRecord(writer, std::span<const std::byte>(buf.data(), wire::encodedLength(m)));
  }
  void header(std::uint64_t orders) {
    record<SnapshotHeader>([&](auto& m) {
      m.formatVersion(snapshot::kFormatVersion).lastAppliedSeq(5).nextInstrumentId(2).instrumentCount(1).orderCount(orders);
    });
  }
  void instrument() {
    record<InstrumentSnapshot>([&](auto& m) {
      m.instrumentId(1).putSymbol(wire::fixed<8>("ABC").data()).tickSize(kUnit / 100).lotSize(kUnit).maxOrderQty(lots(1000)).marketPrice(0);
    });
  }
  void order(std::uint64_t id, Side::Value side, std::int64_t price, std::int64_t qty) {
    record<OrderSnapshot>([&](auto& m) {
      m.orderId(id).sessionId(1).putSenderCompId(wire::fixed<16>("ACME").data()).putAccount(wire::fixed<16>("").data())
          .putClOrdId(wire::fixed<20>("o" + std::to_string(id)).data()).instrumentId(1).side(side).ordType(OrdType::Limit)
          .timeInForce(TimeInForce::Day).price(price).quantity(qty).cumQty(0).leavesQty(qty);
      m.cumNotional().lo(0).hi(0);
    });
  }
  void end(std::uint64_t orders) {
    record<SnapshotEnd>([&](auto& m) { m.orderCount(orders); });
  }
};

TEST(Snapshot, ACrossedSnapshotIsRefused) {
  TempDir dir;
  {
    SnapshotBuilder b(dir.path / "crossed");
    b.header(2);
    b.instrument();
    b.order(2, Side::Buy, px(10), lots(1));
    b.order(3, Side::Sell, px(9), lots(1));  // would match the bid on re-insertion
    b.end(2);
  }
  OrderBookStateMachine sm;
  EXPECT_THROW(load(sm, dir.path / "crossed"), std::runtime_error);
}

TEST(Snapshot, ATruncatedOrMiscountedSnapshotIsRefused) {
  TempDir dir;
  {
    SnapshotBuilder b(dir.path / "short");
    b.header(1);
    b.instrument();
    b.order(2, Side::Buy, px(10), lots(1));
    // no SnapshotEnd
  }
  OrderBookStateMachine sm;
  EXPECT_THROW(load(sm, dir.path / "short"), std::exception);
  {
    SnapshotBuilder b(dir.path / "miscount");
    b.header(2);
    b.instrument();
    b.order(2, Side::Buy, px(10), lots(1));
    b.end(2);
  }
  EXPECT_THROW(load(sm, dir.path / "miscount"), std::runtime_error);
  {
    std::ofstream out(dir.path / "garbage", std::ios::binary);
    out << "NOTASNAPSHOT....";
  }
  EXPECT_THROW(load(sm, dir.path / "garbage"), std::exception);
}

}  // namespace
