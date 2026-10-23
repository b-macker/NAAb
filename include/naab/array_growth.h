#pragma once
// ============================================================
// array_growth.h -- the one check every array growth site calls (D10)
//
// `limits.array_size` (limits.data.array_size) is a HARD limit, enforced by
// GovernanceEngine::checkArraySize() through enforce(). It used to be called
// on list literals, concatenation (#206) and the VM's `a..b` only, while
// .push/.append/.add, .unshift, .insert, array.push, array.unshift and range()
// called the fixed 10,000,000-element cap alone (range() not even that: it
// built its whole list first). Observed with the limit at 1000: each of those
// grew to 1500 and the run exited 0 under "Governance: PASS".
//
// Call it with the size the array is ABOUT to have, before allocating. It
// applies the fixed cap (throws) and then the configured limit; a non-empty
// return is a violation the caller must raise in its engine's own way (the VM
// through runtimeError, the tree-walker and the stdlib by throwing). A HARD
// block has already thrown GovernanceHardError by then.
//
// `gov` may be null (no engine on this path) and the engine may be inactive
// (`mode: "off"`); both leave only the fixed cap.
// ============================================================
#include "naab/governance.h"
#include "naab/limits.h"
#include <cstddef>
#include <string>

namespace naab {

inline std::string checkArrayGrowth(governance::GovernanceEngine* gov, size_t new_size) {
    limits::checkArraySize(new_size);
    if (gov && gov->isActive()) return gov->checkArraySize(new_size);
    return "";
}

}  // namespace naab
