#pragma once

// Type Marshaller - Convert between NAAb Value and C++ types
// Handles marshalling for dynamic C++ function calls

#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace naab {
namespace interpreter {
    class Value;  // Forward declaration
}

namespace runtime {

// Supported C++ types for marshalling
enum class CppType {
    INT,
    DOUBLE,
    STRING,
    BOOL,
    VOID,
    UNKNOWN
};

// Represents a marshalled C++ value
struct CppValue {
    CppType type;
    union {
        int64_t i;
        double d;
        bool b;
    };
    std::string s;  // For strings

    CppValue() : type(CppType::VOID), i(0) {}
};

// Type Marshaller - converts between NAAb Value and C++ types
class TypeMarshaller {
public:
    TypeMarshaller() = default;

    // NAAb → C++
    CppValue toCpp(const std::shared_ptr<interpreter::Value>& val);
    int toInt(const std::shared_ptr<interpreter::Value>& val);
    double toDouble(const std::shared_ptr<interpreter::Value>& val);
    std::string toString(const std::shared_ptr<interpreter::Value>& val);
    bool toBool(const std::shared_ptr<interpreter::Value>& val);

    // C++ → NAAb
    std::shared_ptr<interpreter::Value> fromInt(int i);
    std::shared_ptr<interpreter::Value> fromDouble(double d);
    std::shared_ptr<interpreter::Value> fromString(const std::string& s);
    std::shared_ptr<interpreter::Value> fromBool(bool b);

    // Detect C++ type from string signature
    CppType detectType(const std::string& type_str);

    // Format type name for error messages
    std::string typeName(CppType type);
};

} // namespace runtime
} // namespace naab

