#ifndef NAAB_TYPE_SYSTEM_H
#define NAAB_TYPE_SYSTEM_H

// NAAb Type System - Phase 2.3
// Provides complete type checking and generic type support

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <unordered_map>

namespace naab {
namespace types {

// ============================================================================
// BaseType Enum - Fundamental types
// ============================================================================
enum class BaseType {
    Any,      // Any type (top type)
    Void,     // No value (bottom type for return)
    Int,      // Integer numbers
    Float,    // Floating-point numbers
    String,   // Text strings
    Bool,     // Boolean true/false
    Array,    // Array<T> - homogeneous arrays
    Dict,     // Dict<K,V> - key-value maps
    Function  // Function types (for higher-order functions)
};

// Convert BaseType to string
std::string baseTypeToString(BaseType type);

// Parse string to BaseType
std::optional<BaseType> stringToBaseType(const std::string& str);

// ============================================================================
// Type - Complete type with generic parameters
// ============================================================================
class Type {
public:
    // Constructors
    explicit Type(BaseType base);
    Type(BaseType base, std::vector<Type> params);

    // Static factory methods for common types
    static Type Any();
    static Type Void();
    static Type Int();
    static Type Float();
    static Type String();
    static Type Bool();
    static Type Array(const Type& element_type);
    static Type Dict(const Type& key_type, const Type& value_type);
    static Type Function(const std::vector<Type>& param_types, const Type& return_type);

    // Parse type from string (e.g., "array<string>", "dict<string,int>")
    static std::optional<Type> parse(const std::string& type_str);

    // Convert type to string representation
    std::string toString() const;

    // Type compatibility checking
    bool isCompatibleWith(const Type& other) const;

    // Type equality
    bool operator==(const Type& other) const;
    bool operator!=(const Type& other) const;

    // Type coercion - can this type be safely coerced to target?
    bool canCoerceTo(const Type& target) const;

    // Accessors
    BaseType getBase() const { return base_; }
    const std::vector<Type>& getParams() const { return params_; }
    bool isGeneric() const { return !params_.empty(); }

    // Type properties
    bool isNumeric() const;
    bool isCollection() const;
    bool isPrimitive() const;

private:
    BaseType base_;
    std::vector<Type> params_;  // Generic type parameters

    // Helper for parsing
    static std::optional<Type> parseImpl(const std::string& str, size_t& pos);
    static std::string trim(const std::string& str);
    static std::vector<std::string> splitParams(const std::string& params_str);
};

// ============================================================================
// TypeChecker - Utilities for type checking
// ============================================================================
class TypeChecker {
public:
    // Check if a value can be used as type
    static bool checkValue(const std::string& value, const Type& type);

    // Find common type between two types (type inference)
    static std::optional<Type> commonType(const Type& a, const Type& b);

    // Check if array of types are all compatible
    static bool allCompatible(const std::vector<Type>& types, const Type& target);
};

// ============================================================================
// TypedValue - Value with runtime type information
// ============================================================================
struct TypedValue {
    Type type;
    std::string value;  // String representation of value

    TypedValue(Type t, std::string v) : type(std::move(t)), value(std::move(v)) {}

    std::string toString() const;
};

// ============================================================================
// Type Aliases for convenience
// ============================================================================
using TypeMap = std::unordered_map<std::string, Type>;

} // namespace types
} // namespace naab

#endif // NAAB_TYPE_SYSTEM_H
