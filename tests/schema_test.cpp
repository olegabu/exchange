// docs/spec.md §4, build step 2: the wire format.
//
// Two jobs. (1) Round-trip every message through the generated codecs
// via wire.hpp, including the Fill group. (2) Freeze the layout: the
// block lengths below are the schema as first written; SBE locates
// fields by offset, so any change here is a change every journal ever
// written must survive. A failing golden is a review, not a number to
// update.
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

#include <sequencer/journal/writer.hpp>

#include <exchange/AddInstrument.h>
#include <exchange/CancelOrder.h>
#include <exchange/Fill.h>
#include <exchange/InstrumentAdded.h>
#include <exchange/InstrumentRejected.h>
#include <exchange/InstrumentSnapshot.h>
#include <exchange/NewOrder.h>
#include <exchange/OrderAccepted.h>
#include <exchange/OrderCancelRejected.h>
#include <exchange/OrderCancelled.h>
#include <exchange/OrderRejected.h>
#include <exchange/OrderReplaceRejected.h>
#include <exchange/OrderReplaced.h>
#include <exchange/OrderSnapshot.h>
#include <exchange/ReplaceOrder.h>
#include <exchange/SnapshotEnd.h>
#include <exchange/SnapshotHeader.h>

#include "state_machine/limits.hpp"
#include "wire/wire.hpp"

