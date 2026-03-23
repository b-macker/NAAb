// NAAb Interpreter — Expression Evaluation
// Split from interpreter.cpp for maintainability
//
// Contains: visit(BinaryExpr), visit(UnaryExpr), visit(IdentifierExpr),
//           visit(LiteralExpr), visit(DictExpr), visit(ListExpr),
//           visit(RangeExpr), visit(StructLiteralExpr)

#include "naab/interpreter.h"
#include "naab/lexer.h"
#include "naab/parser.h"
#include "naab/error_helpers.h"
#include "naab/logger.h"
#include "naab/struct_registry.h"
#include <fmt/core.h>
#include <sstream>
#include <climits>

namespace naab {
namespace interpreter {

// File-local helper (duplicated from interpreter.cpp — static linkage)
static std::string getTypeName(const std::shared_ptr<Value>& val) {
    return std::visit([](auto&& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int>) {
            return "int";
        } else if constexpr (std::is_same_v<T, double>) {
            return "float";
        } else if constexpr (std::is_same_v<T, bool>) {
            return "bool";
        } else if constexpr (std::is_same_v<T, std::string>) {
            return "string";
        } else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<Value>>>) {
            return "array";
        } else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<Value>>>) {
            return "dict";
        } else if constexpr (std::is_same_v<T, std::shared_ptr<FunctionValue>>) {
            return "function";
        } else if constexpr (std::is_same_v<T, std::shared_ptr<StructValue>>) {
            return "struct";
        } else if constexpr (std::is_same_v<T, std::shared_ptr<FutureValue>>) {
            return "future";
        } else if constexpr (std::is_same_v<T, std::monostate>) {
            return "null";
        }
        return "unknown";
    }, val->data);
}

