#include "state_machine/order_book_state_machine.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

namespace exchange {

namespace {

// Placeholder snapshot: a fixed 16-byte header, so that a node can
// snapshot and restore before the book exists. Replaced by the SBE
// snapshot records in build step 4 (spec §6).
constexpr std::array<char, 8> kSnapshotMagic{'X', 'C', 'H', 'G', 'S', 'N', 'A', 'P'};
constexpr std::uint32_t kSnapshotFormatVersion = 0;

}  // namespace

void OrderBookStateMachine::apply(std::uint64_t /*sequenceNumber*/, sequencer::Payload /*input*/,
                                  sequencer::OutputCollector& /*outputs*/) {
  // Step 3.
}

void OrderBookStateMachine::snapshotSave(sequencer::SnapshotWriter& writer) {
  writer.write(kSnapshotMagic.data(), kSnapshotMagic.size());
  writer.write(&kSnapshotFormatVersion, sizeof(kSnapshotFormatVersion));
  const std::uint32_t reserved = 0;
  writer.write(&reserved, sizeof(reserved));
}

void OrderBookStateMachine::snapshotLoad(sequencer::SnapshotReader& reader) {
  std::array<char, 8> magic{};
  reader.read(magic.data(), magic.size());
  if (magic != kSnapshotMagic) {
    throw std::runtime_error("OrderBookStateMachine::snapshotLoad: bad magic");
  }
  std::uint32_t version = 0;
  reader.read(&version, sizeof(version));
  if (version != kSnapshotFormatVersion) {
    throw std::runtime_error("OrderBookStateMachine::snapshotLoad: unsupported format version");
  }
  std::uint32_t reserved = 0;
  reader.read(&reserved, sizeof(reserved));
}

}  // namespace exchange
