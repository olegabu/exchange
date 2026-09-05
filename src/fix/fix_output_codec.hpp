#pragma once

// SBE -> FIX 4.4, the output side. See fix_codecs.hpp for the mapping.

#include <cstdint>
#include <optional>

#include <sequencer/output_codec.hpp>

namespace exchange::fix {

// Delivers only the outputs addressed to ITS OWN gateway, and strips
// the gateway half of the id before handing it to the transport. See
// fix_input_codec.hpp for why the id is composed that way.
class ExchangeFixOutputCodec final : public sequencer::OutputCodec {
 public:
  explicit ExchangeFixOutputCodec(std::uint32_t gatewayId = 0) : gatewayId_(gatewayId) {}

  void toOutput(const sequencer::journal::RecordView& record, sequencer::Fanout& fanout) override;

 private:
  // Returns the connection id if this gateway owns the session, or
  // nothing if the output belongs to another gateway's client.
  std::optional<sequencer::SessionId> mine(std::uint64_t wireSessionId) const {
    if (static_cast<std::uint32_t>(wireSessionId >> 32) != gatewayId_) {
      return std::nullopt;
    }
    return static_cast<sequencer::SessionId>(wireSessionId & 0xFFFFFFFFull);
  }

  std::uint32_t gatewayId_ = 0;
};

}  // namespace exchange::fix
