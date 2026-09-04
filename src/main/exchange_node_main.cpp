// The exchange node: sequencer's node harness around the exchange's
// state machine (docs/spec.md §2). Every flag is sequencer's
// (node/src/run_node.cpp): --peer, --peers, --data_dir, --group, ...
#include <memory>

#include <sequencer/node.hpp>

#include "state_machine/order_book_state_machine.hpp"

int main(int argc, char** argv) {
  return sequencer::RunNode(argc, argv, std::make_unique<exchange::OrderBookStateMachine>());
}
