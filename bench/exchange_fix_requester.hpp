#pragma once

// A FIX order-entry load sender for the exchange: sequencer's
// LoadGeneratorRequester over the same session core the gateway's
// acceptor runs, in Initiator role, sending real NewOrderSingles.
//
// FORKED from sequencer/bench/load_generator/include/sequencer/bench/
// fix_requester.hpp (docs/design.md §4). That one hardcodes the
// counter's U1 body and its internals are private, so there is no hook
// to send a 35=D through it. Generalising it upstream -- a virtual body
// builder and a configurable MsgType -- is the right long-run answer
// and is a change to that repository; this fork keeps the measurement
// unblocked meanwhile. Everything below except buildOrder(), onReply()
// and the order-shaping members is that file, comments included, so a
// diff against it stays readable.
//
// CORRELATION IS ClOrdID (tag 11), not a private tag. Every
// ExecutionReport an exchange sends echoes the ClOrdID of the order
// that caused it, so the standard field already does the job the
// counter needed tag 5000 for. The ClOrdID sent is the same nonce
// scheme -- (clientId << shift) | sequence, rendered decimal -- so it
// is unique per session and parses straight back to the harness's key.
//
// WHAT IS MEASURED is a NewOrderSingle to its FIRST ExecutionReport:
// the accept for an order that rests, or the accept for one that
// crosses (the fill follows it in the same journal record and arrives
// just after). Later reports for the same ClOrdID -- a passive fill on
// an order resting from earlier -- find no pending entry and are
// ignored, which is correct: they answer no outstanding request.

#include <sequencer/bench/load_generator.hpp>
#include <sequencer/fix/fix_session.hpp>

#include <netinet/in.h>
#include <netinet/tcp.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <sys/socket.h>
#include <sys/time.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>

#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace exchange::bench {

using sequencer::bench::LoadGeneratorRequester;

// FIX tags this sender reads and writes.
inline constexpr int kClOrdIdTag = 11;

// The shape of the order flow, and the reasons it is shaped this way.
//
// A benchmark measures whatever its inputs actually do, so the flow is
// a designed artefact with two invariants, both asserted by
// tests/load_generator_shape_test.cpp:
//
//   1. IT MATCHES. A run that only rests orders measures a book
//      growing, not an exchange matching.
//   2. DEPTH IS BOUNDED. Independent of run length -- a seventeen-rate
//      sweep shares one cluster, so anything that grows linearly ends
//      the ladder with tens of millions of resting orders.
//
// It also exercises ALL THREE order-entry messages, because an
// exchange that is only ever measured on NewOrderSingle is measured on
// one of its three paths, and real venues see more cancels than
// trades.
//
// The cycle is seven messages and nets to zero:
//
//   0  35=D  maker rests inside the band
//   1  35=D  taker, IMMEDIATE-OR-CANCEL, prices through the band
//   2  35=D  maker rests
//   3  35=F  cancels step 2                 -> nothing rests
//   4  35=D  maker rests
//   5  35=G  replaces step 4 to a new price -> still rests
//   6  35=D  taker, IMMEDIATE-OR-CANCEL, prices through the band
//
// Five NewOrderSingles, one cancel, one replace; two matches. Makers
// swap sides every cycle, so both sides of the book are worked.
//
// WHO TRADES WITH WHOM. A cycle stays on one FIX session (see
// ExchangeFixFanoutRequester, which distributes whole cycles), so its
// cancel and replace can reference an order that session actually
// sent -- a ClOrdID is scoped to its CompID, and referencing another
// session's would simply be rejected. The consequence is that in a
// SINGLE-session run a client matches against itself. That is legal
// here only because self-trade prevention is v2 (docs/spec.md §11);
// once it lands, this flow has to change with it. On the fleet it is
// largely moot: every client box quotes the same band on the same
// symbol, so a taker crosses the best resting maker, which is usually
// somebody else's.
//
// Two earlier shapes were wrong in ways only a run showed -- buys
// stepping down from the mid and sells stepping up never overlap, so
// nothing matched; both sides on one band matched a quarter of the
// flow but still accumulated buys low and sells high, linear in run
// length. Caught by a loopback run before any fleet time
// (docs/spec.md §10.1: instrument, do not theorise).
struct OrderShape {
  std::string symbol = "ABC";
  std::int64_t midTicks = 10000;   // 100.00 at a 0.01 tick
  std::int64_t priceLevels = 5;    // how wide the band makers rest in
  std::int64_t quantityLots = 1;
};

