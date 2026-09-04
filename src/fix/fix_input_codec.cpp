#include <hffix.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include <exchange/CancelOrder.h>
#include <exchange/NewOrder.h>
#include <exchange/ReplaceOrder.h>

#include "fix/fix_input_codec.hpp"
#include "fix/fix_price.hpp"
#include "wire/wire.hpp"

namespace exchange::fix {

namespace {

using Result = sequencer::Result<sequencer::Bytes>;

// Every field of a complete message, by tag, without allocating.
// Header fields (49 SenderCompID among them) are in the walk.
struct Fields {
  std::string_view clOrdId, origClOrdId, symbol, side, ordType, tif, price, qty, account, sender;
  void read(const hffix::message_reader& m) {
    for (auto it = m.begin(); it != m.end(); ++it) {
      const std::string_view v(it->value().begin(), it->value().size());
      switch (it->tag()) {
        case 1: account = v; break;
        case 11: clOrdId = v; break;
        case 38: qty = v; break;
        case 40: ordType = v; break;
        case 41: origClOrdId = v; break;
        case 44: price = v; break;
        case 49: sender = v; break;
        case 54: side = v; break;
        case 55: symbol = v; break;
        case 59: tif = v; break;
        default: break;
      }
    }
  }
};

Result error(const char* what) { return Result::Error(what); }

bool fits(std::string_view v, std::size_t width) { return !v.empty() && v.size() <= width; }

bool parseSide(std::string_view v, Side::Value& out) {
  if (v == "1") { out = Side::Buy; return true; }
  if (v == "2") { out = Side::Sell; return true; }
  return false;
}

bool parseOrdType(std::string_view v, OrdType::Value& out) {
  if (v == "1") { out = OrdType::Market; return true; }
  if (v == "2") { out = OrdType::Limit; return true; }
  return false;
}

bool parseTif(std::string_view v, TimeInForce::Value& out) {
  if (v.empty() || v == "0") { out = TimeInForce::Day; return true; }
  if (v == "1") { out = TimeInForce::GoodTillCancel; return true; }
  if (v == "3") { out = TimeInForce::ImmediateOrCancel; return true; }
  if (v == "4") { out = TimeInForce::FillOrKill; return true; }
  return false;
}

template <class Msg>
Result finish(std::array<std::byte, 256>& buffer, const Msg& msg) {
  const auto n = wire::encodedLength(msg);
  return Result::Ok(sequencer::Bytes(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(n)));
}

}  // namespace

Result ExchangeFixInputCodec::toInput(const sequencer::ClientRequest& request) {
  const std::string_view raw(reinterpret_cast<const char*>(request.body.data()), request.body.size());
  hffix::message_reader reader(raw.data(), raw.size());
  if (!reader.is_complete() || !reader.is_valid()) {
    return error("not a well-formed FIX message");
  }
  const auto typeField = reader.message_type();
  if (typeField == reader.end()) {
    return error("no MsgType");
  }
  const std::string_view type(typeField->value().begin(), typeField->value().size());
  if (type != "D" && type != "F" && type != "G") {
    return error("unsupported MsgType: this session takes 35=D, 35=F and 35=G");
  }

  Fields f;
  f.read(reader);
  if (!fits(f.sender, 16)) return error("SenderCompID (49) missing or longer than 16");
  if (!fits(f.clOrdId, 20)) return error("ClOrdID (11) missing or longer than 20");
  if (!fits(f.symbol, 8)) return error("Symbol (55) missing or longer than 8");
  if (f.account.size() > 16) return error("Account (1) longer than 16");
  Side::Value side;
  if (!parseSide(f.side, side)) return error("Side (54) must be 1 or 2");

  std::array<std::byte, 256> buffer{};
  if (type == "D") {
    OrdType::Value ordType;
    if (!parseOrdType(f.ordType, ordType)) return error("OrdType (40) must be 1 (Market) or 2 (Limit)");
    TimeInForce::Value tif;
    if (!parseTif(f.tif, tif)) return error("TimeInForce (59) must be 0, 1, 3 or 4");
    const auto qty = parseDecimal(f.qty);
    if (!qty) return error("OrderQty (38) missing or not a decimal");
    std::int64_t price = 0;
    if (ordType == OrdType::Limit) {
      const auto p = parseDecimal(f.price);
      if (!p) return error("Price (44) missing or not a decimal");
      price = *p;
    } else if (!f.price.empty()) {
      return error("a Market order carries no Price (44)");
    }
    auto m = wire::encode<NewOrder>(buffer);
    m.sessionId(request.sessionId)
        .putSenderCompId(wire::fixed<16>(f.sender).data())
        .putAccount(wire::fixed<16>(f.account).data())
        .putClOrdId(wire::fixed<20>(f.clOrdId).data())
        .putSymbol(wire::fixed<8>(f.symbol).data())
        .side(side)
        .ordType(ordType)
        .timeInForce(tif)
        .price(price)
        .quantity(*qty);
    return finish(buffer, m);
  }

  if (!fits(f.origClOrdId, 20)) return error("OrigClOrdID (41) missing or longer than 20");
  if (type == "F") {
    auto m = wire::encode<CancelOrder>(buffer);
    m.sessionId(request.sessionId)
        .putSenderCompId(wire::fixed<16>(f.sender).data())
        .putAccount(wire::fixed<16>(f.account).data())
        .putClOrdId(wire::fixed<20>(f.clOrdId).data())
        .putOrigClOrdId(wire::fixed<20>(f.origClOrdId).data())
        .putSymbol(wire::fixed<8>(f.symbol).data())
        .side(side);
    return finish(buffer, m);
  }

  const auto qty = parseDecimal(f.qty);
  if (!qty) return error("OrderQty (38) missing or not a decimal");
  const auto price = parseDecimal(f.price);
  if (!price) return error("Price (44) missing or not a decimal");
  auto m = wire::encode<ReplaceOrder>(buffer);
  m.sessionId(request.sessionId)
      .putSenderCompId(wire::fixed<16>(f.sender).data())
      .putAccount(wire::fixed<16>(f.account).data())
      .putClOrdId(wire::fixed<20>(f.clOrdId).data())
      .putOrigClOrdId(wire::fixed<20>(f.origClOrdId).data())
      .putSymbol(wire::fixed<8>(f.symbol).data())
      .side(side)
      .quantity(*qty)
      .price(*price);
  return finish(buffer, m);
}

sequencer::Bytes ExchangeFixInputCodec::toOutput(const sequencer::Receipt& /*receipt*/,
                                                 std::span<const sequencer::Payload> /*designatedOutputs*/) {
  return sequencer::Bytes();
}

std::optional<sequencer::Bytes> ExchangeFixInputCodec::onDisconnect(const sequencer::SessionInfo& /*session*/) {
  // Cancel-on-disconnect is v2 (spec §0): it is an OMS policy, and it
  // needs the input to name the session's orders.
  return std::nullopt;
}

}  // namespace exchange::fix
