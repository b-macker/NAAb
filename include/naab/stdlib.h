#pragma once

// NAAb Standard Library
// Built-in modules for common operations

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>

namespace naab {
namespace interpreter {
    class Value;  // Forward declaration
    class NaabVal;  // Forward declaration
}

namespace stdlib {

// Pipe mode: when enabled, io.write() redirects to stderr
// so stdout stays clean for machine-readable output via io.output()
void setPipeMode(bool enabled);
bool getPipeMode();

// Module interface
class Module {
public:
    virtual ~Module() = default;
    virtual std::string getName() const = 0;
    virtual interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) = 0;
    virtual bool hasFunction(const std::string& name) const = 0;

    // Returns true if the function mutates its first argument
    // Used for automatic mutation handling (array.push, etc.)
    virtual bool isMutatingFunction(const std::string& function_name) const {
        return false;  // Default: not mutating
    }
};

// IO Module - File operations
class IOModule : public Module {
public:
    std::string getName() const override { return "io"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;

private:
    interpreter::NaabVal read_file(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal write_file(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal exists(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal list_dir(
        std::vector<interpreter::NaabVal>& args);
};

// JSON Module - JSON parsing and serialization
class JSONModule : public Module {
public:
    std::string getName() const override { return "json"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;

private:
    interpreter::NaabVal parse(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal stringify(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal parse_object(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal parse_array(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal is_valid(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal pretty(
        std::vector<interpreter::NaabVal>& args);
};

// HTTP Module - HTTP client operations
class HTTPModule : public Module {
public:
    std::string getName() const override { return "http"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;

private:
    interpreter::NaabVal get(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal post(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal put(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal del(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal head(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal patch(
        std::vector<interpreter::NaabVal>& args);
};

// Collections Module - Advanced data structures
class CollectionsModule : public Module {
public:
    std::string getName() const override { return "collections"; }
    bool hasFunction(const std::string& name) const override;
    interpreter::NaabVal call(
        const std::string& function_name,
        std::vector<interpreter::NaabVal>& args) override;

private:
    interpreter::NaabVal set_create(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal set_add(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal set_contains(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal set_remove(
        std::vector<interpreter::NaabVal>& args);
    interpreter::NaabVal set_size(
        std::vector<interpreter::NaabVal>& args);
};

// Forward declarations for new stdlib modules (implemented in separate files)
class StringModule;
class ArrayModule;
class MathModule;
class TimeModule;
class EnvModule;
class CsvModule;
class RegexModule;
class CryptoModule;
class FileModule;

// Standard Library Manager
class StdLib {
public:
    StdLib();

    // Get module by name
    std::shared_ptr<Module> getModule(const std::string& name) const;

    // Check if module exists
    bool hasModule(const std::string& name) const;

    // List all available modules
    std::vector<std::string> listModules() const;

private:
    std::unordered_map<std::string, std::shared_ptr<Module>> modules_;
    void registerModules();
};

} // namespace stdlib
} // namespace naab

