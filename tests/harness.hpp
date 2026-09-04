#pragma once

// Test-side view of the wire: build inputs, apply them, and read the
// outputs back as plain structs, so tests assert on fields rather than
// on SBE accessors. Test code only -- allocation is fine here.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sequencer/state_machine.hpp>

#include <exchange/AddInstrument.h>
#include <exchange/CancelOrder.h>
#include <exchange/Fill.h>
#include <exchange/InstrumentAdded.h>
#include <exchange/InstrumentRejected.h>
#include <exchange/NewOrder.h>
#include <exchange/OrderAccepted.h>
#include <exchange/OrderCancelRejected.h>
#include <exchange/OrderCancelled.h>
#include <exchange/OrderRejected.h>
#include <exchange/OrderReplaceRejected.h>
#include <exchange/OrderReplaced.h>
#include <exchange/ReplaceOrder.h>

#include "state_machine/order_book_state_machine.hpp"
#include "wire/wire.hpp"

namespace exchange::test {

// 1.00000000 in wire units.
inline constexpr std::int64_t kUnit = 100000000;
// A price or quantity from whole units and hundredths, e.g. px(101, 25) == 101.25.
inline constexpr std::int64_t px(std::int64_t whole, std::int64_t hundredths = 0) {
  return whole * kUnit + hundredths * (kUnit / 100);
}
inline constexpr std::int64_t lots(std::int64_t n) { return n * kUnit; }

struct Encoded {
  std::vector<std::byte> bytes;
  sequencer::Payload payload() const { return {bytes.data(), bytes.size()}; }
};

template <class Msg>
struct Builder {
  Encoded out;
  Msg msg;
  Builder() {
    out.bytes.resize(256);
    msg = wire::encode<Msg>(out.bytes);
  }
  Encoded done() {
    out.bytes.resize(wire::encodedLength(msg));
    return out;
  }
};

inline Encoded addInstrument(std::uint64_t session, std::string_view compId, std::string_view clOrdId,
                             std::string_view symbol, std::int64_t tick, std::int64_t lot,
                             std::int64_t maxQty) {
  Builder<AddInstrument> b;
  b.msg.sessionId(session)
      .putSenderCompId(wire::fixed<16>(compId).data())
      .putAccount(wire::fixed<16>("").data())
      .putClOrdId(wire::fixed<20>(clOrdId).data())
      .putSymbol(wire::fixed<8>(symbol).data())
      .tickSize(tick)
      .lotSize(lot)
      .maxOrderQty(maxQty);
  return b.done();
}

struct NewOrderSpec {
  std::uint64_t session = 1;
  std::string compId = "ACME";
  std::string account;
  std::string clOrdId;
  std::string symbol = "ABC";
  Side::Value side = Side::Buy;
  OrdType::Value ordType = OrdType::Limit;
  TimeInForce::Value tif = TimeInForce::Day;
  std::int64_t price = 0;
  std::int64_t quantity = 0;
  // Raw overrides, for malformed-input tests.
  int sideRaw = -1, ordTypeRaw = -1, tifRaw = -1;
};

inline Encoded newOrder(const NewOrderSpec& s) {
  Builder<NewOrder> b;
  b.msg.sessionId(s.session)
      .putSenderCompId(wire::fixed<16>(s.compId).data())
      .putAccount(wire::fixed<16>(s.account).data())
      .putClOrdId(wire::fixed<20>(s.clOrdId).data())
      .putSymbol(wire::fixed<8>(s.symbol).data())
      .side(s.side)
      .ordType(s.ordType)
      .timeInForce(s.tif)
      .price(s.price)
      .quantity(s.quantity);
  // Raw enum bytes, bypassing the typed setters.
  auto* base = reinterpret_cast<std::uint8_t*>(b.out.bytes.data()) + wire::kHeaderLength;
  if (s.sideRaw >= 0) base[NewOrder::sideEncodingOffset()] = static_cast<std::uint8_t>(s.sideRaw);
  if (s.ordTypeRaw >= 0) base[NewOrder::ordTypeEncodingOffset()] = static_cast<std::uint8_t>(s.ordTypeRaw);
  if (s.tifRaw >= 0) base[NewOrder::timeInForceEncodingOffset()] = static_cast<std::uint8_t>(s.tifRaw);
  return b.done();
}

inline Encoded cancelOrder(std::uint64_t session, std::string_view compId, std::string_view clOrdId,
                           std::string_view origClOrdId, std::string_view symbol, Side::Value side) {
  Builder<CancelOrder> b;
  b.msg.sessionId(session)
      .putSenderCompId(wire::fixed<16>(compId).data())
      .putAccount(wire::fixed<16>("").data())
      .putClOrdId(wire::fixed<20>(clOrdId).data())
      .putOrigClOrdId(wire::fixed<20>(origClOrdId).data())
      .putSymbol(wire::fixed<8>(symbol).data())
      .side(side);
  return b.done();
}

inline Encoded replaceOrder(std::uint64_t session, std::string_view compId, std::string_view clOrdId,
                            std::string_view origClOrdId, std::string_view symbol, Side::Value side,
                            std::int64_t quantity, std::int64_t price) {
  Builder<ReplaceOrder> b;
  b.msg.sessionId(session)
      .putSenderCompId(wire::fixed<16>(compId).data())
      .putAccount(wire::fixed<16>("").data())
      .putClOrdId(wire::fixed<20>(clOrdId).data())
      .putOrigClOrdId(wire::fixed<20>(origClOrdId).data())
      .putSymbol(wire::fixed<8>(symbol).data())
      .side(side)
      .quantity(quantity)
      .price(price);
  return b.done();
}

// One decoded output, flattened. Fields not present in a message stay 0/empty.
struct Exec {
  std::uint64_t session = 0;
  std::string compId, account, clOrdId;
  std::uint64_t orderId = 0;
  Side::Value side = Side::NULL_VALUE;
  std::int64_t lastPx = 0, lastQty = 0, leavesQty = 0, cumQty = 0, avgPx = 0;
  std::uint64_t counterparty = 0;
  bool aggressor = false;
};

struct Out {
  std::uint16_t templateId = 0;
  std::string name;
  std::uint64_t session = 0;
  std::string compId, account, clOrdId, origClOrdId, symbol;
  std::uint64_t orderId = 0;
  std::uint32_t instrumentId = 0;
  Side::Value side = Side::NULL_VALUE;
  std::int64_t price = 0, quantity = 0, cumQty = 0, leavesQty = 0, avgPx = 0;
  std::uint8_t reason = 0;  // RejectReason or CancelReason, raw
  std::vector<Exec> fills;
  std::vector<std::byte> raw;

