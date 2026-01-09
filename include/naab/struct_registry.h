#ifndef NAAB_STRUCT_REGISTRY_H
#define NAAB_STRUCT_REGISTRY_H

#include "naab/interpreter.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <set>

namespace naab {
namespace runtime {

class StructRegistry {
public:
    static StructRegistry& instance();

    void registerStruct(std::shared_ptr<interpreter::StructDef> def);
    std::shared_ptr<interpreter::StructDef> getStruct(const std::string& name) const;
    bool hasStruct(const std::string& name) const;

    // Cycle detection
    bool validateStructDef(const interpreter::StructDef& def,
                          std::set<std::string>& visiting);

    // For testing: clear all registered structs
    void clearForTesting();

private:
    StructRegistry() = default;
    StructRegistry(const StructRegistry&) = delete;
    StructRegistry& operator=(const StructRegistry&) = delete;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<interpreter::StructDef>> structs_;
};

} // namespace runtime
} // namespace naab

#endif // NAAB_STRUCT_REGISTRY_H