void Interpreter::visit(ast::BinaryExpr& node) {
    // Handle short-circuit operators BEFORE evaluating right side
    if (node.getOp() == ast::BinaryOp::And) {
        auto left = eval(*node.getLeft());
        if (!left->toBool()) {
            // Short-circuit: left is false, so result is false without evaluating right
            result_ = std::make_shared<Value>(false);
            return;
        }
        // Left is true, now evaluate right
        auto right = eval(*node.getRight());
        result_ = std::make_shared<Value>(right->toBool());
        return;
    }

    if (node.getOp() == ast::BinaryOp::NullCoalesce) {
        auto left_val = eval(*node.getLeft());
        if (!left_val || std::holds_alternative<std::monostate>(left_val->data)) {
            result_ = eval(*node.getRight());
        } else {
            result_ = left_val;
        }
        return;
    }

    if (node.getOp() == ast::BinaryOp::Or) {
        auto left = eval(*node.getLeft());
        if (left->toBool()) {
            // Short-circuit: left is true, so result is true without evaluating right
            result_ = std::make_shared<Value>(true);
            return;
        }
        // Left is false, now evaluate right
        auto right = eval(*node.getRight());
        result_ = std::make_shared<Value>(right->toBool());

        // Hint: detect likely null-coalesce intent (null || "value")
        if (std::holds_alternative<std::monostate>(left->data) &&
            !std::holds_alternative<bool>(right->data) &&
            !std::holds_alternative<std::monostate>(right->data)) {
            fprintf(stderr, "[hint] || always returns boolean in NAAb. "
                    "Did you mean ?? (null coalesce)?\n"
                    "  null || \"value\" -> true (boolean)\n"
                    "  null ?? \"value\" -> \"value\" (the actual value)\n");
        }
        return;
    }

    // Phase 2.4.6: Handle assignment specially to avoid evaluating left side as a read operation
    if (node.getOp() == ast::BinaryOp::Assign) {
        auto right = eval(*node.getRight());

        if (auto* id = dynamic_cast<ast::IdentifierExpr*>(node.getLeft())) {
            // Simple variable assignment: x = value
            // Deep copy arrays and dicts to prevent silent mutations (same as VarDeclStmt)
            auto value_to_assign = right;
            if (std::holds_alternative<std::vector<std::shared_ptr<Value>>>(right->data) ||
                std::holds_alternative<std::unordered_map<std::string, std::shared_ptr<Value>>>(right->data)) {
                value_to_assign = copyValue(right);
            }
            current_env_->set(id->getName(), value_to_assign);
            result_ = value_to_assign;

            // Governance v4: Taint tracking on assignment (REFACTOR-1)
            if (governance_ && governance_->isActive()) {
                if (checkRhsTainted(node.getRight())) {
                    governance_->markTainted(id->getName());
                } else {
                    governance_->clearTaint(id->getName());
                }
                if (checkRhsSanitized(node.getRight())) {
                    governance_->clearTaint(id->getName());
                }
            }
        } else if (auto* member = dynamic_cast<ast::MemberExpr*>(node.getLeft())) {
            // Struct field assignment: obj.field = value
            auto obj = eval(*member->getObject());

            if (auto* struct_ptr = std::get_if<std::shared_ptr<StructValue>>(&obj->data)) {
                auto& struct_val = *struct_ptr;
                struct_val->setField(member->getMember(), right);
                result_ = right;

                // FIX-4: Taint propagation for struct field assignment (REFACTOR-1)
                if (governance_ && governance_->isActive() && checkRhsTainted(node.getRight())) {
                    auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member->getObject());
                    if (obj_id) governance_->markTainted(obj_id->getName());
                }
            } else {
                std::ostringstream oss;
                oss << "Type error: Cannot assign to property of non-struct value\n\n";
                oss << "  Tried to assign to: " << getTypeName(obj) << "." << member->getMember() << "\n";
                oss << "  Expected: struct\n\n";
                oss << "  Help:\n";
                oss << "  - Only structs support property assignment with dot notation\n";
                oss << "  - For dictionaries, use subscript: dict[\"field\"] = value\n";
                oss << "  - Define a struct type if you need named fields\n\n";
                oss << "  Example:\n";
                oss << "    ✗ Wrong: let x = 42; x.field = 10\n";
                oss << "    ✓ Right: struct Point { x: int, y: int }\n";
                oss << "             let p = Point{x: 0, y: 0}; p.x = 10\n";
                throw std::runtime_error(oss.str());
            }
        } else if (auto* subscript = dynamic_cast<ast::BinaryExpr*>(node.getLeft())) {
            // Array/dict element assignment: arr[index] = value, dict[key] = value
            if (subscript->getOp() == ast::BinaryOp::Subscript) {
                // Evaluate the container (e.g., arr or dict)
                auto container = eval(*subscript->getLeft());
                // Evaluate the index/key
                auto index_or_key = eval(*subscript->getRight());

                // Check if container is a list
                if (auto* list_ptr = std::get_if<std::vector<std::shared_ptr<Value>>>(&container->data)) {
                    auto& list = *list_ptr;
                    int index = index_or_key->toInt();

                    if (index < 0 || index >= static_cast<int>(list.size())) {
                        std::ostringstream oss;
                        oss << "Index error: Array index out of bounds\n\n";
                        oss << "  Index: " << index << "\n";
                        oss << "  Array size: " << list.size() << "\n";
                        oss << "  Valid range: 0 to " << (list.size() > 0 ? list.size() - 1 : 0) << "\n\n";
                        oss << "  Help:\n";
                        oss << "  - Array indices start at 0\n";
                        oss << "  - Check array size before accessing\n";
                        oss << "  - Use array.length(arr) to get size\n\n";
                        oss << "  Example:\n";
                        oss << "    let arr = [10, 20, 30]  // size = 3\n";
                        oss << "    ✗ Wrong: arr[3]  // out of bounds\n";
                        oss << "    ✓ Right: arr[2]  // last element\n";
                        throw std::runtime_error(oss.str());
                    }

                    // Modify list in place (safe cast after bounds check)
                    list[static_cast<size_t>(index)] = right;
                    result_ = right;

                    // FIX-3: Taint propagation for array subscript assignment (REFACTOR-1)
                    if (governance_ && governance_->isActive() && checkRhsTainted(node.getRight())) {
                        auto* container_id = dynamic_cast<ast::IdentifierExpr*>(subscript->getLeft());
                        if (container_id) governance_->markTainted(container_id->getName());
                    }
                }
                // Check if container is a dictionary
                else if (auto* dict_ptr = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&container->data)) {
                    auto& dict = *dict_ptr;
                    std::string key = index_or_key->toString();

                    // Insert or update the key
                    dict[key] = right;
                    result_ = right;

                    // FIX-3: Taint propagation for dict subscript assignment (REFACTOR-1)
                    if (governance_ && governance_->isActive() && checkRhsTainted(node.getRight())) {
                        auto* container_id = dynamic_cast<ast::IdentifierExpr*>(subscript->getLeft());
                        if (container_id) governance_->markTainted(container_id->getName());
                    }
                } else {
                    std::ostringstream oss;
                    oss << "Type error: Subscript assignment not supported\n\n";
                    oss << "  Tried to assign to: " << getTypeName(container) << "[...]\n";
                    oss << "  Supported types: array, dict\n\n";
                    oss << "  Help:\n";
                    oss << "  - Only arrays and dicts support subscript assignment\n";
                    oss << "  - Arrays use integer indices: arr[0] = value\n";
                    oss << "  - Dicts use string keys: dict[\"key\"] = value\n\n";
                    oss << "  Example:\n";
                    oss << "    ✗ Wrong: let x = 42; x[0] = 10\n";
                    oss << "    ✓ Right: let arr = [1, 2]; arr[0] = 10\n";
                    oss << "    ✓ Right: let dict = {}; dict[\"x\"] = 10\n";
                    throw std::runtime_error(oss.str());
                }
            } else {
                throw std::runtime_error(
                    "Syntax error: Invalid assignment target\n\n"
                    "  Assignment target must be:\n"
                    "  - Variable: name = value\n"
                    "  - Struct field: obj.field = value\n"
                    "  - Array element: arr[index] = value\n"
                    "  - Dict entry: dict[\"key\"] = value\n\n"
                    "  Example:\n"
                    "    ✗ Wrong: 42 = x  // can't assign to literal\n"
                    "    ✓ Right: x = 42\n"
                );
            }
        } else {
            throw std::runtime_error(
                "Syntax error: Invalid assignment target\n\n"
                "  Assignment target must be:\n"
                "  - Variable: name = value\n"
                "  - Struct field: obj.field = value\n"
                "  - Array element: arr[index] = value\n"
                "  - Dict entry: dict[\"key\"] = value\n\n"
                "  Help:\n"
                "  - Cannot assign to expressions or literals\n"
                "  - Use let to declare new variables\n\n"
                "  Example:\n"
                "    ✗ Wrong: getValue() = 10  // can't assign to function result\n"
                "    ✓ Right: let result = getValue(); result = 10\n"
            );
        }
        return;
    }

    // Handle Pipeline operator specially (like short-circuit operators)
    // Don't evaluate right side yet - it needs special handling
    std::shared_ptr<Value> left, right;
    if (node.getOp() == ast::BinaryOp::Pipeline) {
        left = eval(*node.getLeft());
        // Right side handling is in the switch case below - don't eval here!
        // This prevents the CallExpr from being evaluated with wrong arg count
    } else {
        // For all other operators, evaluate both sides
        left = eval(*node.getLeft());
        right = eval(*node.getRight());
    }

    switch (node.getOp()) {
        case ast::BinaryOp::Add:
            // List concatenation
            if (std::holds_alternative<std::vector<std::shared_ptr<Value>>>(left->data) &&
                std::holds_alternative<std::vector<std::shared_ptr<Value>>>(right->data)) {
                auto& left_vec = std::get<std::vector<std::shared_ptr<Value>>>(left->data);
                auto& right_vec = std::get<std::vector<std::shared_ptr<Value>>>(right->data);

                std::vector<std::shared_ptr<Value>> combined;
                combined.reserve(left_vec.size() + right_vec.size());
                combined.insert(combined.end(), left_vec.begin(), left_vec.end());
                combined.insert(combined.end(), right_vec.begin(), right_vec.end());

                result_ = std::make_shared<Value>(combined);
            }
            // String concatenation or numeric addition
            else if (std::holds_alternative<std::string>(left->data) ||
                std::holds_alternative<std::string>(right->data)) {
                result_ = std::make_shared<Value>(left->toString() + right->toString());
            } else if (std::holds_alternative<double>(left->data) ||
                       std::holds_alternative<double>(right->data)) {
                result_ = std::make_shared<Value>(left->toFloat() + right->toFloat());
            } else {
                // Integer addition with overflow detection
                int a = left->toInt();
                int b = right->toInt();

                // Check for overflow before addition
                if ((b > 0 && a > INT_MAX - b) || (b < 0 && a < INT_MIN - b)) {
                    std::ostringstream oss;
                    oss << "Math error: Integer overflow in addition\n\n";
                    oss << "  Expression: " << a << " + " << b << "\n";
                    oss << "  INT_MAX: " << INT_MAX << "\n";
                    oss << "  INT_MIN: " << INT_MIN << "\n";
                    oss << "\n  Help:\n";
                    oss << "  - Integer overflow occurs when result exceeds 32-bit int range\n";
                    oss << "  - Use float for larger numbers: " << a << ".0 + " << b << ".0\n";
                    oss << "  - Or check values before adding:\n";
                    oss << "\n  Example:\n";
                    oss << "    ✗ Wrong: let result = " << INT_MAX << " + 1  (overflow!)\n";
                    oss << "    ✓ Right: let result = " << INT_MAX << ".0 + 1.0  (use float)\n";
                    throw std::runtime_error(oss.str());
                }

                result_ = std::make_shared<Value>(a + b);
            }
            break;

        case ast::BinaryOp::Sub: {
            // Type check: Subtraction requires numeric types
            bool left_is_numeric = std::holds_alternative<int>(left->data) ||
                                  std::holds_alternative<double>(left->data) ||
                                  std::holds_alternative<bool>(left->data);
            bool right_is_numeric = std::holds_alternative<int>(right->data) ||
                                   std::holds_alternative<double>(right->data) ||
                                   std::holds_alternative<bool>(right->data);

            if (!left_is_numeric || !right_is_numeric) {
                std::ostringstream oss;
                oss << "Type error: Subtraction (-) requires numeric types\n\n";

                if (!left_is_numeric && !right_is_numeric) {
                    oss << "  Both operands are non-numeric:\n";
                    oss << "    Left: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Right: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                } else if (!left_is_numeric) {
                    oss << "  Left operand is non-numeric:\n";
                    oss << "    Got: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Expected: int, float, or bool\n";
                } else {
                    oss << "  Right operand is non-numeric:\n";
                    oss << "    Got: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                    oss << "    Expected: int, float, or bool\n";
                }

                oss << "\n  Help:\n";
                oss << "  - For numbers: Use int or float values\n";
                oss << "  - For strings: Parse to numeric first\n";
                oss << "  - String concatenation uses +, not -\n";
                oss << "\n  Example:\n";
                oss << "    ✗ Wrong: \"10\" - 5    (string - int)\n";
                oss << "    ✓ Right: 10 - 5       (int - int)\n";

                throw std::runtime_error(oss.str());
            }

            if (std::holds_alternative<double>(left->data) ||
                std::holds_alternative<double>(right->data)) {
                result_ = std::make_shared<Value>(left->toFloat() - right->toFloat());
            } else {
                // Integer subtraction with overflow detection
                int a = left->toInt();
                int b = right->toInt();

                // Check for overflow before subtraction
                if ((b < 0 && a > INT_MAX + b) || (b > 0 && a < INT_MIN + b)) {
                    std::ostringstream oss;
                    oss << "Math error: Integer overflow in subtraction\n\n";
                    oss << "  Expression: " << a << " - " << b << "\n";
                    oss << "  INT_MAX: " << INT_MAX << "\n";
                    oss << "  INT_MIN: " << INT_MIN << "\n";
                    oss << "\n  Help:\n";
                    oss << "  - Integer overflow occurs when result exceeds 32-bit int range\n";
                    oss << "  - Use float for larger numbers: " << a << ".0 - " << b << ".0\n";
                    oss << "  - Or check values before subtracting:\n";
                    oss << "\n  Example:\n";
                    oss << "    ✗ Wrong: let result = " << INT_MIN << " - 1  (underflow!)\n";
                    oss << "    ✓ Right: let result = " << INT_MIN << ".0 - 1.0  (use float)\n";
                    throw std::runtime_error(oss.str());
                }

                result_ = std::make_shared<Value>(a - b);
            }
            break;
        }

        case ast::BinaryOp::Mul: {
            // String repetition: "abc" * 3 → "abcabcabc", 3 * "abc" → "abcabcabc"
            {
                std::string* str_val = nullptr;
                int repeat = 0;
                bool is_string_repeat = false;
                if (auto* s = std::get_if<std::string>(&left->data)) {
                    if (std::holds_alternative<int>(right->data)) {
                        str_val = s; repeat = std::get<int>(right->data); is_string_repeat = true;
                    }
                } else if (auto* s2 = std::get_if<std::string>(&right->data)) {
                    if (std::holds_alternative<int>(left->data)) {
                        str_val = s2; repeat = std::get<int>(left->data); is_string_repeat = true;
                    }
                }
                if (is_string_repeat) {
                    std::string result;
                    for (int i = 0; i < repeat; i++) result += *str_val;
                    result_ = std::make_shared<Value>(result);
                    break;
                }
            }

            // Type check: Multiplication requires numeric types
            bool left_is_numeric = std::holds_alternative<int>(left->data) ||
                                  std::holds_alternative<double>(left->data) ||
                                  std::holds_alternative<bool>(left->data);
            bool right_is_numeric = std::holds_alternative<int>(right->data) ||
                                   std::holds_alternative<double>(right->data) ||
                                   std::holds_alternative<bool>(right->data);

            if (!left_is_numeric || !right_is_numeric) {
                std::ostringstream oss;
                oss << "Type error: Multiplication (*) requires numeric types or string * int\n\n";

                if (!left_is_numeric && !right_is_numeric) {
                    oss << "  Both operands are non-numeric:\n";
                    oss << "    Left: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Right: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                } else if (!left_is_numeric) {
                    oss << "  Left operand is non-numeric:\n";
                    oss << "    Got: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Expected: int, float, bool, or string\n";
                } else {
                    oss << "  Right operand is non-numeric:\n";
                    oss << "    Got: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                    oss << "    Expected: int, float, bool, or string\n";
                }

                oss << "\n  Help:\n";
                oss << "  - For numbers: Use int or float values\n";
                oss << "  - For string repetition: \"abc\" * 3 or 3 * \"abc\"\n";
                oss << "  - For concatenation: Use + operator\n";
                oss << "\n  Example:\n";
                oss << "    ✓ Right: 5 * 3           (int * int)\n";
                oss << "    ✓ Right: \"ha\" * 3        (string repeat)\n";

                throw std::runtime_error(oss.str());
            }

            if (std::holds_alternative<double>(left->data) ||
                std::holds_alternative<double>(right->data)) {
                result_ = std::make_shared<Value>(left->toFloat() * right->toFloat());
            } else {
                // Integer multiplication with overflow detection
                int a = left->toInt();
                int b = right->toInt();

                // Check for overflow before multiplication
                // Handle special cases first
                if (a == 0 || b == 0) {
                    result_ = std::make_shared<Value>(0);
                } else if (a == INT_MIN || b == INT_MIN) {
                    // INT_MIN * anything (except 0, 1) will overflow
                    if ((a == INT_MIN && (b != 1 && b != 0)) || (b == INT_MIN && (a != 1 && a != 0))) {
                        std::ostringstream oss;
                        oss << "Math error: Integer overflow in multiplication\n\n";
                        oss << "  Expression: " << a << " * " << b << "\n";
                        oss << "  INT_MAX: " << INT_MAX << "\n";
                        oss << "  INT_MIN: " << INT_MIN << "\n";
                        oss << "\n  Help:\n";
                        oss << "  - Integer overflow occurs when result exceeds 32-bit int range\n";
                        oss << "  - Use float for larger numbers: " << a << ".0 * " << b << ".0\n";
                        oss << "\n  Example:\n";
                        oss << "    ✗ Wrong: let result = 1000000 * 10000  (overflow!)\n";
                        oss << "    ✓ Right: let result = 1000000.0 * 10000.0  (use float)\n";
                        throw std::runtime_error(oss.str());
                    }
                    result_ = std::make_shared<Value>(a * b);
                } else if ((a > 0 && b > 0 && a > INT_MAX / b) ||
                           (a < 0 && b < 0 && a < INT_MAX / b) ||
                           (a > 0 && b < 0 && b < INT_MIN / a) ||
                           (a < 0 && b > 0 && a < INT_MIN / b)) {
                    std::ostringstream oss;
                    oss << "Math error: Integer overflow in multiplication\n\n";
                    oss << "  Expression: " << a << " * " << b << "\n";
                    oss << "  INT_MAX: " << INT_MAX << "\n";
                    oss << "  INT_MIN: " << INT_MIN << "\n";
                    oss << "\n  Help:\n";
                    oss << "  - Integer overflow occurs when result exceeds 32-bit int range\n";
                    oss << "  - Use float for larger numbers: " << a << ".0 * " << b << ".0\n";
                    oss << "\n  Example:\n";
                    oss << "    ✗ Wrong: let result = 1000000 * 10000  (overflow!)\n";
                    oss << "    ✓ Right: let result = 1000000.0 * 10000.0  (use float)\n";
                    throw std::runtime_error(oss.str());
                } else {
                    result_ = std::make_shared<Value>(a * b);
                }
            }
            break;
        }

        case ast::BinaryOp::Div: {
            // Type check: Division requires numeric types
            bool left_is_numeric = std::holds_alternative<int>(left->data) ||
                                  std::holds_alternative<double>(left->data) ||
                                  std::holds_alternative<bool>(left->data);
            bool right_is_numeric = std::holds_alternative<int>(right->data) ||
                                   std::holds_alternative<double>(right->data) ||
                                   std::holds_alternative<bool>(right->data);

            if (!left_is_numeric || !right_is_numeric) {
                std::ostringstream oss;
                oss << "Type error: Division (/) requires numeric types\n\n";

                if (!left_is_numeric && !right_is_numeric) {
                    oss << "  Both operands are non-numeric:\n";
                    oss << "    Left: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Right: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                } else if (!left_is_numeric) {
                    oss << "  Left operand is non-numeric:\n";
                    oss << "    Got: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Expected: int, float, or bool\n";
                } else {
                    oss << "  Right operand is non-numeric:\n";
                    oss << "    Got: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                    oss << "    Expected: int, float, or bool\n";
                }

                oss << "\n  Help:\n";
                oss << "  - For numbers: Use int or float values\n";
                oss << "  - For string splitting: Use string.split() instead\n";
                oss << "\n  Example:\n";
                oss << "    ✗ Wrong: \"10\" / 2     (string / int)\n";
                oss << "    ✓ Right: 10 / 2        (int / int)\n";

                throw std::runtime_error(oss.str());
            }

            // Check for division by zero
            double divisor = right->toFloat();
            if (divisor == 0.0) {
                std::ostringstream oss;
                oss << "Math error: Division by zero\n\n";
                oss << "  Expression: " << left->toString() << " / 0\n";
                oss << "\n  Help:\n";
                oss << "  - Division by zero is undefined in mathematics\n";
                oss << "  - Check if divisor is zero before dividing\n";
                oss << "  - Use conditional to handle zero case:\n";
                oss << "\n  Example:\n";
                oss << "    ✗ Wrong: let result = x / 0\n";
                oss << "    ✓ Right: let result = if (y != 0) { x / y } else { 0 }\n";
                oss << "\n  Common causes:\n";
                oss << "  - User input not validated\n";
                oss << "  - Variable initialized to 0\n";
                oss << "  - Logic error in calculation\n";
                throw std::runtime_error(oss.str());
            }

            result_ = std::make_shared<Value>(left->toFloat() / divisor);
            break;
        }

        case ast::BinaryOp::Mod: {
            // Type check: Modulo requires integer types
            bool left_is_int = std::holds_alternative<int>(left->data) ||
                              std::holds_alternative<bool>(left->data);
            bool right_is_int = std::holds_alternative<int>(right->data) ||
                               std::holds_alternative<bool>(right->data);

            if (!left_is_int || !right_is_int) {
                std::ostringstream oss;
                oss << "Type error: Modulo (%) requires integer types\n\n";

                if (!left_is_int && !right_is_int) {
                    oss << "  Both operands are non-integer:\n";
                    oss << "    Left: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Right: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                } else if (!left_is_int) {
                    oss << "  Left operand is non-integer:\n";
                    oss << "    Got: " << getTypeName(left) << " = \"" << left->toString() << "\"\n";
                    oss << "    Expected: int or bool\n";
                } else {
                    oss << "  Right operand is non-integer:\n";
                    oss << "    Got: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                    oss << "    Expected: int or bool\n";
                }

                oss << "\n  Help:\n";
                oss << "  - Modulo requires integers (int or bool)\n";
                oss << "  - For floats: Use fmod() or convert to int first\n";
                oss << "  - For string formatting: Use string interpolation\n";
                oss << "\n  Example:\n";
                oss << "    ✗ Wrong: \"10\" % 3     (string % int)\n";
                oss << "    ✗ Wrong: 10.5 % 3     (float % int)\n";
                oss << "    ✓ Right: 10 % 3       (int % int)\n";

                throw std::runtime_error(oss.str());
            }

            // Check for modulo by zero
            int divisor = right->toInt();
            if (divisor == 0) {
                std::ostringstream oss;
                oss << "Math error: Modulo by zero\n\n";
                oss << "  Expression: " << left->toString() << " % 0\n";
                oss << "\n  Help:\n";
                oss << "  - Modulo by zero is undefined in mathematics\n";
                oss << "  - Check if divisor is zero before using modulo\n";
                oss << "  - Use conditional to handle zero case:\n";
                oss << "\n  Example:\n";
                oss << "    ✗ Wrong: let remainder = x % 0\n";
                oss << "    ✓ Right: let remainder = if (y != 0) { x % y } else { 0 }\n";
                oss << "\n  Common causes:\n";
                oss << "  - User input not validated\n";
                oss << "  - Variable initialized to 0\n";
                oss << "  - Logic error in calculation\n";
                throw std::runtime_error(oss.str());
            }

            result_ = std::make_shared<Value>(left->toInt() % divisor);
            break;
        }

        case ast::BinaryOp::Eq: {
            // Type-aware equality comparison
            // For numeric types: compare numerically (10 == 10.0 is true)
            // For strings: compare as strings
            // For different types: false (no implicit coercion)

            // Check for null comparisons first (null == null should be true)
            bool left_null = isNull(left);
            bool right_null = isNull(right);

            if (left_null && right_null) {
                // Both null: equal
                result_ = std::make_shared<Value>(true);
            } else if (left_null || right_null) {
                // One null, one non-null: not equal
                result_ = std::make_shared<Value>(false);
            } else {
                // Neither is null - proceed with type-specific comparisons
                bool left_is_numeric = std::holds_alternative<int>(left->data) ||
                                      std::holds_alternative<double>(left->data);
                bool right_is_numeric = std::holds_alternative<int>(right->data) ||
                                       std::holds_alternative<double>(right->data);

                if (std::holds_alternative<bool>(left->data) &&
                    std::holds_alternative<bool>(right->data)) {
                    // Both bools: compare as bools
                    result_ = std::make_shared<Value>(left->toBool() == right->toBool());
                } else if (left_is_numeric && right_is_numeric) {
                    // Both numeric: compare as numbers
                    result_ = std::make_shared<Value>(left->toFloat() == right->toFloat());
                } else if (std::holds_alternative<std::string>(left->data) &&
                           std::holds_alternative<std::string>(right->data)) {
                    // Both strings: compare as strings
                    result_ = std::make_shared<Value>(left->toString() == right->toString());
                } else {
                    // Different types: not equal
                    result_ = std::make_shared<Value>(false);
                }
            }
            break;
        }

        case ast::BinaryOp::Ne: {
            // Type-aware inequality comparison (inverse of Eq)

            // Check for null comparisons first (null != null should be false)
            bool left_null = isNull(left);
            bool right_null = isNull(right);

            if (left_null && right_null) {
                // Both null: not different (false)
                result_ = std::make_shared<Value>(false);
            } else if (left_null || right_null) {
                // One null, one non-null: different (true)
                result_ = std::make_shared<Value>(true);
            } else {
                // Neither is null - proceed with type-specific comparisons
                bool left_is_numeric = std::holds_alternative<int>(left->data) ||
                                      std::holds_alternative<double>(left->data);
                bool right_is_numeric = std::holds_alternative<int>(right->data) ||
                                       std::holds_alternative<double>(right->data);

                if (std::holds_alternative<bool>(left->data) &&
                    std::holds_alternative<bool>(right->data)) {
                    // Both bools: compare as bools
                    result_ = std::make_shared<Value>(left->toBool() != right->toBool());
                } else if (left_is_numeric && right_is_numeric) {
                    // Both numeric: compare as numbers
                    result_ = std::make_shared<Value>(left->toFloat() != right->toFloat());
                } else if (std::holds_alternative<std::string>(left->data) &&
                           std::holds_alternative<std::string>(right->data)) {
                    // Both strings: compare as strings
                    result_ = std::make_shared<Value>(left->toString() != right->toString());
                } else {
                    // Different types: not equal
                    result_ = std::make_shared<Value>(true);
                }
            }
            break;
        }

        case ast::BinaryOp::Lt:
            if (std::holds_alternative<std::string>(left->data) &&
                std::holds_alternative<std::string>(right->data)) {
                result_ = std::make_shared<Value>(
                    std::get<std::string>(left->data) < std::get<std::string>(right->data));
            } else {
                result_ = std::make_shared<Value>(left->toFloat() < right->toFloat());
            }
            break;

        case ast::BinaryOp::Le:
            if (std::holds_alternative<std::string>(left->data) &&
                std::holds_alternative<std::string>(right->data)) {
                result_ = std::make_shared<Value>(
                    std::get<std::string>(left->data) <= std::get<std::string>(right->data));
            } else {
                result_ = std::make_shared<Value>(left->toFloat() <= right->toFloat());
            }
            break;

        case ast::BinaryOp::Gt:
            if (std::holds_alternative<std::string>(left->data) &&
                std::holds_alternative<std::string>(right->data)) {
                result_ = std::make_shared<Value>(
                    std::get<std::string>(left->data) > std::get<std::string>(right->data));
            } else {
                result_ = std::make_shared<Value>(left->toFloat() > right->toFloat());
            }
            break;

        case ast::BinaryOp::Ge:
            if (std::holds_alternative<std::string>(left->data) &&
                std::holds_alternative<std::string>(right->data)) {
                result_ = std::make_shared<Value>(
                    std::get<std::string>(left->data) >= std::get<std::string>(right->data));
            } else {
                result_ = std::make_shared<Value>(left->toFloat() >= right->toFloat());
            }
            break;

        case ast::BinaryOp::Pipeline: {
            // Phase 3.4: Pipeline operator (|>)
            // Passes left value as first argument to right function
            // Example: data |> func means func(data)
            //         data |> func(x) means func(data, x)

            LOG_DEBUG("[Pipeline] Starting pipeline operation\n");
            LOG_DEBUG("[Pipeline] Left value: {}\n", left->toString());

            // Right side must be a function call or identifier
            if (auto* call = dynamic_cast<ast::CallExpr*>(node.getRight())) {
                LOG_DEBUG("[Pipeline] Right side is CallExpr with {} args\n", call->getArgs().size());

                // If right is a call: func(args...), insert left as first arg
                // Create a new argument list with left prepended
                std::vector<std::shared_ptr<Value>> args;
                args.push_back(left);  // Piped value goes first

                // Add existing arguments
                for (const auto& arg_expr : call->getArgs()) {
                    auto arg_val = eval(*arg_expr);
                    LOG_DEBUG("[Pipeline] Adding arg: {}\n", arg_val->toString());
                    args.push_back(arg_val);
                }

                LOG_DEBUG("[Pipeline] Total args after prepending: {}\n", args.size());

                // Evaluate the callee
                auto callee = eval(*call->getCallee());
                LOG_DEBUG("[Pipeline] Callee evaluated\n");

                // Call the function with the modified arguments
                if (auto* block = std::get_if<std::shared_ptr<BlockValue>>(&callee->data)) {
                    // Block execution - call the block's main function
                    auto* executor = (*block)->getExecutor();
                    if (!executor) {
                        throw std::runtime_error("No executor for block in pipeline");
                    }
                    result_ = executor->callFunction((*block)->metadata.block_id, args);
                    flushExecutorOutput(executor);  // Phase 11.1: Flush captured output

                    // Phase 4.4: Record block usage for analytics
                    if (block_loader_) {
                        int tokens_saved = ((*block)->metadata.token_count > 0)
                            ? (*block)->metadata.token_count
                            : 50;
                        block_loader_->recordBlockUsage((*block)->metadata.block_id, tokens_saved);

                        // Record block pair if there was a previous block
                        if (!last_executed_block_id_.empty()) {
                            block_loader_->recordBlockPair(last_executed_block_id_, (*block)->metadata.block_id);
                        }
                        last_executed_block_id_ = (*block)->metadata.block_id;
                    }
                } else if (auto* func = std::get_if<std::shared_ptr<FunctionValue>>(&callee->data)) {
                    // User-defined function execution - use callFunction for proper validation
                    // This ensures argument count validation and default parameter handling
                    LOG_DEBUG("[Pipeline] Calling function {} with {} args\n", (*func)->name, args.size());
                    result_ = callFunction(callee, args);
                } else {
                    std::ostringstream oss;
                    oss << "Type error: Pipeline requires callable function\n\n";
                    oss << "  Right side type: " << getTypeName(callee) << "\n";
                    oss << "  Expected: function or block\n\n";
                    oss << "  Help:\n";
                    oss << "  - Pipeline operator |> passes left value to a function\n";
                    oss << "  - Right side must be a function call or identifier\n\n";
                    oss << "  Example:\n";
                    oss << "    ✗ Wrong: value |> 42\n";
                    oss << "    ✓ Right: value |> processFunc()\n";
                    oss << "    ✓ Right: value |> transform\n";
                    throw std::runtime_error(oss.str());
                }

            } else if (auto* id = dynamic_cast<ast::IdentifierExpr*>(node.getRight())) {
                // If right is identifier: funcName, create call with left as argument
                auto callee = eval(*id);
                std::vector<std::shared_ptr<Value>> args = {left};

                if (auto* block = std::get_if<std::shared_ptr<BlockValue>>(&callee->data)) {
                    auto* executor = (*block)->getExecutor();
                    if (!executor) {
                        throw std::runtime_error("No executor for block in pipeline");
                    }
                    result_ = executor->callFunction((*block)->metadata.block_id, args);
                    flushExecutorOutput(executor);  // Phase 11.1: Flush captured output

                    // Phase 4.4: Record block usage for analytics
                    if (block_loader_) {
                        int tokens_saved = ((*block)->metadata.token_count > 0)
                            ? (*block)->metadata.token_count
                            : 50;
                        block_loader_->recordBlockUsage((*block)->metadata.block_id, tokens_saved);

                        // Record block pair if there was a previous block
                        if (!last_executed_block_id_.empty()) {
                            block_loader_->recordBlockPair(last_executed_block_id_, (*block)->metadata.block_id);
                        }
                        last_executed_block_id_ = (*block)->metadata.block_id;
                    }
                } else if (auto* func = std::get_if<std::shared_ptr<FunctionValue>>(&callee->data)) {
                    (void)func;  // Type check only, value not needed
                    // Use callFunction for proper validation
                    std::vector<std::shared_ptr<Value>> args = {left};
                    result_ = callFunction(callee, args);
                } else {
                    std::ostringstream oss;
                    oss << "Type error: Pipeline requires callable function\n\n";
                    oss << "  Identifier type: " << getTypeName(callee) << "\n";
                    oss << "  Expected: function or block\n\n";
                    oss << "  Help:\n";
                    oss << "  - Pipeline operator |> passes left value to a function\n";
                    oss << "  - The identifier must refer to a function\n\n";
                    oss << "  Example:\n";
                    oss << "    let transform = function(x) { return x * 2 }\n";
                    oss << "    ✗ Wrong: let x = 42; value |> x  // x is not a function\n";
                    oss << "    ✓ Right: value |> transform\n";
                    throw std::runtime_error(oss.str());
                }
            } else {
                // Try evaluating the right side as an expression (handles lambdas, etc.)
                auto callee = eval(*node.getRight());
                if (auto* func = std::get_if<std::shared_ptr<FunctionValue>>(&callee->data)) {
                    (void)func;
                    std::vector<std::shared_ptr<Value>> args = {left};
                    result_ = callFunction(callee, args);
                } else {
                    std::ostringstream oss;
                    oss << "Type error: Pipeline requires callable function\n\n";
                    oss << "  Right side type: " << getTypeName(callee) << "\n";
                    oss << "  Expected: function, lambda, or block\n\n";
                    oss << "  Help:\n";
                    oss << "  - Use function call: value |> func(arg1, arg2)\n";
                    oss << "  - Use identifier: value |> transform\n";
                    oss << "  - Use lambda: value |> (x) => x * 2\n\n";
                    oss << "  Example:\n";
                    oss << "    ✓ Right: 100 |> subtract(50)\n";
                    oss << "    ✓ Right: 100 |> double\n";
                    oss << "    ✓ Right: 100 |> (x) => x * 2\n";
                    throw std::runtime_error(oss.str());
                }
            }
            break;
        }

        case ast::BinaryOp::In: {
            // Containment check: item in collection
            // dict: check if key exists
            if (auto* dict_ptr = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&right->data)) {
                std::string key = left->toString();
                result_ = std::make_shared<Value>(dict_ptr->find(key) != dict_ptr->end());
            }
            // array: check if item is in array
            else if (auto* arr_ptr = std::get_if<std::vector<std::shared_ptr<Value>>>(&right->data)) {
                bool found = false;
                std::string needle = left->toString();
                for (const auto& item : *arr_ptr) {
                    if (item->toString() == needle) { found = true; break; }
                }
                result_ = std::make_shared<Value>(found);
            }
            // string: check if substring exists
            else if (auto* str_ptr = std::get_if<std::string>(&right->data)) {
                std::string needle = left->toString();
                result_ = std::make_shared<Value>(str_ptr->find(needle) != std::string::npos);
            }
            else {
                throw std::runtime_error(
                    "Type error: 'in' operator requires a dict, array, or string on the right side\n\n"
                    "  Got: " + getTypeName(right) + "\n\n"
                    "  Example:\n"
                    "    if \"key\" in my_dict { }    // dict key check\n"
                    "    if item in my_array { }     // array membership\n"
                    "    if \"sub\" in my_string { }  // substring check\n");
            }
            break;
        }

        case ast::BinaryOp::Subscript: {
            // Dictionary or list subscript: obj[key] or arr[index]

            // Check if left is a dictionary
            if (auto* dict_ptr = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&left->data)) {
                auto& dict = *dict_ptr;
                std::string key = right->toString();

                auto it = dict.find(key);
                if (it == dict.end()) {
                    std::ostringstream oss;
                    oss << "Key error: Dictionary key not found\n\n";
                    oss << "  Key: \"" << key << "\"\n";

                    if (dict.empty()) {
                        oss << "  Dictionary is empty\n";
                    } else {
                        oss << "  Available keys: ";
                        size_t count = 0;
                        for (const auto& pair : dict) {
                            if (count > 0) oss << ", ";
                            oss << "\"" << pair.first << "\"";
                            if (++count >= 10) {
                                oss << "...";
                                break;
                            }
                        }
                        oss << "\n";
                    }

                    oss << "\n  Help:\n";
                    oss << "  - Use dict.get(\"" << key << "\") for safe access (returns null if missing)\n";
                    oss << "  - Use dict.get(\"" << key << "\", default_value) to provide a default\n";
                    oss << "  - Use dict.has(\"" << key << "\") to check before accessing\n";
                    oss << "  - Keys are case-sensitive\n\n";
                    oss << "  Example:\n";
                    oss << "    let d = {\"name\": \"Alice\"}\n";
                    oss << "    ✗ Throws: d[\"" << key << "\"]\n";
                    oss << "    ✓ Safe:   d.get(\"" << key << "\", \"default\")\n";
                    throw std::runtime_error(oss.str());
                }

                result_ = it->second;
                break;
            }

            // Check if left is a list
            if (auto* list_ptr = std::get_if<std::vector<std::shared_ptr<Value>>>(&left->data)) {
                auto& list = *list_ptr;

                // Type check: Array indices must be integers, not strings
                if (!std::holds_alternative<int>(right->data) &&
                    !std::holds_alternative<bool>(right->data)) {
                    std::ostringstream oss;
                    oss << "Type error: Array index must be an integer\n\n";
                    oss << "  Got: " << getTypeName(right) << " = \"" << right->toString() << "\"\n";
                    oss << "  Expected: int\n";
                    oss << "\n  Help:\n";
                    oss << "  - Array indices must be integers (int or bool)\n";
                    oss << "  - Strings are not automatically converted to numbers\n";
                    oss << "  - For string keys, use a dictionary instead\n";
                    oss << "\n  Example:\n";
                    oss << "    ✗ Wrong: arr[\"0\"]      (string index)\n";
                    oss << "    ✓ Right: arr[0]         (int index)\n";
                    oss << "    ✓ Right: dict[\"key\"]   (use dict for string keys)\n";
                    throw std::runtime_error(oss.str());
                }

                int index = right->toInt();

                if (index < 0 || index >= static_cast<int>(list.size())) {
                    std::ostringstream oss;
                    oss << "Index error: Array index out of bounds\n\n";
                    oss << "  Index: " << index << "\n";
                    oss << "  Array size: " << list.size() << "\n";
                    oss << "  Valid range: 0 to " << (list.size() > 0 ? list.size() - 1 : 0) << "\n\n";
                    oss << "  Help:\n";
                    oss << "  - Array indices start at 0\n";
                    oss << "  - Check array size before accessing\n";
                    oss << "  - Use array.length(arr) to get size\n\n";
                    oss << "  Example:\n";
                    oss << "    let arr = [10, 20, 30]  // size = 3\n";
                    oss << "    ✗ Wrong: arr[3]  // out of bounds\n";
                    oss << "    ✓ Right: arr[2]  // last element\n";
                    throw std::runtime_error(oss.str());
                }

                result_ = list[static_cast<size_t>(index)];
                break;
            }

            std::ostringstream oss;
            std::string type_name = getTypeName(left);
            oss << "Type error: Subscript operation not supported\n\n";
            oss << "  Tried to subscript: " << type_name << "\n";
            oss << "  Supported types: array, dict\n\n";
            oss << "  Help:\n";
            if (type_name == "null" || type_name == "unknown") {
                oss << "  - The value is null/undefined. This often means:\n";
                oss << "    - A polyglot block (<<python/js/cpp>>) failed and returned null\n";
                oss << "    - A function didn't return a value\n";
                oss << "    - A variable was never assigned\n";
                oss << "  - Check the output above for [PY ADAPTER ERROR] or similar messages\n";
                oss << "  - Add error handling: if result != null { result[\"key\"] }\n\n";
            } else {
                oss << "  - Only arrays and dictionaries support subscript access []\n";
                oss << "  - For arrays: use integer indices (arr[0], arr[1])\n";
                oss << "  - For dicts: use string keys (dict[\"key\"])\n\n";
            }
            oss << "  Example:\n";
            oss << "    ✗ Wrong: let x = 42; x[0]  // int doesn't support subscript\n";
            oss << "    ✓ Right: let arr = [1, 2, 3]; arr[0]\n";
            oss << "    ✓ Right: let dict = {\"a\": 1}; dict[\"a\"]\n";
            throw std::runtime_error(oss.str());
        }

        default:
            result_ = std::make_shared<Value>();
    }

    // Phase 3.2: Track allocation for automatic GC
    trackAllocation();
}

