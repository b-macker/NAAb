#ifndef NAAB_STDLIB_NEW_MODULES_H
#define NAAB_STDLIB_NEW_MODULES_H

// New stdlib modules declarations
// Implementations are in src/stdlib/*_impl.cpp files

#include "naab/stdlib.h"
#include <functional>

namespace naab {
namespace stdlib {

// String Module
class StringModule : public Module {
public:
    std::string getName() const override; // GEMINI FIX: Removed inline definition
    bool hasFunction(const std::string& name) const override;
    std::shared_ptr<interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override;
};

// Array Module
class ArrayModule : public Module {
public:
    // Type for function evaluator callback
    using FunctionEvaluator = std::function<std::shared_ptr<interpreter::Value>(
        std::shared_ptr<interpreter::Value> fn,
        const std::vector<std::shared_ptr<interpreter::Value>>& args)>;

    ArrayModule() = default;
    explicit ArrayModule(FunctionEvaluator evaluator) : evaluator_(std::move(evaluator)) {}

    std::string getName() const override; // GEMINI FIX: Removed inline definition
    bool hasFunction(const std::string& name) const override;
    std::shared_ptr<interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override;

    // Set function evaluator (for higher-order functions like map/filter/reduce)
    void setFunctionEvaluator(FunctionEvaluator evaluator) { evaluator_ = std::move(evaluator); }

private:
    FunctionEvaluator evaluator_;
};

// Math Module
class MathModule : public Module {
public:
    std::string getName() const override; // GEMINI FIX: Removed inline definition
    bool hasFunction(const std::string& name) const override;
    std::shared_ptr<interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override;
};

// Time Module
class TimeModule : public Module {
public:
    std::string getName() const override; // GEMINI FIX: Removed inline definition
    bool hasFunction(const std::string& name) const override;
    std::shared_ptr<interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override;
};

// Env Module
class EnvModule : public Module {
public:
    EnvModule(naab::interpreter::Interpreter* interpreter);
    std::string getName() const override; // GEMINI FIX: Removed inline definition
    bool hasFunction(const std::string& name) const override;
    std::shared_ptr<naab::interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<naab::interpreter::Value>>& args) override;
private:
    naab::interpreter::Interpreter* interpreter_;
};

// CSV Module
class CsvModule : public Module {
public:
    std::string getName() const override; // GEMINI FIX: Removed inline definition
    bool hasFunction(const std::string& name) const override;
    std::shared_ptr<interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override;
};

// Regex Module
class RegexModule : public Module {
public:
    std::string getName() const override; // GEMINI FIX: Removed inline definition
    bool hasFunction(const std::string& name) const override;
    std::shared_ptr<interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override;
};

// Crypto Module
class CryptoModule : public Module {
public:
    std::string getName() const override; // GEMINI FIX: Removed inline definition
    bool hasFunction(const std::string& name) const override;
    std::shared_ptr<interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override;
};

// File Module
class FileModule : public Module {
public:
    std::string getName() const override; // GEMINI FIX: Removed inline definition
    bool hasFunction(const std::string& name) const override;
    std::shared_ptr<interpreter::Value> call(
        const std::string& function_name,
        const std::vector<std::shared_ptr<interpreter::Value>>& args) override;
};


} // namespace stdlib
} // namespace naab

#endif // NAAB_STDLIB_NEW_MODULES_H
