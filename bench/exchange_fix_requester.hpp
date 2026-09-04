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

// The shape of the order flow. Prices step around a mid so that a
// steady stream both rests and crosses: with kPriceLevels levels and
// alternating sides, roughly half of each side's orders find a resting
// counterparty, which is what makes the measurement exercise matching
// rather than a book that only ever grows.
struct OrderShape {
  std::string symbol = "ABC";
  std::int64_t midTicks = 10000;   // 100.00 at a 0.01 tick
  std::int64_t priceLevels = 5;    // ticks either side of the mid
  std::int64_t quantityLots = 1;
};

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
      // sequence, for the same reason the counter's sender is: replies
      // arrive in journal order, which is not request order.
      std::lock_guard<std::mutex> lock(pendingMutex_);
      pending_[nonce] = std::move(onDone);
    }
    // Built OUTSIDE the lock: a FIX session must assign MsgSeqNum and
    // emit bytes in one order, so sendApplication() has to be
    // serialized, but nothing else does. Formatting the body under the
    // lock made every sender wait on every other sender's string work.
    const std::string body = buildOrder(nonce, sequence);
    {
      std::lock_guard<std::mutex> lock(sendMutex_);
      session_->sendApplication("D", body);
    }
  }

  // One NewOrderSingle. Sides alternate and the price walks the levels
  // around the mid, so the flow both rests and crosses.
  std::string buildOrder(std::int64_t nonce, std::int64_t sequence) const {
    const bool buy = (sequence & 1) == 0;
    const std::int64_t step = sequence % shape_.priceLevels;
    const std::int64_t ticks = buy ? shape_.midTicks - step : shape_.midTicks + step;
    // Ticks of 0.01 -> a decimal string, without floating point.
    std::string price = std::to_string(ticks / 100);
    const std::int64_t cents = ticks % 100;
    if (cents != 0) {
      price += '.';
      if (cents < 10) {
        price += '0';
      }
      std::string digits = std::to_string(cents);
      while (digits.size() > 1 && digits.back() == '0') {
        digits.pop_back();
      }
      price += digits;
    }
    return "11=" + std::to_string(nonce) + "\00155=" + shape_.symbol + "\00154=" + (buy ? "1" : "2") +
           "\00160=20260904-00:00:00\00138=" + std::to_string(shape_.quantityLots) + "\00140=2\00144=" + price +
           "\00159=0\001";
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
    // By sequence, not by a shared counter: the harness's sequence is
    // already monotonic, so this needs no synchronization of its own
    // and distributes exactly evenly.
    const std::size_t which =
        static_cast<std::size_t>(sequence) % sessions_.size();
    sessions_[which]->send(sequence, sendTimeUs, std::move(onDone));
  }

 private:
  std::vector<std::unique_ptr<ExchangeFixRequester>> sessions_;
};

}  // namespace exchange::bench
