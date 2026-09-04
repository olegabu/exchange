#include "state_machine/order_book_state_machine.hpp"

#include <bit>
#include <cstring>
#include <stdexcept>

#include <exchange/AddInstrument.h>
#include <exchange/CancelOrder.h>
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
#include <exchange/Side.h>

#include "wire/wire.hpp"

namespace exchange {

namespace {

// The Fill buffer: header, root block, group dimensions, every entry
// admission allows, and slack.
constexpr std::size_t kFillBytes = wire::kHeaderLength + Fill::sbeBlockLength() + 4 +
                                   kMaxFillEntriesPerInput * Fill::Executions::sbeBlockLength() + 64;
// numInGroup inside groupSizeEncoding {u16 blockLength, u16 numInGroup}:
// written up-front by SBE, patched once the last match is known.
constexpr std::size_t kFillCountOffset = wire::kHeaderLength + Fill::sbeBlockLength() + 2;
static_assert(std::endian::native == std::endian::little, "the schema is little-endian; so is the patch");

// Placeholder snapshot until build step 4 (spec §6).
constexpr std::array<char, 8> kSnapshotMagic{'X', 'C', 'H', 'G', 'S', 'N', 'A', 'P'};
constexpr std::uint32_t kSnapshotFormatVersion = 0;

template <std::size_t N>
std::array<char, N> key(const char* field) noexcept {
  std::array<char, N> k{};
  std::memcpy(k.data(), field, N);
  return k;
}

template <std::size_t N>
bool empty(const std::array<char, N>& k) noexcept {
  return k[0] == '\0';
}

// A positive multiple of `unit`.
bool onGrid(std::int64_t value, book::Quantity unit) noexcept {
  return value > 0 && unit > 0 && static_cast<book::Quantity>(value) % unit == 0;
}

bool validSide(std::uint8_t raw) noexcept { return raw == Side::Buy || raw == Side::Sell; }
bool validOrdType(std::uint8_t raw) noexcept { return raw == OrdType::Market || raw == OrdType::Limit; }
bool validTimeInForce(std::uint8_t raw) noexcept {
  return raw == TimeInForce::Day || raw == TimeInForce::GoodTillCancel ||
         raw == TimeInForce::ImmediateOrCancel || raw == TimeInForce::FillOrKill;
}

Side::Value sideOf(const book::Order& order) noexcept { return order.buy ? Side::Buy : Side::Sell; }

}  // namespace

OrderBookStateMachine::OrderBookStateMachine() {
  fillBuffer_.resize(kFillBytes);
  staged_.reserve(kSmallSlots + 1);
  done_.reserve(kMaxFillEntriesPerInput + 2);
}

OrderBookStateMachine::~OrderBookStateMachine() = default;

// --- apply -------------------------------------------------------------

void OrderBookStateMachine::apply(std::uint64_t sequenceNumber, sequencer::Payload input,
                                  sequencer::OutputCollector& outputs) {
  beginInput(sequenceNumber);
  const auto header = wire::peekHeader(input);
  if (header && header->schemaId == NewOrder::sbeSchemaId()) {
    if (header->version > NewOrder::sbeSchemaVersion()) {
      // Written by a newer binary. Applying what this one half
      // understands would diverge from replicas that understand it
      // all; stopping is the honest outcome (design.md §3).
      throw std::runtime_error("OrderBookStateMachine: input schema version is newer than this binary");
    }
    switch (header->templateId) {
      case AddInstrument::sbeTemplateId():
        onAddInstrument(input);
        break;
      case NewOrder::sbeTemplateId():
        onNewOrder(input);
        break;
      case CancelOrder::sbeTemplateId():
        onCancelOrder(input);
        break;
      case ReplaceOrder::sbeTemplateId():
        onReplaceOrder(input);
        break;
      default:
        // An unknown message, or a malformed one: there is no
        // session to tell, and refusing to touch state is the
        // deterministic answer. Every replica sees the same bytes and
        // says the same nothing.
        break;
    }
  }
  finishInput(outputs);
}

void OrderBookStateMachine::beginInput(std::uint64_t seq) noexcept {
  ctx_ = Context{};
  ctx_.seq = seq;
  lastAppliedSeq_ = seq;
  smallUsed_ = 0;
  fillEntries_ = 0;
  fillOpen_ = false;
  fillGroup_ = nullptr;
  staged_.clear();
  done_.clear();
  fault_ = nullptr;
}

void OrderBookStateMachine::finishInput(sequencer::OutputCollector& outputs) {
  if (fault_ != nullptr) {
    // Something admission promised could not happen, happened. Every
    // replica hits this on the same input; stopping here beats
    // continuing from a state no replica can vouch for.
    throw std::runtime_error(std::string("OrderBookStateMachine: ") + fault_);
  }
  finalizeFill();
  for (std::size_t i = 0; i < staged_.size(); ++i) {
    outputs.emit(sequencer::Payload(staged_[i].data, staged_[i].length));
    if (staged_[i].designated) {
      outputs.designateOutput(i);
    }
  }
  for (const book::OrderId id : done_) {
    retire(id);
  }
}

void OrderBookStateMachine::retire(book::OrderId id) noexcept {
  const auto it = orders_.find(id);
  if (it == orders_.end()) {
    return;
  }
  live_.erase(LiveKey{it->second.compId, it->second.clOrdId});
  orders_.erase(it);
}

// --- lookups --------------------------------------------------------------

OrderBookStateMachine::InstrumentState* OrderBookStateMachine::findInstrument(const SymbolKey& symbol) noexcept {
  const auto id = symbolIndex_.find(symbol);
  if (id == symbolIndex_.end()) {
    return nullptr;
  }
  const auto it = instruments_.find(id->second);
  return it == instruments_.end() ? nullptr : &it->second;
}

OrderRecord* OrderBookStateMachine::findLive(const CompIdKey& compId, const ClOrdIdKey& clOrdId) noexcept {
  const auto id = live_.find(LiveKey{compId, clOrdId});
  if (id == live_.end()) {
    return nullptr;
  }
  const auto it = orders_.find(id->second);
  return it == orders_.end() ? nullptr : &it->second;
}

// --- admission ----------------------------------------------------------

void OrderBookStateMachine::onAddInstrument(sequencer::Payload input) {
  auto m = wire::decode<AddInstrument>(input);
  if (!m) {
    return;
  }
  ctx_.kind = InputKind::AddInstrument;
  ctx_.session = m->sessionId();
  ctx_.compId = key<16>(m->senderCompId());
  ctx_.account = key<16>(m->account());
  ctx_.clOrdId = key<20>(m->clOrdId());
  const SymbolKey symbol = key<8>(m->symbol());
  const std::int64_t tick = m->tickSize();
  const std::int64_t lot = m->lotSize();
  const std::int64_t maxQty = m->maxOrderQty();

  const auto reject = [&](RejectReason::Value reason) noexcept {
    const auto slot = nextSmallSlot();
    if (slot.empty()) {
      return;
    }
    auto out = wire::encode<InstrumentRejected>(slot);
    out.sessionId(ctx_.session)
        .putSenderCompId(ctx_.compId.data())
        .putAccount(ctx_.account.data())
        .putClOrdId(ctx_.clOrdId.data())
        .putSymbol(symbol.data())
        .reason(reason);
    stage(slot.data(), wire::encodedLength(out), true);
  };

  if (empty(symbol) || empty(ctx_.compId) || empty(ctx_.clOrdId)) {
    return reject(RejectReason::MalformedMessage);
  }
  if (tick <= 0 || lot <= 0 || maxQty <= 0 || !onGrid(maxQty, static_cast<book::Quantity>(lot))) {
    return reject(RejectReason::InvalidInstrument);
  }
  // spec §4: a sweep of maxOrderQty against one-lot resting orders must
  // fit one Fill.
  if (static_cast<book::Quantity>(maxQty) / static_cast<book::Quantity>(lot) > kMaxMatchesPerInput) {
    return reject(RejectReason::InvalidInstrument);
  }
  if (symbolIndex_.count(symbol) != 0) {
    return reject(RejectReason::InstrumentExists);
  }

  const InstrumentId id = nextInstrumentId_++;
  Instrument instrument;
  instrument.id = id;
  instrument.symbol = symbol;
  instrument.tickSize = static_cast<book::Price>(tick);
  instrument.lotSize = static_cast<book::Quantity>(lot);
  instrument.maxOrderQty = static_cast<book::Quantity>(maxQty);
  instruments_.try_emplace(id, InstrumentState{instrument, book::Book(*this)});
  symbolIndex_.emplace(symbol, id);

  const auto slot = nextSmallSlot();
  if (slot.empty()) {
    return;
  }
  auto out = wire::encode<InstrumentAdded>(slot);
  out.sessionId(ctx_.session)
      .putSenderCompId(ctx_.compId.data())
      .putAccount(ctx_.account.data())
      .putClOrdId(ctx_.clOrdId.data())
      .instrumentId(id)
      .putSymbol(symbol.data())
      .tickSize(tick)
      .lotSize(lot)
      .maxOrderQty(maxQty);
  stage(slot.data(), wire::encodedLength(out), true);
}

void OrderBookStateMachine::onNewOrder(sequencer::Payload input) {
  auto m = wire::decode<NewOrder>(input);
  if (!m) {
    return;
  }
  ctx_.kind = InputKind::NewOrder;
  ctx_.session = m->sessionId();
  ctx_.compId = key<16>(m->senderCompId());
  ctx_.account = key<16>(m->account());
  ctx_.clOrdId = key<20>(m->clOrdId());
  const SymbolKey symbol = key<8>(m->symbol());
  const std::uint8_t side = m->sideRaw();
  const std::uint8_t ordType = m->ordTypeRaw();
  const std::uint8_t tif = m->timeInForceRaw();
  const std::int64_t price = m->price();
  const std::int64_t quantity = m->quantity();

  const auto reject = [&](RejectReason::Value reason) noexcept {
    const auto slot = nextSmallSlot();
    if (slot.empty()) {
      return;
    }
    auto out = wire::encode<OrderRejected>(slot);
    out.sessionId(ctx_.session)
        .putSenderCompId(ctx_.compId.data())
        .putAccount(ctx_.account.data())
        .putClOrdId(ctx_.clOrdId.data())
        .putSymbol(symbol.data())
        .side(validSide(side) ? static_cast<Side::Value>(side) : Side::NULL_VALUE)
        .ordType(validOrdType(ordType) ? static_cast<OrdType::Value>(ordType) : OrdType::NULL_VALUE)
        .timeInForce(validTimeInForce(tif) ? static_cast<TimeInForce::Value>(tif) : TimeInForce::NULL_VALUE)
        .price(price)
        .quantity(quantity)
        .reason(reason);
    stage(slot.data(), wire::encodedLength(out), true);
  };

  // The order of these checks is part of the contract: two replicas
  // must reject for the same reason.
  if (empty(ctx_.compId) || empty(ctx_.clOrdId) || !validSide(side)) {
    return reject(RejectReason::MalformedMessage);
  }
  InstrumentState* const inst = findInstrument(symbol);
  if (inst == nullptr) {
    return reject(RejectReason::UnknownInstrument);
  }
  if (!validOrdType(ordType)) {
    return reject(RejectReason::UnsupportedOrdType);
  }
  if (!validTimeInForce(tif)) {
    return reject(RejectReason::UnsupportedTimeInForce);
  }
  const Instrument& def = inst->instrument;
  if (!onGrid(quantity, def.lotSize)) {
    return reject(RejectReason::QuantityNotOnLot);
  }
  if (static_cast<book::Quantity>(quantity) > def.maxOrderQty) {
    return reject(RejectReason::QuantityTooLarge);
  }
  if (ordType == OrdType::Limit ? !onGrid(price, def.tickSize) : price != 0) {
    return reject(RejectReason::PriceNotOnTick);
  }
  if (findLive(ctx_.compId, ctx_.clOrdId) != nullptr) {
    return reject(RejectReason::DuplicateClOrdId);
  }

  // Execution. The order's id is the sequence number that admitted it.
  const auto [it, inserted] = orders_.try_emplace(ctx_.seq);
  if (!inserted) {
    return fault("an order with this sequence number already exists");
  }
  OrderRecord& order = it->second;
  order.id = ctx_.seq;
  order.buy = side == Side::Buy;
  order.px = ordType == OrdType::Limit ? static_cast<book::Price>(price) : 0;
  order.qty = static_cast<book::Quantity>(quantity);
  order.bookQty = order.qty;
  order.aon = tif == TimeInForce::FillOrKill;
  // A market order never rests: what it cannot fill now is cancelled.
  order.ioc = tif == TimeInForce::ImmediateOrCancel || tif == TimeInForce::FillOrKill ||
              ordType == OrdType::Market;
  order.session = ctx_.session;
  order.compId = ctx_.compId;
  order.account = ctx_.account;
  order.clOrdId = ctx_.clOrdId;
  order.instrumentId = def.id;
  order.symbol = def.symbol;
  order.ordType = static_cast<OrdType::Value>(ordType);
  order.timeInForce = static_cast<TimeInForce::Value>(tif);
  live_.emplace(LiveKey{order.compId, order.clOrdId}, order.id);

  ctx_.instrument = inst;
  inst->book.add(order);
}

void OrderBookStateMachine::onCancelOrder(sequencer::Payload input) {
  auto m = wire::decode<CancelOrder>(input);
  if (!m) {
    return;
  }
  ctx_.kind = InputKind::CancelOrder;
  ctx_.session = m->sessionId();
  ctx_.compId = key<16>(m->senderCompId());
  ctx_.account = key<16>(m->account());
  ctx_.clOrdId = key<20>(m->clOrdId());
  ctx_.origClOrdId = key<20>(m->origClOrdId());
  const SymbolKey symbol = key<8>(m->symbol());
  const std::uint8_t side = m->sideRaw();

  const auto reject = [&](book::OrderId orderId, RejectReason::Value reason) noexcept {
    const auto slot = nextSmallSlot();
    if (slot.empty()) {
      return;
    }
    auto out = wire::encode<OrderCancelRejected>(slot);
    out.sessionId(ctx_.session)
        .putSenderCompId(ctx_.compId.data())
        .putAccount(ctx_.account.data())
        .putClOrdId(ctx_.clOrdId.data())
        .putOrigClOrdId(ctx_.origClOrdId.data())
        .orderId(orderId)
        .putSymbol(symbol.data())
        .reason(reason);
    stage(slot.data(), wire::encodedLength(out), true);
  };

  if (empty(ctx_.compId) || empty(ctx_.clOrdId) || empty(ctx_.origClOrdId) || !validSide(side)) {
    return reject(0, RejectReason::MalformedMessage);
  }
  OrderRecord* const order = findLive(ctx_.compId, ctx_.origClOrdId);
  if (order == nullptr) {
    return reject(0, RejectReason::UnknownOrder);
  }
  if (order->symbol != symbol) {
    return reject(order->id, RejectReason::SymbolMismatch);
  }
  if (order->buy != (side == Side::Buy)) {
    return reject(order->id, RejectReason::SideMismatch);
  }
  const auto inst = instruments_.find(order->instrumentId);
  if (inst == instruments_.end()) {
    return fault("a live order refers to an unknown instrument");
  }
  ctx_.instrument = &inst->second;
  inst->second.book.cancel(*order);
}

void OrderBookStateMachine::onReplaceOrder(sequencer::Payload input) {
  auto m = wire::decode<ReplaceOrder>(input);
  if (!m) {
    return;
  }
  ctx_.kind = InputKind::ReplaceOrder;
  ctx_.session = m->sessionId();
  ctx_.compId = key<16>(m->senderCompId());
  ctx_.account = key<16>(m->account());
  ctx_.clOrdId = key<20>(m->clOrdId());
  ctx_.origClOrdId = key<20>(m->origClOrdId());
  const SymbolKey symbol = key<8>(m->symbol());
  const std::uint8_t side = m->sideRaw();
  const std::int64_t quantity = m->quantity();
  const std::int64_t price = m->price();

  const auto reject = [&](book::OrderId orderId, RejectReason::Value reason) noexcept {
    const auto slot = nextSmallSlot();
    if (slot.empty()) {
      return;
    }
    auto out = wire::encode<OrderReplaceRejected>(slot);
    out.sessionId(ctx_.session)
        .putSenderCompId(ctx_.compId.data())
        .putAccount(ctx_.account.data())
        .putClOrdId(ctx_.clOrdId.data())
        .putOrigClOrdId(ctx_.origClOrdId.data())
        .orderId(orderId)
        .putSymbol(symbol.data())
        .reason(reason);
    stage(slot.data(), wire::encodedLength(out), true);
  };

  if (empty(ctx_.compId) || empty(ctx_.clOrdId) || empty(ctx_.origClOrdId) || !validSide(side)) {
    return reject(0, RejectReason::MalformedMessage);
  }
  OrderRecord* const order = findLive(ctx_.compId, ctx_.origClOrdId);
  if (order == nullptr) {
    return reject(0, RejectReason::UnknownOrder);
  }
  if (order->symbol != symbol) {
    return reject(order->id, RejectReason::SymbolMismatch);
  }
  if (order->buy != (side == Side::Buy)) {
    return reject(order->id, RejectReason::SideMismatch);
  }
  const auto inst = instruments_.find(order->instrumentId);
  if (inst == instruments_.end()) {
    return fault("a live order refers to an unknown instrument");
  }
  const Instrument& def = inst->second.instrument;
  if (!onGrid(quantity, def.lotSize)) {
    return reject(order->id, RejectReason::QuantityNotOnLot);
  }
  if (static_cast<book::Quantity>(quantity) > def.maxOrderQty) {
    return reject(order->id, RejectReason::QuantityTooLarge);
  }
  if (static_cast<book::Quantity>(quantity) <= order->cumQty) {
    return reject(order->id, RejectReason::ReplaceQuantityBelowFilled);
  }
  if (!onGrid(price, def.tickSize)) {
    return reject(order->id, RejectReason::PriceNotOnTick);
  }
  if (findLive(ctx_.compId, ctx_.clOrdId) != nullptr) {
    return reject(order->id, RejectReason::DuplicateClOrdId);
  }

  ctx_.instrument = &inst->second;
  inst->second.book.replace(*order, static_cast<book::Quantity>(quantity), static_cast<book::Price>(price));
}

// --- execution callbacks ------------------------------------------------

void OrderBookStateMachine::onAccept(book::Order& o, book::Quantity /*filledSoFar*/) noexcept {
  const auto& order = static_cast<const OrderRecord&>(o);
  const auto slot = nextSmallSlot();
  if (slot.empty()) {
    return;
  }
  auto out = wire::encode<OrderAccepted>(slot);
  out.sessionId(order.session)
      .putSenderCompId(order.compId.data())
      .putAccount(order.account.data())
      .putClOrdId(order.clOrdId.data())
      .orderId(order.id)
      .instrumentId(order.instrumentId)
      .putSymbol(order.symbol.data())
      .side(sideOf(order))
      .ordType(order.ordType)
      .timeInForce(order.timeInForce)
      .price(static_cast<std::int64_t>(order.px))
      .quantity(static_cast<std::int64_t>(order.qty));
  stage(slot.data(), wire::encodedLength(out), true);
}

void OrderBookStateMachine::onReject(book::Order& /*order*/, const char* /*reason*/) noexcept {
  // Admission guarantees a positive quantity, the only thing liquibook rejects.
  fault("liquibook rejected an admitted order");
}

void OrderBookStateMachine::onFill(book::Order& inbound, book::Order& resting, book::Quantity qty,
                                   book::Price px, bool inboundDone, bool restingDone) noexcept {
  const auto& taker = static_cast<const OrderRecord&>(inbound);
  const auto& maker = static_cast<const OrderRecord&>(resting);
  addFillEntry(taker, maker, qty, px, true);
  addFillEntry(maker, taker, qty, px, false);
  if (inboundDone) {
    done_.push_back(taker.id);
  }
  if (restingDone) {
    done_.push_back(maker.id);
  }
}

void OrderBookStateMachine::onCancel(book::Order& o, book::Quantity /*openQty*/) noexcept {
  const auto& order = static_cast<const OrderRecord&>(o);
  CancelReason::Value reason = CancelReason::Requested;
  ClOrdIdKey clOrdId = order.clOrdId;
  switch (ctx_.kind) {
    case InputKind::NewOrder:
      reason = order.aon && order.ioc ? CancelReason::FillOrKillUnfilled : CancelReason::ImmediateOrCancelRemainder;
      break;
    case InputKind::CancelOrder:
      reason = CancelReason::Requested;
      clOrdId = ctx_.clOrdId;
      break;
    case InputKind::ReplaceOrder:
      reason = CancelReason::Replaced;
      clOrdId = ctx_.clOrdId;
      break;
    default:
      break;
  }
  const auto slot = nextSmallSlot();
  if (slot.empty()) {
    return;
  }
  auto out = wire::encode<OrderCancelled>(slot);
  out.sessionId(order.session)
      .putSenderCompId(order.compId.data())
      .putAccount(order.account.data())
      .putClOrdId(clOrdId.data())
      .putOrigClOrdId(order.clOrdId.data())
      .orderId(order.id)
      .instrumentId(order.instrumentId)
      .putSymbol(order.symbol.data())
      .side(sideOf(order))
      .reason(reason)
      .price(static_cast<std::int64_t>(order.px))
      .quantity(static_cast<std::int64_t>(order.qty))
      .cumQty(static_cast<std::int64_t>(order.cumQty));
  stage(slot.data(), wire::encodedLength(out), true);
  done_.push_back(order.id);
}

void OrderBookStateMachine::onCancelReject(book::Order& /*order*/, const char* /*reason*/) noexcept {
  // Only live orders are cancelled; liquibook not finding one means
  // the two of us disagree about what is resting.
  fault("liquibook could not find a live order to cancel");
}

void OrderBookStateMachine::onReplace(book::Order& o, book::Quantity /*newQty*/, book::Price /*newPx*/) noexcept {
  auto& order = static_cast<OrderRecord&>(o);
  const ClOrdIdKey previous = order.clOrdId;
  live_.erase(LiveKey{order.compId, previous});
  order.clOrdId = ctx_.clOrdId;
  live_.emplace(LiveKey{order.compId, order.clOrdId}, order.id);

  const auto slot = nextSmallSlot();
  if (slot.empty()) {
    return;
  }
  auto out = wire::encode<OrderReplaced>(slot);
  out.sessionId(order.session)
      .putSenderCompId(order.compId.data())
      .putAccount(order.account.data())
      .putClOrdId(order.clOrdId.data())
      .putOrigClOrdId(previous.data())
      .orderId(order.id)
      .instrumentId(order.instrumentId)
      .putSymbol(order.symbol.data())
      .side(sideOf(order))
      .price(static_cast<std::int64_t>(order.px))
      .quantity(static_cast<std::int64_t>(order.qty))
      .leavesQty(static_cast<std::int64_t>(order.leavesQty()))
      .cumQty(static_cast<std::int64_t>(order.cumQty));
  stage(slot.data(), wire::encodedLength(out), true);
}

void OrderBookStateMachine::onReplaceReject(book::Order& /*order*/, const char* /*reason*/) noexcept {
  fault("liquibook rejected an admitted replace");
}

// --- output staging ----------------------------------------------------

std::span<std::byte> OrderBookStateMachine::nextSmallSlot() noexcept {
  if (smallUsed_ >= kSmallSlots) {
    fault("more small outputs than one input can produce");
    return {};
  }
  return small_[smallUsed_++];
}

void OrderBookStateMachine::stage(const std::byte* data, std::size_t length, bool designated) noexcept {
  staged_.push_back(Staged{data, length, designated});
}

void OrderBookStateMachine::addFillEntry(const OrderRecord& party, const OrderRecord& counterparty,
                                         book::Quantity qty, book::Price px, bool aggressor) noexcept {
  if (!fillOpen_) {
    fill_ = wire::encode<Fill>(fillBuffer_);
    fill_.instrumentId(ctx_.instrument != nullptr ? ctx_.instrument->instrument.id : party.instrumentId)
        .putSymbol(party.symbol.data());
    // SBE writes the group count up-front; it is patched in
    // finalizeFill once the last match is known.
    fillGroup_ = &fill_.executionsCount(static_cast<std::uint16_t>(kMaxFillEntriesPerInput));
    stage(fillBuffer_.data(), 0, true);
    fillSlot_ = staged_.size() - 1;
    fillOpen_ = true;
  }
  if (fillEntries_ >= kMaxFillEntriesPerInput) {
    // Admission bounds this (maxOrderQty / lotSize); reaching it means
    // the bound is wrong, not that the order is.
    fault("more fills in one input than admission allows");
    return;
  }
  auto& e = fillGroup_->next();
  e.sessionId(party.session)
      .putSenderCompId(party.compId.data())
      .putAccount(party.account.data())
      .putClOrdId(party.clOrdId.data())
      .orderId(party.id)
      .side(sideOf(party))
      .lastPx(static_cast<std::int64_t>(px))
      .lastQty(static_cast<std::int64_t>(qty))
      .leavesQty(static_cast<std::int64_t>(party.leavesQty()))
      .cumQty(static_cast<std::int64_t>(party.cumQty))
      .avgPx(static_cast<std::int64_t>(party.avgPx()))
      .counterpartyOrderId(counterparty.id)
      .aggressor(aggressor ? Bool::True : Bool::False);
  ++fillEntries_;
}

void OrderBookStateMachine::finalizeFill() noexcept {
  if (!fillOpen_) {
    return;
  }
  const std::uint16_t count = fillEntries_;
  std::memcpy(fillBuffer_.data() + kFillCountOffset, &count, sizeof(count));
  staged_[fillSlot_].length = wire::encodedLength(fill_);
  fillOpen_ = false;
}

void OrderBookStateMachine::fault(const char* what) noexcept {
  if (fault_ == nullptr) {
    fault_ = what;
  }
}

// --- snapshot (placeholder until step 4) ----------------------------------

void OrderBookStateMachine::snapshotSave(sequencer::SnapshotWriter& writer) {
  writer.write(kSnapshotMagic.data(), kSnapshotMagic.size());
  writer.write(&kSnapshotFormatVersion, sizeof(kSnapshotFormatVersion));
  const std::uint32_t reserved = 0;
  writer.write(&reserved, sizeof(reserved));
}

void OrderBookStateMachine::snapshotLoad(sequencer::SnapshotReader& reader) {
  std::array<char, 8> magic{};
  reader.read(magic.data(), magic.size());
  if (magic != kSnapshotMagic) {
    throw std::runtime_error("OrderBookStateMachine::snapshotLoad: bad magic");
  }
  std::uint32_t version = 0;
  reader.read(&version, sizeof(version));
  if (version != kSnapshotFormatVersion) {
    throw std::runtime_error("OrderBookStateMachine::snapshotLoad: unsupported format version");
  }
  std::uint32_t reserved = 0;
  reader.read(&reserved, sizeof(reserved));
}

}  // namespace exchange
