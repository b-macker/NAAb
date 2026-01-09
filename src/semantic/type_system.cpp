// NAAb Type System Implementation - Phase 2.3
// Complete type checking and generic type support

#include "naab/type_system.h"
#include <algorithm>
#include <sstream>
#include <cctype>
#include <stdexcept>

namespace naab {
namespace types {

// ============================================================================
// BaseType Utilities
// ============================================================================

std::string baseTypeToString(BaseType type) {
    switch (type) {
        case BaseType::Any: return "any";
        case BaseType::Void: return "void";
        case BaseType::Int: return "int";
        case BaseType::Float: return "float";
        case BaseType::String: return "string";
        case BaseType::Bool: return "bool";
        case BaseType::Array: return "array";
        case BaseType::Dict: return "dict";
        case BaseType::Function: return "function";
        default: return "unknown";
    }
}

std::optional<BaseType> stringToBaseType(const std::string& str) {
    static const std::unordered_map<std::string, BaseType> type_map = {
        {"any", BaseType::Any},
        {"void", BaseType::Void},
        {"int", BaseType::Int},
        {"float", BaseType::Float},
        {"string", BaseType::String},
        {"bool", BaseType::Bool},
        {"array", BaseType::Array},
        {"dict", BaseType::Dict},
        {"function", BaseType::Function}
    };

    auto lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    auto it = type_map.find(lower);
    if (it != type_map.end()) {
        return it->second;
    }
    return std::nullopt;
}

// ============================================================================
// Type Implementation
// ============================================================================

Type::Type(BaseType base) : base_(base) {}

Type::Type(BaseType base, std::vector<Type> params)
    : base_(base), params_(std::move(params)) {}

// Static factory methods
Type Type::Any() { return Type(BaseType::Any); }
Type Type::Void() { return Type(BaseType::Void); }
Type Type::Int() { return Type(BaseType::Int); }
Type Type::Float() { return Type(BaseType::Float); }
Type Type::String() { return Type(BaseType::String); }
Type Type::Bool() { return Type(BaseType::Bool); }

Type Type::Array(const Type& element_type) {
    return Type(BaseType::Array, {element_type});
}

Type Type::Dict(const Type& key_type, const Type& value_type) {
    return Type(BaseType::Dict, {key_type, value_type});
}

Type Type::Function(const std::vector<Type>& param_types, const Type& return_type) {
    std::vector<Type> all_params = param_types;
    all_params.push_back(return_type);
    return Type(BaseType::Function, all_params);
}

// ============================================================================
// Type Parsing
// ============================================================================

std::string Type::trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

std::vector<std::string> Type::splitParams(const std::string& params_str) {
    std::vector<std::string> result;
    std::string current;
    int depth = 0;

    for (char c : params_str) {
        if (c == '<') {
            depth++;
            current += c;
        } else if (c == '>') {
            depth--;
            current += c;
        } else if (c == ',' && depth == 0) {
            result.push_back(trim(current));
            current.clear();
        } else {
            current += c;
        }
    }

    if (!current.empty()) {
        result.push_back(trim(current));
    }

    return result;
}

std::optional<Type> Type::parse(const std::string& type_str) {
    size_t pos = 0;
    return parseImpl(type_str, pos);
}

std::optional<Type> Type::parseImpl(const std::string& str, size_t& pos) {
    std::string trimmed = trim(str.substr(pos));
    if (trimmed.empty()) {
        return std::nullopt;
    }

    // Find base type name (before '<' if present)
    size_t bracket_pos = trimmed.find('<');
    std::string base_name;

    if (bracket_pos == std::string::npos) {
        // Simple type without parameters
        base_name = trim(trimmed);
        auto base_type = stringToBaseType(base_name);
        if (!base_type) {
            return std::nullopt;
        }
        return Type(*base_type);
    }

    // Complex type with parameters
    base_name = trim(trimmed.substr(0, bracket_pos));
    auto base_type = stringToBaseType(base_name);
    if (!base_type) {
        return std::nullopt;
    }

    // Find matching closing bracket
    int depth = 1;
    size_t start = bracket_pos + 1;
    size_t end = start;

    while (end < trimmed.length() && depth > 0) {
        if (trimmed[end] == '<') depth++;
        else if (trimmed[end] == '>') depth--;
        end++;
    }

    if (depth != 0) {
        return std::nullopt;  // Unmatched brackets
    }

    // Parse parameters
    std::string params_str = trimmed.substr(start, end - start - 1);
    auto param_strs = splitParams(params_str);

    std::vector<Type> param_types;
    for (const auto& param_str : param_strs) {
        size_t param_pos = 0;
        auto param_type = parseImpl(param_str, param_pos);
        if (!param_type) {
            return std::nullopt;
        }
        param_types.push_back(*param_type);
    }

    // Validate parameter count for specific types
    if (*base_type == BaseType::Array && param_types.size() != 1) {
        return std::nullopt;
    }
    if (*base_type == BaseType::Dict && param_types.size() != 2) {
        return std::nullopt;
    }

    return Type(*base_type, param_types);
}

// ============================================================================
// Type to String Conversion
// ============================================================================

std::string Type::toString() const {
    std::string result = baseTypeToString(base_);

    if (!params_.empty()) {
        result += "<";
        for (size_t i = 0; i < params_.size(); ++i) {
            if (i > 0) result += ",";
            result += params_[i].toString();
        }
        result += ">";
    }

    return result;
}

// ============================================================================
// Type Compatibility
// ============================================================================

bool Type::isCompatibleWith(const Type& other) const {
    // Any is compatible with everything
    if (base_ == BaseType::Any || other.base_ == BaseType::Any) {
        return true;
    }

    // Exact match
    if (*this == other) {
        return true;
    }

    // Numeric compatibility (int can be used as float)
    if (base_ == BaseType::Int && other.base_ == BaseType::Float) {
        return true;
    }

    // Array compatibility (element types must be compatible)
    if (base_ == BaseType::Array && other.base_ == BaseType::Array) {
        if (params_.empty() || other.params_.empty()) {
            return true;  // Untyped arrays are compatible
        }
        return params_[0].isCompatibleWith(other.params_[0]);
    }

    // Dict compatibility (both key and value types must be compatible)
    if (base_ == BaseType::Dict && other.base_ == BaseType::Dict) {
        if (params_.size() < 2 || other.params_.size() < 2) {
            return true;  // Untyped dicts are compatible
        }
        return params_[0].isCompatibleWith(other.params_[0]) &&
               params_[1].isCompatibleWith(other.params_[1]);
    }

    return false;
}

bool Type::operator==(const Type& other) const {
    if (base_ != other.base_) {
        return false;
    }

    if (params_.size() != other.params_.size()) {
        return false;
    }

    for (size_t i = 0; i < params_.size(); ++i) {
        if (params_[i] != other.params_[i]) {
            return false;
        }
    }

    return true;
}

bool Type::operator!=(const Type& other) const {
    return !(*this == other);
}

bool Type::canCoerceTo(const Type& target) const {
    // Same as isCompatibleWith for now
    // Could be extended with more specific coercion rules
    return isCompatibleWith(target);
}

// ============================================================================
// Type Properties
// ============================================================================

bool Type::isNumeric() const {
    return base_ == BaseType::Int || base_ == BaseType::Float;
}

bool Type::isCollection() const {
    return base_ == BaseType::Array || base_ == BaseType::Dict;
}

bool Type::isPrimitive() const {
    return base_ == BaseType::Int || base_ == BaseType::Float ||
           base_ == BaseType::String || base_ == BaseType::Bool;
}

// ============================================================================
// TypeChecker Implementation
// ============================================================================

bool TypeChecker::checkValue(const std::string& value, const Type& type) {
    // Basic validation (could be expanded)
    if (type.getBase() == BaseType::Any) {
        return true;
    }

    if (type.getBase() == BaseType::Int) {
        try {
            std::stoi(value);
            return true;
        } catch (...) {
            return false;
        }
    }

    if (type.getBase() == BaseType::Float) {
        try {
            std::stod(value);
            return true;
        } catch (...) {
            return false;
        }
    }

    if (type.getBase() == BaseType::Bool) {
        return value == "true" || value == "false" || value == "0" || value == "1";
    }

    // Strings are always valid (any text)
    if (type.getBase() == BaseType::String) {
        return true;
    }

    return false;
}

std::optional<Type> TypeChecker::commonType(const Type& a, const Type& b) {
    // If either is Any, return the other
    if (a.getBase() == BaseType::Any) return b;
    if (b.getBase() == BaseType::Any) return a;

    // If same type, return it
    if (a == b) return a;

    // Numeric promotion: int + float -> float
    if (a.isNumeric() && b.isNumeric()) {
        if (a.getBase() == BaseType::Float || b.getBase() == BaseType::Float) {
            return Type::Float();
        }
        return Type::Int();
    }

    // No common type
    return std::nullopt;
}

bool TypeChecker::allCompatible(const std::vector<Type>& types, const Type& target) {
    for (const auto& type : types) {
        if (!type.isCompatibleWith(target)) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// TypedValue Implementation
// ============================================================================

std::string TypedValue::toString() const {
    return value + " : " + type.toString();
}

} // namespace types
} // namespace naab
