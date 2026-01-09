// Type Marshaller Implementation
// Converts between NAAb Value and C++ types

#include "naab/type_marshaller.h"
#include "naab/interpreter.h"
#include <fmt/core.h>
#include <stdexcept>

namespace naab {
namespace runtime {

CppValue TypeMarshaller::toCpp(const std::shared_ptr<interpreter::Value>& val) {
    CppValue cpp_val;

    if (!val) {
        cpp_val.type = CppType::VOID;
        return cpp_val;
    }

    // Determine type from Value using std::holds_alternative
    if (std::holds_alternative<bool>(val->data)) {
        cpp_val.type = CppType::BOOL;
        cpp_val.b = std::get<bool>(val->data);
    } else if (std::holds_alternative<int>(val->data)) {
        cpp_val.type = CppType::INT;
        cpp_val.i = std::get<int>(val->data);
    } else if (std::holds_alternative<double>(val->data)) {
        cpp_val.type = CppType::DOUBLE;
        cpp_val.d = std::get<double>(val->data);
    } else if (std::holds_alternative<std::string>(val->data)) {
        cpp_val.type = CppType::STRING;
        cpp_val.s = std::get<std::string>(val->data);
    } else {
        cpp_val.type = CppType::UNKNOWN;
    }

    return cpp_val;
}

int TypeMarshaller::toInt(const std::shared_ptr<interpreter::Value>& val) {
    if (!val) {
        throw std::runtime_error("Cannot convert null to int");
    }

    if (std::holds_alternative<int>(val->data)) {
        return std::get<int>(val->data);
    } else if (std::holds_alternative<double>(val->data)) {
        return static_cast<int>(std::get<double>(val->data));
    } else if (std::holds_alternative<bool>(val->data)) {
        return std::get<bool>(val->data) ? 1 : 0;
    } else {
        throw std::runtime_error(fmt::format(
            "Cannot convert {} to int", val->toString()));
    }
}

double TypeMarshaller::toDouble(const std::shared_ptr<interpreter::Value>& val) {
    if (!val) {
        throw std::runtime_error("Cannot convert null to double");
    }

    if (std::holds_alternative<double>(val->data)) {
        return std::get<double>(val->data);
    } else if (std::holds_alternative<int>(val->data)) {
        return static_cast<double>(std::get<int>(val->data));
    } else {
        throw std::runtime_error(fmt::format(
            "Cannot convert {} to double", val->toString()));
    }
}

std::string TypeMarshaller::toString(const std::shared_ptr<interpreter::Value>& val) {
    if (!val) {
        return "";
    }
    return val->toString();
}

bool TypeMarshaller::toBool(const std::shared_ptr<interpreter::Value>& val) {
    if (!val) {
        return false;
    }

    if (std::holds_alternative<bool>(val->data)) {
        return std::get<bool>(val->data);
    } else if (std::holds_alternative<int>(val->data)) {
        return std::get<int>(val->data) != 0;
    } else if (std::holds_alternative<double>(val->data)) {
        return std::get<double>(val->data) != 0.0;
    } else {
        return !val->toString().empty();
    }
}

std::shared_ptr<interpreter::Value> TypeMarshaller::fromInt(int i) {
    return std::make_shared<interpreter::Value>(i);
}

std::shared_ptr<interpreter::Value> TypeMarshaller::fromDouble(double d) {
    return std::make_shared<interpreter::Value>(d);
}

std::shared_ptr<interpreter::Value> TypeMarshaller::fromString(const std::string& s) {
    return std::make_shared<interpreter::Value>(s);
}

std::shared_ptr<interpreter::Value> TypeMarshaller::fromBool(bool b) {
    return std::make_shared<interpreter::Value>(b);
}

CppType TypeMarshaller::detectType(const std::string& type_str) {
    // Simple type detection from C++ type signatures
    if (type_str == "int" || type_str == "int64_t" || type_str == "long") {
        return CppType::INT;
    } else if (type_str == "double" || type_str == "float") {
        return CppType::DOUBLE;
    } else if (type_str == "bool") {
        return CppType::BOOL;
    } else if (type_str == "std::string" || type_str == "string" || type_str == "char*") {
        return CppType::STRING;
    } else if (type_str == "void") {
        return CppType::VOID;
    } else {
        return CppType::UNKNOWN;
    }
}

std::string TypeMarshaller::typeName(CppType type) {
    switch (type) {
        case CppType::INT: return "int";
        case CppType::DOUBLE: return "double";
        case CppType::STRING: return "string";
        case CppType::BOOL: return "bool";
        case CppType::VOID: return "void";
        default: return "unknown";
    }
}

} // namespace runtime
} // namespace naab