// Messages per cycle. A cycle must stay on one session, so the fanout
// distributes cycles rather than individual messages.
inline constexpr std::int64_t kCycleLength = 7;

enum class Action : std::uint8_t { NewOrder, Cancel, Replace };

// What the sequence-th message is. A free function taking no socket, so
// the flow can be replayed through a state machine in a test.
struct PlannedStep {
  Action action = Action::NewOrder;
  bool buy = true;
  std::int64_t priceTicks = 0;
  bool maker = true;  // NewOrder only
  // Cancel and Replace always reference the immediately preceding
  // message, which is the maker they act on.
  bool targetsPrevious = false;
};

inline PlannedStep plan(const OrderShape& shape, std::int64_t sequence) {
  const std::int64_t levels = shape.priceLevels < 1 ? 1 : shape.priceLevels;
  const std::int64_t cycle = sequence / kCycleLength;
  const std::int64_t step = sequence % kCycleLength;
  const bool makerBuys = (cycle & 1) == 0;
  const std::int64_t away = 1 + (cycle % levels);
  const std::int64_t makerPrice = makerBuys ? shape.midTicks - away : shape.midTicks + away;
  // A different tick, still inside the band, so the replace really
  // moves the order and the taker after it still crosses.
  const std::int64_t replaceAway = 1 + ((cycle + 1) % levels);
  const std::int64_t replacePrice =
      makerBuys ? shape.midTicks - replaceAway : shape.midTicks + replaceAway;
  // Through the whole band on the other side: crosses the best resting
  // order and, at one lot, exactly one of them.
  const std::int64_t takerPrice =
      makerBuys ? shape.midTicks - levels - 1 : shape.midTicks + levels + 1;

  switch (step) {
    case 1:
    case 6:
      return {Action::NewOrder, !makerBuys, takerPrice, false, false};
    case 3:
      return {Action::Cancel, makerBuys, makerPrice, false, true};
    case 5:
      return {Action::Replace, makerBuys, replacePrice, false, true};
    default:  // 0, 2, 4
      return {Action::NewOrder, makerBuys, makerPrice, true, false};
  }
}

// Ticks (hundredths) as an exact decimal string. No floating point.
inline std::string formatTicks(std::int64_t ticks) {
  std::string out = std::to_string(ticks / 100);
  const std::int64_t cents = ticks % 100;
  if (cents != 0) {
    out += '.';
    if (cents < 10) {
      out += '0';
    }
    std::string digits = std::to_string(cents);
    while (digits.size() > 1 && digits.back() == '0') {
      digits.pop_back();
    }
    out += digits;
  }
  return out;
}

// The application body for one planned step, and the MsgType it goes
// out under. The session core adds BeginString, BodyLength, CompIDs,
// MsgSeqNum, SendingTime and CheckSum.
//
// A free function so a test can drive the exact bytes this puts on the
// wire through the real input codec, rather than approximating them:
// the flow being admissible in principle and the flow encoding to FIX
// the codec accepts are two different claims.
inline const char* msgTypeOf(Action action) {
  switch (action) {
    case Action::Cancel:
      return "F";
    case Action::Replace:
      return "G";
    default:
      return "D";
  }
}

inline std::string buildBody(const OrderShape& shape, const PlannedStep& planned, std::int64_t nonce,
                             std::int64_t target) {
  const std::string side = planned.buy ? "1" : "2";
  const std::string common = "\00155=" + shape.symbol + "\00154=" + side + "\00160=20260904-00:00:00\001";
  switch (planned.action) {
    case Action::Cancel:
      return "11=" + std::to_string(nonce) + "\00141=" + std::to_string(target) + common;
    case Action::Replace:
      return "11=" + std::to_string(nonce) + "\00141=" + std::to_string(target) + common + "38=" +
             std::to_string(shape.quantityLots) + "\00140=2\00144=" + formatTicks(planned.priceTicks) +
             "\001";
    default:
      // 59=3 immediate-or-cancel for a taker, 59=0 day for a maker.
      return "11=" + std::to_string(nonce) + common + "38=" + std::to_string(shape.quantityLots) +
             "\00140=2\00144=" + formatTicks(planned.priceTicks) +
             (planned.maker ? "\00159=0\001" : "\00159=3\001");
  }
}


