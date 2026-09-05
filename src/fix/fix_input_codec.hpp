#pragma once

// FIX 4.4 -> SBE, the input side. See fix_codecs.hpp for the mapping.

#include <cstdint>
#include <optional>
#include <span>

#include <sequencer/input_codec.hpp>

namespace exchange::fix {

// A gateway's session ids are a counter starting at 1 INSIDE THAT
// PROCESS (sequencer's FixInputTransport::nextSessionId), while the
// journal is shared by every gateway tailing it. So with two gateways
// running, both hand out session 1, and an output addressed to
// "session 1" is delivered by both -- once to the right client and once
// to somebody else's. Measured: two gateways lost ~30% of replies as
// unanswered.
//
// The fix is to make the id that goes ON THE WIRE globally unique, by
// composing it with an operator-assigned gateway id:
//
//     wire sessionId = (gatewayId << 32) | connectionId
//
// The output codec (which is given the same id) delivers only what its
// own high half addresses, and strips the half back off before handing
// the id to the transport. Both halves live in this repository, so no
// change to sequencer is needed.
//
// It does not fix the reconnect gap (olegabu/sequencer#1): the low half
// is still a per-connection counter, so it is reused after a restart.
class ExchangeFixInputCodec final : public sequencer::InputCodec {
 public:
  explicit ExchangeFixInputCodec(std::uint32_t gatewayId = 0) : gatewayId_(gatewayId) {}

  sequencer::Result<sequencer::Bytes> toInput(const sequencer::ClientRequest& request) override;
  // Nothing goes back on the propose path for a session transport
  // (§8.11): the transport discards this. --inline_designated_outputs
  // is not offered by exchange_fix_gateway: it is sound only where every
  // output a session receives comes from its own inputs, and a passive
  // fill is exactly not that.
  sequencer::Bytes toOutput(const sequencer::Receipt& receipt,
                            std::span<const sequencer::Payload> designatedOutputs) override;
  std::optional<sequencer::Bytes> onDisconnect(const sequencer::SessionInfo& session) override;

  // The wire id for a connection on this gateway.
  static std::uint64_t wireSessionId(std::uint32_t gatewayId, std::uint64_t connectionId) {
    return (static_cast<std::uint64_t>(gatewayId) << 32) | (connectionId & 0xFFFFFFFFull);
  }

 private:
  std::uint32_t gatewayId_ = 0;
};

}  // namespace exchange::fix
