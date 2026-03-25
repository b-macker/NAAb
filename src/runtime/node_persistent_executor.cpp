// NAAb Node.js Persistent Executor Implementation
// Node.js process kept alive via pipes, state persists between blocks

#include "naab/node_persistent_executor.h"
#include "naab/interpreter.h"
#include <climits>
#include <stdexcept>
#include <fmt/core.h>

namespace naab {
namespace runtime {

// Node.js eval loop script passed via -e
// Reads code blocks delimited by __NAAB_CODE_END__, evals them, prints sentinel.
// Uses Function() constructor instead of eval() for multi-statement support.
static const char* NODE_EVAL_LOOP = R"JS(
const __SENTINEL = '__NAAB_BLOCK_DONE__';
const __DELIM = '__NAAB_CODE_END__';
const readline = require('readline');
const rl = readline.createInterface({ input: process.stdin, terminal: false });

let __codeBuffer = '';
let __collecting = false;

process.stdout.write(__SENTINEL + '\n');

rl.on('line', (line) => {
  if (line.trim() === __DELIM) {
    const __code = __codeBuffer;
    __codeBuffer = '';
    // Execute the collected code
    try {
      const __result = (0, eval)(__code);
      if (__result !== undefined && __result !== null) {
        if (typeof __result === 'object' || Array.isArray(__result)) {
          process.stdout.write('__NAAB_JSON__:' + JSON.stringify(__result) + '\n');
        } else {
          process.stdout.write(String(__result) + '\n');
        }
      }
    } catch(__e) {
      process.stderr.write('__NAAB_ERROR__:' + (__e.stack || __e.message || String(__e)) + '\n');
    }
    process.stdout.write(__SENTINEL + '\n');
  } else {
    __codeBuffer += line + '\n';
  }
});

rl.on('close', () => {
  process.exit(0);
});
)JS";

NodePersistentExecutor::NodePersistentExecutor()
    : PersistentProcessExecutor("node", "node", {"-e", NODE_EVAL_LOOP}) {
}

std::string NodePersistentExecutor::getSentinel() const {
    return "__NAAB_BLOCK_DONE__";
}

std::string NodePersistentExecutor::getStartupCode() const {
    // The -e code already prints the initial sentinel.
    // Return non-empty to trigger readUntilSentinel() in start().
    return "\n";
}

std::string NodePersistentExecutor::getExitCommand() const {
    return ""; // Closing stdin (in stop()) triggers 'close' event → exit
}

std::string NodePersistentExecutor::wrapCodeForExecution(const std::string& code) const {
    // Send user code followed by the code delimiter
    return code + "\n__NAAB_CODE_END__\n";
}

interpreter::NaabVal NodePersistentExecutor::parseOutput(
    const std::string& stdout_text, const std::string& stderr_text,
    int implicit_exit_code) const {

    // Check for error marker in stderr
    std::string error_prefix = "__NAAB_ERROR__:";
    if (stderr_text.find(error_prefix) != std::string::npos) {
        auto pos = stderr_text.find(error_prefix);
        std::string error_msg = stderr_text.substr(pos + error_prefix.size());
        // Trim trailing newlines
        while (!error_msg.empty() && error_msg.back() == '\n') {
            error_msg.pop_back();
        }
        throw std::runtime_error(error_msg);
    }

    // Check for JSON result marker
    std::string json_prefix = "__NAAB_JSON__:";
    std::string result = stdout_text;

    // Trim trailing whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' ||
                                result.back() == ' ' || result.back() == '\t')) {
        result.pop_back();
    }

    // Split into lines to find the last meaningful line
    // Lines before the result are console.log output
    std::string last_line;
    auto last_nl = result.rfind('\n');
    if (last_nl != std::string::npos) {
        last_line = result.substr(last_nl + 1);
    } else {
        last_line = result;
    }

    // Check if last line has JSON prefix
    if (last_line.find(json_prefix) == 0) {
        std::string json_str = last_line.substr(json_prefix.size());
        return interpreter::NaabVal::makeString(json_str);
    }

    // For multi-line output: the last line is the eval result, preceding lines are
    // console.log output. Use only the last line for value parsing.
    if (last_nl != std::string::npos && !last_line.empty()) {
        return PersistentProcessExecutor::parseOutput(last_line, stderr_text, implicit_exit_code);
    }

    // Use default parsing on the full stdout
    return PersistentProcessExecutor::parseOutput(stdout_text, stderr_text, implicit_exit_code);
}

} // namespace runtime
} // namespace naab
