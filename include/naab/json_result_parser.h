#pragma once

#include "naab/value.h"
#include <memory>
#include <string>

namespace naab {
namespace runtime {

/**
 * Parses JSON output from language executors and converts to NAAb Values.
 * Preserves types (int, string, array, object) instead of returning everything as strings.
 */
class JsonResultParser {
public:
    // Parse JSON output from language executor
    // Returns appropriate Value type based on JSON content
    static std::shared_ptr<interpreter::Value> parse(const std::string& json_output);

    // Parse simple output (non-JSON) - attempts to infer type
    static std::shared_ptr<interpreter::Value> parseSimple(const std::string& output);
};

// Phase 12: Polyglot output parsing result
struct PolyglotOutput {
    std::shared_ptr<interpreter::Value> return_value;  // Parsed return value (may be null)
    std::string log_output;  // Non-return stdout lines (logs/debug output)
};

// Phase 12: Parse polyglot stdout with sentinel detection and JSON scanning
PolyglotOutput parsePolyglotOutput(const std::string& stdout_output, const std::string& return_type);

} // namespace runtime
} // namespace naab

