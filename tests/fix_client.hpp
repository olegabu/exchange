#pragma once

// A FIX 4.4 initiator on a socket, for end-to-end tests: sequencer's
// session core in the initiator role (the same code its gateway runs
// as acceptor), pumped by hand. Adapted from
// examples/counter/tests/fix_gateway_test.cpp. Every application
// message received is kept as an ordered list of (tag, value) pairs.

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <hffix.hpp>

#include <sequencer/fix/fix_session.hpp>

namespace exchange::test {

namespace net = boost::asio;
using tcp = net::ip::tcp;

class MemoryStore : public sequencer::fix::SequenceStore {
 public:
  sequencer::fix::SequenceNumbers load(const std::string& k) override { return n_[k]; }
  void store(const std::string& k, const sequencer::fix::SequenceNumbers& v) override { n_[k] = v; }

 private:
  std::map<std::string, sequencer::fix::SequenceNumbers> n_;
};

struct FixMessage {
  std::string type;
  std::vector<std::pair<int, std::string>> fields;
  std::string get(int tag) const {
    for (const auto& [t, v] : fields) {
      if (t == tag) return v;
    }
    return "";
  }
  bool has(int tag) const {
    for (const auto& [t, v] : fields) {
      if (t == tag) return true;
    }
    return false;
  }
};

class FixClient {
 public:
  // `store` outlives the client so a reconnecting client keeps its
  // sequence numbers, as a real one would.
  FixClient(int port, std::string compId, MemoryStore& store, std::string targetCompId = "EXCHANGE")
      : socket_(ioContext_) {
    tcp::resolver resolver(ioContext_);
    net::connect(socket_, resolver.resolve("127.0.0.1", std::to_string(port)));
    sequencer::fix::SessionConfig config;
    config.role = sequencer::fix::Role::Initiator;
    config.senderCompId = std::move(compId);
    config.targetCompId = std::move(targetCompId);
    config.heartBtInt = 30;
    session_ = std::make_unique<sequencer::fix::FixSession>(config, store, [] {
      return static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
              .count());
    });
    session_->setSendFn([this](std::string_view f) { net::write(socket_, net::buffer(f.data(), f.size())); });
    session_->setAppMessageFn([this](const hffix::message_reader& m) {
      FixMessage msg;
      const auto t = m.message_type();
      if (t != m.end()) {
        msg.type.assign(t->value().begin(), t->value().size());
      }
      for (auto it = m.begin(); it != m.end(); ++it) {
        msg.fields.emplace_back(it->tag(), std::string(it->value().begin(), it->value().size()));
      }
      received_.push_back(std::move(msg));
    });
  }

  ~FixClient() {
    boost::system::error_code ec;
    socket_.close(ec);
  }

  void logon() {
    session_->start();
    pumpFor(std::chrono::milliseconds(2000), [this] { return session_->isLoggedOn(); });
  }
  void logout() {
    session_->logout();
    pumpFor(std::chrono::milliseconds(500));
  }
  bool isLoggedOn() const { return session_->isLoggedOn(); }

  void send(std::string_view msgType, std::string body) { session_->sendApplication(msgType, body); }

  // 35=D: ClOrdID, Symbol, Side ('1'/'2'), OrderQty, Price ("" for market), OrdType, TIF.
  void newOrder(const std::string& clOrdId, const std::string& symbol, char side, const std::string& qty,
                const std::string& price, char ordType = '2', char tif = '0', const std::string& account = "") {
    std::string body = "11=" + clOrdId + "\001";
    if (!account.empty()) body += "1=" + account + "\001";
    body += "55=" + symbol + "\00154=" + std::string(1, side) + "\00160=20260904-00:00:00\00138=" + qty +
            "\00140=" + std::string(1, ordType) + "\001";
    if (!price.empty()) body += "44=" + price + "\001";
    body += "59=" + std::string(1, tif) + "\001";
    send("D", body);
  }
  void cancel(const std::string& clOrdId, const std::string& orig, const std::string& symbol, char side) {
    send("F", "41=" + orig + "\00111=" + clOrdId + "\00155=" + symbol + "\00154=" + std::string(1, side) +
                  "\00160=20260904-00:00:00\001");
  }
  void replace(const std::string& clOrdId, const std::string& orig, const std::string& symbol, char side,
               const std::string& qty, const std::string& price) {
    send("G", "41=" + orig + "\00111=" + clOrdId + "\00155=" + symbol + "\00154=" + std::string(1, side) +
                  "\00160=20260904-00:00:00\00138=" + qty + "\00140=2\00144=" + price + "\001");
  }
  void requestResend(std::uint64_t begin, std::uint64_t end) {
    send("2", "7=" + std::to_string(begin) + "\00116=" + std::to_string(end) + "\001");
  }

  void pumpFor(std::chrono::milliseconds budget, const std::function<bool()>& until = [] { return false; }) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    std::vector<char> buffer(65536);
    socket_.non_blocking(true);
    while (std::chrono::steady_clock::now() < deadline) {
      boost::system::error_code ec;
      const std::size_t n = socket_.read_some(net::buffer(buffer.data(), buffer.size()), ec);
      if (!ec && n > 0) {
        raw_.append(buffer.data(), n);
        session_->onBytes(std::string_view(buffer.data(), n));
      }
      session_->poll();
      if (until()) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  const std::vector<FixMessage>& received() const { return received_; }
  std::size_t count(std::string_view type) const {
    std::size_t n = 0;
    for (const auto& m : received_) n += m.type == type;
    return n;
  }
  const sequencer::fix::SequenceNumbers& sequences() const { return session_->sequences(); }

  // Every frame that arrived on the socket, parsed independently of the
  // session core -- which (correctly) drops a PossDup resend of a
  // message it has already processed before the app callback. A resend
  // is therefore asserted here, where a client's engine sees it.
  std::vector<FixMessage> rawFrames() const {
    std::vector<FixMessage> frames;
    std::size_t pos = 0;
    while (true) {
      const auto start = raw_.find("8=FIX.4.4\001", pos);
      if (start == std::string::npos) break;
      const auto end = raw_.find("\00110=", start);
      if (end == std::string::npos) break;
      const auto stop = raw_.find('\001', end + 1);
      if (stop == std::string::npos) break;
      FixMessage m;
      std::size_t i = start;
      while (i <= stop) {
        const auto eq = raw_.find('=', i);
        const auto soh = raw_.find('\001', eq);
        if (eq == std::string::npos || soh == std::string::npos || soh > stop) break;
        const int tag = std::stoi(raw_.substr(i, eq - i));
        const std::string value = raw_.substr(eq + 1, soh - eq - 1);
        if (tag == 35) m.type = value;
        m.fields.emplace_back(tag, value);
        i = soh + 1;
      }
      frames.push_back(std::move(m));
      pos = stop + 1;
    }
    return frames;
  }

 private:
  net::io_context ioContext_;
  tcp::socket socket_;
  std::unique_ptr<sequencer::fix::FixSession> session_;
  std::vector<FixMessage> received_;
  std::string raw_;
};

}  // namespace exchange::test
