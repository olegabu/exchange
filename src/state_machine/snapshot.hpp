#pragma once

// Snapshot framing (docs/spec.md §6). A snapshot is a sequence of SBE
// messages from schema/exchange.xml -- SnapshotHeader, then per
// instrument an InstrumentSnapshot followed by its resting orders as
// OrderSnapshots in book order (bids best-first, asks best-first, FIFO
// within a price), then SnapshotEnd -- each prefixed by its length as
// a little-endian uint32, because sequencer's SnapshotReader hands
// back exactly the byte count it is asked for and an SBE message's
// length is only known once its header has been read.
//
// Restore re-inserts every order through the same Book::add() a live
// order takes, so a restored book cannot differ structurally from one
// that never restarted; and a snapshot whose orders would match on
// re-insertion is refused, because a live book is never crossed.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

#include <sequencer/state_machine.hpp>

namespace exchange::snapshot {

inline constexpr std::uint32_t kFormatVersion = 1;
// Larger than any snapshot record; a length above it is corruption,
// not a big record.
inline constexpr std::uint32_t kMaxRecordBytes = 4096;

inline void writeRecord(sequencer::SnapshotWriter& writer, std::span<const std::byte> record) {
  const auto length = static_cast<std::uint32_t>(record.size());
  writer.write(&length, sizeof(length));
  writer.write(record.data(), record.size());
}

// Reads one length-prefixed record into `buffer` (resized to fit) and
// returns it. Throws on a length that cannot be a record, or at EOF
// (SnapshotReader raises on a short read).
inline std::span<const std::byte> readRecord(sequencer::SnapshotReader& reader, std::vector<std::byte>& buffer) {
  std::uint32_t length = 0;
  reader.read(&length, sizeof(length));
  if (length == 0 || length > kMaxRecordBytes) {
    throw std::runtime_error("snapshot: record length out of range");
  }
  buffer.resize(length);
  reader.read(buffer.data(), length);
  return {buffer.data(), length};
}

}  // namespace exchange::snapshot
