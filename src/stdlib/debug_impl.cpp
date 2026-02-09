// NAAb Standard Library - Debug Module
// Provides debugging utilities for inspecting complex types

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"
#include "naab/utils/error_formatter.h"
#include <sstream>
#include <iomanip>

namespace naab {
namespace stdlib {

// Helper to serialize any value to a debug string
static std::string valueToDebugString(const std::shared_ptr<interpreter::Value>& val, int indent = 0) {
    std::ostringstream oss;
    std::string indent_str(static_cast<size_t>(indent * 2), ' ');

    if (!val) {
        return "null";
    }

    if (std::holds_alternative<int>(val->data)) {
        oss << std::get<int>(val->data);
    } else if (std::holds_alternative<double>(val->data)) {
        oss << std::fixed << std::setprecision(2) << std::get<double>(val->data);
    } else if (std::holds_alternative<bool>(val->data)) {
        oss << (std::get<bool>(val->data) ? "true" : "false");
    } else if (std::holds_alternative<std::string>(val->data)) {
        oss << "\"" << std::get<std::string>(val->data) << "\"";
    } else if (std::holds_alternative<std::monostate>(val->data)) {
        oss << "null";
    } else if (std::holds_alternative<std::vector<std::shared_ptr<interpreter::Value>>>(val->data)) {
        // Array
        const auto& arr = std::get<std::vector<std::shared_ptr<interpreter::Value>>>(val->data);
        oss << "[";
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << valueToDebugString(arr[i], indent + 1);
        }
        oss << "]";
    } else if (std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>(val->data)) {
        // Dictionary
        const auto& dict = std::get<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>(val->data);
        oss << "{\n";
        bool first = true;
        for (const auto& [key, value] : dict) {
            if (!first) oss << ",\n";
            first = false;
            oss << indent_str << "  \"" << key << "\": " << valueToDebugString(value, indent + 1);
        }
        oss << "\n" << indent_str << "}";
    } else if (std::holds_alternative<std::shared_ptr<interpreter::StructValue>>(val->data)) {
        // Struct
        const auto& struct_val = std::get<std::shared_ptr<interpreter::StructValue>>(val->data);
        oss << struct_val->type_name << " {\n";
        for (size_t i = 0; i < struct_val->field_values.size(); ++i) {
            if (i < struct_val->definition->fields.size()) {
                const auto& field = struct_val->definition->fields[i];
                oss << indent_str << "  " << field.name << ": "
                    << valueToDebugString(struct_val->field_values[i], indent + 1);
                if (i < struct_val->field_values.size() - 1) oss << ",";
                oss << "\n";
            }
        }
        oss << indent_str << "}";
    } else if (std::holds_alternative<std::shared_ptr<interpreter::FunctionValue>>(val->data)) {
        const auto& func = std::get<std::shared_ptr<interpreter::FunctionValue>>(val->data);
        oss << "[Function: " << func->name << "(";
        for (size_t i = 0; i < func->params.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << func->params[i];
        }
        oss << ")]";
    } else {
        oss << "[Complex Type]";
    }

    return oss.str();
}

// Debug Module Implementation
bool DebugModule::hasFunction(const std::string& name) const {
    return name == "inspect" || name == "type";
}

std::shared_ptr<interpreter::Value> DebugModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    if (function_name == "inspect") {
        // debug.inspect(value) - Pretty-print any value
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "debug.inspect",
                    {"value"},
                    1,
                    static_cast<int>(args.size())
                )
            );
        }

        std::string debug_str = valueToDebugString(args[0]);
        return std::make_shared<interpreter::Value>(debug_str);

    } else if (function_name == "type") {
        // debug.type(value) - Get the type name of a value
        if (args.size() != 1) {
            throw std::runtime_error(
                utils::ErrorFormatter::formatArgumentError(
                    "debug.type",
                    {"value"},
                    1,
                    static_cast<int>(args.size())
                )
            );
        }

        const auto& val = args[0];
        std::string type_name;

        if (std::holds_alternative<int>(val->data)) {
            type_name = "int";
        } else if (std::holds_alternative<double>(val->data)) {
            type_name = "float";
        } else if (std::holds_alternative<bool>(val->data)) {
            type_name = "bool";
        } else if (std::holds_alternative<std::string>(val->data)) {
            type_name = "string";
        } else if (std::holds_alternative<std::monostate>(val->data)) {
            type_name = "null";
        } else if (std::holds_alternative<std::vector<std::shared_ptr<interpreter::Value>>>(val->data)) {
            type_name = "array";
        } else if (std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<interpreter::Value>>>(val->data)) {
            type_name = "dict";
        } else if (std::holds_alternative<std::shared_ptr<interpreter::StructValue>>(val->data)) {
            const auto& struct_val = std::get<std::shared_ptr<interpreter::StructValue>>(val->data);
            type_name = "struct:" + struct_val->type_name;
        } else if (std::holds_alternative<std::shared_ptr<interpreter::FunctionValue>>(val->data)) {
            type_name = "function";
        } else {
            type_name = "unknown";
        }

        return std::make_shared<interpreter::Value>(type_name);
    }

    throw std::runtime_error("Unknown debug function: " + function_name);
}

} // namespace stdlib
} // namespace naab
