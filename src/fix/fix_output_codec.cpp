#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <exchange/Fill.h>
#include <exchange/InstrumentAdded.h>
#include <exchange/InstrumentRejected.h>
#include <exchange/OrderAccepted.h>
#include <exchange/OrderCancelRejected.h>
#include <exchange/OrderCancelled.h>
#include <exchange/OrderRejected.h>
#include <exchange/OrderReplaceRejected.h>
#include <exchange/OrderReplaced.h>

#include "fix/fix_output_codec.hpp"
#include "fix/fix_price.hpp"
#include "wire/wire.hpp"

namespace exchange::fix {

namespace {

// A FIX body: "35=<type>\001" first (FixOutputTransport's contract:
// it splits MsgType off and the session core supplies BeginString,
// BodyLength, CompIDs, MsgSeqNum, SendingTime and CheckSum), then the
// application fields in the order below. Byte-identical for the same
// record every time, so a resend is the original message.
class Body {
 public:
  explicit Body(std::string_view msgType) { tag(35, msgType); }
  Body& tag(int t, std::string_view v) {
    body_.append(std::to_string(t)).push_back('=');
    body_.append(v).push_back('\001');
    return *this;
  }
  Body& tag(int t, std::uint64_t v) { return tag(t, std::string_view(std::to_string(v))); }
  Body& decimal(int t, std::int64_t v) { return tag(t, formatDecimal(v).view()); }
  Body& fixed(int t, const char* field, std::size_t width) {
    const auto v = wire::view(field, width);
    return v.empty() ? *this : tag(t, v);
  }
  sequencer::Bytes bytes() const {
    return sequencer::Bytes(reinterpret_cast<const std::byte*>(body_.data()),
                            reinterpret_cast<const std::byte*>(body_.data()) + body_.size());
  }

