#pragma once
// `naab-lang mcp` -- governed execution over the Model Context Protocol (stdio).
//
// Design, measurements and the premises behind it: docs/plan-mcp-service.md.
// Every call runs in a fresh `naab-lang` worker process; a per-caller ledger of
// behavioural-sequence events seeds each worker, so a sequence split across
// calls is still caught. No user code ever runs in the server's own process.
#include <string>

namespace naab {
namespace api {

// Serve one caller over stdin/stdout until stdin closes. `self_exe` is the
// path of this naab-lang binary, used to start workers. Returns the process
// exit code.
int runMcpServer(const std::string& self_exe);

}  // namespace api
}  // namespace naab