namespace {

using namespace exchange;

TEST(Schema, IdentityIsFrozen) {
  EXPECT_EQ(NewOrder::sbeSchemaId(), 1);
  EXPECT_EQ(NewOrder::sbeSchemaVersion(), 0);
  EXPECT_EQ(wire::kHeaderLength, 8u);
  EXPECT_EQ(NewOrder::priceExponent(), -8);
}

// Template ids and root block lengths, as first generated. Append-only
// from here: a new field bumps the version and grows the block; an
// existing number never changes.
TEST(Schema, LayoutIsFrozen) {
  const std::vector<std::tuple<std::string_view, int, int>> expected = {
      {"AddInstrument", AddInstrument::sbeTemplateId(), AddInstrument::sbeBlockLength()},
      {"NewOrder", NewOrder::sbeTemplateId(), NewOrder::sbeBlockLength()},
      {"CancelOrder", CancelOrder::sbeTemplateId(), CancelOrder::sbeBlockLength()},
      {"ReplaceOrder", ReplaceOrder::sbeTemplateId(), ReplaceOrder::sbeBlockLength()},
      {"OrderAccepted", OrderAccepted::sbeTemplateId(), OrderAccepted::sbeBlockLength()},
      {"OrderRejected", OrderRejected::sbeTemplateId(), OrderRejected::sbeBlockLength()},
      {"Fill", Fill::sbeTemplateId(), Fill::sbeBlockLength()},
      {"OrderCancelled", OrderCancelled::sbeTemplateId(), OrderCancelled::sbeBlockLength()},
      {"OrderCancelRejected", OrderCancelRejected::sbeTemplateId(), OrderCancelRejected::sbeBlockLength()},
      {"OrderReplaced", OrderReplaced::sbeTemplateId(), OrderReplaced::sbeBlockLength()},
      {"OrderReplaceRejected", OrderReplaceRejected::sbeTemplateId(), OrderReplaceRejected::sbeBlockLength()},
      {"InstrumentAdded", InstrumentAdded::sbeTemplateId(), InstrumentAdded::sbeBlockLength()},
      {"InstrumentRejected", InstrumentRejected::sbeTemplateId(), InstrumentRejected::sbeBlockLength()},
      {"SnapshotHeader", SnapshotHeader::sbeTemplateId(), SnapshotHeader::sbeBlockLength()},
      {"InstrumentSnapshot", InstrumentSnapshot::sbeTemplateId(), InstrumentSnapshot::sbeBlockLength()},
      {"OrderSnapshot", OrderSnapshot::sbeTemplateId(), OrderSnapshot::sbeBlockLength()},
      {"SnapshotEnd", SnapshotEnd::sbeTemplateId(), SnapshotEnd::sbeBlockLength()},
  };
  const std::vector<std::tuple<std::string_view, int, int>> golden = {
      {"AddInstrument", 1, 92},        {"NewOrder", 2, 87},
      {"CancelOrder", 3, 89},          {"ReplaceOrder", 4, 105},
      {"OrderAccepted", 101, 99},      {"OrderRejected", 102, 88},
      {"Fill", 103, 12},               {"OrderCancelled", 104, 126},
      {"OrderCancelRejected", 105, 97}, {"OrderReplaced", 106, 133},
      {"OrderReplaceRejected", 107, 97}, {"InstrumentAdded", 108, 96},
      {"InstrumentRejected", 109, 69}, {"SnapshotHeader", 201, 28},
      {"InstrumentSnapshot", 202, 44}, {"OrderSnapshot", 203, 123},
      {"SnapshotEnd", 204, 8},
  };
  EXPECT_EQ(expected, golden);
  EXPECT_EQ(Fill::Executions::sbeBlockLength(), 118);
}

TEST(Wire, NewOrderRoundTrips) {
  std::array<std::byte, 256> buffer{};
  auto order = wire::encode<NewOrder>(buffer);
  order.sessionId(7)
      .putSenderCompId(wire::fixed<16>("ACME").data())
      .putAccount(wire::fixed<16>("").data())
      .putClOrdId(wire::fixed<20>("ord-1").data())
      .putSymbol(wire::fixed<8>("ABC").data())
      .side(Side::Buy)
      .ordType(OrdType::Limit)
      .timeInForce(TimeInForce::Day)
      .price(10125000000)  // 101.25
      .quantity(100 * 100000000LL);
  const auto length = wire::encodedLength(order);
  EXPECT_EQ(length, wire::kHeaderLength + NewOrder::sbeBlockLength());

  const wire::Bytes bytes(buffer.data(), length);
  const auto header = wire::peekHeader(bytes);
  ASSERT_TRUE(header);
  EXPECT_EQ(header->templateId, NewOrder::sbeTemplateId());
  EXPECT_EQ(header->schemaId, 1);
  EXPECT_EQ(header->version, 0);

  auto decoded = wire::decode<NewOrder>(bytes);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->sessionId(), 7u);
  EXPECT_EQ(wire::view(decoded->senderCompId(), NewOrder::senderCompIdLength()), "ACME");
  EXPECT_EQ(wire::view(decoded->account(), NewOrder::accountLength()), "");
  EXPECT_EQ(wire::view(decoded->clOrdId(), NewOrder::clOrdIdLength()), "ord-1");
  EXPECT_EQ(wire::view(decoded->symbol(), NewOrder::symbolLength()), "ABC");
  EXPECT_EQ(decoded->side(), Side::Buy);
  EXPECT_EQ(decoded->ordType(), OrdType::Limit);
  EXPECT_EQ(decoded->timeInForce(), TimeInForce::Day);
  EXPECT_EQ(decoded->price(), 10125000000);
  EXPECT_EQ(decoded->quantity(), 10000000000);
}

TEST(Wire, DecodeRefusesWhatItCannotInterpret) {
  std::array<std::byte, 256> buffer{};
  auto order = wire::encode<NewOrder>(buffer);
  const auto length = wire::encodedLength(order);
  const wire::Bytes bytes(buffer.data(), length);

  EXPECT_EQ(wire::decode<CancelOrder>(bytes).rejection, wire::Rejection::WrongTemplate);
  EXPECT_EQ(wire::decode<NewOrder>(bytes.subspan(0, 4)).rejection, wire::Rejection::TooShort);
  EXPECT_EQ(wire::decode<NewOrder>(bytes.subspan(0, length - 1)).rejection, wire::Rejection::TooShort);

  auto tampered = buffer;
  MessageHeader header;
  header.wrap(reinterpret_cast<char*>(tampered.data()), 0, 0, tampered.size());
  header.version(1);
  EXPECT_EQ(wire::decode<NewOrder>(wire::Bytes(tampered.data(), length)).rejection,
            wire::Rejection::FutureVersion);
  header.version(0).schemaId(2);
  EXPECT_EQ(wire::decode<NewOrder>(wire::Bytes(tampered.data(), length)).rejection,
            wire::Rejection::WrongSchema);
}