class ExchangeFixRequester : public LoadGeneratorRequester {
 public:
  // `clientId` identifies THIS session among all of them. It goes in
  // the HIGH BITS of the value sent as the application payload:
  //
  //     payload = (clientId << clientIdShift) | sequence
  //
  // rendered decimal as the ClOrdID. The exchange addresses every
  // report to the session that submitted the order, so unlike the
  // counter there is no topic to recover the high bits for -- they are
  // here so that ClOrdIDs stay unique across sessions, which the
  // exchange requires (a duplicate live ClOrdID from one CompID is
  // rejected).
  //
  // Must be unique per session in a multi-session run. Sharing it
  // silently makes sessions complete each other's requests, which reads
  // as impossibly good latency rather than as an error.
  ExchangeFixRequester(const std::string& host, int port, std::string senderCompId,
                       std::string targetCompId, std::int64_t clientId = 0,
                       int clientIdShift = 40, OrderShape shape = {})
      : socket_(ioContext_), clientId_(clientId), clientIdShift_(clientIdShift),
        shape_(std::move(shape)) {
    boost::asio::ip::tcp::resolver resolver(ioContext_);
    boost::asio::connect(socket_, resolver.resolve(host, std::to_string(port)));
    // Nagle off. The client is the side that both FIX arms share, so a
    // delayed-ACK stall here shows up in every FIX measurement this
    // repository makes -- and it did: one sample per session per run at
    // ~40ms, in the hffix and QuickFIX sweeps alike. See
    // fix_input_transport.cpp for the numbers.
    {
      const int noDelay = 1;
      ::setsockopt(socket_.native_handle(), IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));
    }

    sequencer::fix::SessionConfig config;
    config.role = sequencer::fix::Role::Initiator;
    config.senderCompId = std::move(senderCompId);
    config.targetCompId = std::move(targetCompId);
    // Heartbeats off: a benchmark run is short and continuously busy,
    // so the only thing an interval timer could do here is add work to
    // the path being measured.
    config.heartBtInt = 0;

    // ResetSeqNumFlag on Logon, and this is NOT optional for a rig.
    //
    // Each run is a fresh process with fresh in-memory counters, while
    // the gateway persists this CompID's numbers across runs. Without
    // the reset the client logs on at MsgSeqNum 1 against a gateway
    // expecting thousands, which is MsgSeqNum-too-low -- unrecoverable
    // in FIX, so the gateway logs it out and drops it. 141=Y is exactly
    // the mechanism for "I have no history, start us both at 1".
    //
    // Its absence cost a great deal here. Every benchmark run after the
    // FIRST against a given gateway was measuring a session the gateway
    // had already refused: sends went nowhere, replies never came, and
    // the harness reported it as dropped-by-rig. That was read as the
    // gateway collapsing above 1,000-2,000 msg/s, and several rounds of
    // profiling chased it. Distinct CompIDs per run hid it; repeating
    // one run exposed it in three lines.
    config.resetSeqNumOnLogon = true;

