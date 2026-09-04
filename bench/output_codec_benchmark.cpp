// What the OUTPUT codec costs per journal record (docs/spec.md §9.1).
//
// It exists because the fleet knee could not be explained by anything
// measured on the node: apply() costs 1us, no thread was saturated,
// and the same flow without matching reached 343k. The output codec is
// the one component on the critical path that no probe had covered --
// it runs once per journal record, on the output gateway's single
// journal-tail thread, so its cost per record is a hard ceiling on
// records per second.
//
// Two shapes, because they are what the fleet actually compared:
//   reject  1 output  -> 1 ExecutionReport      (the 343k arm)
//   fill    2 outputs -> 3 ExecutionReports     (the 25k arm)
#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include <sequencer/journal/record_view.hpp>

#include <exchange/Fill.h>
#include <exchange/OrderAccepted.h>
#include <exchange/OrderRejected.h>

#include "fix/fix_output_codec.hpp"
#include "wire/wire.hpp"

namespace {
std::atomic<std::uint64_t> g_allocations{0};
}

void* operator new(std::size_t n) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using namespace exchange;

// Counts what the codec publishes without keeping it: the transport's
// own work is not what is being measured here.
class NullFanout : public sequencer::Fanout {
 public:
  std::size_t messages = 0;
  void toSession(sequencer::SessionId, sequencer::Bytes) override { ++messages; }
  void broadcast(const std::string&, sequencer::Bytes) override { ++messages; }
};

void put(std::vector<std::byte>& r, const void* p, std::size_t n) {
  const auto* b = static_cast<const std::byte*>(p);
  r.insert(r.end(), b, b + n);
}

// sequencer's record layout: u64 seq, u32 inputLen, input, u16 n,
// {u32 len, output}*n.
std::vector<std::byte> makeRecord(std::uint64_t seq, const std::vector<std::vector<std::byte>>& outputs) {
  std::vector<std::byte> r;
  put(r, &seq, 8);
  const std::uint32_t inLen = 0;
  put(r, &inLen, 4);
  const std::uint16_t n = static_cast<std::uint16_t>(outputs.size());
  put(r, &n, 2);
  for (const auto& o : outputs) {
    const std::uint32_t len = static_cast<std::uint32_t>(o.size());
    put(r, &len, 4);
    put(r, o.data(), o.size());
  }
  return r;
}

template <class Msg, class Fn>
std::vector<std::byte> encode(Fn&& fill) {
  std::vector<std::byte> buf(4096);
  auto m = wire::encode<Msg>(buf);
  fill(m);
  buf.resize(wire::encodedLength(m));
  return buf;
}

std::vector<std::byte> acceptedOutput() {
  return encode<OrderAccepted>([](auto& m) {
    m.sessionId(7)
        .putSenderCompId(wire::fixed<16>("LOADGEN-0").data())
        .putAccount(wire::fixed<16>("").data())
        .putClOrdId(wire::fixed<20>("1099511627776").data())
        .orderId(4242424)
        .instrumentId(1)
        .putSymbol(wire::fixed<8>("ABC").data())
        .side(Side::Buy)
        .ordType(OrdType::Limit)
        .timeInForce(TimeInForce::Day)
        .price(9995000000)
        .quantity(100000000);
  });
}

std::vector<std::byte> rejectedOutput() {
  return encode<OrderRejected>([](auto& m) {
    m.sessionId(7)
        .putSenderCompId(wire::fixed<16>("LOADGEN-0").data())
        .putAccount(wire::fixed<16>("").data())
        .putClOrdId(wire::fixed<20>("1099511627776").data())
        .putSymbol(wire::fixed<8>("NOPE").data())
        .side(Side::Buy)
        .ordType(OrdType::Limit)
        .timeInForce(TimeInForce::Day)
        .price(9995000000)
        .quantity(100000000)
        .reason(RejectReason::UnknownInstrument);
  });
}

std::vector<std::byte> fillOutput() {
  std::vector<std::byte> buf(4096);
  auto m = wire::encode<Fill>(buf);
  m.instrumentId(1).putSymbol(wire::fixed<8>("ABC").data());
  auto& g = m.executionsCount(2);
  for (int i = 0; i < 2; ++i) {
    g.next()
        .sessionId(static_cast<std::uint64_t>(7 + i))
        .putSenderCompId(wire::fixed<16>("LOADGEN-0").data())
        .putAccount(wire::fixed<16>("").data())
        .putClOrdId(wire::fixed<20>("1099511627776").data())
        .orderId(static_cast<std::uint64_t>(4242424 + i))
        .side(i == 0 ? Side::Sell : Side::Buy)
        .lastPx(9995000000)
        .lastQty(100000000)
        .leavesQty(0)
        .cumQty(100000000)
        .avgPx(9995000000)
        .counterpartyOrderId(static_cast<std::uint64_t>(4242425 - i))
        .aggressor(i == 0 ? Bool::True : Bool::False);
  }
  buf.resize(wire::encodedLength(m));
  return buf;
}

void run(benchmark::State& state, const std::vector<std::byte>& record) {
  fix::ExchangeFixOutputCodec codec;
  const sequencer::journal::RecordView view(record.data(), static_cast<std::uint32_t>(record.size()));
  NullFanout fanout;
  const auto before = g_allocations.load();
  std::uint64_t records = 0;
  for (auto _ : state) {
    codec.toOutput(view, fanout);
    ++records;
  }
  const auto allocations = g_allocations.load() - before;
  state.SetItemsProcessed(static_cast<std::int64_t>(records));
  state.counters["msgs/record"] = static_cast<double>(fanout.messages) / static_cast<double>(records);
  state.counters["allocs/record"] = static_cast<double>(allocations) / static_cast<double>(records);
  // The ceiling this cost alone implies, on the single journal-tail
  // thread the output gateway runs.
  state.counters["records/s ceiling"] =
      benchmark::Counter(static_cast<double>(records), benchmark::Counter::kIsRate);
}

// The 343k arm: one output, one ExecutionReport.
void BM_Reject(benchmark::State& state) { run(state, makeRecord(1, {rejectedOutput()})); }

// The 25k arm: an accept plus a two-party fill -> three ExecutionReports.
void BM_AcceptAndFill(benchmark::State& state) {
  run(state, makeRecord(1, {acceptedOutput(), fillOutput()}));
}

}  // namespace

BENCHMARK(BM_Reject)->Unit(benchmark::kMicrosecond);
BENCHMARK(BM_AcceptAndFill)->Unit(benchmark::kMicrosecond);