TEST(Wire, FillGroupRoundTrips) {
  std::vector<std::byte> buffer(4096);
  auto fill = wire::encode<Fill>(buffer);
  fill.instrumentId(3).putSymbol(wire::fixed<8>("ABC").data());
  auto& entries = fill.executionsCount(3);
  for (std::uint16_t i = 0; i < 3; ++i) {
    entries.next()
        .sessionId(10 + i)
        .putSenderCompId(wire::fixed<16>("ACME").data())
        .putAccount(wire::fixed<16>("acct").data())
        .putClOrdId(wire::fixed<20>("c").data())
        .orderId(100 + i)
        .side(i % 2 == 0 ? Side::Buy : Side::Sell)
        .lastPx(5)
        .lastQty(6)
        .leavesQty(7)
        .cumQty(8)
        .avgPx(9)
        .counterpartyOrderId(200 + i)
        .aggressor(i == 0 ? Bool::True : Bool::False);
  }
  const auto length = wire::encodedLength(fill);
  EXPECT_EQ(length, wire::kHeaderLength + Fill::sbeBlockLength() + 4 + 3 * Fill::Executions::sbeBlockLength());

  auto decoded = wire::decode<Fill>(wire::Bytes(buffer.data(), length));
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->instrumentId(), 3u);
  auto& group = decoded->executions();
  EXPECT_EQ(group.count(), 3u);
  std::uint16_t i = 0;
  while (group.hasNext()) {
    auto& e = group.next();
    EXPECT_EQ(e.sessionId(), 10u + i);
    EXPECT_EQ(e.orderId(), 100u + i);
    EXPECT_EQ(e.counterpartyOrderId(), 200u + i);
    EXPECT_EQ(e.aggressor(), i == 0 ? Bool::True : Bool::False);
    ++i;
  }
  EXPECT_EQ(i, 3);
  EXPECT_EQ(decoded->decodeLength() + wire::kHeaderLength, length);
}

// spec §4: the worst-case record -- an input plus an accept plus a Fill
// carrying kMaxFillEntriesPerInput entries plus a cancel -- must fit the
// journal's default record size, or a sweep would crash every replica.
TEST(Schema, WorstCaseRecordFitsTheJournal) {
  const std::size_t input = wire::kHeaderLength + NewOrder::sbeBlockLength();
  const std::size_t accepted = wire::kHeaderLength + OrderAccepted::sbeBlockLength();
  const std::size_t fill = wire::kHeaderLength + Fill::sbeBlockLength() + 4 +
                           kMaxFillEntriesPerInput * Fill::Executions::sbeBlockLength();
  const std::size_t cancelled = wire::kHeaderLength + OrderCancelled::sbeBlockLength();
  // Journal framing (sequencer record_view.hpp): u64 seq, u32 input
  // length, u16 output count, u32 per output length.
  const std::size_t framing = 8 + 4 + 2 + 3 * 4;
  const std::size_t worst = framing + input + accepted + fill + cancelled;
  const sequencer::journal::JournalOptions defaults;
  EXPECT_LT(worst, defaults.maxRecordBytes)
      << "worst-case record " << worst << " bytes exceeds the journal's " << defaults.maxRecordBytes;
  EXPECT_LE(kMaxFillEntriesPerInput, 65535u) << "the group counter is uint16";
}

}  // namespace