    session_ = std::make_unique<sequencer::fix::FixSession>(
        config, store_,
        [] {
          return static_cast<std::uint64_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count());
        });
    // Encoded frames go into a buffer, NOT onto the socket. The write
    // syscall happens on the writer thread below, so it is off the
    // critical section every sender is serialized through.
    session_->setSendFn([this](std::string_view frame) {
      std::lock_guard<std::mutex> lock(outMutex_);
      outBuffer_.append(frame.data(), frame.size());
    });
    session_->setAppMessageFn([this](const hffix::message_reader& message) { onReply(message); });
  }

  ~ExchangeFixRequester() override { stop(); }

  // Logs on and starts the receive loop. Returns false if the session
  // does not establish, which the harness must treat as a configuration
  // error rather than a slow run.
  bool start() {
    reader_ = std::thread([this] { receiveLoop(); });
    writer_ = std::thread([this] { writeLoop(); });
    session_->start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!session_->isLoggedOn() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!session_->isLoggedOn()) {
      return false;
    }
    // Confirm the session SURVIVES, rather than trusting the moment it
    // came up. A gateway that rejects the logon on sequence grounds
    // replies with a valid Logon echo first and disconnects immediately
    // after, so isLoggedOn() is briefly true for a session that is
    // already dead -- which is how a whole benchmark run could be spent
    // sending into a closed session and reporting it as the gateway's
    // fault.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    return session_->isLoggedOn();
  }

  void stop() {
    if (stopping_.exchange(true)) {
      return;
    }
    // shutdown() BEFORE close(), and the order matters. close() alone
    // does not reliably wake a thread parked in a read on Linux -- it
    // drops this process's handle without disturbing the blocked call
    // -- whereas shutdown() tears down the connection itself and makes
    // the pending read return at once. Closing alone left the load
    // generator printing its summary and then hanging forever on the
    // join below, which a sweep script would wait on indefinitely.
    boost::system::error_code ec;
    ::shutdown(socket_.native_handle(), SHUT_RDWR);
    socket_.close(ec);
    if (reader_.joinable()) {
      reader_.join();
    }
    if (writer_.joinable()) {
      writer_.join();
    }
  }

  // Subscribes to a broadcast topic by its FIX-standard mechanism: a
  // MarketDataRequest naming the topic as a Symbol (tag 55). Needed
  // whenever the application under test broadcasts its replies rather
  // than addressing the submitting session -- examples/counter does,
  // for the reason its codec header gives.
  void send(std::int64_t sequence, std::int64_t /*sendTimeUs*/,
             std::function<void(bool ok)> onDone) override {
    const std::int64_t nonce = (clientId_ << clientIdShift_) | sequence;
    {
      // Keyed by the ClOrdID that comes back, not by the harness
      // sequence, because replies arrive in journal order, which is not
      // request order. Every reply -- an accept, a cancel confirmation,
      // a replace confirmation, or a reject -- echoes the ClOrdID of
      // the request that caused it, so all three message types
      // correlate the same way.
      std::lock_guard<std::mutex> lock(pendingMutex_);
      pending_[nonce] = std::move(onDone);
    }
    const PlannedStep planned = plan(shape_, sequence);
    // The maker a cancel or replace acts on is the previous message on
    // this same session -- the fanout distributes whole cycles for
    // exactly this reason.
    const std::int64_t target = (clientId_ << clientIdShift_) | (sequence - 1);
    // Built OUTSIDE the lock: a FIX session must assign MsgSeqNum and
    // emit bytes in one order, so sendApplication() has to be
    // serialized, but nothing else does. Formatting the body under the
    // lock made every sender wait on every other sender's string work.
    const std::string body = buildBody(shape_, planned, nonce, target);
    const char* msgType = msgTypeOf(planned.action);
    {
      std::lock_guard<std::mutex> lock(sendMutex_);
      session_->sendApplication(msgType, body);
    }
  }

 private:
  void onReply(const hffix::message_reader& message) {
    std::int64_t correlation = -1;
    for (auto it = message.begin(); it != message.end(); ++it) {
      if (it->tag() == kClOrdIdTag) {
        correlation =
            std::strtoll(std::string(it->value().begin(), it->value().size()).c_str(), nullptr, 10);
        break;
      }
    }
    if (correlation < 0) {
      return;  // 35=9 with no ClOrdID, or a message this sender did not cause
    }
    // No FIFO fallback, unlike the counter's sender. Every report an
    // exchange sends carries the ClOrdID of the order that caused it,
    // so a reply that cannot be correlated is one this sender did not
    // ask for -- completing the oldest outstanding request with it
    // would be inventing a measurement.
    std::function<void(bool)> done;
    {
      std::lock_guard<std::mutex> lock(pendingMutex_);
      const auto it = pending_.find(correlation);
      if (it == pending_.end()) {
        // Already completed: a later fill on an order that rested, or
        // a duplicate. Not an error, and not a completion either.
        return;
      }
      done = std::move(it->second);
      pending_.erase(it);
    }
    if (done) {
      done(true);
    }
  }

  // Drains whatever has been encoded and writes it as ONE syscall.
  //
  // This is the rule the relay, output and input gateways all arrived
  // at -- gather what is available now, send once, never delay a send
  // to wait for more -- applied to the rig, which needed it as much as
  // they did. With the write inline under the send lock, adding sender
  // threads made throughput WORSE: at 2,000/s one thread carried it
  // with zero drops while two dropped 9,001 (gateway/fix/README.md).
  void writeLoop() {
    std::string batch;
    while (!stopping_.load(std::memory_order_relaxed)) {
      {
        std::lock_guard<std::mutex> lock(outMutex_);
        batch.swap(outBuffer_);
        outBuffer_.clear();
      }
      if (batch.empty()) {
        // Nothing waiting. A short sleep rather than a spin: this
        // thread is not latency-critical, it is throughput-critical,
        // and burning a core here would take one from the senders.
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        continue;
      }
      boost::system::error_code ec;
      boost::asio::write(socket_, boost::asio::buffer(batch.data(), batch.size()), ec);
      batch.clear();
      if (ec) {
        return;
      }
    }
  }

  void receiveLoop() {
    // A receive timeout so the blocking read returns periodically and
    // the loop can see stopping_. Closing the socket from another
    // thread does NOT reliably wake a thread parked in read_some --
    // the same trap that deadlocked FixInputTransport::stop() earlier,
    // reproduced here because this file was written from the same
    // assumption. Without it the load generator ran fine and then hung
    // forever at shutdown, never printing its summary.
    struct timeval timeout {};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    ::setsockopt(socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::vector<char> buffer(64 * 1024);
    while (!stopping_.load(std::memory_order_relaxed)) {
      boost::system::error_code ec;
      const std::size_t n =
          socket_.read_some(boost::asio::buffer(buffer.data(), buffer.size()), ec);
      if (ec) {
        if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
          continue;
        }
        return;
      }
      session_->onBytes(std::string_view(buffer.data(), n));
    }
  }

  class MemoryStore : public sequencer::fix::SequenceStore {
   public:
    sequencer::fix::SequenceNumbers load(const std::string& key) override { return numbers_[key]; }
    void store(const std::string& key, const sequencer::fix::SequenceNumbers& n) override {
      numbers_[key] = n;
    }

   private:
    std::map<std::string, sequencer::fix::SequenceNumbers> numbers_;
  };

  boost::asio::io_context ioContext_;
  boost::asio::ip::tcp::socket socket_;
  MemoryStore store_;
  std::unique_ptr<sequencer::fix::FixSession> session_;
  std::thread reader_;
  std::atomic<bool> stopping_{false};
  std::mutex sendMutex_;   // serializes MsgSeqNum assignment and encoding
  std::mutex outMutex_;    // guards the encoded-bytes buffer
  std::string outBuffer_;
  const std::int64_t clientId_ = 0;
  const int clientIdShift_ = 40;
  const OrderShape shape_{};
  std::thread writer_;
  std::mutex pendingMutex_;
  // std::map, not unordered_map: the FIFO fallback above needs
  // begin() to be the OLDEST outstanding request, and the harness's
  // sequence numbers are monotonic, so ordered-by-key is ordered by
  // age.
  std::map<std::int64_t, std::function<void(bool)>> pending_;
};

