#pragma once

// The exchange's state machine (docs/spec.md §2, §5, §6): the one
// apply() that sequencer replicates.
//
// Structure follows sequencer's specification.md §4.2: an ADMISSION
// layer -- decode, validate, look up, and either reject (one output,
// no state touched) or hand a fully validated command to the
// EXECUTION core -- which is src/book, and which knows nothing about
// sessions, accounts or FIX. Everything identity-shaped lives here.
//
// Determinism (sequencer §4.1, binding): no clocks, no floating point,
// ordered containers only, integer ids minted from the sequence
// number, outputs built into memory this object owns and valid until
// the next apply().

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include <sequencer/state_machine.hpp>

#include <exchange/CancelReason.h>
#include <exchange/Fill.h>
#include <exchange/OrdType.h>
#include <exchange/RejectReason.h>
#include <exchange/TimeInForce.h>

#include "book/book.hpp"
#include "book/order.hpp"
#include "state_machine/instrument.hpp"
#include "state_machine/limits.hpp"

namespace exchange {

using SessionId = std::uint64_t;
using CompIdKey = std::array<char, 16>;
using AccountKey = std::array<char, 16>;
using ClOrdIdKey = std::array<char, 20>;

// A live order: the matcher's part plus who it belongs to.
struct OrderRecord : book::Order {
  SessionId session = 0;
  CompIdKey compId{};
  AccountKey account{};
  ClOrdIdKey clOrdId{};
  InstrumentId instrumentId = 0;
  SymbolKey symbol{};
  OrdType::Value ordType = OrdType::Limit;
  TimeInForce::Value timeInForce = TimeInForce::Day;
};

// FIX identity of a live order: ClOrdID is unique per sender, not globally.
struct LiveKey {
  CompIdKey compId{};
  ClOrdIdKey clOrdId{};
  friend bool operator<(const LiveKey& a, const LiveKey& b) noexcept {
    return a.compId != b.compId ? a.compId < b.compId : a.clOrdId < b.clOrdId;
  }
};

class OrderBookStateMachine : public sequencer::StateMachine, private book::Listener {
 public:
  OrderBookStateMachine();
  ~OrderBookStateMachine() override;

  void apply(std::uint64_t sequenceNumber, sequencer::Payload input,
             sequencer::OutputCollector& outputs) override;

  void snapshotSave(sequencer::SnapshotWriter& writer) override;
  void snapshotLoad(sequencer::SnapshotReader& reader) override;

  // For tests: state without decoding a journal.
  std::size_t liveOrderCount() const noexcept { return orders_.size(); }
  std::size_t instrumentCount() const noexcept { return instruments_.size(); }
  std::uint64_t lastAppliedSequence() const noexcept { return lastAppliedSeq_; }

 private:
  struct InstrumentState {
    Instrument instrument;
    book::Book book;
  };

  // What the input being applied is, for the callbacks that encode
  // its consequences.
  enum class InputKind : std::uint8_t { None, AddInstrument, NewOrder, CancelOrder, ReplaceOrder };
  struct Context {
    std::uint64_t seq = 0;
    InputKind kind = InputKind::None;
    SessionId session = 0;
    CompIdKey compId{};
    AccountKey account{};
    ClOrdIdKey clOrdId{};
    ClOrdIdKey origClOrdId{};
    InstrumentState* instrument = nullptr;
  };

  // --- admission -------------------------------------------------------
  void onAddInstrument(sequencer::Payload input);
  void onNewOrder(sequencer::Payload input);
  void onCancelOrder(sequencer::Payload input);
  void onReplaceOrder(sequencer::Payload input);

  InstrumentState* findInstrument(const SymbolKey& symbol) noexcept;
  OrderRecord* findLive(const CompIdKey& compId, const ClOrdIdKey& clOrdId) noexcept;

  // --- execution callbacks (book::Listener) ----------------------------
  void onAccept(book::Order& order, book::Quantity filledSoFar) noexcept override;
  void onReject(book::Order& order, const char* reason) noexcept override;
  void onFill(book::Order& inbound, book::Order& resting, book::Quantity qty, book::Price px,
              bool inboundDone, bool restingDone) noexcept override;
  void onCancel(book::Order& order, book::Quantity openQty) noexcept override;
  void onCancelReject(book::Order& order, const char* reason) noexcept override;
  void onReplace(book::Order& order, book::Quantity newQty, book::Price newPx) noexcept override;
  void onReplaceReject(book::Order& order, const char* reason) noexcept override;

  // --- output staging ----------------------------------------------------
  // Outputs are encoded into buffers this object owns and handed to
  // the collector, in callback order, once the book call has returned:
  // a Fill's length is only known when the last match has been added.
  struct Staged {
    const std::byte* data;
    std::size_t length;
    bool designated;
  };
  static constexpr std::size_t kSmallSlots = 8;
  static constexpr std::size_t kSmallBytes = 512;

  std::span<std::byte> nextSmallSlot() noexcept;
  void stage(const std::byte* data, std::size_t length, bool designated) noexcept;
  void addFillEntry(const OrderRecord& party, const OrderRecord& counterparty, book::Quantity qty,
                    book::Price px, bool aggressor) noexcept;
  void finalizeFill() noexcept;
  void fault(const char* what) noexcept;
  void beginInput(std::uint64_t seq) noexcept;
  void finishInput(sequencer::OutputCollector& outputs);
  void retire(book::OrderId id) noexcept;


  // --- state ------------------------------------------------------------
  std::map<SymbolKey, InstrumentId> symbolIndex_;
  std::map<InstrumentId, InstrumentState> instruments_;
  std::map<book::OrderId, OrderRecord> orders_;  // live orders; node addresses are stable
  std::map<LiveKey, book::OrderId> live_;
  InstrumentId nextInstrumentId_ = 1;
  std::uint64_t lastAppliedSeq_ = 0;

  // --- per-input scratch, reused ------------------------------------------
  Context ctx_;
  std::array<std::array<std::byte, kSmallBytes>, kSmallSlots> small_{};
  std::size_t smallUsed_ = 0;
  std::vector<std::byte> fillBuffer_;
  Fill fill_;
  Fill::Executions* fillGroup_ = nullptr;
  std::uint16_t fillEntries_ = 0;
  std::size_t fillSlot_ = 0;  // index into staged_ once a Fill is open
  bool fillOpen_ = false;
  std::vector<Staged> staged_;
  std::vector<book::OrderId> done_;
  const char* fault_ = nullptr;

  // --- snapshot ----------------------------------------------------------
  friend class SnapshotVisitor;
  void clearState() noexcept;
  bool loading_ = false;  // callbacks during snapshotLoad: accept is silent, a fill is corruption
  std::vector<std::byte> snapshotBuffer_;
};

}  // namespace exchange
