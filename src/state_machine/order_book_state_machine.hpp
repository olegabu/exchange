#pragma once

// The exchange's state machine (docs/spec.md §2, §5, §6): the one
// apply() that sequencer replicates. Identity, admission and output
// encoding live here; matching lives in src/book and knows nothing
// about sessions, accounts or FIX (spec §8, the layer rule).
//
// Build step 1: an empty state machine that compiles and runs a node.
// Nothing matches yet.

#include <cstdint>

#include <sequencer/state_machine.hpp>

namespace exchange {

class OrderBookStateMachine : public sequencer::StateMachine {
 public:
  void apply(std::uint64_t sequenceNumber, sequencer::Payload input,
             sequencer::OutputCollector& outputs) override;

  void snapshotSave(sequencer::SnapshotWriter& writer) override;
  void snapshotLoad(sequencer::SnapshotReader& reader) override;
};

}  // namespace exchange
