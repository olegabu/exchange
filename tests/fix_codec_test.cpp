// docs/spec.md §7, build step 5: the two codecs in isolation. Exact
// decimal parsing; FIX in -> SBE fields; SBE outputs -> FIX bodies; every
// input rejection; and the property a ResendRequest depends on: the
// same record encodes to the same bytes every time.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <sequencer/journal/record_view.hpp>

#include "fix/fix_codecs.hpp"
#include "fix/fix_price.hpp"
#include "harness.hpp"

namespace {

using namespace exchange;
using namespace exchange::test;

TEST(FixPrice, ParsesExactly) {
  EXPECT_EQ(fix::parseDecimal("101.25"), 10125000000);
  EXPECT_EQ(fix::parseDecimal("0.00000001"), 1);
  EXPECT_EQ(fix::parseDecimal("5"), 500000000);
  EXPECT_EQ(fix::parseDecimal("5."), 500000000);
  EXPECT_EQ(fix::parseDecimal(".5"), 50000000);
  EXPECT_EQ(fix::parseDecimal("0"), 0);
  EXPECT_EQ(fix::parseDecimal("92233720368.54775807"), INT64_MAX);
  EXPECT_FALSE(fix::parseDecimal(""));
  EXPECT_FALSE(fix::parseDecimal("."));
  EXPECT_FALSE(fix::parseDecimal("-1"));
  EXPECT_FALSE(fix::parseDecimal("1e5"));
  EXPECT_FALSE(fix::parseDecimal("0.000000001")) << "nine decimals";
  EXPECT_FALSE(fix::parseDecimal("92233720368.54775808")) << "overflow";
  EXPECT_FALSE(fix::parseDecimal("1 "));
}

TEST(FixPrice, FormatsAndRoundTrips) {
  EXPECT_EQ(fix::formatDecimal(10125000000).view(), "101.25");
  EXPECT_EQ(fix::formatDecimal(1).view(), "0.00000001");
  EXPECT_EQ(fix::formatDecimal(500000000).view(), "5");
  EXPECT_EQ(fix::formatDecimal(0).view(), "0");
  EXPECT_EQ(fix::formatDecimal(-250000000).view(), "-2.5");
  for (const std::string s : {"101.25", "0.00000001", "5", "1234567.8901", "0.1"}) {
    EXPECT_EQ(fix::formatDecimal(*fix::parseDecimal(s)).view(), s);
  }
}

// A complete FIX 4.4 message around a body, as the gateway hands the
// codec the raw bytes it received.
std::string frame(const std::string& type, const std::string& body, const std::string& sender = "ACME") {
  const std::string inner =
      "35=" + type + "\00149=" + sender + "\00156=EXCHANGE\00134=7\00152=20260904-12:00:00\001" + body;
  std::string msg = "8=FIX.4.4\0019=" + std::to_string(inner.size()) + "\001" + inner;
  unsigned sum = 0;
  for (unsigned char c : msg) sum += c;
  char cs[8];
  snprintf(cs, sizeof cs, "%03u", sum % 256);
  return msg + "10=" + cs + "\001";
}

sequencer::Result<sequencer::Bytes> in(fix::ExchangeFixInputCodec& codec, const std::string& raw,
                                       std::uint64_t session = 3) {
  sequencer::ClientRequest request;
  request.body = sequencer::Payload(reinterpret_cast<const std::byte*>(raw.data()), raw.size());
  request.sessionId = session;
  return codec.toInput(request);
}

TEST(FixInputCodec, NewOrderSingleBecomesNewOrder) {
  fix::ExchangeFixInputCodec codec;
  auto r = in(codec, frame("D", "11=c1\0011=acct\00155=ABC\00154=1\00138=100\00140=2\00144=101.25\00159=1\001"));
  ASSERT_TRUE(r.ok()) << r.error();
  auto m = wire::decode<NewOrder>(wire::Bytes(r.value().data(), r.value().size()));
  ASSERT_TRUE(m);
  EXPECT_EQ(m->sessionId(), 3u);
  EXPECT_EQ(wire::view(m->senderCompId(), 16), "ACME");
  EXPECT_EQ(wire::view(m->account(), 16), "acct");
  EXPECT_EQ(wire::view(m->clOrdId(), 20), "c1");
  EXPECT_EQ(wire::view(m->symbol(), 8), "ABC");
  EXPECT_EQ(m->side(), Side::Buy);
  EXPECT_EQ(m->ordType(), OrdType::Limit);
  EXPECT_EQ(m->timeInForce(), TimeInForce::GoodTillCancel);
  EXPECT_EQ(m->price(), 10125000000);
  EXPECT_EQ(m->quantity(), 10000000000);

  r = in(codec, frame("D", "11=m\00155=ABC\00154=2\00138=5\00140=1\001"));
  ASSERT_TRUE(r.ok()) << r.error();
  m = wire::decode<NewOrder>(wire::Bytes(r.value().data(), r.value().size()));
  EXPECT_EQ(m->ordType(), OrdType::Market);
  EXPECT_EQ(m->price(), 0);
  EXPECT_EQ(m->timeInForce(), TimeInForce::Day) << "59 absent defaults to Day";
  EXPECT_EQ(wire::view(m->account(), 16), "");
}

TEST(FixInputCodec, CancelAndReplace) {
  fix::ExchangeFixInputCodec codec;
  auto r = in(codec, frame("F", "41=c1\00111=x1\00155=ABC\00154=1\001"));
  ASSERT_TRUE(r.ok()) << r.error();
  auto c = wire::decode<CancelOrder>(wire::Bytes(r.value().data(), r.value().size()));
  ASSERT_TRUE(c);
  EXPECT_EQ(wire::view(c->clOrdId(), 20), "x1");
  EXPECT_EQ(wire::view(c->origClOrdId(), 20), "c1");
  EXPECT_EQ(c->side(), Side::Buy);

  r = in(codec, frame("G", "41=c1\00111=c2\00155=ABC\00154=1\00138=50\00140=2\00144=102\001"));
  ASSERT_TRUE(r.ok()) << r.error();
  auto g = wire::decode<ReplaceOrder>(wire::Bytes(r.value().data(), r.value().size()));
  ASSERT_TRUE(g);
  EXPECT_EQ(wire::view(g->clOrdId(), 20), "c2");
  EXPECT_EQ(wire::view(g->origClOrdId(), 20), "c1");
  EXPECT_EQ(g->quantity(), 5000000000);
  EXPECT_EQ(g->price(), 10200000000);
}

TEST(FixInputCodec, RejectsWhatItCannotEncode) {
  fix::ExchangeFixInputCodec codec;
  EXPECT_FALSE(in(codec, "garbage").ok());
  EXPECT_FALSE(in(codec, frame("V", "262=r\001263=1\001")).ok()) << "market data is not order entry";
  EXPECT_FALSE(in(codec, frame("D", "55=ABC\00154=1\00138=1\00140=2\00144=1\001")).ok()) << "no ClOrdID";
  EXPECT_FALSE(in(codec, frame("D", "11=c\00155=ABCDEFGHI\00154=1\00138=1\00140=2\00144=1\001")).ok()) << "symbol too long";
  EXPECT_FALSE(in(codec, frame("D", "11=c\00155=ABC\00154=3\00138=1\00140=2\00144=1\001")).ok()) << "bad side";
  EXPECT_FALSE(in(codec, frame("D", "11=c\00155=ABC\00154=1\00138=1\00140=3\00144=1\001")).ok()) << "stop order";
  EXPECT_FALSE(in(codec, frame("D", "11=c\00155=ABC\00154=1\00138=1\00140=2\001")).ok()) << "limit without price";
  EXPECT_FALSE(in(codec, frame("D", "11=c\00155=ABC\00154=1\00138=1\00140=1\00144=1\001")).ok()) << "market with price";
  EXPECT_FALSE(in(codec, frame("D", "11=c\00155=ABC\00154=1\00138=1x\00140=2\00144=1\001")).ok()) << "bad quantity";
  EXPECT_FALSE(in(codec, frame("D", "11=c\00155=ABC\00154=1\00138=1\00140=2\00144=1\00159=6\001")).ok()) << "bad tif";
  EXPECT_FALSE(in(codec, frame("D", "11=c\00155=ABC\00154=1\00138=1\00140=2\00144=1\001", "ABCDEFGHIJKLMNOPQ")).ok()) << "sender too long";
  EXPECT_FALSE(in(codec, frame("F", "11=x\00155=ABC\00154=1\001")).ok()) << "cancel without OrigClOrdID";
  EXPECT_FALSE(in(codec, frame("G", "41=c1\00111=c2\00155=ABC\00154=1\00138=50\001")).ok()) << "replace without price";
  sequencer::Receipt receipt{1};
  EXPECT_TRUE(codec.toOutput(receipt, {}).empty()) << "nothing on the propose path (§8.11)";
  EXPECT_FALSE(codec.onDisconnect(sequencer::SessionInfo{1}).has_value());
}

// Collects what the output codec publishes, with the addressee.
class Capture : public sequencer::Fanout {
 public:
  std::vector<std::pair<sequencer::SessionId, std::string>> sent;
  void toSession(sequencer::SessionId owner, sequencer::Bytes bytes) override {
    sent.emplace_back(owner, std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
  }
  void broadcast(const std::string&, sequencer::Bytes) override { FAIL() << "nothing is broadcast"; }
};

// A journal record from one apply: sequencer's record layout, built by hand.
std::vector<std::byte> recordBytes(std::uint64_t seq, const Encoded& input, const sequencer::OutputCollector& c) {
  std::vector<std::byte> r;
  auto put = [&](const void* p, std::size_t n) {
    const auto* b = static_cast<const std::byte*>(p);
    r.insert(r.end(), b, b + n);
  };
  put(&seq, 8);
  const std::uint32_t inLen = static_cast<std::uint32_t>(input.bytes.size());
  put(&inLen, 4);
  put(input.bytes.data(), input.bytes.size());
  const std::uint16_t n = static_cast<std::uint16_t>(c.outputs().size());
  put(&n, 2);
  for (const auto& o : c.outputs()) {
    const std::uint32_t len = static_cast<std::uint32_t>(o.size());
    put(&len, 4);
    put(o.data(), o.size());
  }
  return r;
}

std::map<int, std::string> parse(const std::string& body) {
  std::map<int, std::string> m;
  std::size_t i = 0;
  while (i < body.size()) {
    const auto eq = body.find('=', i);
    const auto soh = body.find('\001', eq);
    m[std::stoi(body.substr(i, eq - i))] = body.substr(eq + 1, soh - eq - 1);
    i = soh + 1;
  }
  return m;
}

TEST(FixOutputCodec, ExecutionReportsAreAddressedAndDeterministic) {
  Harness h;
  h.addAbc();
  NewOrderSpec b;
  b.session = 7;
  b.clOrdId = "buy1";
  b.account = "A1";
  b.price = px(10);
  b.quantity = lots(100);
  h.apply(newOrder(b));
  NewOrderSpec s;
  s.session = 9;
  s.compId = "OTHER";
  s.clOrdId = "sell1";
  s.side = Side::Sell;
  s.price = px(9, 5);
  s.quantity = lots(40);
  const Encoded sellInput = newOrder(s);
  h.apply(sellInput);
  const auto bytes = recordBytes(h.seq, sellInput, h.collector);
  const sequencer::journal::RecordView record(bytes.data(), static_cast<std::uint32_t>(bytes.size()));

  fix::ExchangeFixOutputCodec codec;
  Capture first, second;
  codec.toOutput(record, first);
  codec.toOutput(record, second);
  ASSERT_EQ(first.sent, second.sent) << "the same record is the same bytes: what a resend relies on";

  ASSERT_EQ(first.sent.size(), 3u) << "accept for the seller, then one report per party";
  EXPECT_EQ(first.sent[0].first, 9u);
  auto m = parse(first.sent[0].second);
  EXPECT_EQ(m[35], "8");
  EXPECT_EQ(m[150], "0");
  EXPECT_EQ(m[39], "0");
  EXPECT_EQ(m[11], "sell1");
  EXPECT_EQ(m[37], std::to_string(h.seq));
  EXPECT_EQ(m[17], std::to_string(h.seq) + "-0-0");
  EXPECT_EQ(m[55], "ABC");
  EXPECT_EQ(m[54], "2");
  EXPECT_EQ(m[38], "40");
  EXPECT_EQ(m[44], "9.05");
  EXPECT_EQ(m[151], "40");
  EXPECT_EQ(m[14], "0");
  EXPECT_EQ(m[6], "0");
  EXPECT_FALSE(m.count(1)) << "no account, no tag 1";

  EXPECT_EQ(first.sent[1].first, 9u) << "the taker's fill";
  m = parse(first.sent[1].second);
  EXPECT_EQ(m[150], "F");
  EXPECT_EQ(m[39], "2") << "filled";
  EXPECT_EQ(m[17], std::to_string(h.seq) + "-1-0");
  EXPECT_EQ(m[32], "40");
  EXPECT_EQ(m[31], "10");
  EXPECT_EQ(m[151], "0");
  EXPECT_EQ(m[14], "40");
  EXPECT_EQ(m[6], "10");
  EXPECT_EQ(m[38], "40");

  EXPECT_EQ(first.sent[2].first, 7u) << "the maker's fill goes to the maker's session";
  m = parse(first.sent[2].second);
  EXPECT_EQ(m[11], "buy1");
  EXPECT_EQ(m[1], "A1");
  EXPECT_EQ(m[39], "1") << "partially filled";
  EXPECT_EQ(m[17], std::to_string(h.seq) + "-1-1");
  EXPECT_EQ(m[151], "60");
  EXPECT_EQ(m[14], "40");
  EXPECT_EQ(m[38], "100");
  EXPECT_EQ(m[54], "1");
}

TEST(FixOutputCodec, RejectsCancelsReplacesAndInstrumentsMap) {
  Harness h;
  h.addAbc();
  fix::ExchangeFixOutputCodec codec;
  auto encodeLast = [&](const Encoded& input) {
    const auto bytes = recordBytes(h.seq, input, h.collector);
    Capture c;
    codec.toOutput(sequencer::journal::RecordView(bytes.data(), static_cast<std::uint32_t>(bytes.size())), c);
    return c.sent;
  };
  // The instrument's own outputs produce no FIX message.
  const Encoded xyz = addInstrument(99, "ADMIN", "add-XYZ", "XYZ", kUnit / 100, kUnit, lots(1000));
  h.apply(xyz);
  EXPECT_TRUE(encodeLast(xyz).empty());

  NewOrderSpec bad;
  bad.clOrdId = "bad";
  bad.price = px(10) + 1;
  bad.quantity = lots(1);
  Encoded e = newOrder(bad);
  h.apply(e);
  auto sent = encodeLast(e);
  ASSERT_EQ(sent.size(), 1u);
  auto m = parse(sent[0].second);
  EXPECT_EQ(m[150], "8");
  EXPECT_EQ(m[39], "8");
  EXPECT_EQ(m[37], "NONE");
  EXPECT_EQ(m[103], "99");
  EXPECT_EQ(m[58], "price not on tick");

  NewOrderSpec ok;
  ok.clOrdId = "o1";
  ok.price = px(10);
  ok.quantity = lots(10);
  h.apply(newOrder(ok));
  const auto id = h.seq;
  e = replaceOrder(1, "ACME", "o2", "o1", "ABC", Side::Buy, lots(8), px(11));
  h.apply(e);
  sent = encodeLast(e);
  ASSERT_EQ(sent.size(), 1u);
  m = parse(sent[0].second);
  EXPECT_EQ(m[150], "5");
  EXPECT_EQ(m[39], "0");
  EXPECT_EQ(m[11], "o2");
  EXPECT_EQ(m[41], "o1");
  EXPECT_EQ(m[37], std::to_string(id));
  EXPECT_EQ(m[38], "8");
  EXPECT_EQ(m[44], "11");
  EXPECT_EQ(m[151], "8");

  e = cancelOrder(1, "ACME", "x1", "o2", "ABC", Side::Buy);
  h.apply(e);
  sent = encodeLast(e);
  ASSERT_EQ(sent.size(), 1u);
  m = parse(sent[0].second);
  EXPECT_EQ(m[150], "4");
  EXPECT_EQ(m[39], "4");
  EXPECT_EQ(m[11], "x1");
  EXPECT_EQ(m[41], "o2");
  EXPECT_EQ(m[151], "0");
  EXPECT_EQ(m[14], "0");
  EXPECT_FALSE(m.count(58));

  e = cancelOrder(1, "ACME", "x2", "o2", "ABC", Side::Buy);
  h.apply(e);
  sent = encodeLast(e);
  ASSERT_EQ(sent.size(), 1u);
  m = parse(sent[0].second);
  EXPECT_EQ(m[35], "9");
  EXPECT_EQ(m[434], "1");
  EXPECT_EQ(m[102], "1");
  EXPECT_EQ(m[37], "NONE");
  EXPECT_EQ(m[11], "x2");
  EXPECT_EQ(m[41], "o2");
  EXPECT_EQ(m[58], "unknown order");

  e = replaceOrder(1, "ACME", "o3", "o2", "ABC", Side::Buy, lots(8), px(11));
  h.apply(e);
  sent = encodeLast(e);
  m = parse(sent[0].second);
  EXPECT_EQ(m[35], "9");
  EXPECT_EQ(m[434], "2");

  NewOrderSpec ioc;
  ioc.clOrdId = "ioc";
  ioc.side = Side::Sell;
  ioc.price = px(10);
  ioc.quantity = lots(5);
  ioc.tif = TimeInForce::ImmediateOrCancel;
  e = newOrder(ioc);
  h.apply(e);
  sent = encodeLast(e);
  ASSERT_EQ(sent.size(), 2u) << "accepted, then cancelled: nothing to match";
  m = parse(sent[1].second);
  EXPECT_EQ(m[150], "4");
  EXPECT_EQ(m[58], "immediate-or-cancel remainder");
  EXPECT_EQ(m[11], "ioc");
  EXPECT_EQ(m[41], "ioc");
}

}  // namespace
