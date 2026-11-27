// debug_local.h -- one local as the VM reports it to the debug module (F46).
//
// debug.env() and debug.snapshot() read the running function's locals. On the
// VM they come from the frame (VM::debugScope()), each with the run-time taint
// a load of it would carry -- its slot, or its container -- so what the debug
// module copies out cannot reach a sink as clean data.
#ifndef NAAB_DEBUG_LOCAL_H
#define NAAB_DEBUG_LOCAL_H

#include <string>
#include "naab_val.h"

namespace naab {
namespace stdlib {

struct DebugLocal {
    std::string name;
    interpreter::NaabVal value;
    bool tainted = false;
};

}  // namespace stdlib
}  // namespace naab

#endif  // NAAB_DEBUG_LOCAL_H
