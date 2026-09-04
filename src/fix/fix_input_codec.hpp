#pragma once

// FIX 4.4 -> SBE, the input side. See fix_codecs.hpp for the mapping.

#include <optional>
#include <span>

#include <sequencer/input_codec.hpp>

namespace exchange::fix {

class ExchangeFixInputCodec final : public sequencer::InputCodec {
 public:
  sequencer::Result<sequencer::Bytes> toInput(const sequencer::ClientRequest& request) override;
  // Nothing goes back on the propose path for a session transport
  // (§8.11): the transport discards this. --inline_designated_outputs
  // is not offered by exchange_fix_gateway: it is sound only where every
  // output a session receives comes from its own inputs, and a passive
  // fill is exactly not that.
  sequencer::Bytes toOutput(const sequencer::Receipt& receipt,
                            std::span<const sequencer::Payload> designatedOutputs) override;
  std::optional<sequencer::Bytes> onDisconnect(const sequencer::SessionInfo& session) override;
};

}  // namespace exchange::fix
