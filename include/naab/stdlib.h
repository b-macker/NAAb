#ifndef NAAB_STDLIB_H
#define NAAB_STDLIB_H

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
}

namespace stdlib {

// Module interface
class Module {
public:
    virtual ~Module() = default;
    virtual std::string getName() const = 0;
    virtual std::shared_ptr<interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) = 0;
    virtual bool hasFunction(const std::string& name) const = 0;
};

// IO Module - File operations
class IOModule : public Module {
public:
    std::string getName() const override { return "io"; }
    bool hasFunction(const std::string& name) const override;
    std::shared_ptr<interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override;

private:
    std::shared_ptr<interpreter::Value> read_file(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
    std::shared_ptr<interpreter::Value> write_file(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
    std::shared_ptr<interpreter::Value> exists(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
    std::shared_ptr<interpreter::Value> list_dir(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
};

// JSON Module - JSON parsing and serialization
class JSONModule : public Module {
public:
    std::string getName() const override { return "json"; }
    bool hasFunction(const std::string& name) const override;
    std::shared_ptr<interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override;

private:
    std::shared_ptr<interpreter::Value> parse(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
    std::shared_ptr<interpreter::Value> stringify(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
    std::shared_ptr<interpreter::Value> parse_object(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
    std::shared_ptr<interpreter::Value> parse_array(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
    std::shared_ptr<interpreter::Value> is_valid(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
    std::shared_ptr<interpreter::Value> pretty(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
};

// HTTP Module - HTTP client operations
class HTTPModule : public Module {
public:
    std::string getName() const override { return "http"; }
    bool hasFunction(const std::string& name) const override;
    std::shared_ptr<interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override;

private:
    std::shared_ptr<interpreter::Value> get(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
    std::shared_ptr<interpreter::Value> post(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
    std::shared_ptr<interpreter::Value> put(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
    std::shared_ptr<interpreter::Value> del(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
};

// Collections Module - Advanced data structures
class CollectionsModule : public Module {
public:
    std::string getName() const override { return "collections"; }
    bool hasFunction(const std::string& name) const override;
    std::shared_ptr<interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override;

private:
    std::shared_ptr<interpreter::Value> set_create(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
    std::shared_ptr<interpreter::Value> set_add(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
    std::shared_ptr<interpreter::Value> set_contains(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
    std::shared_ptr<interpreter::Value> set_remove(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
    std::shared_ptr<interpreter::Value> set_size(
        const std::vector<std::shared_ptr<interpreter::Value>>& args);
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

#endif // NAAB_STDLIB_H
