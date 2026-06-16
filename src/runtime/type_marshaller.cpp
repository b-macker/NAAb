// Type Marshaller Implementation
// Converts between NAAb NaabVal and C++ types

#include "naab/type_marshaller.h"
#include "naab/interpreter.h"
#include "naab/safe_math.h"
#include "naab/naab_val.h"
#include <fmt/core.h>
#include <stdexcept>

namespace naab {
namespace runtime {

CppValue TypeMarshaller::toCpp(const interpreter::NaabVal& val) {
    CppValue cpp_val;

    if (val.isNull()) {
        cpp_val.type = CppType::VOID;
        return cpp_val;
    }

    if (val.isBool()) {
        cpp_val.type = CppType::BOOL;
        cpp_val.b = val.asBool();
    } else if (val.isInt()) {
        cpp_val.type = CppType::INT;
        cpp_val.i = val.asInt();
    } else if (val.isDouble()) {
        cpp_val.type = CppType::DOUBLE;
        cpp_val.d = val.asDouble();
    } else if (val.isString()) {
        cpp_val.type = CppType::STRING;
        cpp_val.s = val.asString();
    } else {
        cpp_val.type = CppType::UNKNOWN;
    }

    return cpp_val;
}

int TypeMarshaller::toInt(const interpreter::NaabVal& val) {
    if (val.isNull()) {
        throw std::runtime_error("Cannot convert null to int");
    }

    if (val.isInt()) {
        return val.asInt();
    } else if (val.isDouble()) {
        return naab::math::safeDoubleToInt(val.asDouble());
    } else if (val.isBool()) {
        return val.asBool() ? 1 : 0;
    } else {
        throw std::runtime_error(fmt::format(
            "Cannot convert {} to int", val.toString()));
    }
}

double TypeMarshaller::toDouble(const interpreter::NaabVal& val) {
    if (val.isNull()) {
        throw std::runtime_error("Cannot convert null to double");
    }

    if (val.isDouble()) {
        return val.asDouble();
    } else if (val.isInt()) {
        return static_cast<double>(val.asInt());
    } else {
        throw std::runtime_error(fmt::format(
            "Cannot convert {} to double", val.toString()));
    }
}

std::string TypeMarshaller::toString(const interpreter::NaabVal& val) {
    if (val.isNull()) {
        return "";
    }
    return val.toString();
}

bool TypeMarshaller::toBool(const interpreter::NaabVal& val) {
    if (val.isNull()) {
        return false;
    }

    if (val.isBool()) {
        return val.asBool();
    } else if (val.isInt()) {
        return val.asInt() != 0;
    } else if (val.isDouble()) {
        return val.asDouble() != 0.0;
    } else {
        return !val.toString().empty();
    }
}

interpreter::NaabVal TypeMarshaller::fromInt(int i) {
    return interpreter::NaabVal::makeInt(i);
}

interpreter::NaabVal TypeMarshaller::fromDouble(double d) {
    return interpreter::NaabVal::makeDouble(d);
}

interpreter::NaabVal TypeMarshaller::fromString(const std::string& s) {
    return interpreter::NaabVal::makeString(s);
}

interpreter::NaabVal TypeMarshaller::fromBool(bool b) {
    return interpreter::NaabVal::makeBool(b);
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