  bool is(std::string_view n) const { return name == n; }
};

template <class Msg>
std::string sv(const char* p, std::size_t n) {
  return std::string(wire::view(p, n));
}

inline Out decodeOutput(sequencer::Payload bytes) {
  Out o;
  o.raw.assign(bytes.begin(), bytes.end());
  const auto h = wire::peekHeader(bytes);
  if (!h) {
    o.name = "?";
    return o;
  }
  o.templateId = h->templateId;
#define EXCHANGE_TEST_IDENTITY(m)                                          \
  o.session = m->sessionId();                                              \
  o.compId = sv<void>(m->senderCompId(), 16);                              \
  o.account = sv<void>(m->account(), 16);                                  \
  o.clOrdId = sv<void>(m->clOrdId(), 20);
  switch (h->templateId) {
    case OrderAccepted::sbeTemplateId(): {
      auto m = wire::decode<OrderAccepted>(bytes);
      o.name = "OrderAccepted";
      EXCHANGE_TEST_IDENTITY(m)
      o.orderId = m->orderId();
      o.instrumentId = m->instrumentId();
      o.symbol = sv<void>(m->symbol(), 8);
      o.side = m->side();
      o.price = m->price();
      o.quantity = m->quantity();
      break;
    }
    case OrderRejected::sbeTemplateId(): {
      auto m = wire::decode<OrderRejected>(bytes);
      o.name = "OrderRejected";
      EXCHANGE_TEST_IDENTITY(m)
      o.symbol = sv<void>(m->symbol(), 8);
      o.price = m->price();
      o.quantity = m->quantity();
      o.reason = m->reasonRaw();
      break;
    }
    case Fill::sbeTemplateId(): {
      auto m = wire::decode<Fill>(bytes);
      o.name = "Fill";
      o.instrumentId = m->instrumentId();
      o.symbol = sv<void>(m->symbol(), 8);
      auto& g = m->executions();
      while (g.hasNext()) {
        auto& e = g.next();
        Exec x;
        x.session = e.sessionId();
        x.compId = sv<void>(e.senderCompId(), 16);
        x.account = sv<void>(e.account(), 16);
        x.clOrdId = sv<void>(e.clOrdId(), 20);
        x.orderId = e.orderId();
        x.side = e.side();
        x.lastPx = e.lastPx();
        x.lastQty = e.lastQty();
        x.leavesQty = e.leavesQty();
        x.cumQty = e.cumQty();
        x.avgPx = e.avgPx();
        x.counterparty = e.counterpartyOrderId();
        x.aggressor = e.aggressor() == Bool::True;
        o.fills.push_back(x);
      }
      break;
    }
    case OrderCancelled::sbeTemplateId(): {
      auto m = wire::decode<OrderCancelled>(bytes);
      o.name = "OrderCancelled";
      EXCHANGE_TEST_IDENTITY(m)
      o.origClOrdId = sv<void>(m->origClOrdId(), 20);
      o.orderId = m->orderId();
      o.instrumentId = m->instrumentId();
      o.symbol = sv<void>(m->symbol(), 8);
      o.side = m->side();
      o.reason = m->reasonRaw();
      o.price = m->price();
      o.quantity = m->quantity();
      o.cumQty = m->cumQty();
      o.avgPx = m->avgPx();
      break;
    }
    case OrderCancelRejected::sbeTemplateId(): {
      auto m = wire::decode<OrderCancelRejected>(bytes);
      o.name = "OrderCancelRejected";
      EXCHANGE_TEST_IDENTITY(m)
      o.origClOrdId = sv<void>(m->origClOrdId(), 20);
      o.orderId = m->orderId();
      o.symbol = sv<void>(m->symbol(), 8);
      o.reason = m->reasonRaw();
      break;
    }
    case OrderReplaced::sbeTemplateId(): {
      auto m = wire::decode<OrderReplaced>(bytes);
      o.name = "OrderReplaced";
      EXCHANGE_TEST_IDENTITY(m)
      o.origClOrdId = sv<void>(m->origClOrdId(), 20);
      o.orderId = m->orderId();
      o.instrumentId = m->instrumentId();
      o.symbol = sv<void>(m->symbol(), 8);
      o.side = m->side();
      o.price = m->price();
      o.quantity = m->quantity();
      o.leavesQty = m->leavesQty();
      o.cumQty = m->cumQty();
      o.avgPx = m->avgPx();
      break;
    }
    case OrderReplaceRejected::sbeTemplateId(): {
      auto m = wire::decode<OrderReplaceRejected>(bytes);
      o.name = "OrderReplaceRejected";
      EXCHANGE_TEST_IDENTITY(m)
      o.origClOrdId = sv<void>(m->origClOrdId(), 20);
      o.orderId = m->orderId();
      o.symbol = sv<void>(m->symbol(), 8);
      o.reason = m->reasonRaw();
      break;
    }
    case InstrumentAdded::sbeTemplateId(): {
      auto m = wire::decode<InstrumentAdded>(bytes);
      o.name = "InstrumentAdded";
      EXCHANGE_TEST_IDENTITY(m)
      o.instrumentId = m->instrumentId();
      o.symbol = sv<void>(m->symbol(), 8);
      break;
    }
    case InstrumentRejected::sbeTemplateId(): {
      auto m = wire::decode<InstrumentRejected>(bytes);
      o.name = "InstrumentRejected";
      EXCHANGE_TEST_IDENTITY(m)
      o.symbol = sv<void>(m->symbol(), 8);
      o.reason = m->reasonRaw();
      break;
    }
    default:
      o.name = "unknown";
  }
#undef EXCHANGE_TEST_IDENTITY
  return o;
}

// A state machine plus a sequence counter, applying one input at a time.
class Harness {
 public:
  OrderBookStateMachine sm;
  std::uint64_t seq = 0;
  sequencer::OutputCollector collector;
  std::size_t designatedCount = 0;

  std::vector<Out> apply(const Encoded& input) {
    collector.reset();
    sm.apply(++seq, input.payload(), collector);
    std::vector<Out> outs;
    for (const auto& p : collector.outputs()) {
      outs.push_back(decodeOutput(p));
    }
    designatedCount = collector.designatedOutputs().size();
    return outs;
  }

  // The usual instrument: tick 0.01, lot 1, max 1000 lots.
  std::vector<Out> addAbc(std::string_view symbol = "ABC") {
    return apply(addInstrument(99, "ADMIN", std::string("add-") + std::string(symbol), symbol, kUnit / 100,
                               kUnit, lots(1000)));
  }
};

}  // namespace exchange::test