void Interpreter::visit(ast::UnaryExpr& node) {
    auto operand = eval(*node.getOperand());

    switch (node.getOp()) {
        case ast::UnaryOp::Neg:
            if (std::holds_alternative<double>(operand->data)) {
                result_ = std::make_shared<Value>(-operand->toFloat());
            } else {
                int val = operand->toInt();
                if (val == INT_MIN) {
                    std::ostringstream oss;
                    oss << "Math error: Integer overflow in negation\n\n";
                    oss << "  Expression: -(" << val << ")\n";
                    oss << "  -INT_MIN (" << val << ") exceeds INT_MAX (" << INT_MAX << ")\n\n";
                    oss << "  Help:\n";
                    oss << "  - Use float for this value: -(" << val << ".0)\n";
                    throw std::runtime_error(oss.str());
                }
                result_ = std::make_shared<Value>(-val);
            }
            break;

        case ast::UnaryOp::Not:
            result_ = std::make_shared<Value>(!operand->toBool());
            break;

        default:
            result_ = operand;
    }

    // Phase 3.2: Track allocation for automatic GC
    trackAllocation();
}

void Interpreter::visit(ast::IdentifierExpr& node) {
    // Get all names from current scope for better error suggestions
    auto all_names = current_env_->getAllNames();

    try {
        result_ = current_env_->get(node.getName());
    } catch (const std::runtime_error& e) {
        // Phase 3.1: Enhanced error reporting with source context and suggestions from current scope
        if (!source_code_.empty()) {
            auto loc = node.getLocation();

            // Generate main error message
            std::string main_msg = "Undefined variable: " + node.getName();

            // Generate suggestion from current scope (not from parent where error was thrown)
            auto suggestion = error::suggestForUndefinedVariable(node.getName(), all_names);

            error_reporter_.error(main_msg, static_cast<size_t>(loc.line), static_cast<size_t>(loc.column));
            if (!suggestion.empty()) {
                error_reporter_.addSuggestion(suggestion);
            }

            error_reporter_.printAllWithSource();
            error_reporter_.clear();
        }

        // BUGFIX: Convert to NaabError with full call stack for cross-module error reporting
        throw createError(e.what(), ErrorType::RUNTIME_ERROR);
    }
}

