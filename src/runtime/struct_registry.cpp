#include "naab/struct_registry.h"
#include "naab/ast.h"
#include <stdexcept>
#include <fmt/core.h>

namespace naab {
namespace runtime {

StructRegistry& StructRegistry::instance() {
    static StructRegistry instance;
    return instance;
}

void StructRegistry::registerStruct(std::shared_ptr<interpreter::StructDef> def) {
    std::lock_guard<std::mutex> lock(mutex_);

    // ISS-036 FIX: Allow idempotent registration for module system
    if (structs_.count(def->name)) {
        auto& existing = structs_[def->name];

        // Validate that the duplicate has the same definition
        bool same_definition = true;
        std::string mismatch_reason;

        // Check field count
        if (existing->fields.size() != def->fields.size()) {
            same_definition = false;
            mismatch_reason = fmt::format("field count mismatch ({} vs {})",
                                         existing->fields.size(), def->fields.size());
        }
        // Check field names and types
        else {
            for (size_t i = 0; i < existing->fields.size(); i++) {
                if (existing->fields[i].name != def->fields[i].name) {
                    same_definition = false;
                    mismatch_reason = fmt::format("field[{}] name mismatch ('{}' vs '{}')",
                                                 i, existing->fields[i].name, def->fields[i].name);
                    break;
                }
                // Note: Type comparison is complex, so we only check names for now
                // Full type comparison would require deep type equality checking
            }
        }

        if (same_definition) {
            // Same struct definition - safe to ignore (normal for module reuse, silent)
        } else {
            // Different struct definitions with same name - warn user but allow it
            // (First definition wins, this is for backward compatibility)
            fmt::print("[WARN] Struct '{}' already registered with different definition!\n", def->name);
            fmt::print("[WARN]   Reason: {}\n", mismatch_reason);
            fmt::print("[WARN]   Using existing definition (first one wins)\n");
            fmt::print("[WARN]   This may indicate a naming conflict between modules\n");
        }

        return;  // Skip re-registration
    }

    // First time registration
    structs_[def->name] = def;
    // Registered new struct (silent)
}

std::shared_ptr<interpreter::StructDef> StructRegistry::getStruct(
    const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = structs_.find(name);
    if (it != structs_.end()) {
        return it->second;
    }
    return nullptr;
}

bool StructRegistry::hasStruct(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return structs_.count(name) > 0;
}

bool StructRegistry::validateStructDef(const interpreter::StructDef& def,
                                       std::set<std::string>& visiting) {
    if (visiting.count(def.name)) {
        throw std::runtime_error("Circular struct dependency detected: " +
                               def.name);
    }
    visiting.insert(def.name);

    for (const auto& field : def.fields) {
        if (field.type.kind == ast::TypeKind::Struct) {
            auto dep = getStruct(field.type.getStructName());
            if (dep) {
                validateStructDef(*dep, visiting);
            }
        }
    }

    visiting.erase(def.name);
    return true;
}

void StructRegistry::clearForTesting() {
    std::lock_guard<std::mutex> lock(mutex_);
    structs_.clear();
}

} // namespace runtime
} // namespace naab
