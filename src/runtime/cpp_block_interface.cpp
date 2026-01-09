// NAAb C++ Block Interface Implementation
// Implements the C FFI for dynamically loaded C++ blocks

#include "naab/cpp_block_interface.h"
#include "naab/interpreter.h"
#include "naab/struct_registry.h"
#include <cstring>

// Helper to cast void* to Value*
static naab::interpreter::Value* toValue(void* value) {
    return reinterpret_cast<naab::interpreter::Value*>(value);
}

// Helper to cast Value* to void*
static void* fromValue(naab::interpreter::Value* value) {
    return reinterpret_cast<void*>(value);
}

// ============================================================================
// Struct Type Interface (Week 6 - Task 49)
// ============================================================================

const char* naab_value_get_struct_type_name(void* value) {
    if (!value) return nullptr;

    auto* val = toValue(value);
    if (!std::holds_alternative<std::shared_ptr<naab::interpreter::StructValue>>(val->data)) {
        return nullptr;
    }

    auto struct_val = std::get<std::shared_ptr<naab::interpreter::StructValue>>(val->data);
    if (!struct_val) return nullptr;

    // Return pointer to internal string (valid as long as struct_val exists)
    return struct_val->type_name.c_str();
}

int naab_value_get_struct_field_count(void* value) {
    if (!value) return -1;

    auto* val = toValue(value);
    if (!std::holds_alternative<std::shared_ptr<naab::interpreter::StructValue>>(val->data)) {
        return -1;
    }

    auto struct_val = std::get<std::shared_ptr<naab::interpreter::StructValue>>(val->data);
    if (!struct_val || !struct_val->definition) return -1;

    return static_cast<int>(struct_val->definition->fields.size());
}

const char* naab_value_get_struct_field_name(void* value, int field_index) {
    if (!value || field_index < 0) return nullptr;

    auto* val = toValue(value);
    if (!std::holds_alternative<std::shared_ptr<naab::interpreter::StructValue>>(val->data)) {
        return nullptr;
    }

    auto struct_val = std::get<std::shared_ptr<naab::interpreter::StructValue>>(val->data);
    if (!struct_val || !struct_val->definition) return nullptr;

    if (static_cast<size_t>(field_index) >= struct_val->definition->fields.size()) {
        return nullptr;
    }

    return struct_val->definition->fields[field_index].name.c_str();
}

void* naab_value_get_struct_field(void* value, const char* field_name) {
    if (!value || !field_name) return nullptr;

    auto* val = toValue(value);
    if (!std::holds_alternative<std::shared_ptr<naab::interpreter::StructValue>>(val->data)) {
        return nullptr;
    }

    auto struct_val = std::get<std::shared_ptr<naab::interpreter::StructValue>>(val->data);
    if (!struct_val || !struct_val->definition) return nullptr;

    // Find field index
    auto it = struct_val->definition->field_index.find(field_name);
    if (it == struct_val->definition->field_index.end()) {
        return nullptr;
    }

    size_t idx = it->second;
    if (idx >= struct_val->field_values.size()) {
        return nullptr;
    }

    return fromValue(struct_val->field_values[idx].get());
}

int naab_value_set_struct_field(void* struct_value, const char* field_name, void* field_value) {
    if (!struct_value || !field_name || !field_value) return -1;

    auto* val = toValue(struct_value);
    if (!std::holds_alternative<std::shared_ptr<naab::interpreter::StructValue>>(val->data)) {
        return -1;
    }

    auto struct_val = std::get<std::shared_ptr<naab::interpreter::StructValue>>(val->data);
    if (!struct_val || !struct_val->definition) return -1;

    // Find field index
    auto it = struct_val->definition->field_index.find(field_name);
    if (it == struct_val->definition->field_index.end()) {
        return -1;  // Field not found
    }

    size_t idx = it->second;
    if (idx >= struct_val->field_values.size()) {
        return -1;
    }

    // Set field value (make a copy)
    auto* fval = toValue(field_value);
    struct_val->field_values[idx] = std::make_shared<naab::interpreter::Value>(*fval);

    return 0;  // Success
}

void* naab_value_create_struct(const char* type_name) {
    if (!type_name) return nullptr;

    // Get struct definition from registry
    auto struct_def = naab::runtime::StructRegistry::instance().getStruct(type_name);
    if (!struct_def) {
        return nullptr;  // Type not registered
    }

    // Create struct value
    auto struct_val = std::make_shared<naab::interpreter::StructValue>(
        type_name, struct_def);

    // Initialize all fields to null
    for (size_t i = 0; i < struct_def->fields.size(); ++i) {
        struct_val->field_values[i] = std::make_shared<naab::interpreter::Value>();
    }

    // Wrap in Value and return
    auto* val = new naab::interpreter::Value(struct_val);
    return fromValue(val);
}