 private:
  std::string body_;
};

std::string execId(std::uint64_t seq, std::size_t outputIndex, std::size_t entry) {
  return std::to_string(seq) + "-" + std::to_string(outputIndex) + "-" + std::to_string(entry);
}

char sideChar(std::uint8_t raw) { return raw == Side::Sell ? '2' : '1'; }
char ordTypeChar(std::uint8_t raw) { return raw == OrdType::Market ? '1' : '2'; }
char tifChar(std::uint8_t raw) {
  switch (raw) {
    case TimeInForce::GoodTillCancel: return '1';
    case TimeInForce::ImmediateOrCancel: return '3';
    case TimeInForce::FillOrKill: return '4';
    default: return '0';
  }
}

// FIX OrdRejReason (103) and a Text (58) for each admission reason.
struct RejectText {
  const char* ordRejReason;
  const char* text;
};

RejectText rejectText(std::uint8_t reason) {
  switch (reason) {
    case RejectReason::MalformedMessage: return {"99", "malformed request"};
    case RejectReason::UnknownInstrument: return {"1", "unknown symbol"};
    case RejectReason::InstrumentExists: return {"99", "instrument exists"};
    case RejectReason::InvalidInstrument: return {"99", "invalid instrument"};
    case RejectReason::PriceNotOnTick: return {"99", "price not on tick"};
    case RejectReason::QuantityNotOnLot: return {"99", "quantity not on lot"};
    case RejectReason::QuantityTooLarge: return {"3", "order exceeds limit"};
    case RejectReason::DuplicateClOrdId: return {"6", "duplicate order"};
    case RejectReason::UnknownOrder: return {"5", "unknown order"};
    case RejectReason::NotOrderOwner: return {"5", "unknown order"};
    case RejectReason::UnsupportedOrdType: return {"11", "unsupported order type"};
    case RejectReason::UnsupportedTimeInForce: return {"11", "unsupported time in force"};
    case RejectReason::SideMismatch: return {"99", "side does not match the order"};
    case RejectReason::NotAuthorized: return {"99", "not authorized"};
    case RejectReason::ReplaceQuantityBelowFilled: return {"99", "quantity at or below filled quantity"};
    case RejectReason::SymbolMismatch: return {"99", "symbol does not match the order"};
    default: return {"99", "rejected"};
  }
}

const char* cancelText(std::uint8_t reason) {
  switch (reason) {
    case CancelReason::ImmediateOrCancelRemainder: return "immediate-or-cancel remainder";
    case CancelReason::FillOrKillUnfilled: return "fill-or-kill unfilled";
    case CancelReason::Replaced: return "replaced";
    default: return nullptr;
  }
}

template <class Msg>
void identity(Body& b, Msg& m) {
  b.fixed(11, m.clOrdId(), Msg::clOrdIdLength());
}

}  // namespace

void ExchangeFixOutputCodec::toOutput(const sequencer::journal::RecordView& record, sequencer::Fanout& fanout) {
  const std::uint64_t seq = record.sequenceNumber();
  for (std::size_t i = 0; i < record.outputCount(); ++i) {
    const sequencer::Payload out = record.output(static_cast<std::uint16_t>(i));
    const auto header = wire::peekHeader(out);
    if (!header || header->schemaId != OrderAccepted::sbeSchemaId()) {
      continue;
    }
    switch (header->templateId) {
      case OrderAccepted::sbeTemplateId(): {
        auto m = wire::decode<OrderAccepted>(out);
        if (!m) break;
        Body b("8");
        b.tag(37, m->orderId());
        identity(b, *m);
        b.tag(17, execId(seq, i, 0)).tag(150, "0").tag(39, "0");
        b.fixed(1, m->account(), OrderAccepted::accountLength());
        b.fixed(55, m->symbol(), OrderAccepted::symbolLength());
        b.tag(54, std::string(1, sideChar(m->sideRaw())));
        b.tag(40, std::string(1, ordTypeChar(m->ordTypeRaw())));
        b.tag(59, std::string(1, tifChar(m->timeInForceRaw())));
        b.decimal(38, m->quantity());
        if (m->price() > 0) b.decimal(44, m->price());
        b.decimal(151, m->quantity()).decimal(14, 0).decimal(6, 0);
        fanout.toSession(m->sessionId(), b.bytes());
        break;
      }
      case OrderRejected::sbeTemplateId(): {
        auto m = wire::decode<OrderRejected>(out);
        if (!m) break;
        const RejectText rt = rejectText(m->reasonRaw());
        Body b("8");
        b.tag(37, "NONE");
        identity(b, *m);
        b.tag(17, execId(seq, i, 0)).tag(150, "8").tag(39, "8");
        b.fixed(1, m->account(), OrderRejected::accountLength());
        b.fixed(55, m->symbol(), OrderRejected::symbolLength());
        b.tag(54, std::string(1, sideChar(m->sideRaw())));
        b.tag(40, std::string(1, ordTypeChar(m->ordTypeRaw())));
        b.tag(59, std::string(1, tifChar(m->timeInForceRaw())));
        b.decimal(38, m->quantity() < 0 ? 0 : m->quantity());
        if (m->price() > 0) b.decimal(44, m->price());
        b.decimal(151, 0).decimal(14, 0).decimal(6, 0);
        b.tag(103, rt.ordRejReason).tag(58, rt.text);
        fanout.toSession(m->sessionId(), b.bytes());
        break;
      }
      case Fill::sbeTemplateId(): {
        auto m = wire::decode<Fill>(out);
        if (!m) break;
        auto& group = m->executions();
        std::size_t k = 0;
        while (group.hasNext()) {
          auto& e = group.next();
          Body b("8");
          b.tag(37, e.orderId());
          b.fixed(11, e.clOrdId(), Fill::Executions::clOrdIdLength());
          b.tag(17, execId(seq, i, k)).tag(150, "F").tag(39, e.leavesQty() == 0 ? "2" : "1");
          b.fixed(1, e.account(), Fill::Executions::accountLength());
          b.fixed(55, m->symbol(), Fill::symbolLength());
          b.tag(54, std::string(1, sideChar(e.sideRaw())));
          b.decimal(38, e.leavesQty() + e.cumQty());
          b.decimal(32, e.lastQty()).decimal(31, e.lastPx());
          b.decimal(151, e.leavesQty()).decimal(14, e.cumQty()).decimal(6, e.avgPx());
          fanout.toSession(e.sessionId(), b.bytes());
          ++k;
        }
        break;
      }
      case OrderCancelled::sbeTemplateId(): {
        auto m = wire::decode<OrderCancelled>(out);
        if (!m) break;
        Body b("8");
        b.tag(37, m->orderId());
        identity(b, *m);
        b.fixed(41, m->origClOrdId(), OrderCancelled::origClOrdIdLength());
        b.tag(17, execId(seq, i, 0)).tag(150, "4").tag(39, "4");
        b.fixed(1, m->account(), OrderCancelled::accountLength());
        b.fixed(55, m->symbol(), OrderCancelled::symbolLength());
        b.tag(54, std::string(1, sideChar(m->sideRaw())));
        b.decimal(38, m->quantity());
        if (m->price() > 0) b.decimal(44, m->price());
        b.decimal(151, 0).decimal(14, m->cumQty()).decimal(6, m->avgPx());
        if (const char* text = cancelText(m->reasonRaw())) b.tag(58, text);
        fanout.toSession(m->sessionId(), b.bytes());
        break;
      }
      case OrderReplaced::sbeTemplateId(): {
        auto m = wire::decode<OrderReplaced>(out);
        if (!m) break;
        Body b("8");
        b.tag(37, m->orderId());
        identity(b, *m);
        b.fixed(41, m->origClOrdId(), OrderReplaced::origClOrdIdLength());
        b.tag(17, execId(seq, i, 0)).tag(150, "5").tag(39, m->cumQty() == 0 ? "0" : "1");
        b.fixed(1, m->account(), OrderReplaced::accountLength());
        b.fixed(55, m->symbol(), OrderReplaced::symbolLength());
        b.tag(54, std::string(1, sideChar(m->sideRaw())));
        b.decimal(38, m->quantity()).decimal(44, m->price());
        b.decimal(151, m->leavesQty()).decimal(14, m->cumQty()).decimal(6, m->avgPx());
        fanout.toSession(m->sessionId(), b.bytes());
        break;
      }
      case OrderCancelRejected::sbeTemplateId():
      case OrderReplaceRejected::sbeTemplateId(): {
        // Both decode through the same layout; only the template id and
        // CxlRejResponseTo (434) differ.
        const bool isReplace = header->templateId == OrderReplaceRejected::sbeTemplateId();
        auto m = wire::decode<OrderCancelRejected>(isReplace ? out : out);
        std::uint64_t sessionId, orderId; std::uint8_t reason;
        const char *clOrdId, *origClOrdId;
        if (isReplace) {
          auto r = wire::decode<OrderReplaceRejected>(out);
          if (!r) break;
          sessionId = r->sessionId(); orderId = r->orderId(); reason = r->reasonRaw();
          clOrdId = r->clOrdId(); origClOrdId = r->origClOrdId();
        } else {
          if (!m) break;
          sessionId = m->sessionId(); orderId = m->orderId(); reason = m->reasonRaw();
          clOrdId = m->clOrdId(); origClOrdId = m->origClOrdId();
        }
        const RejectText rt = rejectText(reason);
        Body b("9");
        if (orderId == 0) b.tag(37, "NONE"); else b.tag(37, orderId);
        b.fixed(11, clOrdId, 20).fixed(41, origClOrdId, 20);
        b.tag(39, "8").tag(434, isReplace ? "2" : "1");
        b.tag(102, reason == RejectReason::UnknownOrder || reason == RejectReason::NotOrderOwner ? "1" : "99");
        b.tag(58, rt.text);
        fanout.toSession(sessionId, b.bytes());
        break;
      }
      default:
        // InstrumentAdded / InstrumentRejected answer the admin tool
        // over brpc; there is no FIX message for them. Snapshot records
        // never appear in the journal.
        break;
    }
  }
}

}  // namespace exchange::fix