// Spreads one harness's requests round-robin across several FIX
// sessions.
//
// It exists to keep gateways EVENLY loaded. With one session per client
// and an odd number of clients, some gateway always draws more clients
// than another -- five clients over two gateways is a 3/2 split, so one
// carries 60% of the offered rate -- and the merged latency then blends
// a busy gateway with a quiet one, which is neither gateway's real
// number. Giving every client a session on every gateway makes the
// split exact.
class ExchangeFixFanoutRequester : public LoadGeneratorRequester {
 public:
  void add(std::unique_ptr<ExchangeFixRequester> session) { sessions_.push_back(std::move(session)); }

  void send(std::int64_t sequence, std::int64_t sendTimeUs,
             std::function<void(bool ok)> onDone) override {
    // By CYCLE, not by message. A cycle's cancel and replace reference
    // the maker sent immediately before them, and a ClOrdID is scoped
    // to the CompID that sent it, so splitting a cycle across sessions
    // would make every cancel and replace reference an order the
    // receiving session never sent -- all of them rejected, and a
    // sweep that measured rejects rather than matching. Whole cycles
    // still distribute evenly, since the harness's sequence is
    // monotonic.
    const std::size_t which =
        static_cast<std::size_t>(sequence / kCycleLength) % sessions_.size();
    sessions_[which]->send(sequence, sendTimeUs, std::move(onDone));
  }

 private:
  std::vector<std::unique_ptr<ExchangeFixRequester>> sessions_;
};

}  // namespace exchange::bench