void Interpreter::visit(ast::LiteralExpr& node) {
    switch (node.getLiteralKind()) {
        case ast::LiteralKind::Int: {
            // Use stod first to avoid overflow, then check if it fits in int
            try {
                double d = std::stod(node.getValue());
                if (d >= INT_MIN && d <= INT_MAX && d == static_cast<int>(d)) {
                    result_ = std::make_shared<Value>(static_cast<int>(d));
                } else {
                    // Too large for int, store as double
                    result_ = std::make_shared<Value>(d);
                }
            } catch (const std::exception& e) {
                throw std::runtime_error("Invalid integer literal: " + node.getValue());
            }
            break;
        }
        case ast::LiteralKind::Float:
            result_ = std::make_shared<Value>(std::stod(node.getValue()));
            break;

        case ast::LiteralKind::String: {
            const std::string& raw = node.getValue();
            // Check if string contains interpolation ${...}
            if (raw.find("${") != std::string::npos) {
                std::string result;
                size_t i = 0;
                while (i < raw.size()) {
                    if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
                        // Extract expression inside ${...}
                        i += 2; // skip ${
                        int depth = 1;
                        std::string expr_text;
                        while (i < raw.size() && depth > 0) {
                            if (raw[i] == '{') depth++;
                            else if (raw[i] == '}') {
                                depth--;
                                if (depth == 0) break;
                            }
                            expr_text += raw[i];
                            i++;
                        }
                        if (i < raw.size()) i++; // skip closing }

                        // Lex, parse, and evaluate the expression
                        try {
                            naab::lexer::Lexer expr_lexer(expr_text);
                            auto expr_tokens = expr_lexer.tokenize();
                            naab::parser::Parser expr_parser(expr_tokens);
                            auto expr_ast = expr_parser.parseExpression();
                            expr_ast->accept(*this);
                            if (result_) {
                                result += result_->toString();
                            }
                        } catch (const std::exception& e) {
                            // Rethrow with context about the interpolation
                            std::string interp_err = std::string(e.what());
                            interp_err += "\n\n  Error occurred inside string interpolation: ${" + expr_text + "}\n"
                                         "  The expression inside ${...} must be a valid NAAb expression.\n"
                                         "  If calling a function stored in a variable, call it directly:\n"
                                         "    \"${myFunc()}\"              // correct\n"
                                         "    \"${Sys.callFunction(fn)}\"  // WRONG - no Sys in NAAb";
                            throw std::runtime_error(interp_err);
                        }
                    } else {
                        result += raw[i];
                        i++;
                    }
                }
                result_ = std::make_shared<Value>(result);
            } else {
                result_ = std::make_shared<Value>(raw);
            }
            break;
        }

        case ast::LiteralKind::Bool:
            result_ = std::make_shared<Value>(node.getValue() == "true");
            break;

        case ast::LiteralKind::Null:
            result_ = std::make_shared<Value>();  // monostate represents null
            break;
    }
}

