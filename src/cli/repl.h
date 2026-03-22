#pragma once

#include "naab/interpreter.h"
#include <string>

namespace naab {
namespace repl {

// Run the NAAb REPL (Read-Eval-Print Loop)
// Returns exit code (0 = normal exit)
int run(bool no_governance = false);

} // namespace repl
} // namespace naab
