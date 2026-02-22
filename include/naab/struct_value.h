#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include "naab/ast.h" // For ast::StructField

// GEMINI FIX: Forward declaration of interpreter::Value
namespace naab {
namespace interpreter {
    class Value;
}
}

namespace naab {
namespace interpreter {

// Forward declarations for struct types
struct StructDef;
struct StructValue;

// Struct type definition
struct StructDef {
    std::string name;
    std::vector<naab::ast::StructField> fields;
    std::unordered_map<std::string, size_t> field_index;

    StructDef() = default;
    StructDef(std::string n, std::vector<naab::ast::StructField> f)
        : name(std::move(n)), fields(std::move(f)) {
        for (size_t i = 0; i < fields.size(); ++i) {
            field_index[fields[i].name] = i;
        }
    }
};

// Struct instance value
struct StructValue {
    std::string type_name;
    std::shared_ptr<StructDef> definition; // Pointer to the struct's definition
    std::vector<std::shared_ptr<Value>> field_values; // Actual values for each field

    StructValue() = default;
    StructValue(std::string name, std::shared_ptr<StructDef> def)
        : type_name(std::move(name)), definition(def) {
        if (def) {
            field_values.resize(def->fields.size());
            for (size_t i = 0; i < field_values.size(); ++i) {
                field_values[i] = std::make_shared<Value>();
            }
        }
    }

    // Optimized: inline for zero call overhead
    inline std::shared_ptr<Value> getField(const std::string& name) const {
        if (!definition) [[unlikely]] {
            throw std::runtime_error("Struct has no definition");
        }
        auto it = definition->field_index.find(name);
        if (it == definition->field_index.end()) [[unlikely]] {
            throw std::runtime_error("Field '" + name +
                                   "' not found in struct '" + type_name + "'");
        }
        return field_values[it->second];
    }
    // Optimized: inline for zero call overhead
    inline void setField(const std::string& name, std::shared_ptr<Value> value) {
        if (!definition) [[unlikely]] {
            throw std::runtime_error("Struct has no definition");
        }
        auto it = definition->field_index.find(name);
        if (it == definition->field_index.end()) [[unlikely]] {
            throw std::runtime_error("Field '" + name +
                                   "' not found in struct '" + type_name + "'");
        }
        field_values[it->second] = value;
    }

    // Fast path: direct indexed access (bypasses hash lookup)
    inline std::shared_ptr<Value> getFieldByIndex(size_t index) const {
        if (index >= field_values.size()) [[unlikely]] {
            throw std::runtime_error("Field index out of bounds");
        }
        return field_values[index];
    }

    inline void setFieldByIndex(size_t index, std::shared_ptr<Value> value) {
        if (index >= field_values.size()) [[unlikely]] {
            throw std::runtime_error("Field index out of bounds");
        }
        field_values[index] = value;
    }

    // Get field index by name (for caching)
    inline size_t getFieldIndex(const std::string& name) const {
        if (!definition) [[unlikely]] {
            throw std::runtime_error("Struct has no definition");
        }
        auto it = definition->field_index.find(name);
        if (it == definition->field_index.end()) [[unlikely]] {
            throw std::runtime_error("Field '" + name +
                                   "' not found in struct '" + type_name + "'");
        }
        return it->second;
    }
};

} // namespace interpreter
} // namespace naab

