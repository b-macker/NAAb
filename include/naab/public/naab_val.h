#pragma once
// NAAb Value Type — Public API
// NaabVal: NaN-boxed 8-byte value representing null, bool, int, double,
// string, array, dict, function, struct, or future.
//
// Re-exports naab::interpreter::NaabVal as naab::Val for clean embedding.
// Stable within minor versions (0.7.x).

#include "naab/naab_val.h"

namespace naab {

// Re-export into top-level naab:: namespace for clean embedding API
using Val = interpreter::NaabVal;

} // namespace naab
