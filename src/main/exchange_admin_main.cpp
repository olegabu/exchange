// Administrative inputs proposed straight to the raft group over brpc
// (docs/spec.md §4, §11): instrument static data is sequenced, never
// compiled in or read from a file, so it enters through Propose like
// any other input and is journaled like any other input.
//
//   exchange_admin add-instrument --node_peers=127.0.0.1:8100,127.0.0.1:8101
//       --symbol=ABC --tick=0.01 --lot=1 --max_qty=1000   (one line)
//
// Prints the sequence number and the state machine's designated
// answer (InstrumentAdded or InstrumentRejected). Exit 0 on added.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <brpc/channel.h>
#include <brpc/controller.h>
#include <gflags/gflags.h>
#include <glog/logging.h>

#include <sequencer/comma_separated.hpp>

#include <exchange/AddInstrument.h>
#include <exchange/InstrumentAdded.h>
#include <exchange/InstrumentRejected.h>

#include "fix/fix_price.hpp"
#include "node.pb.h"
#include "wire/wire.hpp"

DEFINE_string(node_peers, "", "Comma-separated raft group Propose endpoints (required)");
DEFINE_string(symbol, "", "Instrument symbol, up to 8 characters (required)");
DEFINE_string(tick, "0.01", "Tick size, decimal");
DEFINE_string(lot, "1", "Lot size, decimal");
DEFINE_string(max_qty, "1000", "Maximum order quantity, decimal; max_qty/lot must not exceed kMaxMatchesPerInput");
DEFINE_string(comp_id, "ADMIN", "SenderCompID recorded on the request");
DEFINE_string(request_id, "", "ClOrdID recorded on the request; defaults to add-<symbol>");
DEFINE_int32(timeout_s, 15, "How long to keep following redirects and retrying");

namespace {

std::string stripPeerIdIndex(const std::string& peerId) {
  const auto lastColon = peerId.rfind(':');
  return lastColon == std::string::npos ? peerId : peerId.substr(0, lastColon);
}

struct Outcome {
  bool ok = false;
  std::uint64_t sequenceNumber = 0;
  std::vector<std::string> designated;
  std::string error;
};

// Follows redirects exactly as an input gateway would (sequencer §8.1),
// including the window right after startup before any leader exists.
Outcome propose(const std::vector<std::string>& endpoints, const std::vector<std::byte>& payload, int timeoutSeconds) {
  std::string target = endpoints.front();
  std::size_t nextEndpoint = 1 % endpoints.size();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
  Outcome outcome;
  while (std::chrono::steady_clock::now() < deadline) {
    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.timeout_ms = 500;
    options.max_retry = 0;
    bool tryAnother = false;
    if (channel.Init(target.c_str(), &options) == 0) {
      sequencer::node::proto::ProposeService_Stub stub(&channel);
      sequencer::node::proto::ProposeRequest request;
      request.set_input(payload.data(), payload.size());
      sequencer::node::proto::ProposeResponse response;
      brpc::Controller cntl;
      stub.Propose(&cntl, &request, &response, nullptr);
      if (!cntl.Failed() && !response.redirect() && response.error_message().empty()) {
        outcome.ok = true;
        outcome.sequenceNumber = response.sequence_number();
        for (int i = 0; i < response.designated_outputs_size(); ++i) {
          outcome.designated.push_back(response.designated_outputs(i));
        }
        return outcome;
      }
      if (!cntl.Failed() && response.redirect() && !response.leader_hint().empty()) {
        target = stripPeerIdIndex(response.leader_hint());
      } else {
        outcome.error = cntl.Failed() ? cntl.ErrorText() : response.error_message();
        tryAnother = true;
      }
    } else {
      tryAnother = true;
    }
    if (tryAnother) {
      target = endpoints[nextEndpoint];
      nextEndpoint = (nextEndpoint + 1) % endpoints.size();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (outcome.error.empty()) {
    outcome.error = "no leader answered within the timeout";
  }
  return outcome;
}

int addInstrument() {
  const auto tick = exchange::fix::parseDecimal(FLAGS_tick);
  const auto lot = exchange::fix::parseDecimal(FLAGS_lot);
  const auto maxQty = exchange::fix::parseDecimal(FLAGS_max_qty);
  if (FLAGS_symbol.empty() || FLAGS_symbol.size() > 8 || !tick || !lot || !maxQty || FLAGS_comp_id.size() > 16) {
    std::cerr << "exchange_admin: --symbol (1-8 chars), --tick, --lot, --max_qty (decimals) are required\n";
    return 2;
  }
  const std::string requestId = FLAGS_request_id.empty() ? "add-" + FLAGS_symbol : FLAGS_request_id;
  if (requestId.size() > 20) {
    std::cerr << "exchange_admin: --request_id longer than 20\n";
    return 2;
  }
  std::vector<std::byte> payload(256);
  auto m = exchange::wire::encode<exchange::AddInstrument>(payload);
  m.sessionId(0)
      .putSenderCompId(exchange::wire::fixed<16>(FLAGS_comp_id).data())
      .putAccount(exchange::wire::fixed<16>("").data())
      .putClOrdId(exchange::wire::fixed<20>(requestId).data())
      .putSymbol(exchange::wire::fixed<8>(FLAGS_symbol).data())
      .tickSize(*tick)
      .lotSize(*lot)
      .maxOrderQty(*maxQty);
  payload.resize(exchange::wire::encodedLength(m));

  const Outcome outcome = propose(sequencer::splitCommaSeparated(FLAGS_node_peers), payload, FLAGS_timeout_s);
  if (!outcome.ok) {
    std::cerr << "exchange_admin: propose failed: " << outcome.error << "\n";
    return 1;
  }
  std::cout << "sequence_number=" << outcome.sequenceNumber << "\n";
  for (const std::string& d : outcome.designated) {
    const exchange::wire::Bytes bytes(reinterpret_cast<const std::byte*>(d.data()), d.size());
    if (auto added = exchange::wire::decode<exchange::InstrumentAdded>(bytes)) {
      std::cout << "added instrument_id=" << added->instrumentId() << " symbol="
                << exchange::wire::view(added->symbol(), 8) << "\n";
      return 0;
    }
    if (auto rejected = exchange::wire::decode<exchange::InstrumentRejected>(bytes)) {
      std::cout << "rejected reason=" << static_cast<int>(rejected->reasonRaw()) << "\n";
      return 1;
    }
  }
  std::cout << "no designated answer\n";
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::SetUsageMessage("exchange_admin add-instrument --node_peers=... --symbol=...");
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  if (argc < 2 || std::strcmp(argv[1], "add-instrument") != 0) {
    std::cerr << "usage: exchange_admin add-instrument --node_peers=... --symbol=... [--tick --lot --max_qty]\n";
    return 2;
  }
  return addInstrument();
}
