#pragma once

#include <cstddef>

namespace exchange {

// docs/spec.md §4: a `Fill` carries every match of one input in a
// single output, because sequencer allows 64 outputs per input; the
// binding ceiling is then the journal's maxRecordBytes (256 KiB by
// default, input and all outputs). Admission keeps the worst case
// under it: AddInstrument is rejected unless
// maxOrderQty / lotSize <= kMaxMatchesPerInput, so a sweep can never
// produce more entries than fit. schema_test proves the arithmetic.
inline constexpr std::size_t kMaxMatchesPerInput = 1000;

// Two parties per match.
inline constexpr std::size_t kMaxFillEntriesPerInput = 2 * kMaxMatchesPerInput;

}  // namespace exchange