void Interpreter::visit(ast::DictExpr& node) {
    std::unordered_map<std::string, std::shared_ptr<Value>> dict;
    for (const auto& [key_expr, val_expr] : node.getEntries()) {
        auto key = eval(*key_expr);
        auto val = eval(*val_expr);
        dict[key->toString()] = val;
    }
    result_ = std::make_shared<Value>(dict);

    // Phase 3.2: Track allocation for automatic GC
    trackAllocation();
}

void Interpreter::visit(ast::ListExpr& node) {
    std::vector<std::shared_ptr<Value>> list;
    for (auto& elem : node.getElements()) {
        list.push_back(eval(*elem));
    }
    result_ = std::make_shared<Value>(list);

    // Phase 3.2: Track allocation for automatic GC
    trackAllocation();
}

void Interpreter::visit(ast::RangeExpr& node) {
    // Evaluate start and end expressions
    auto start_val = eval(*node.getStart());
    auto end_val = eval(*node.getEnd());

    // Both must be integers
    int start = start_val->toInt();
    int end = end_val->toInt();

    // Create a special dict to represent the range
    // This is a lightweight representation - actual values generated during iteration
    std::unordered_map<std::string, std::shared_ptr<Value>> range_dict;
    range_dict["__is_range"] = std::make_shared<Value>(true);
    range_dict["__range_start"] = std::make_shared<Value>(start);
    range_dict["__range_end"] = std::make_shared<Value>(end);
    range_dict["__range_inclusive"] = std::make_shared<Value>(node.isInclusive());

    result_ = std::make_shared<Value>(range_dict);

    // Phase 3.2: Track allocation for automatic GC
    trackAllocation();
}

