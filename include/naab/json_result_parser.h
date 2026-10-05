#pragma once

#include "naab/naab_val.h"
#include <string>

namespace naab {
namespace runtime {

/**
 * Parses JSON output from language executors and converts to NAAb NaabVal.
 * Preserves types (int, string, array, object) instead of returning everything as strings.
 */
class JsonResultParser {
public:
    // Parse JSON output from language executor
    // Returns appropriate NaabVal type based on JSON content
    static interpreter::NaabVal parse(const std::string& json_output);

    // Parse simple output (non-JSON) - attempts to infer type
    static interpreter::NaabVal parseSimple(const std::string& output);
};

// Phase 12: Polyglot output parsing result
struct PolyglotOutput {
    interpreter::NaabVal return_value;  // Parsed return value (null if none)
    std::string log_output;  // Non-return stdout lines (logs/debug output)
};

// Phase 12: Parse polyglot stdout with sentinel detection and JSON scanning
PolyglotOutput parsePolyglotOutput(const std::string& stdout_output, const std::string& return_type);

// A shell block returns a ShellResult {exit_code, stdout, stderr} struct.
// Unwrap it to stdout, or THROW when the command failed. ONE implementation
// for both engines: this lived in the tree-walker alone (polyglot.cpp), so on
// the VM -- the default engine -- a failing shell command left the struct as
// the block's value and the program ran on under exit 0.
void unwrapShellResult(interpreter::NaabVal& result);

} // namespace runtime
} // namespace naab
