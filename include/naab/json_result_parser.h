#ifndef NAAB_JSON_RESULT_PARSER_H
#define NAAB_JSON_RESULT_PARSER_H

#include "naab/value.h"
#include <nlohmann/json.hpp>
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

private:
    static std::shared_ptr<interpreter::Value> parseValue(const nlohmann::json& j);
};

} // namespace runtime
} // namespace naab

#endif // NAAB_JSON_RESULT_PARSER_H
