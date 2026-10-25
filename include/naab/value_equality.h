#pragma once
// The language's `==` and `!=` -- one definition for both engines (A72).
//
// Scalars: null equals null; bools by value; numbers numerically (5 == 5.0);
// strings by text; no coercion between types ("1" != 1, true != 1).
// Containers by value, with the same rules for every element: lists element
// by element in order, dictionaries key by key (order does not matter),
// structs by type name and fields. Anything else is unequal.
//
// Before this, both engines handled only the scalars and called every list,
// dictionary and struct unequal -- `a == a` was false for an array.

#include "naab/naab_val.h"

namespace naab {
namespace interpreter {

bool valuesEqual(const NaabVal& a, const NaabVal& b);

}  // namespace interpreter
}  // namespace naab
