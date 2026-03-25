#pragma once

// Type Marshaller - Convert between NAAb NaabVal and C++ types
// Handles marshalling for dynamic C++ function calls

#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include "naab/naab_val.h"

namespace naab {
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

// Type Marshaller - converts between NAAb NaabVal and C++ types
class TypeMarshaller {
public:
    TypeMarshaller() = default;

    // NAAb → C++
    CppValue toCpp(const interpreter::NaabVal& val);
    int toInt(const interpreter::NaabVal& val);
    double toDouble(const interpreter::NaabVal& val);
    std::string toString(const interpreter::NaabVal& val);
    bool toBool(const interpreter::NaabVal& val);

    // C++ → NAAb
    interpreter::NaabVal fromInt(int i);
    interpreter::NaabVal fromDouble(double d);
    interpreter::NaabVal fromString(const std::string& s);
    interpreter::NaabVal fromBool(bool b);

    // Detect C++ type from string signature
    CppType detectType(const std::string& type_str);

    // Format type name for error messages
    std::string typeName(CppType type);
};

} // namespace runtime
} // namespace naab
