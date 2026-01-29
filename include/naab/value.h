#ifndef NAAB_VALUE_H
#define NAAB_VALUE_H

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <variant>
#include <functional> // For std::function in Value::traverse

// GEMINI FIX: Forward declarations to break dependencies
// These are types that Value's data variant uses but which have their own headers
namespace naab {
namespace interpreter {
    struct BlockValue;
    struct FunctionValue;
    class PythonObjectValue; // Corrected: class not struct
    struct StructValue;
    struct StructDef; // Added for StructValue
}
}

namespace naab {
namespace interpreter {

// Runtime value types
using ValueData = std::variant<
    std::monostate,  // void/null (index 0)
    int,             // index 1
    double,          // index 2
    bool,            // index 3
    std::string,     // index 4
    std::vector<std::shared_ptr<Value>>,  // list (index 5)
    std::unordered_map<std::string, std::shared_ptr<Value>>,  // dict (index 6)
    std::shared_ptr<BlockValue>,  // block (index 7)
    std::shared_ptr<FunctionValue>,  // function (index 8)
    std::shared_ptr<PythonObjectValue>,  // python object (index 9)
    std::shared_ptr<StructValue>  // struct (index 10)
>;

class Value {
public:
    ValueData data;

    Value() : data(std::monostate{}) {}
    explicit Value(int v) : data(v) {}
    explicit Value(double v) : data(v) {}
    explicit Value(bool v) : data(v) {}
    explicit Value(std::string v) : data(std::move(v)) {}
    explicit Value(std::vector<std::shared_ptr<Value>> v) : data(std::move(v)) {}
    explicit Value(std::unordered_map<std::string, std::shared_ptr<Value>> v) : data(std::move(v)) {}
    explicit Value(std::shared_ptr<BlockValue> v) : data(std::move(v)) {}
    explicit Value(std::shared_ptr<FunctionValue> v) : data(std::move(v)) {}
    explicit Value(std::shared_ptr<PythonObjectValue> v) : data(std::move(v)) {}
    explicit Value(std::shared_ptr<StructValue> v) : data(std::move(v)) {}

    std::string toString() const;
    bool toBool() const;
    int toInt() const;
    double toFloat() const;

    // Phase 3.2: Value traversal for garbage collection
    void traverse(std::function<void(std::shared_ptr<Value>)> visitor) const; // Needs Value definition
};

// Forward declaration of specific Value methods to avoid circular includes
// These are implemented in interpreter.cpp
// This is to prevent Value.h from including interpreter.h
namespace interpreter {
    // These need Value defined, so can't be in Value.h itself
}

} // namespace interpreter
} // namespace naab

#endif // NAAB_VALUE_H
