// docs/spec.md §9.1: what one apply() costs, and how many heap
// allocations it makes, on books of 10, 1k and 100k resting orders.
//
// Budget: apply cost per input < 1/rate -- 10 us at 100k orders/s,
// 2.5 us at 400k -- because a single pure-spin thread applies
// everything. Inputs are encoded outside the timed loop; only the
// state machine is measured. Each scenario applies a small fixed
// sequence per iteration (an order plus what puts the book back), so
// the book does not drift; `applies` counts them and the report's
// items/s is applies/s. Allocations are counted through a replaced
// operator new and reported per apply.
#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include <filesystem>

#include <sequencer/temp_dir.hpp>

#include "harness.hpp"

namespace {

std::atomic<std::uint64_t> g_allocations{0};

}  // namespace

void* operator new(std::size_t n) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n == 0 ? 1 : n)) {
    return p;
  }
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace exchange;
using namespace exchange::test;

// A book with `depth` resting orders per side: bids from 99.99 down,
// asks from 100.01 up, one lot each, ten orders per price level.
struct Bench {
  OrderBookStateMachine sm;
  sequencer::OutputCollector out;
  std::uint64_t seq = 0;

  void apply(const Encoded& e) {
    out.reset();
    sm.apply(++seq, e.payload(), out);
  }

  explicit Bench(int depth) {
    apply(addInstrument(1, "ADMIN", "add", "ABC", kUnit / 100, kUnit, lots(1000)));
    for (int i = 0; i < depth; ++i) {
      const std::int64_t tickOffset = (i / 10 + 1) * (kUnit / 100);
      NewOrderSpec b;
      b.compId = "MAKER";
      b.clOrdId = "b" + std::to_string(i);
      b.side = Side::Buy;
      b.price = px(100) - tickOffset;
      b.quantity = lots(1);
      apply(newOrder(b));
      NewOrderSpec a = b;
      a.clOrdId = "a" + std::to_string(i);
      a.side = Side::Sell;
      a.price = px(100) + tickOffset;
      apply(newOrder(a));
    }
  }
};

constexpr int kRing = 1024;

NewOrderSpec taker(const std::string& clOrdId, Side::Value side, std::int64_t price, std::int64_t qty) {
  NewOrderSpec s;
  s.compId = "TAKER";
  s.clOrdId = clOrdId;
  s.side = side;
  s.price = price;
  s.quantity = qty;
  return s;
}

void report(benchmark::State& state, std::uint64_t applies, std::uint64_t allocations) {
  state.SetItemsProcessed(static_cast<std::int64_t>(applies));
  state.counters["applies/iter"] = static_cast<double>(applies) / static_cast<double>(state.iterations());
  state.counters["allocs/apply"] = static_cast<double>(allocations) / static_cast<double>(applies);
}

// A resting order added far from the touch, then cancelled: the
// cheapest real order, and the cost of the maps.
void BM_RestAndCancel(benchmark::State& state) {
  Bench b(static_cast<int>(state.range(0)));
  std::vector<Encoded> adds, cancels;
  for (int i = 0; i < kRing; ++i) {
    const std::string id = "r" + std::to_string(i);
    adds.push_back(newOrder(taker(id, Side::Buy, px(50), lots(1))));
    cancels.push_back(cancelOrder(1, "TAKER", "c" + std::to_string(i), id, "ABC", Side::Buy));
  }
  std::uint64_t applies = 0;
  int i = 0;
  const auto before = g_allocations.load();
  for (auto _ : state) {
    b.apply(adds[static_cast<std::size_t>(i)]);
    b.apply(cancels[static_cast<std::size_t>(i)]);
    applies += 2;
    i = (i + 1) % kRing;
  }
  report(state, applies, g_allocations.load() - before);
}

// A sell that fills exactly one resting bid, then that bid replenished.
void BM_CrossOneOrder(benchmark::State& state) {
  Bench b(static_cast<int>(state.range(0)));
  std::vector<Encoded> sells, replenish;
  for (int i = 0; i < kRing; ++i) {
    sells.push_back(newOrder(taker("s" + std::to_string(i), Side::Sell, px(99, 99), lots(1))));
    NewOrderSpec m;
    m.compId = "MAKER";
    m.clOrdId = "m" + std::to_string(i);
    m.side = Side::Buy;
    m.price = px(99, 99);
    m.quantity = lots(1);
    replenish.push_back(newOrder(m));
  }
  std::uint64_t applies = 0;
  int i = 0;
  const auto before = g_allocations.load();
  for (auto _ : state) {
    b.apply(sells[static_cast<std::size_t>(i)]);
    b.apply(replenish[static_cast<std::size_t>(i)]);
    applies += 2;
    i = (i + 1) % kRing;
  }
  report(state, applies, g_allocations.load() - before);
}

