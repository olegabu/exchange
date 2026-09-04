#pragma once

// FIX 4.4 <-> the exchange's SBE messages (docs/spec.md §7). The
// gateway is sequencer's hffix session gateway, journal flavour,
// unchanged; this repository supplies only these two codecs.
//
//   35=D NewOrderSingle           -> NewOrder
//   35=F OrderCancelRequest       -> CancelOrder
//   35=G OrderCancelReplaceRequest-> ReplaceOrder
//
//   OrderAccepted        -> 35=8 150=0   Fill entry      -> 35=8 150=F
//   OrderRejected        -> 35=8 150=8   OrderCancelled  -> 35=8 150=4
//   OrderReplaced        -> 35=8 150=5   *Rejected cancel/replace -> 35=9
//
// Execution reports reach the client from the journal, once, in
// journal order (sequencer §8.11); the output codec is a pure function
// of the record, which is what lets a ResendRequest be served by
// re-running it. Two libraries, one per direction, so a binary links
// only the chassis it needs (design.md §2).

#include "fix/fix_input_codec.hpp"
#include "fix/fix_output_codec.hpp"
