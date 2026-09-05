// The exchange's FIX 4.4 session gateway (docs/spec.md §2, §7):
// sequencer's hffix gateway, journal flavour, with this repository's
// two codecs. Orders in over 35=D/F/G; execution reports out FROM THE
// JOURNAL, never as the synchronous reply (sequencer §8.11).
//
//   exchange_fix_gateway --node_peers=127.0.0.1:8100 --listen_port=8500
//       --data_dir=/data/exchange/node-0
//       --resume_file=/data/exchange/node-0/fix-resume
//       --sequence_store_dir=/data/exchange/node-0/fix-seq
//
// --inline_designated_outputs is deliberately not offered: a passive
// fill is an output a session receives that did not originate in its
// own input, which is the one condition under which that mode is
// unsound (sequencer gateway/input/src/input_gateway_impl.hpp).

#include <memory>
#include <string>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <sequencer/comma_separated.hpp>
#include <sequencer/fix/fix_session_gateway.hpp>

#include "fix/fix_codecs.hpp"

DEFINE_string(node_peers, "", "Comma-separated raft group Propose endpoints, e.g. 127.0.0.1:8100 (required)");
DEFINE_int32(listen_port, 0, "The FIX port clients connect to (required)");
DEFINE_string(data_dir, "", "A node's data directory whose journal this gateway tails, colocated (required)");
DEFINE_string(resume_file, "", "Where this gateway's journal resume position is kept (required)");
DEFINE_string(sequence_store_dir, "",
              "Where per-session FIX sequence counters are persisted; empty keeps them in memory, "
              "which loses a session's numbers across a restart");
DEFINE_string(sender_comp_id, "EXCHANGE", "This gateway's own FIX CompID");
DEFINE_int32(heartbeat_interval, 30, "FIX HeartBtInt, in seconds");
DEFINE_uint32(gateway_id, 0,
              "This gateway's own id, and it MUST be distinct across every gateway tailing the "
              "same journal. A gateway numbers its client sessions from 1 inside its own process, "
              "so two gateways both hand out session 1; this id is composed into the session id "
              "that goes on the wire, and each gateway delivers only what its own id addresses. "
              "Running two gateways with the same id makes each deliver the other's execution "
              "reports to the wrong client.");

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_node_peers.empty() || FLAGS_listen_port == 0 || FLAGS_data_dir.empty() || FLAGS_resume_file.empty()) {
    LOG(ERROR) << "exchange_fix_gateway: --node_peers, --listen_port, --data_dir and --resume_file are required";
    return 1;
  }

  sequencer::fix::SessionGatewayConfig config;
  config.nodeEndpoints = sequencer::splitCommaSeparated(FLAGS_node_peers);
  config.listenPort = FLAGS_listen_port;
  config.dataDir = FLAGS_data_dir;
  config.resumeFile = FLAGS_resume_file;
  config.senderCompId = FLAGS_sender_comp_id;
  config.heartBtInt = FLAGS_heartbeat_interval;
  config.sequenceStoreDir = FLAGS_sequence_store_dir;
  config.inlineDesignatedOutputs = false;

  LOG(INFO) << "exchange FIX session gateway starting: listen_port=" << FLAGS_listen_port
            << " node_peers=" << FLAGS_node_peers << " gateway_id=" << FLAGS_gateway_id;

  return sequencer::fix::RunFixSessionGateway(std::move(config),
                                              std::make_unique<exchange::fix::ExchangeFixInputCodec>(),
                                              std::make_unique<exchange::fix::ExchangeFixOutputCodec>());
}
