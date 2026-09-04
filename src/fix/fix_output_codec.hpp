#pragma once

// SBE -> FIX 4.4, the output side. See fix_codecs.hpp for the mapping.

#include <sequencer/output_codec.hpp>

namespace exchange::fix {

class ExchangeFixOutputCodec final : public sequencer::OutputCodec {
 public:
  void toOutput(const sequencer::journal::RecordView& record, sequencer::Fanout& fanout) override;
};

}  // namespace exchange::fix
