// What a journal actually contains: how many of each message went in,
// what came out, and what the book looked like at the end.
//
// It exists because a fleet run raised the question "is the order flow
// matching, or is every order just resting?" and nothing could answer
// it. Node RSS could not: the node mmaps its own journal, so RSS grows
// with the log whether or not the book does, and reading it as book
// growth is exactly the kind of hypothesis-from-inspection that
// docs/spec.md §10.1 warns about.
//
//   exchange_journal_stats --data_dir=/data/exchange/data
//
// Replays every record through a fresh state machine -- the same path
// exchange_replay uses -- so the live-order count at the end is the
// book the node itself had.

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>

#include <gflags/gflags.h>

#include <sequencer/journal/reader.hpp>

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

DEFINE_string(data_dir, "", "A node's data directory; its journal/ is read (required)");
DEFINE_uint64(replay_through, 0, "Replay at most this many records; 0 = all");

namespace {

const char* nameOf(std::uint16_t templateId) {
  switch (templateId) {
    case exchange::AddInstrument::sbeTemplateId(): return "AddInstrument";
    case exchange::NewOrder::sbeTemplateId(): return "NewOrder";
    case exchange::CancelOrder::sbeTemplateId(): return "CancelOrder";
    case exchange::ReplaceOrder::sbeTemplateId(): return "ReplaceOrder";
    case exchange::OrderAccepted::sbeTemplateId(): return "OrderAccepted";
    case exchange::OrderRejected::sbeTemplateId(): return "OrderRejected";
    case exchange::Fill::sbeTemplateId(): return "Fill";
    case exchange::OrderCancelled::sbeTemplateId(): return "OrderCancelled";
    case exchange::OrderCancelRejected::sbeTemplateId(): return "OrderCancelRejected";
    case exchange::OrderReplaced::sbeTemplateId(): return "OrderReplaced";
    case exchange::OrderReplaceRejected::sbeTemplateId(): return "OrderReplaceRejected";
    case exchange::InstrumentAdded::sbeTemplateId(): return "InstrumentAdded";
    case exchange::InstrumentRejected::sbeTemplateId(): return "InstrumentRejected";
    default: return "?";
  }
}

}  // namespace

int main(int argc, char** argv) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  if (FLAGS_data_dir.empty()) {
    std::cerr << "exchange_journal_stats: --data_dir is required\n";
    return 2;
  }
  sequencer::journal::JournalReader reader(std::filesystem::path(FLAGS_data_dir) / "journal");
  const std::uint64_t total = reader.committedCount();
  const std::uint64_t through =
      FLAGS_replay_through == 0 ? total : std::min<std::uint64_t>(total, FLAGS_replay_through);

  std::map<std::string, std::uint64_t> inputs, outputs;
  std::map<std::uint8_t, std::uint64_t> rejectReasons;
  std::uint64_t fillEntries = 0, inputBytes = 0, outputBytes = 0;

  exchange::OrderBookStateMachine sm;
  sequencer::OutputCollector collector;

  for (std::uint64_t seq = 1; seq <= through; ++seq) {
    const auto record = reader.record(seq);
    const auto input = record.input();
    inputBytes += input.size();
    if (const auto header = exchange::wire::peekHeader(input)) {
      ++inputs[nameOf(header->templateId)];
    }
    // The node's own outputs, as journaled.
    for (std::uint16_t i = 0; i < record.outputCount(); ++i) {
      const auto out = record.output(i);
      outputBytes += out.size();
      const auto header = exchange::wire::peekHeader(out);
      if (!header) continue;
      ++outputs[nameOf(header->templateId)];
      if (header->templateId == exchange::Fill::sbeTemplateId()) {
        if (auto fill = exchange::wire::decode<exchange::Fill>(out)) {
          fillEntries += fill->executions().count();
        }
      } else if (header->templateId == exchange::OrderRejected::sbeTemplateId()) {
        if (auto m = exchange::wire::decode<exchange::OrderRejected>(out)) ++rejectReasons[m->reasonRaw()];
      } else if (header->templateId == exchange::OrderCancelRejected::sbeTemplateId()) {
        if (auto m = exchange::wire::decode<exchange::OrderCancelRejected>(out)) ++rejectReasons[m->reasonRaw()];
      } else if (header->templateId == exchange::OrderReplaceRejected::sbeTemplateId()) {
        if (auto m = exchange::wire::decode<exchange::OrderReplaceRejected>(out)) ++rejectReasons[m->reasonRaw()];
      }
    }
    // Replay it, so the final book is the node's own.
    collector.reset();
    sm.apply(seq, input, collector);
  }

  std::cout << "records            " << through << " of " << total << "\n"
            << "input bytes        " << inputBytes << "  (" << (through ? inputBytes / through : 0) << "/record)\n"
            << "output bytes       " << outputBytes << "  (" << (through ? outputBytes / through : 0) << "/record)\n"
            << "\ninputs:\n";
  for (const auto& [name, n] : inputs) std::cout << "  " << std::setw(22) << std::left << name << n << "\n";
  std::cout << "outputs:\n";
  for (const auto& [name, n] : outputs) std::cout << "  " << std::setw(22) << std::left << name << n << "\n";
  std::cout << "  " << std::setw(22) << std::left << "(Fill entries)" << fillEntries
            << "   -> " << fillEntries / 2 << " matches\n";
  if (!rejectReasons.empty()) {
    std::cout << "reject reasons (raw enum):\n";
    for (const auto& [reason, n] : rejectReasons)
      std::cout << "  " << std::setw(22) << std::left << static_cast<int>(reason) << n << "\n";
  }
  std::cout << "\nbook at the end:\n"
            << "  live orders        " << sm.liveOrderCount() << "\n"
            << "  instruments        " << sm.instrumentCount() << "\n";
  return 0;
}
