#ifndef NAAB_STDLIB_H
#define NAAB_STDLIB_H

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional> // For std::function

// GEMINI FIX: Forward declarations to break circular dependency
namespace naab {
namespace interpreter {
    class Interpreter; // Forward declare Interpreter
    class Value;       // Forward declare Value
}
}

namespace naab {
namespace stdlib {

// Base class for all standard library modules
class Module {
public:
    virtual ~Module() = default;
    virtual std::string getName() const = 0; // GEMINI FIX: Added virtual getName()
    virtual bool hasFunction(const std::string& name) const = 0;
    virtual std::shared_ptr<naab::interpreter::Value> call( // GEMINI FIX: Use naab::interpreter::Value
        const std::string& function_name,
        const std::vector<std::shared_ptr<naab::interpreter::Value>>& args) = 0; // GEMINI FIX: Use naab::interpreter::Value
};

// Standard Library Manager
class StdLib {
public:
    // GEMINI FIX: Constructor now takes naab::interpreter::Interpreter pointer
    StdLib(naab::interpreter::Interpreter* interpreter);
    void registerModules();
    std::shared_ptr<Module> getModule(const std::string& name) const;
    bool hasModule(const std::string& name) const;
    std::vector<std::string> listModules() const;

private:
    std::unordered_map<std::string, std::shared_ptr<Module>> modules_;
    // GEMINI FIX: Store pointer to naab::interpreter::Interpreter
    naab::interpreter::Interpreter* interpreter_;
};

} // namespace stdlib
} // namespace naab

#endif // NAAB_STDLIB_H