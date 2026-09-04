// Step 1 smoke: the empty state machine honours the StateMachine
// contract -- apply emits nothing, a snapshot round-trips, a corrupt
// snapshot is refused rather than silently loaded.
#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>

#include <sequencer/temp_dir.hpp>

#include "state_machine/order_book_state_machine.hpp"

namespace {

TEST(OrderBookStateMachine, ApplyEmitsNothingYet) {
  exchange::OrderBookStateMachine sm;
  sequencer::OutputCollector outputs;
  const std::byte input[4]{};
  sm.apply(1, sequencer::Payload(input, sizeof(input)), outputs);
  EXPECT_EQ(outputs.outputs().size(), 0u);
}

TEST(OrderBookStateMachine, SnapshotRoundTrips) {
  const auto dir = sequencer::makeTempDir("exchange-snapshot");
  const auto path = dir / "snapshot";
  {
    exchange::OrderBookStateMachine sm;
    sequencer::SnapshotWriter writer(path);
    sm.snapshotSave(writer);
  }
  // Header record (4 + 8 + 28) and end record (4 + 8 + 8): the empty snapshot, frozen.
  EXPECT_EQ(std::filesystem::file_size(path), 60u);
  exchange::OrderBookStateMachine restored;
  sequencer::SnapshotReader reader(path);
  EXPECT_NO_THROW(restored.snapshotLoad(reader));
  std::filesystem::remove_all(dir);
}

TEST(OrderBookStateMachine, RefusesACorruptSnapshot) {
  const auto dir = sequencer::makeTempDir("exchange-snapshot");
  const auto path = dir / "snapshot";
  {
    std::ofstream out(path, std::ios::binary);
    out << "NOTASNAPSHOT....";
  }
  exchange::OrderBookStateMachine sm;
  sequencer::SnapshotReader reader(path);
  EXPECT_THROW(sm.snapshotLoad(reader), std::runtime_error);
  std::filesystem::remove_all(dir);
}

}  // namespace
