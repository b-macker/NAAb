#include "naab/struct_registry.h"
#include "naab/ast.h"
#include <stdexcept>

namespace naab {
namespace runtime {

StructRegistry& StructRegistry::instance() {
    static StructRegistry instance;
    return instance;
}

void StructRegistry::registerStruct(std::shared_ptr<interpreter::StructDef> def) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (structs_.count(def->name)) {
        throw std::runtime_error("Struct '" + def->name + "' already defined");
    }
    structs_[def->name] = def;
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
