// The exchange's FIX load generator: sequencer's LoadGenerator harness
// driving ExchangeFixRequester (docs/spec.md §9.1 step 6).
//
// The FIX arm only. The counter's load generator carries five arms
// (input gateway, direct propose, relay, output observers, FIX); an
// exchange has one client-facing transport in v1, so the other four
// would be dead flags here. Everything else -- the open/closed-loop
// modes, the pacing, the HdrHistogram output that merge-hdr.py
// consumes -- is sequencer's harness, unchanged.
//
//   exchange_load_generator --fix_gateway_addr=10.0.0.1:8700,10.0.0.2:8700
//       --rate 100000 --mode open --warmup 10 --measure 30 --hdr_raw_out raw.csv
//
// What it measures: NewOrderSingle to its first ExecutionReport, from
// the journal, over a real FIX session (see exchange_fix_requester.hpp).

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <sequencer/bench/load_generator.hpp>
#include <sequencer/comma_separated.hpp>

#include "exchange_fix_requester.hpp"

DEFINE_string(fix_gateway_addr, "",
              "A FIX session gateway's \"ip:port\", or a comma-separated list of them. One "
              "session is opened per address and requests are spread round-robin across them, "
              "which is what keeps several gateways evenly loaded (required)");
DEFINE_string(fix_sender_comp_id, "LOADGEN", "This sender's FIX CompID; a per-session suffix is appended");
DEFINE_string(fix_target_comp_id, "EXCHANGE", "The gateway's FIX CompID");
DEFINE_int64(client_id, 0,
             "Identifies this client among all of them. It goes in the high bits of every "
             "ClOrdID, so ClOrdIDs stay unique across clients -- the exchange rejects a "
             "duplicate live ClOrdID from one CompID. MUST be distinct per client box.");
DEFINE_int32(client_id_shift, 40, "How far to shift --client_id in a ClOrdID");
DEFINE_string(symbol, "ABC", "The instrument to trade; must already exist (exchange_admin add-instrument)");
DEFINE_int64(mid_ticks, 10000, "The mid price, in ticks (10000 = 100.00 at a 0.01 tick)");
DEFINE_int64(price_levels, 5, "How many ticks either side of the mid the flow walks");
DEFINE_int64(quantity_lots, 1, "Order quantity, in lots");
DEFINE_string(flow, "cycle",
              "\"cycle\": the real flow -- makers rest, takers cross, with cancels and replaces. "
              "\"rest-cancel\": a CONTROL that never matches -- every order is a buy, cancelled "
              "immediately -- which isolates everything except matching");

DEFINE_string(mode, "open", "\"open\" (fixed rate) or \"closed\" (fixed in-flight)");
DEFINE_int64(rate, 10000, "Offered rate, requests/sec (open mode)");
DEFINE_int64(burst, 1, "Requests per pacing tick");
DEFINE_int64(max_inflight, 0, "Cap on outstanding requests; 0 = unlimited (closed mode uses it as the target)");
DEFINE_string(pace, "spin", "\"spin\" or \"park\" between pacing ticks");
DEFINE_int32(threads, 1, "Sender threads");
DEFINE_int32(warmup, 5, "Warm-up seconds, not measured");
DEFINE_int32(measure, 20, "Measured seconds");
DEFINE_int32(drain_timeout, 10, "Seconds to wait for outstanding replies after the measured window");
DEFINE_string(hdr_out, "", "Write an HdrHistogram summary here");
DEFINE_string(hdr_raw_out, "", "Write raw value,count histogram buckets here (merge-hdr.py's input)");

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  if (FLAGS_fix_gateway_addr.empty()) {
    LOG(ERROR) << "exchange_load_generator: --fix_gateway_addr is required";
    return 1;
  }
  const std::vector<std::string> addrs = sequencer::splitCommaSeparated(FLAGS_fix_gateway_addr);
  if (addrs.empty()) {
    LOG(ERROR) << "exchange_load_generator: --fix_gateway_addr must be \"ip:port\" or a list of them";
    return 1;
  }

  exchange::bench::OrderShape shape;
  if (FLAGS_flow == "rest-cancel") {
    shape.flow = exchange::bench::Flow::RestCancel;
  } else if (FLAGS_flow != "cycle") {
    LOG(ERROR) << "exchange_load_generator: --flow must be \"cycle\" or \"rest-cancel\"";
    return 1;
  }
  shape.symbol = FLAGS_symbol;
  shape.midTicks = FLAGS_mid_ticks;
  shape.priceLevels = FLAGS_price_levels < 1 ? 1 : FLAGS_price_levels;
  shape.quantityLots = FLAGS_quantity_lots;

  auto fan = std::make_unique<exchange::bench::ExchangeFixFanoutRequester>(
      exchange::bench::cycleLength(shape.flow));
  for (std::size_t i = 0; i < addrs.size(); ++i) {
    const std::size_t colon = addrs[i].rfind(':');
    if (colon == std::string::npos) {
      LOG(ERROR) << "exchange_load_generator: bad FIX address \"" << addrs[i] << "\"";
      return 1;
    }
    // Every SESSION gets its own id, not just every client: two
    // sessions sharing one would mint the same ClOrdIDs.
    const std::int64_t sessionId = FLAGS_client_id * 100 + static_cast<std::int64_t>(i);
    auto one = std::make_unique<exchange::bench::ExchangeFixRequester>(
        addrs[i].substr(0, colon), std::stoi(addrs[i].substr(colon + 1)),
        FLAGS_fix_sender_comp_id + "-" + std::to_string(i), FLAGS_fix_target_comp_id, sessionId,
        FLAGS_client_id_shift, shape);
    if (!one->start()) {
      LOG(ERROR) << "exchange_load_generator: FIX session did not establish against " << addrs[i];
      return 1;
    }
    fan->add(std::move(one));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  LOG(INFO) << "FIX: " << addrs.size() << " session(s) established";

  sequencer::bench::LoadGeneratorConfig config;
  config.mode = FLAGS_mode;
  config.rate = FLAGS_rate;
  config.burst = FLAGS_burst;
  config.maxInflight = FLAGS_max_inflight;
  config.pace = FLAGS_pace;
  config.threadNum = FLAGS_threads;
  config.warmupSeconds = FLAGS_warmup;
  config.measureSeconds = FLAGS_measure;
  config.drainTimeoutSeconds = FLAGS_drain_timeout;
  config.hdrOut = FLAGS_hdr_out;
  config.hdrRawOut = FLAGS_hdr_raw_out;

  sequencer::bench::LoadGenerator generator(*fan, config);
  return generator.run() ? 0 : 1;
}
