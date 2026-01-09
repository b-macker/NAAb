//
// NAAb Standard Library - String Module
// STUB IMPLEMENTATION - Full C++ implementation pending
//

#include "naab/stdlib_new_modules.h"
#include "naab/interpreter.h"

namespace naab {
namespace stdlib {

bool StringModule::hasFunction(const std::string& name) const {
    return false;  // Stub - not implemented
}

std::shared_ptr<interpreter::Value> StringModule::call(
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {
    throw std::runtime_error("string." + function_name + "() - C++ stdlib pending, use Python");
}

} // namespace stdlib
} // namespace naab