// A sell sweeping ten resting bids (one full price level), then the
// level replenished: eleven applies, one of them the sweep.
void BM_SweepTenOrders(benchmark::State& state) {
  Bench b(static_cast<int>(state.range(0)));
  std::vector<Encoded> sweeps;
  std::vector<std::vector<Encoded>> replenish;
  for (int i = 0; i < kRing; ++i) {
    sweeps.push_back(newOrder(taker("w" + std::to_string(i), Side::Sell, px(99, 99), lots(10))));
    std::vector<Encoded> level;
    for (int k = 0; k < 10; ++k) {
      NewOrderSpec m;
      m.compId = "MAKER";
      m.clOrdId = "m" + std::to_string(i) + "-" + std::to_string(k);
      m.side = Side::Buy;
      m.price = px(99, 99);
      m.quantity = lots(1);
      level.push_back(newOrder(m));
    }
    replenish.push_back(std::move(level));
  }
  std::uint64_t applies = 0;
  int i = 0;
  const auto before = g_allocations.load();
  for (auto _ : state) {
    b.apply(sweeps[static_cast<std::size_t>(i)]);
    for (const Encoded& e : replenish[static_cast<std::size_t>(i)]) {
      b.apply(e);
    }
    applies += 11;
    i = (i + 1) % kRing;
  }
  report(state, applies, g_allocations.load() - before);
}

// Rest, replace (price up one tick, no cross), cancel.
void BM_Replace(benchmark::State& state) {
  Bench b(static_cast<int>(state.range(0)));
  std::vector<Encoded> adds, replaces, cancels;
  for (int i = 0; i < kRing; ++i) {
    const std::string id = "r" + std::to_string(i);
    const std::string id2 = "q" + std::to_string(i);
    adds.push_back(newOrder(taker(id, Side::Buy, px(50), lots(2))));
    replaces.push_back(replaceOrder(1, "TAKER", id2, id, "ABC", Side::Buy, lots(1), px(50, 1)));
    cancels.push_back(cancelOrder(1, "TAKER", "c" + std::to_string(i), id2, "ABC", Side::Buy));
  }
  std::uint64_t applies = 0;
  int i = 0;
  const auto before = g_allocations.load();
  for (auto _ : state) {
    b.apply(adds[static_cast<std::size_t>(i)]);
    b.apply(replaces[static_cast<std::size_t>(i)]);
    b.apply(cancels[static_cast<std::size_t>(i)]);
    applies += 3;
    i = (i + 1) % kRing;
  }
  report(state, applies, g_allocations.load() - before);
}

// An off-tick order: decode, admission, one reject output, no state.
void BM_Reject(benchmark::State& state) {
  Bench b(static_cast<int>(state.range(0)));
  const Encoded bad = newOrder(taker("x", Side::Buy, px(50) + 1, lots(1)));
  std::uint64_t applies = 0;
  const auto before = g_allocations.load();
  for (auto _ : state) {
    b.apply(bad);
    ++applies;
  }
  report(state, applies, g_allocations.load() - before);
}

// spec §9.1: a snapshot stalls the apply thread. Save at `depth`
// resting orders per side; load into a fresh instance.
void BM_SnapshotSave(benchmark::State& state) {
  Bench b(static_cast<int>(state.range(0)));
  const auto dir = sequencer::makeTempDir("exchange-bench");
  const auto path = dir / "snapshot";
  const auto before = g_allocations.load();
  std::uint64_t saves = 0;
  for (auto _ : state) {
    sequencer::SnapshotWriter writer(path);
    b.sm.snapshotSave(writer);
    ++saves;
  }
  state.counters["bytes"] = static_cast<double>(std::filesystem::file_size(path));
  report(state, saves, g_allocations.load() - before);
  std::filesystem::remove_all(dir);
}

void BM_SnapshotLoad(benchmark::State& state) {
  Bench b(static_cast<int>(state.range(0)));
  const auto dir = sequencer::makeTempDir("exchange-bench");
  const auto path = dir / "snapshot";
  {
    sequencer::SnapshotWriter writer(path);
    b.sm.snapshotSave(writer);
  }
  const auto before = g_allocations.load();
  std::uint64_t loads = 0;
  for (auto _ : state) {
    OrderBookStateMachine fresh;
    sequencer::SnapshotReader reader(path);
    fresh.snapshotLoad(reader);
    ++loads;
  }
  report(state, loads, g_allocations.load() - before);
  std::filesystem::remove_all(dir);
}

}  // namespace

BENCHMARK(BM_RestAndCancel)->Arg(10)->Arg(1000)->Arg(100000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_CrossOneOrder)->Arg(10)->Arg(1000)->Arg(100000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_SweepTenOrders)->Arg(10)->Arg(1000)->Arg(100000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Replace)->Arg(10)->Arg(1000)->Arg(100000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_Reject)->Arg(10)->Arg(100000)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_SnapshotSave)->Arg(1000)->Arg(100000)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_SnapshotLoad)->Arg(1000)->Arg(100000)->Unit(benchmark::kMillisecond);
