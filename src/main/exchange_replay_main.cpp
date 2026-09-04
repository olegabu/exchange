// sequencer's determinism gate (its specification.md §11) over the
// exchange: re-apply a node's journal through a fresh state machine and
// byte-compare every record. Flags are sequencer's: --data_dir,
// --replay_output_dir.
#include <memory>

#include <sequencer/replay.hpp>

#include "state_machine/order_book_state_machine.hpp"

int main(int argc, char** argv) {
  return sequencer::RunReplayCheck(argc, argv, std::make_unique<exchange::OrderBookStateMachine>());
}