void Interpreter::visit(ast::StructLiteralExpr& node) {
    explain("Creating instance of struct '" + node.getStructName() + "'");

    profileStart("Struct creation");

    std::shared_ptr<StructDef> struct_def;
    std::string struct_name = node.getStructName();

    // ISS-024 Fix: Check for module-qualified struct name (module.StructName)
    size_t dot_pos = struct_name.find('.');
    if (dot_pos != std::string::npos) {
        // Split into module and struct name
        std::string module_alias = struct_name.substr(0, dot_pos);
        std::string actual_struct_name = struct_name.substr(dot_pos + 1);

        // Look up module environment
        auto module_it = loaded_modules_.find(module_alias);
        if (module_it == loaded_modules_.end()) {
            throw std::runtime_error("Module not found: " + module_alias);
        }

        // Get struct from module's exported structs
        auto& module_env = module_it->second;
        auto struct_it = module_env->exported_structs_.find(actual_struct_name);
        if (struct_it == module_env->exported_structs_.end()) {
            throw std::runtime_error("Struct '" + actual_struct_name +
                                   "' not found in module '" + module_alias + "'");
        }
        struct_def = struct_it->second;
        struct_name = actual_struct_name;  // Use unqualified name for error messages
    } else {
        // Normal struct lookup
        struct_def = runtime::StructRegistry::instance().getStruct(struct_name);
        if (!struct_def) {
            throw std::runtime_error("Undefined struct: " + struct_name);
        }
    }

    // Phase 2.4.1: Handle generic structs via monomorphization
    std::shared_ptr<StructDef> actual_def = struct_def;
    std::string actual_type_name = struct_name;  // ISS-024 Fix: Use unqualified name

    if (!struct_def->type_parameters.empty()) {
        // This is a generic struct - infer type arguments from field values
        auto type_bindings = inferTypeBindings(
            struct_def->type_parameters,
            struct_def->fields,
            node.getFieldInits()
        );

        // Create monomorphized version
        actual_def = monomorphizeStruct(struct_def, type_bindings);
        actual_type_name = actual_def->name;  // Use mangled name (e.g., "Box_int")

        // Register the specialized struct if not already registered
        if (!runtime::StructRegistry::instance().getStruct(actual_type_name)) {
            runtime::StructRegistry::instance().registerStruct(actual_def);

            if (isVerboseMode()) {
                fmt::print("[VERBOSE] Monomorphized {} -> {}\n",
                           node.getStructName(), actual_type_name);
            }
        }
    }

    auto struct_val = std::make_shared<StructValue>();
    struct_val->type_name = actual_type_name;
    struct_val->definition = actual_def;
    struct_val->field_values.resize(actual_def->fields.size());

    // Initialize from literals
    for (const auto& [field_name, init_expr] : node.getFieldInits()) {
        if (!actual_def->field_index.count(field_name)) {
            throw std::runtime_error("Unknown field '" + field_name +
                                   "' in struct '" + node.getStructName() + "'");
        }

        auto field_value = eval(*init_expr);
        size_t idx = actual_def->field_index.at(field_name);

        // Phase 2.4.2: Validate field value type matches declared type
        const ast::Type& field_type = actual_def->fields[idx].type;

        // Check union field types
        if (field_type.kind == ast::TypeKind::Union) {
            if (!valueMatchesUnion(field_value, field_type.union_types)) {
                throw std::runtime_error(
                    "Type error: Field '" + field_name +
                    "' of struct '" + node.getStructName() +
                    "' expects " + formatTypeName(field_type) +
                    ", but got " + getValueTypeName(field_value)
                );
            }
        }
        // Check non-union field types (if not Any)
        else if (field_type.kind != ast::TypeKind::Any) {
            if (!valueMatchesType(field_value, field_type)) {
                throw std::runtime_error(
                    "Type error: Field '" + field_name +
                    "' of struct '" + node.getStructName() +
                    "' expects " + formatTypeName(field_type) +
                    ", but got " + getValueTypeName(field_value)
                );
            }
        }

        struct_val->field_values[idx] = field_value;
    }

    // Check required fields (all fields are currently required - no default values yet)
    for (size_t i = 0; i < actual_def->fields.size(); ++i) {
        if (!struct_val->field_values[i]) {
            const auto& field = actual_def->fields[i];
            throw std::runtime_error("Missing required field '" + field.name +
                                   "' in struct '" + node.getStructName() + "'");
        }
    }

    result_ = std::make_shared<Value>(struct_val);

    profileEnd("Struct creation");

    // Phase 3.2: Track allocation for automatic GC
    trackAllocation();
}

} // namespace interpreter
} // namespace naab
