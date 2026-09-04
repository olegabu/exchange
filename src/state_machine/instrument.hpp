#pragma once

// Instrument static data (docs/spec.md §4, §11): sequenced in by
// AddInstrument, never compiled in or read from a file. Admission
// data -- the matcher never sees it, only integer prices and
// quantities that have already passed these rules.

#include <array>
#include <cstdint>

#include "book/order.hpp"

namespace exchange {

using InstrumentId = std::uint32_t;
using SymbolKey = std::array<char, 8>;

struct Instrument {
  InstrumentId id = 0;
  SymbolKey symbol{};
  book::Price tickSize = 0;
  book::Quantity lotSize = 0;
  book::Quantity maxOrderQty = 0;
};

}  // namespace exchange
