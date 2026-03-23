// NAAb Interpreter — Function Calls & Method Dispatch
// Split from interpreter.cpp for maintainability
//
// Contains: callFunction, visit(CallExpr), visit(MemberExpr)
// These three methods are kept together because BUG-E1 was caused by
// callFunction and CallExpr having divergent type checking paths.

#include "naab/interpreter.h"
#include "naab/logger.h"
#include "naab/limits.h"
#include "naab/language_registry.h"
#include "naab/stdlib_new_modules.h"
#include "naab/struct_registry.h"
#include "naab/error_helpers.h"
#include "naab/js_executor_adapter.h"
#include <fmt/core.h>
#include <iostream>
#include <sstream>
#include <climits>

namespace naab {
namespace interpreter {

// File-local helper (duplicated from interpreter.cpp — static linkage)
static std::string getTypeName(const std::shared_ptr<Value>& val) {
    return std::visit([](auto&& arg) -> std::string {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, int>) { return "int"; }
        else if constexpr (std::is_same_v<T, double>) { return "float"; }
        else if constexpr (std::is_same_v<T, bool>) { return "bool"; }
        else if constexpr (std::is_same_v<T, std::string>) { return "string"; }
        else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<Value>>>) { return "array"; }
        else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<Value>>>) { return "dict"; }
        else if constexpr (std::is_same_v<T, std::shared_ptr<FunctionValue>>) { return "function"; }
        else if constexpr (std::is_same_v<T, std::shared_ptr<StructValue>>) { return "struct"; }
        else if constexpr (std::is_same_v<T, std::shared_ptr<FutureValue>>) { return "future"; }
        else if constexpr (std::is_same_v<T, std::monostate>) { return "null"; }
        return "unknown";
    }, val->data);
}


// Call a function value with arguments (for higher-order functions like map/filter/reduce)
std::shared_ptr<Value> Interpreter::callFunction(std::shared_ptr<Value> fn,
                                                  const std::vector<std::shared_ptr<Value>>& args) {
    // Week 1, Task 1.3: Check call depth to prevent stack overflow
    // Governance: Use governance call depth limit if configured
    size_t max_depth = limits::MAX_CALL_STACK_DEPTH;
    if (governance_ && governance_->isActive()) {
        auto& rules = governance_->getRules();
        if (rules.max_call_depth > 0) {
            max_depth = static_cast<size_t>(rules.max_call_depth);
        }
        std::string err = governance_->checkCallDepth(call_depth_ + 1);
        if (!err.empty()) throw std::runtime_error(err);

        // BUG-AD: Reset lastReturnTainted before each function call
        // Prevents stale taint from a previous function leaking to the next
        governance_->setLastReturnTainted(false);
    }
    if (++call_depth_ > max_depth) {
        --call_depth_;
        throw limits::RecursionLimitException(
            "Call stack depth exceeded: " +
            std::to_string(call_depth_) + " > " + std::to_string(max_depth)
        );
    }

    // Ensure depth is decremented on return
    struct CallDepthGuard {
        size_t& depth;
        explicit CallDepthGuard(size_t& d) : depth(d) {}
        ~CallDepthGuard() { --depth; }
    } guard(call_depth_);

    // Check if it's a function value
    auto* func_ptr = std::get_if<std::shared_ptr<FunctionValue>>(&fn->data);
    if (!func_ptr) {
        std::ostringstream oss;
        oss << "Type error: Cannot call non-function value\n\n";
        oss << "  Attempted to call: " << getTypeName(fn) << "\n";
        oss << "  Expected: function\n\n";
        oss << "  Help:\n";
        oss << "  - Only functions can be called with ()\n";
        oss << "  - Check if the variable holds a function\n";
        oss << "  - Use typeof() or debug.type() to inspect the type\n\n";
        oss << "  Example:\n";
        oss << "    ✗ Wrong: let x = 42; x()  // calling an int\n";
        oss << "    ✓ Right: let f = function() { ... }; f()\n";
        throw std::runtime_error(oss.str());
    }
    auto& func = *func_ptr;

    // Phase 5: Generator function — return GeneratorValue instead of executing
    if (func->is_generator) {
        // FIX-E1b: Validate param types before creating generator
        for (size_t i = 0; i < args.size() && i < func->param_types.size(); i++) {
            const auto& pt = func->param_types[i];
            if (pt.kind == ast::TypeKind::Any) continue;
            if (!pt.is_nullable && isNull(args[i])) {
                throw std::runtime_error(
                    "Null safety error: Cannot pass null to non-nullable parameter '" +
                    func->params[i] + "' of function '" + func->name + "'");
            }
            if (pt.kind == ast::TypeKind::Union) {
                if (!valueMatchesUnion(args[i], pt.union_types)) {
                    throw std::runtime_error(
                        "Type error: Parameter '" + func->params[i] + "' of function '" +
                        func->name + "' expects " + formatTypeName(pt) +
                        ", but got " + getValueTypeName(args[i]));
                }
            } else if (!valueMatchesType(args[i], pt)) {
                throw std::runtime_error(
                    "Type error: Parameter '" + func->params[i] + "' of function '" +
                    func->name + "' expects " + formatTypeName(pt) +
                    ", but got " + getValueTypeName(args[i]));
            }
        }
        auto gen = std::make_shared<GeneratorValue>();
        gen->func = func;
        gen->args = args;
        result_ = std::make_shared<Value>(gen);
        return result_;
    }

    // Phase 6: Async function — launch on separate thread, return FutureValue
    if (func->is_async) {
        // Validate args synchronously first
        size_t min_args_check = 0;
        for (size_t i = 0; i < func->params.size(); i++) {
            if (!func->defaults[i]) min_args_check = i + 1;
        }
        if (args.size() < min_args_check || args.size() > func->params.size()) {
            throw std::runtime_error(
                "Function " + func->name + " expects " + std::to_string(min_args_check) +
                "-" + std::to_string(func->params.size()) + " arguments, got " + std::to_string(args.size())
            );
        }

        // Build environment for async execution
        auto parent_env = func->closure ? func->closure : global_env_;
        auto func_env = std::make_shared<Environment>(parent_env);
        for (size_t i = 0; i < args.size(); i++) {
            func_env->define(func->params[i], args[i]);
        }
        // Bind defaults for missing args
        for (size_t i = args.size(); i < func->params.size(); i++) {
            if (func->defaults[i]) {
                auto saved_env = current_env_;
                current_env_ = func_env;
                func->defaults[i]->accept(*this);
                auto default_val = result_;
                current_env_ = saved_env;
                func_env->define(func->params[i], default_val);
            }
        }

        // Capture what the async body needs
        auto body = func->body;
        auto global = global_env_;
        auto func_name = func->name;

        // BUG-I fix: Capture governance config path for async interpreter
        std::string gov_path;
        // BUG-AC: Capture taint state to propagate to async interpreter
        std::unordered_set<std::string> parent_taint;
        if (governance_ && governance_->isActive()) {
            gov_path = governance_->getLoadedPath();
            parent_taint = governance_->saveTaintState();
        }

        // BUG-AwaitExpr fix: Create FutureValue BEFORE lambda so lambda can capture taint flag
        auto future_val = std::make_shared<FutureValue>();
        future_val->description = "async fn " + func->name;
        future_val->func_name = func->name;  // BUG-K: for return contract check at await
        auto taint_flag = future_val->return_tainted;  // shared_ptr copy for lifetime safety

        auto shared_future = std::async(std::launch::async, [body, func_env, global, func_name, gov_path, parent_taint, taint_flag]() -> std::shared_ptr<Value> {
            Interpreter async_interp;
            async_interp.setGlobalEnv(global);

            // BUG-I fix: Load governance in async interpreter from same config
            if (!gov_path.empty()) {
                auto* gov = async_interp.getGovernance();
                if (gov) {
                    auto dir = std::filesystem::path(gov_path).parent_path();
                    gov->discoverAndLoad(dir.string());
                    // BUG-AC: Restore parent taint state in async interpreter
                    if (!parent_taint.empty()) {
                        gov->restoreTaintState(parent_taint);
                    }
                }
            }

            try {
                auto result = async_interp.executeBodyInEnv(*body, func_env);
                // BUG-AwaitExpr fix: Capture taint state BEFORE async_interp is destroyed
                if (async_interp.wasLastReturnTainted()) {
                    taint_flag->store(true);
                }
                return result;
            } catch (const std::exception& e) {
                throw std::runtime_error("Error in async " + func_name + ": " + e.what());
            }
        }).share();

        future_val->future = shared_future;
        return std::make_shared<Value>(future_val);
    }

    // Check parameter count
    size_t min_args = 0;
    for (size_t i = 0; i < func->params.size(); i++) {
        if (!func->defaults[i]) {
            min_args = i + 1;
        }
    }

    if (args.size() < min_args || args.size() > func->params.size()) {
        // Build parameter list for error message
        std::ostringstream oss;
        oss << "Function " << func->name << " expects " << min_args << "-"
            << func->params.size() << " arguments, got " << args.size() << "\n"
            << "  Function: " << func->name << "(";

        // Show parameters
        for (size_t i = 0; i < func->params.size(); ++i) {
            if (i > 0) oss << ", ";
            oss << func->params[i];
        }
        oss << ")\n";

        // Show what was provided
        oss << "  Provided: " << args.size() << " argument(s)";

        // Helpful hint if this might be a pipeline issue
        if (args.size() == 1 && func->params.size() > 1) {
            oss << "\n\n  Hint: If using pipeline operator (|>), it only passes the left side as the FIRST argument.\n"
                << "        For multi-arg functions: 100 |> subtract(50) becomes subtract(100, 50)";
        }

        throw std::runtime_error(oss.str());
    }

    // Governance v4: Check input contracts before execution
    if (governance_ && governance_->isActive() && !func->name.empty()) {
        std::vector<std::string> arg_types;
        for (const auto& arg : args) {
            arg_types.push_back(arg ? getTypeName(arg) : "null");
        }
        std::string input_err = governance_->checkFunctionInputContract(
            func->name, arg_types, func->source_line);
        if (!input_err.empty()) {
            governance_->logContractCheck(func->name, "FAIL", input_err,
                                          current_file_, func->source_line);
            throw std::runtime_error(input_err);
        }
    }

    // Create new environment for function execution with closure as parent (lexical scoping)
    auto parent_env = func->closure ? func->closure : global_env_;
    auto func_env = std::make_shared<Environment>(parent_env);

    // Bind provided arguments
    for (size_t i = 0; i < args.size(); i++) {
        func_env->define(func->params[i], args[i]);
    }

    // Bind default values for missing arguments
    for (size_t i = args.size(); i < func->params.size(); i++) {
        if (func->defaults[i]) {
            auto saved_env = current_env_;
            current_env_ = func_env;
            func->defaults[i]->accept(*this);
            auto default_val = result_;
            current_env_ = saved_env;
            func_env->define(func->params[i], default_val);
        }
    }

    // FIX-E1: Validate parameter types (same as CallExpr path at line ~6494)
    // This ensures pipeline, stdlib callbacks, and method dispatch all enforce types
    for (size_t i = 0; i < args.size() && i < func->param_types.size(); i++) {
        const auto& param_type = func->param_types[i];
        // Skip Any types (no constraint)
        if (param_type.kind == ast::TypeKind::Any) continue;

        // Null safety check
        if (!param_type.is_nullable && isNull(args[i])) {
            throw std::runtime_error(
                "Null safety error: Cannot pass null to non-nullable parameter '" +
                func->params[i] + "' of function '" + func->name + "'" +
                "\n  Expected: " + formatTypeName(param_type) +
                "\n  Got: null" +
                "\n  Help: Change parameter to nullable: " +
                formatTypeName(param_type) + "?"
            );
        }

        // Union type check
        if (param_type.kind == ast::TypeKind::Union) {
            if (!valueMatchesUnion(args[i], param_type.union_types)) {
                throw std::runtime_error(
                    "Type error: Parameter '" + func->params[i] +
                    "' of function '" + func->name +
                    "' expects " + formatTypeName(param_type) +
                    ", but got " + getValueTypeName(args[i])
                );
            }
        }
        // Non-union type check
        else if (!valueMatchesType(args[i], param_type)) {
            throw std::runtime_error(
                "Type error: Parameter '" + func->params[i] +
                "' of function '" + func->name +
                "' expects " + formatTypeName(param_type) +
                ", but got " + getValueTypeName(args[i])
            );
        }
    }

    // Save current environment and execute function body
    auto saved_env = current_env_;
    auto saved_returning = returning_;
    auto saved_file = current_file_;  // Phase 3.1: Save current file for cross-module calls
    env_stack_.push_back(current_env_);  // BUG-10 fix: Make caller env visible to GC
    current_env_ = func_env;
    returning_ = false;
    current_file_ = func->source_file;  // Phase 3.1: Set file to function's source file

    // Issue #3: Push file context for function's source file
    if (!func->source_file.empty()) {
        pushFileContext(func->source_file);
    }

    pushStackFrame(func->name, func->source_line);  // Phase 3.1: Use actual line number

    try {
        executeStmt(*func->body);
    } catch (...) {
        popStackFrame();
        // Issue #3: Pop file context on error
        if (!func->source_file.empty()) {
            popFileContext();
        }
        if (!env_stack_.empty()) env_stack_.pop_back();  // BUG-10 fix
        current_env_ = saved_env;
        returning_ = saved_returning;
        current_file_ = saved_file;  // Phase 3.1: Restore file
        throw;
    }

    popStackFrame();

    // Issue #3: Pop file context on success
    if (!func->source_file.empty()) {
        popFileContext();
    }

    // Restore environment
    if (!env_stack_.empty()) env_stack_.pop_back();  // BUG-10 fix
    current_env_ = saved_env;
    current_file_ = saved_file;  // Phase 3.1: Restore file
    auto return_value = result_;
    returning_ = saved_returning;

    // Phase 3 Governance: Check function contracts
    if (governance_ && governance_->isActive() && !func->name.empty()) {
        std::string result_str = return_value ? return_value->toString() : "null";
        std::string result_type = return_value ? getTypeName(return_value) : "null";
        std::string contract_err = governance_->checkFunctionContract(
            func->name, result_str, result_type, func->source_line);
        if (!contract_err.empty()) {
            governance_->logContractCheck(func->name, "FAIL", contract_err,
                                          current_file_, func->source_line);
            throw std::runtime_error(contract_err);
        } else {
            governance_->logContractCheck(func->name, "PASS", "return_type=" + result_type,
                                          current_file_, func->source_line);
        }
    }

    return return_value;
}



void Interpreter::visit(ast::CallExpr& node) {
    // Evaluate arguments first
    std::vector<std::shared_ptr<Value>> args;
    for (auto& arg : node.getArgs()) {
        args.push_back(eval(*arg));
    }

    // Check if this is a member expression call (for method chaining)
    auto* member_expr = dynamic_cast<ast::MemberExpr*>(node.getCallee());
    if (member_expr) {
        // Phase 6: Check for StructName.method() pattern (qualified function name)
        auto* callee_id = dynamic_cast<ast::IdentifierExpr*>(member_expr->getObject());
        if (callee_id) {
            std::string qualified_fn = callee_id->getName() + "." + member_expr->getMember();
            if (current_env_->has(qualified_fn)) {
                auto fn_val = current_env_->get(qualified_fn);
                if (fn_val && std::holds_alternative<std::shared_ptr<FunctionValue>>(fn_val->data)) {
                    callFunction(fn_val, args);
                    return;
                }
            }
        }

        // First check if the object is a dict/array/string with built-in methods
        // We need to evaluate the object BEFORE evaluating the full member access,
        // because built-in methods like .get(), .has() are not stored as dict keys
        {
            auto obj_val = eval(*member_expr->getObject());
            std::string method_name = member_expr->getMember();

            // Optional chaining: if object is null and ?. was used, return null
            if (member_expr->isOptional()) {
                if (!obj_val || std::holds_alternative<std::monostate>(obj_val->data)) {
                    result_ = std::make_shared<Value>();  // null
                    return;
                }
            }

            // Phase 12: Check if this is a persistent runtime .exec() call
            if (auto* str_val = std::get_if<std::string>(&obj_val->data)) {
                if (str_val->find("__NAAB_RUNTIME__:") == 0 && method_name == "exec") {
                    std::string runtime_name = str_val->substr(17);  // len("__NAAB_RUNTIME__:")
                    auto it = named_runtimes_.find(runtime_name);
                    if (it == named_runtimes_.end()) {
                        throw std::runtime_error("Runtime error: Runtime '" + runtime_name + "' not found");
                    }

                    // Extract the code from the argument (expects InlineCodeExpr or string)
                    if (args.empty()) {
                        throw std::runtime_error(
                            "Runtime error: " + runtime_name + ".exec() requires a polyglot block argument.\n\n"
                            "  Example: " + runtime_name + ".exec(<<" + it->second.language + "\n"
                            "    your code here\n"
                            "  >>)\n");
                    }

                    // The argument should be the result of an InlineCodeExpr evaluation
                    // OR a string value. For inline code blocks, the block was already executed
                    // by the InlineCodeExpr visitor. We need a different approach.
                    // For now: accept a string argument and execute it on the persistent runtime
                    std::string code;
                    if (auto* code_str = std::get_if<std::string>(&args[0]->data)) {
                        code = *code_str;
                    } else {
                        // If the argument is the result of an InlineCodeExpr, use result directly
                        result_ = args[0];
                        return;
                    }

                    auto& rt = it->second;

                    // BUG-AA: Governance checks for persistent runtime execution
                    if (governance_ && governance_->isActive()) {
                        std::string gov_err = governance_->checkPolyglotBlock(
                            rt.language, code, current_file_, node.getLocation().line, 0);
                        if (!gov_err.empty()) throw std::runtime_error(gov_err);

                        std::string count_err = governance_->incrementAndCheckPolyglotBlockCount();
                        if (!count_err.empty()) throw std::runtime_error(count_err);

                        governance_->logPolyglotExecution(rt.language, {}, 0,
                            current_file_, node.getLocation().line);
                    }

                    // For subprocess-based languages, accumulate code
                    bool is_embedded = (rt.language == "python" || rt.language == "javascript" ||
                                       rt.language == "js");
                    if (!is_embedded) {
                        rt.code_buffer += code + "\n";
                        code = rt.code_buffer;
                    }

                    // Detect if code is a statement (no return value expected)
                    // vs an expression (return value expected)
                    std::string trimmed_code = code;
                    size_t fc = trimmed_code.find_first_not_of(" \t\n\r");
                    if (fc != std::string::npos) trimmed_code = trimmed_code.substr(fc);

                    bool is_statement = (
                        trimmed_code.find("var ") == 0 ||
                        trimmed_code.find("let ") == 0 ||
                        trimmed_code.find("const ") == 0 ||
                        trimmed_code.find("function ") == 0 ||
                        trimmed_code.find("import ") == 0 ||
                        trimmed_code.find("class ") == 0 ||
                        trimmed_code.find("def ") == 0 ||
                        trimmed_code.find("from ") == 0 ||
                        trimmed_code.find("for ") == 0 ||
                        trimmed_code.find("while ") == 0 ||
                        trimmed_code.find("if ") == 0);

                    // Execute on the persistent executor
                    try {
                        bool is_js = (rt.language == "javascript" || rt.language == "js");
                        if (is_js) {
                            // For JS: Use BLOCK_LIBRARY mode for global scope persistence
                            auto* js_adapter = dynamic_cast<runtime::JsExecutorAdapter*>(rt.executor.get());
                            if (js_adapter) {
                                if (is_statement) {
                                    js_adapter->execute(code, runtime::JsExecutionMode::BLOCK_LIBRARY);
                                    result_ = std::make_shared<Value>();
                                } else {
                                    // For expressions: evaluate directly in global scope
                                    // executeWithReturn wraps in parens for single expr, which
                                    // accesses globals since QuickJS context is shared
                                    result_ = js_adapter->executeWithReturn(code);
                                }
                            } else {
                                result_ = rt.executor->executeWithReturn(code);
                            }
                        } else if (is_statement) {
                            // Statement mode: no return value
                            rt.executor->execute(code);
                            result_ = std::make_shared<Value>();
                        } else {
                            // Expression mode: capture return value
                            result_ = rt.executor->executeWithReturn(code);
                        }
                    } catch (const std::exception& e) {
                        std::string err = e.what();

                        // Detect scope isolation errors and provide helpful guidance
                        bool is_scope_error = (
                            err.find("NameError") != std::string::npos ||
                            err.find("ReferenceError") != std::string::npos ||
                            err.find("ModuleNotFoundError") != std::string::npos ||
                            err.find("ImportError") != std::string::npos ||
                            err.find("Cannot find module") != std::string::npos);

                        if (is_scope_error) {
                            // Extract the undefined name from quotes
                            std::string missing;
                            size_t q1 = err.find('\'');
                            if (q1 != std::string::npos) {
                                size_t q2 = err.find('\'', q1 + 1);
                                if (q2 != std::string::npos) {
                                    missing = err.substr(q1 + 1, q2 - q1 - 1);
                                }
                            }

                            std::ostringstream oss;
                            oss << "Persistent runtime '" << runtime_name << "' scope error: " << err << "\n\n"
                                << "  Help: Each .exec() call shares state with previous calls.\n"
                                << "  Import libraries in an earlier .exec() call:\n\n"
                                << "  Example:\n"
                                << "    runtime " << runtime_name << " = " << rt.language << ".start()\n";
                            if (!missing.empty()) {
                                oss << "    " << runtime_name << ".exec(<<" << rt.language << " import " << missing << " >>)\n";
                            } else {
                                oss << "    " << runtime_name << ".exec(<<" << rt.language << " import your_module >>)\n";
                            }
                            oss << "    let data = " << runtime_name << ".exec(<<" << rt.language << " ... >>)\n";
                            throw std::runtime_error(oss.str());
                        }

                        throw std::runtime_error(
                            "Runtime error in " + runtime_name + ".exec(): " + err);
                    }
                    return;
                }
            }

            // Built-in DICT methods
            if (auto* dict_ptr = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&obj_val->data)) {
                auto& dict = *dict_ptr;

                if (method_name == "get" || method_name == "getString" || method_name == "getInt" ||
                    method_name == "getFloat" || method_name == "getBool" || method_name == "getMap" ||
                    method_name == "getList") {
                    if (args.empty()) throw std::runtime_error("dict." + method_name + "() requires at least 1 argument (key)");
                    auto key = args[0]->toString();
                    auto it = dict.find(key);
                    if (it != dict.end()) {
                        result_ = it->second;
                    } else if (args.size() >= 2) {
                        result_ = args[1];
                    } else {
                        result_ = std::make_shared<Value>();
                    }
                    return;
                }
                if (method_name == "has" || method_name == "contains" || method_name == "containsKey") {
                    if (args.empty()) throw std::runtime_error("dict.has() requires 1 argument (key)");
                    result_ = std::make_shared<Value>(dict.find(args[0]->toString()) != dict.end());
                    return;
                }
                if (method_name == "size" || method_name == "length") {
                    result_ = std::make_shared<Value>(static_cast<int>(dict.size()));
                    return;
                }
                if (method_name == "isEmpty") {
                    result_ = std::make_shared<Value>(dict.empty());
                    return;
                }
                if (method_name == "put" || method_name == "set") {
                    if (args.size() < 2) throw std::runtime_error("dict.put() requires 2 arguments (key, value)");
                    dict[args[0]->toString()] = args[1];
                    auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_expr->getObject());
                    if (obj_id && current_env_->has(obj_id->getName())) {
                        current_env_->set(obj_id->getName(), obj_val);
                    }
                    // FIX-5: Taint propagation for dict.put/set (REFACTOR-1)
                    if (governance_ && governance_->isActive() && obj_id && node.getArgs().size() >= 2
                        && checkRhsTainted(node.getArgs()[1].get())) {
                        governance_->markTainted(obj_id->getName());
                    }
                    result_ = std::make_shared<Value>();
                    return;
                }
                if (method_name == "remove" || method_name == "delete") {
                    if (args.empty()) throw std::runtime_error("dict.remove() requires 1 argument (key)");
                    dict.erase(args[0]->toString());
                    auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_expr->getObject());
                    if (obj_id && current_env_->has(obj_id->getName())) {
                        current_env_->set(obj_id->getName(), obj_val);
                    }
                    result_ = std::make_shared<Value>();
                    return;
                }
                if (method_name == "keys") {
                    std::vector<std::shared_ptr<Value>> keys;
                    for (const auto& pair : dict) keys.push_back(std::make_shared<Value>(pair.first));
                    result_ = std::make_shared<Value>(keys);
                    return;
                }
                if (method_name == "values") {
                    std::vector<std::shared_ptr<Value>> vals;
                    for (const auto& pair : dict) vals.push_back(pair.second);
                    result_ = std::make_shared<Value>(vals);
                    return;
                }
                if (method_name == "clone" || method_name == "copy") {
                    result_ = std::make_shared<Value>(dict);
                    return;
                }
                if (method_name == "entries") {
                    std::vector<std::shared_ptr<Value>> entries;
                    for (const auto& pair : dict) {
                        std::vector<std::shared_ptr<Value>> entry;
                        entry.push_back(std::make_shared<Value>(pair.first));
                        entry.push_back(pair.second);
                        entries.push_back(std::make_shared<Value>(entry));
                    }
                    result_ = std::make_shared<Value>(entries);
                    return;
                }
                if (method_name == "merge") {
                    if (args.empty()) throw std::runtime_error("dict.merge() requires 1 argument (another dict)");
                    auto other = args[0];
                    if (auto* other_dict = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&other->data)) {
                        for (const auto& pair : *other_dict) {
                            dict[pair.first] = pair.second;
                        }
                        auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_expr->getObject());
                        if (obj_id && current_env_->has(obj_id->getName())) {
                            current_env_->set(obj_id->getName(), obj_val);
                        }
                    }
                    result_ = obj_val;
                    return;
                }
                // Not a built-in - fall through to check if it's a function stored in dict
            }

            // Built-in ARRAY methods
            if (auto* arr_ptr = std::get_if<std::vector<std::shared_ptr<Value>>>(&obj_val->data)) {
                auto& arr = *arr_ptr;

                if (method_name == "size" || method_name == "length") {
                    result_ = std::make_shared<Value>(static_cast<int>(arr.size()));
                    return;
                }
                if (method_name == "isEmpty") {
                    result_ = std::make_shared<Value>(arr.empty());
                    return;
                }
                if (method_name == "add" || method_name == "push" || method_name == "append") {
                    if (args.empty()) throw std::runtime_error("array.add() requires 1 argument");
                    arr.push_back(args[0]);
                    auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_expr->getObject());
                    if (obj_id && current_env_->has(obj_id->getName())) {
                        current_env_->set(obj_id->getName(), obj_val);
                    }
                    // FIX-5: Taint propagation for arr.push/add/append (REFACTOR-1)
                    if (governance_ && governance_->isActive() && obj_id && !node.getArgs().empty()
                        && checkRhsTainted(node.getArgs()[0].get())) {
                        governance_->markTainted(obj_id->getName());
                    }
                    result_ = obj_val;
                    return;
                }
                if (method_name == "get") {
                    if (args.empty()) throw std::runtime_error("array.get() requires 1 argument (index)");
                    int idx = std::get<int>(args[0]->data);
                    if (idx < 0 || idx >= static_cast<int>(arr.size())) {
                        throw std::runtime_error(fmt::format("Array index out of bounds: {} (size: {})", idx, arr.size()));
                    }
                    result_ = arr[static_cast<size_t>(idx)];
                    return;
                }
                if (method_name == "contains" || method_name == "includes") {
                    if (args.empty()) throw std::runtime_error("array.contains() requires 1 argument");
                    bool found = false;
                    for (const auto& item : arr) {
                        if (item->toString() == args[0]->toString()) { found = true; break; }
                    }
                    result_ = std::make_shared<Value>(found);
                    return;
                }
                if (method_name == "take") {
                    if (args.empty()) throw std::runtime_error("array.take() requires 1 argument (count)");
                    int count = std::get<int>(args[0]->data);
                    std::vector<std::shared_ptr<Value>> taken;
                    for (int i = 0; i < count && i < static_cast<int>(arr.size()); i++) {
                        taken.push_back(arr[static_cast<size_t>(i)]);
                    }
                    result_ = std::make_shared<Value>(taken);
                    return;
                }
                if (method_name == "clone" || method_name == "copy") {
                    result_ = std::make_shared<Value>(arr);
                    return;
                }
                if (method_name == "remove" || method_name == "removeAt") {
                    if (args.empty()) throw std::runtime_error("array.remove() requires 1 argument (index)");
                    int idx = std::get<int>(args[0]->data);
                    if (idx >= 0 && idx < static_cast<int>(arr.size())) {
                        arr.erase(arr.begin() + idx);
                    }
                    auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_expr->getObject());
                    if (obj_id && current_env_->has(obj_id->getName())) {
                        current_env_->set(obj_id->getName(), obj_val);
                    }
                    result_ = obj_val;
                    return;
                }
                // Type-cast no-ops: .asList(), .toList(), .asArray(), .toArray()
                if (method_name == "asList" || method_name == "toList" ||
                    method_name == "asArray" || method_name == "toArray") {
                    result_ = obj_val; // Already an array, return as-is
                    return;
                }
                // join(separator) - join array elements into a string
                if (method_name == "join") {
                    std::string sep = args.empty() ? "," : args[0]->toString();
                    std::string joined;
                    for (size_t i = 0; i < arr.size(); i++) {
                        if (i > 0) joined += sep;
                        joined += arr[i]->toString();
                    }
                    result_ = std::make_shared<Value>(joined);
                    return;
                }
                // reverse() - return reversed copy
                if (method_name == "reverse" || method_name == "reversed") {
                    std::vector<std::shared_ptr<Value>> rev(arr.rbegin(), arr.rend());
                    result_ = std::make_shared<Value>(rev);
                    return;
                }
                // indexOf(item) - find index of item, -1 if not found
                if (method_name == "indexOf" || method_name == "findIndex" || method_name == "index_of") {
                    if (args.empty()) throw std::runtime_error("array.indexOf() requires 1 argument");
                    for (int i = 0; i < static_cast<int>(arr.size()); i++) {
                        if (arr[i]->toString() == args[0]->toString()) {
                            result_ = std::make_shared<Value>(i);
                            return;
                        }
                    }
                    result_ = std::make_shared<Value>(-1);
                    return;
                }
                // DOT-2: first, last, sort, shift, unshift, find, slice, for_each
                if (method_name == "first") {
                    if (arr.empty()) throw std::runtime_error("array.first() called on empty array");
                    result_ = arr[0];
                    return;
                }
                if (method_name == "last") {
                    if (arr.empty()) throw std::runtime_error("array.last() called on empty array");
                    result_ = arr[arr.size() - 1];
                    return;
                }
                if (method_name == "sort") {
                    std::vector<std::shared_ptr<Value>> sorted = arr;
                    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
                        return a->toFloat() < b->toFloat();
                    });
                    result_ = std::make_shared<Value>(sorted);
                    return;
                }
                if (method_name == "shift") {
                    if (arr.empty()) throw std::runtime_error("array.shift() called on empty array");
                    auto first = arr[0];
                    arr.erase(arr.begin());
                    auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_expr->getObject());
                    if (obj_id && current_env_->has(obj_id->getName())) {
                        current_env_->set(obj_id->getName(), obj_val);
                    }
                    result_ = first;
                    return;
                }
                if (method_name == "unshift") {
                    if (args.empty()) throw std::runtime_error("array.unshift() requires 1 argument");
                    arr.insert(arr.begin(), args[0]);
                    auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_expr->getObject());
                    if (obj_id && current_env_->has(obj_id->getName())) {
                        current_env_->set(obj_id->getName(), obj_val);
                    }
                    result_ = obj_val;
                    return;
                }
                if (method_name == "find") {
                    if (args.empty()) throw std::runtime_error("array.find() requires 1 argument (predicate function)");
                    for (const auto& item : arr) {
                        auto res = callFunction(args[0], {item});
                        if (res && res->toBool()) {
                            result_ = item;
                            return;
                        }
                    }
                    result_ = std::make_shared<Value>(); // null if not found
                    return;
                }
                if (method_name == "for_each" || method_name == "forEach") {
                    if (args.empty()) throw std::runtime_error("array.forEach() requires 1 argument (function)");
                    for (const auto& item : arr) {
                        callFunction(args[0], {item});
                    }
                    result_ = std::make_shared<Value>(); // void
                    return;
                }
                if (method_name == "slice") {
                    if (args.empty()) throw std::runtime_error("array.slice() requires at least 1 argument (start)");
                    int start = args[0]->toInt();
                    int end = static_cast<int>(arr.size());
                    if (args.size() >= 2) end = args[1]->toInt();
                    if (start < 0) start = 0;
                    if (end > static_cast<int>(arr.size())) end = static_cast<int>(arr.size());
                    std::vector<std::shared_ptr<Value>> sliced(arr.begin() + start, arr.begin() + end);
                    result_ = std::make_shared<Value>(sliced);
                    return;
                }
                // HELPER-5: .len() -> .length()
                if (method_name == "len") {
                    throw std::runtime_error(
                        "Unknown method: .len()\n  Did you mean: .length() or the builtin len(x)?"
                    );
                }
                // Not a built-in array method - fall through
            }

            // Built-in STRING methods (skip module markers)
            if (auto* str_ptr = std::get_if<std::string>(&obj_val->data)) {
                auto& str = *str_ptr;

                // Skip stdlib/module markers - these are handled by the module system
                if (str.substr(0, 18) == "__stdlib_module__:" ||
                    str.substr(0, 10) == "__module__:") {
                    // Fall through to normal member access (module.function call)
                    goto normal_member_access;
                }

                if (method_name == "size" || method_name == "length") {
                    result_ = std::make_shared<Value>(static_cast<int>(str.size()));
                    return;
                }
                if (method_name == "isEmpty") {
                    result_ = std::make_shared<Value>(str.empty());
                    return;
                }
                if (method_name == "contains" || method_name == "includes") {
                    if (args.empty()) throw std::runtime_error("string.contains() requires 1 argument");
                    result_ = std::make_shared<Value>(str.find(args[0]->toString()) != std::string::npos);
                    return;
                }
                if (method_name == "indexOf") {
                    if (args.empty()) throw std::runtime_error("string.indexOf() requires 1 argument");
                    auto pos = str.find(args[0]->toString());
                    result_ = std::make_shared<Value>(pos != std::string::npos ? static_cast<int>(pos) : -1);
                    return;
                }
                if (method_name == "lastIndexOf") {
                    if (args.empty()) throw std::runtime_error("string.lastIndexOf() requires 1 argument");
                    auto pos = str.rfind(args[0]->toString());
                    result_ = std::make_shared<Value>(pos != std::string::npos ? static_cast<int>(pos) : -1);
                    return;
                }
                if (method_name == "substring" || method_name == "substr" || method_name == "slice") {
                    if (args.empty()) throw std::runtime_error("string.substring() requires at least 1 argument");
                    int start = std::get<int>(args[0]->data);
                    if (start < 0) start = 0;
                    if (start >= static_cast<int>(str.size())) {
                        result_ = std::make_shared<Value>(std::string(""));
                        return;
                    }
                    if (args.size() >= 2) {
                        int end = std::get<int>(args[1]->data);
                        if (end > static_cast<int>(str.size())) end = static_cast<int>(str.size());
                        result_ = std::make_shared<Value>(str.substr(static_cast<size_t>(start), static_cast<size_t>(end - start)));
                    } else {
                        result_ = std::make_shared<Value>(str.substr(static_cast<size_t>(start)));
                    }
                    return;
                }
                if (method_name == "replace") {
                    if (args.size() < 2) throw std::runtime_error("string.replace() requires 2 arguments (old, new)");
                    std::string old_s = args[0]->toString(), new_s = args[1]->toString();
                    std::string result = str;
                    size_t pos = 0;
                    while ((pos = result.find(old_s, pos)) != std::string::npos) {
                        result.replace(pos, old_s.length(), new_s);
                        pos += new_s.length();
                    }
                    result_ = std::make_shared<Value>(result);
                    return;
                }
                if (method_name == "toUpperCase" || method_name == "upper") {
                    std::string result = str;
                    std::transform(result.begin(), result.end(), result.begin(), ::toupper);
                    result_ = std::make_shared<Value>(result);
                    return;
                }
                if (method_name == "toLowerCase" || method_name == "lower") {
                    std::string result = str;
                    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
                    result_ = std::make_shared<Value>(result);
                    return;
                }
                if (method_name == "trim") {
                    std::string result = str;
                    result.erase(0, result.find_first_not_of(" \t\n\r"));
                    if (!result.empty()) result.erase(result.find_last_not_of(" \t\n\r") + 1);
                    result_ = std::make_shared<Value>(result);
                    return;
                }
                if (method_name == "split") {
                    if (args.empty()) throw std::runtime_error("string.split() requires 1 argument (separator)");
                    std::string sep = args[0]->toString();
                    std::vector<std::shared_ptr<Value>> parts;
                    if (sep.empty()) {
                        for (char c : str) parts.push_back(std::make_shared<Value>(std::string(1, c)));
                    } else {
                        size_t s = 0, p;
                        while ((p = str.find(sep, s)) != std::string::npos) {
                            parts.push_back(std::make_shared<Value>(str.substr(s, p - s)));
                            s = p + sep.size();
                        }
                        parts.push_back(std::make_shared<Value>(str.substr(s)));
                    }
                    result_ = std::make_shared<Value>(parts);
                    return;
                }
                if (method_name == "startsWith" || method_name == "starts_with") {
                    if (args.empty()) throw std::runtime_error("string.startsWith() requires 1 argument");
                    result_ = std::make_shared<Value>(str.find(args[0]->toString()) == 0);
                    return;
                }
                if (method_name == "endsWith" || method_name == "ends_with") {
                    if (args.empty()) throw std::runtime_error("string.endsWith() requires 1 argument");
                    std::string suffix = args[0]->toString();
                    result_ = std::make_shared<Value>(
                        str.size() >= suffix.size() &&
                        str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0
                    );
                    return;
                }
                if (method_name == "index_of") {
                    if (args.empty()) throw std::runtime_error("string.index_of() requires 1 argument");
                    auto pos = str.find(args[0]->toString());
                    result_ = std::make_shared<Value>(pos != std::string::npos ? static_cast<int>(pos) : -1);
                    return;
                }
                // DOT-1: char_at, reverse, repeat
                if (method_name == "char_at") {
                    if (args.empty()) throw std::runtime_error("string.char_at() requires 1 argument (index)");
                    int idx = args[0]->toInt();
                    if (idx < 0 || idx >= static_cast<int>(str.size())) {
                        throw std::runtime_error("String index out of bounds: " + std::to_string(idx) + " (length: " + std::to_string(str.size()) + ")");
                    }
                    result_ = std::make_shared<Value>(std::string(1, str[static_cast<size_t>(idx)]));
                    return;
                }
                if (method_name == "reverse") {
                    std::string rev(str.rbegin(), str.rend());
                    result_ = std::make_shared<Value>(rev);
                    return;
                }
                if (method_name == "repeat") {
                    if (args.empty()) throw std::runtime_error("string.repeat() requires 1 argument (count)");
                    int count = args[0]->toInt();
                    std::string repeated;
                    for (int i = 0; i < count; i++) repeated += str;
                    result_ = std::make_shared<Value>(repeated);
                    return;
                }
                if (method_name == "pad_right") {
                    if (args.empty() || args.size() > 2) throw std::runtime_error("pad_right() takes 1-2 arguments (width[, fill_char])");
                    int width = args[0]->toInt();
                    char fill = ' ';
                    if (args.size() == 2) {
                        auto fs = std::get<std::string>(args[1]->data);
                        if (fs.length() != 1) throw std::runtime_error("pad_right() fill_char must be exactly 1 character");
                        fill = fs[0];
                    }
                    std::string s = str;
                    if (static_cast<int>(s.length()) < width) s.append(width - s.length(), fill);
                    result_ = std::make_shared<Value>(s);
                    return;
                }
                if (method_name == "pad_left") {
                    if (args.empty() || args.size() > 2) throw std::runtime_error("pad_left() takes 1-2 arguments (width[, fill_char])");
                    int width = args[0]->toInt();
                    char fill = ' ';
                    if (args.size() == 2) {
                        auto fs = std::get<std::string>(args[1]->data);
                        if (fs.length() != 1) throw std::runtime_error("pad_left() fill_char must be exactly 1 character");
                        fill = fs[0];
                    }
                    if (static_cast<int>(str.length()) < width) {
                        result_ = std::make_shared<Value>(std::string(width - str.length(), fill) + str);
                    } else {
                        result_ = std::make_shared<Value>(str);
                    }
                    return;
                }
                // HELPER-5: .len() -> .length()
                if (method_name == "len") {
                    throw std::runtime_error(
                        "Unknown method: .len()\n  Did you mean: .length() or the builtin len(x)?"
                    );
                }
                // Not a built-in string method - fall through
            }
        }

        // Evaluate the member expression (returns PythonObjectValue for methods)
        normal_member_access:
        auto callable = eval(*member_expr);

        // If it's a Python object, call it
        if (auto* py_obj_ptr = std::get_if<std::shared_ptr<PythonObjectValue>>(&callable->data)) {
            auto& py_callable = *py_obj_ptr;

#ifdef NAAB_HAS_PYTHON
            LOG_TRACE("[CALL] Invoking Python method with {} args\n", args.size());

            // Build argument tuple for Python
            PyObject* py_args = PyTuple_New(static_cast<Py_ssize_t>(args.size()));
            for (size_t i = 0; i < args.size(); i++) {
                PyObject* py_arg = nullptr;

                // Convert Value to Python object
                if (auto* intval = std::get_if<int>(&args[i]->data)) {
                    py_arg = PyLong_FromLong(*intval);
                } else if (auto* floatval = std::get_if<double>(&args[i]->data)) {
                    py_arg = PyFloat_FromDouble(*floatval);
                } else if (auto* strval = std::get_if<std::string>(&args[i]->data)) {
                    py_arg = PyUnicode_FromString(strval->c_str());
                } else if (auto* boolval = std::get_if<bool>(&args[i]->data)) {
                    py_arg = *boolval ? Py_True : Py_False;
                    Py_INCREF(py_arg);
                } else {
                    py_arg = Py_None;
                    Py_INCREF(py_arg);
                }

                PyTuple_SetItem(py_args, static_cast<Py_ssize_t>(i), py_arg);  // Steals reference
            }

            // Call the Python callable
            PyObject* py_result = PyObject_CallObject(py_callable->obj, py_args);
            Py_DECREF(py_args);

            if (py_result != nullptr) {
                // Convert result to NAAb Value
                if (PyLong_Check(py_result)) {
                    long val = PyLong_AsLong(py_result);
                    result_ = std::make_shared<Value>(static_cast<int>(val));
                    LOG_DEBUG("[SUCCESS] Method returned int: {}\n", val);
                    Py_DECREF(py_result);
                } else if (PyFloat_Check(py_result)) {
                    double val = PyFloat_AsDouble(py_result);
                    result_ = std::make_shared<Value>(val);
                    LOG_DEBUG("[SUCCESS] Method returned float: {}\n", val);
                    Py_DECREF(py_result);
                } else if (PyUnicode_Check(py_result)) {
                    const char* val = PyUnicode_AsUTF8(py_result);
                    result_ = std::make_shared<Value>(std::string(val));
                    LOG_DEBUG("[SUCCESS] Method returned string: {}\n", val);
                    Py_DECREF(py_result);
                } else if (PyBool_Check(py_result)) {
                    bool val = py_result == Py_True;
                    result_ = std::make_shared<Value>(val);
                    LOG_DEBUG("[SUCCESS] Method returned bool: {}\n", val);
                    Py_DECREF(py_result);
                } else if (py_result == Py_None) {
                    result_ = std::make_shared<Value>();
                    LOG_DEBUG("[SUCCESS] Method returned None\n");
                    Py_DECREF(py_result);
                } else {
                    // Complex object - wrap for further chaining
                    auto py_obj = std::make_shared<PythonObjectValue>(py_result);
                    result_ = std::make_shared<Value>(py_obj);
                    LOG_DEBUG("[SUCCESS] Method returned Python object: {}\n", py_obj->repr);
                    Py_DECREF(py_result);
                }
            } else {
                PyErr_Print();
                fmt::print("[ERROR] Python method call failed\n");
                result_ = std::make_shared<Value>();
            }
#else
            throw std::runtime_error("Python support required for method calls");
#endif
            return;
        }

        // Check if it's a BlockValue (for JavaScript/C++ block method calls)
        if (auto* block_ptr = std::get_if<std::shared_ptr<BlockValue>>(&callable->data)) {
            auto& block = *block_ptr;

            LOG_TRACE("[CALL] Invoking block method {}.{} with {} args\n",
                      block->metadata.block_id, block->member_path, args.size());

            // Get executor
            auto* executor = block->getExecutor();
            if (!executor) {
                throw std::runtime_error("No executor for block: " + block->metadata.block_id);
            }

            // Call the specific function in the block
            if (block->metadata.language == "javascript") {
                explain("Calling JavaScript block to evaluate: " + block->member_path);
                if (isVerboseMode()) {
                    fmt::print("[VERBOSE] Calling {}::{}\n", block->metadata.block_id, block->member_path);
                }
                profileStart("BLOCK-JS calls");
                result_ = executor->callFunction(block->member_path, args);
                flushExecutorOutput(executor);  // Phase 11.1: Flush captured output
                profileEnd("BLOCK-JS calls");
                if (isVerboseMode()) {
                    fmt::print("[VERBOSE] Block returned: {}\n", result_->toString());
                }
                LOG_DEBUG("[SUCCESS] JavaScript function returned\n");

                // Phase 4.4: Record block usage
                if (block_loader_) {
                    int tokens_saved = (block->metadata.token_count > 0)
                        ? block->metadata.token_count : 50;
                    block_loader_->recordBlockUsage(block->metadata.block_id, tokens_saved);

                    // Record block pair if there was a previous block
                    if (!last_executed_block_id_.empty()) {
                        block_loader_->recordBlockPair(last_executed_block_id_, block->metadata.block_id);
                    }
                    last_executed_block_id_ = block->metadata.block_id;
                }
                return;
            }

            if (block->metadata.language == "cpp") {
                explain("Calling C++ block to evaluate: " + block->member_path);
                if (isVerboseMode()) {
                    fmt::print("[VERBOSE] Calling {}::{}\n", block->metadata.block_id, block->member_path);
                }
                profileStart("BLOCK-CPP calls");
                result_ = executor->callFunction(block->member_path, args);
                flushExecutorOutput(executor);  // Phase 11.1: Flush captured output
                profileEnd("BLOCK-CPP calls");
                if (isVerboseMode()) {
                    fmt::print("[VERBOSE] Block returned: {}\n", result_->toString());
                }
                LOG_DEBUG("[SUCCESS] C++ function returned\n");

                // Phase 4.4: Record block usage
                if (block_loader_) {
                    int tokens_saved = (block->metadata.token_count > 0)
                        ? block->metadata.token_count : 50;
                    block_loader_->recordBlockUsage(block->metadata.block_id, tokens_saved);

                    // Record block pair if there was a previous block
                    if (!last_executed_block_id_.empty()) {
                        block_loader_->recordBlockPair(last_executed_block_id_, block->metadata.block_id);
                    }
                    last_executed_block_id_ = block->metadata.block_id;
                }
                return;
            }

            if (block->metadata.language == "python") {
                explain("Calling Python block to evaluate: " + block->member_path);
                if (isVerboseMode()) {
                    fmt::print("[VERBOSE] Calling {}::{}\n", block->metadata.block_id, block->member_path);
                }
                profileStart("BLOCK-PY calls");
                result_ = executor->callFunction(block->member_path, args);
                flushExecutorOutput(executor);  // Phase 11.1: Flush captured output
                profileEnd("BLOCK-PY calls");
                if (isVerboseMode()) {
                    fmt::print("[VERBOSE] Block returned: {}\n", result_->toString());
                }
                LOG_DEBUG("[SUCCESS] Python function returned\n");

                // Phase 4.4: Record block usage
                if (block_loader_) {
                    int tokens_saved = (block->metadata.token_count > 0)
                        ? block->metadata.token_count : 50;
                    block_loader_->recordBlockUsage(block->metadata.block_id, tokens_saved);

                    // Record block pair if there was a previous block
                    if (!last_executed_block_id_.empty()) {
                        block_loader_->recordBlockPair(last_executed_block_id_, block->metadata.block_id);
                    }
                    last_executed_block_id_ = block->metadata.block_id;
                }
                return;
            }

            throw std::runtime_error("Member function calls not yet supported for " +
                                    block->metadata.language + " blocks");
        }

        // Check if it's a stdlib function call marker
        if (auto* str_ptr = std::get_if<std::string>(&callable->data)) {
            std::string marker = *str_ptr;

            // Check if it's a stdlib call marker
            if (marker.substr(0, 16) == "__stdlib_call__:") {
                // Parse: __stdlib_call__:module:function
                size_t first_colon = marker.find(':', 16);
                if (first_colon != std::string::npos) {
                    std::string module_alias = marker.substr(16, first_colon - 16);
                    std::string func_name = marker.substr(first_colon + 1);

                    // Look up module
                    auto it = imported_modules_.find(module_alias);
                    if (it == imported_modules_.end()) {
                        throw std::runtime_error("Module not found: " + module_alias);
                    }

                    auto module = it->second;

                    LOG_TRACE("[STDLIB] Calling {}.{}() with {} args\n",
                              module_alias, func_name, args.size());

                    // Governance: Check capability restrictions on stdlib calls
                    if (governance_ && governance_->isActive()) {
                        // Network check for http module
                        if (module_alias == "http") {
                            std::string err = governance_->checkNetworkAllowed();
                            if (!err.empty()) throw std::runtime_error(err);

                            // BUG-F+R: Taint sink check on URL argument (SSRF prevention)
                            if (!node.getArgs().empty()) {
                                std::string terr = checkExpressionTaintedSink(
                                    node.getArgs()[0].get(), "http." + func_name,
                                    current_file_, node.getLocation().line);
                                if (!terr.empty()) throw std::runtime_error(terr);
                            }
                        }
                        // BUG-G+R: Taint sink check for env.set_var (prevent tainted data escaping to env)
                        if (module_alias == "env" && func_name == "set_var") {
                            if (node.getArgs().size() >= 2) {
                                std::string terr = checkExpressionTaintedSink(
                                    node.getArgs()[1].get(), "env.set_var",
                                    current_file_, node.getLocation().line);
                                if (!terr.empty()) throw std::runtime_error(terr);
                            }
                        }
                        // Filesystem check for file module
                        if (module_alias == "file") {
                            // file.read → read mode, everything else → write mode
                            std::string fs_mode = (func_name == "read" || func_name == "read_lines"
                                                   || func_name == "exists" || func_name == "size")
                                                  ? "read" : "write";
                            std::string err = governance_->checkFilesystemAllowed(fs_mode);
                            if (!err.empty()) throw std::runtime_error(err);

                            // BUG-R: Taint tracking — check if write args contain tainted data (expression-level)
                            if (func_name == "write" || func_name == "append") {
                                for (size_t ai = 0; ai < node.getArgs().size(); ++ai) {
                                    std::string terr = checkExpressionTaintedSink(
                                        node.getArgs()[ai].get(), "file." + func_name,
                                        current_file_, node.getLocation().line);
                                    if (!terr.empty()) throw std::runtime_error(terr);
                                }
                            }
                        }
                    }

                    // Call the stdlib function
                    result_ = module->call(func_name, args);
                    LOG_TRACE("[SUCCESS] Stdlib function returned\n");

                    // Auto-mutation: If this is a mutating function, update the original variable
                    if (module->isMutatingFunction(func_name) && !args.empty()) {
                        // Get the first argument expression (the variable being mutated)
                        auto& first_arg_expr = node.getArgs()[0];

                        // Only auto-mutate simple identifiers (not complex expressions)
                        if (auto* id_expr = dynamic_cast<ast::IdentifierExpr*>(first_arg_expr.get())) {
                            std::string var_name = id_expr->getName();

                            // Update the variable
                            if (current_env_->has(var_name)) {
                                // For pop/shift, the modified array is in args[0], not result
                                if (func_name == "pop" || func_name == "shift") {
                                    current_env_->set(var_name, args[0]);
                                } else {
                                    // For push/unshift/reverse/sort, use the result
                                    current_env_->set(var_name, result_);
                                }
                                LOG_TRACE("[MUTATION] Auto-updated {} after {}.{}()\n",
                                         var_name, module_alias, func_name);
                            }
                        }
                    }

                    return;
                }
            }
        }

        // Otherwise continue with normal handling below
    }

    // Handle member access calls (e.g., module.function(...))
    auto* member_call = dynamic_cast<ast::MemberExpr*>(node.getCallee());
    if (member_call) {
        std::string method_name = member_call->getMember();

        // Evaluate the object part to check for built-in methods on dicts/arrays/strings
        auto obj = eval(*member_call->getObject());

        // ===== Built-in DICT methods =====
        if (auto* dict_ptr = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&obj->data)) {
            auto& dict = *dict_ptr;

            if (method_name == "get" || method_name == "getString" || method_name == "getInt" ||
                method_name == "getFloat" || method_name == "getBool" || method_name == "getMap" ||
                method_name == "getList") {
                if (args.empty()) throw std::runtime_error("dict." + method_name + "() requires at least 1 argument (key)");
                auto key = args[0]->toString();
                auto it = dict.find(key);
                if (it != dict.end()) {
                    result_ = it->second;
                } else if (args.size() >= 2) {
                    result_ = args[1];  // default value
                } else {
                    result_ = std::make_shared<Value>();  // null
                }
                return;
            }
            if (method_name == "has" || method_name == "contains" || method_name == "containsKey") {
                if (args.empty()) throw std::runtime_error("dict.has() requires 1 argument (key)");
                auto key = args[0]->toString();
                result_ = std::make_shared<Value>(dict.find(key) != dict.end());
                return;
            }
            if (method_name == "size" || method_name == "length") {
                result_ = std::make_shared<Value>(static_cast<int>(dict.size()));
                return;
            }
            if (method_name == "isEmpty") {
                result_ = std::make_shared<Value>(dict.empty());
                return;
            }
            if (method_name == "put" || method_name == "set") {
                if (args.size() < 2) throw std::runtime_error("dict.put() requires 2 arguments (key, value)");
                auto key = args[0]->toString();
                dict[key] = args[1];
                // Update the original variable
                auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_call->getObject());
                if (obj_id && current_env_->has(obj_id->getName())) {
                    current_env_->set(obj_id->getName(), obj);
                }
                // FIX-5: Taint propagation for dict.put/set (REFACTOR-1)
                if (governance_ && governance_->isActive() && obj_id && node.getArgs().size() >= 2
                    && checkRhsTainted(node.getArgs()[1].get())) {
                    governance_->markTainted(obj_id->getName());
                }
                result_ = std::make_shared<Value>();
                return;
            }
            if (method_name == "remove" || method_name == "delete") {
                if (args.empty()) throw std::runtime_error("dict.remove() requires 1 argument (key)");
                auto key = args[0]->toString();
                dict.erase(key);
                auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_call->getObject());
                if (obj_id && current_env_->has(obj_id->getName())) {
                    current_env_->set(obj_id->getName(), obj);
                }
                result_ = std::make_shared<Value>();
                return;
            }
            if (method_name == "keys") {
                std::vector<std::shared_ptr<Value>> keys;
                for (const auto& pair : dict) {
                    keys.push_back(std::make_shared<Value>(pair.first));
                }
                result_ = std::make_shared<Value>(keys);
                return;
            }
            if (method_name == "values") {
                std::vector<std::shared_ptr<Value>> vals;
                for (const auto& pair : dict) {
                    vals.push_back(pair.second);
                }
                result_ = std::make_shared<Value>(vals);
                return;
            }
            if (method_name == "clone" || method_name == "copy") {
                auto new_dict = dict;  // shallow copy
                result_ = std::make_shared<Value>(new_dict);
                return;
            }
            if (method_name == "entries") {
                std::vector<std::shared_ptr<Value>> entries;
                for (const auto& pair : dict) {
                    std::vector<std::shared_ptr<Value>> entry;
                    entry.push_back(std::make_shared<Value>(pair.first));
                    entry.push_back(pair.second);
                    entries.push_back(std::make_shared<Value>(entry));
                }
                result_ = std::make_shared<Value>(entries);
                return;
            }
            if (method_name == "merge") {
                if (args.empty()) throw std::runtime_error("dict.merge() requires 1 argument (another dict)");
                auto other = args[0];
                if (auto* other_dict = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&other->data)) {
                    for (const auto& pair : *other_dict) {
                        dict[pair.first] = pair.second;
                    }
                    auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_call->getObject());
                    if (obj_id && current_env_->has(obj_id->getName())) {
                        current_env_->set(obj_id->getName(), obj);
                    }
                } else {
                    throw std::runtime_error("dict.merge() argument must be a dict");
                }
                result_ = std::make_shared<Value>();
                return;
            }
            // Helper: dict.update() does not exist
            if (method_name == "update") {
                throw std::runtime_error(
                    "dict.update() does not exist in NAAb\n\n"
                    "  Use dict.merge(other) to merge another dict, or set keys individually:\n"
                    "    my_dict.merge(other_dict)          // merge all keys from other\n"
                    "    my_dict.put(\"key\", value)           // set a single key\n"
                    "    my_dict[\"key\"] = value              // bracket assignment\n"
                );
            }

            // Not a built-in dict method - check if it's a function stored in the dict
            auto it = dict.find(method_name);
            if (it != dict.end()) {
                auto func_value = it->second;
                if (auto* func_ptr = std::get_if<std::shared_ptr<FunctionValue>>(&func_value->data)) {
                    result_ = callFunction(func_value, args);
                    return;
                }
                // Not a function - fall through to error
            }

            // Dict method not found error
            std::ostringstream oss;
            oss << "Name error: Unknown dict method '" << method_name << "'\n\n";
            oss << "  Available dict methods:\n";
            oss << "    .get(key), .get(key, default)   - get value by key\n";
            oss << "    .has(key)                       - check if key exists\n";
            oss << "    .size()                         - number of entries\n";
            oss << "    .isEmpty()                      - check if empty\n";
            oss << "    .put(key, value)                - add/update entry\n";
            oss << "    .remove(key)                    - remove entry\n";
            oss << "    .keys(), .values()              - get keys/values as array\n";
            oss << "    .entries()                      - array of [key, value] pairs\n";
            oss << "    .merge(other_dict)              - merge another dict\n";
            oss << "    .clone()                        - shallow copy\n";
            if (!dict.empty()) {
                oss << "\n  Dict keys: ";
                size_t count = 0;
                for (const auto& pair : dict) {
                    if (count > 0) oss << ", ";
                    oss << pair.first;
                    if (++count >= 10) { oss << "..."; break; }
                }
                oss << "\n";
                oss << "  Access keys with: dict.keyName or dict.get(\"keyName\")\n";
            }
            throw std::runtime_error(oss.str());
        }

        // ===== Built-in ARRAY methods =====
        if (auto* arr_ptr = std::get_if<std::vector<std::shared_ptr<Value>>>(&obj->data)) {
            auto& arr = *arr_ptr;

            if (method_name == "size" || method_name == "length") {
                result_ = std::make_shared<Value>(static_cast<int>(arr.size()));
                return;
            }
            if (method_name == "isEmpty") {
                result_ = std::make_shared<Value>(arr.empty());
                return;
            }
            if (method_name == "add" || method_name == "push" || method_name == "append") {
                if (args.empty()) throw std::runtime_error("array.add() requires 1 argument");
                arr.push_back(args[0]);
                auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_call->getObject());
                if (obj_id && current_env_->has(obj_id->getName())) {
                    current_env_->set(obj_id->getName(), obj);
                }
                // FIX-5: Taint propagation for arr.push/add/append (REFACTOR-1)
                if (governance_ && governance_->isActive() && obj_id && !node.getArgs().empty()
                    && checkRhsTainted(node.getArgs()[0].get())) {
                    governance_->markTainted(obj_id->getName());
                }
                result_ = obj;
                return;
            }
            if (method_name == "get") {
                if (args.empty()) throw std::runtime_error("array.get() requires 1 argument (index)");
                int idx = std::get<int>(args[0]->data);
                if (idx < 0 || idx >= static_cast<int>(arr.size())) {
                    throw std::runtime_error(fmt::format("Array index out of bounds: {} (size: {})", idx, arr.size()));
                }
                result_ = arr[idx];
                return;
            }
            if (method_name == "contains" || method_name == "includes") {
                if (args.empty()) throw std::runtime_error("array.contains() requires 1 argument");
                bool found = false;
                for (const auto& item : arr) {
                    if (item->toString() == args[0]->toString()) { found = true; break; }
                }
                result_ = std::make_shared<Value>(found);
                return;
            }
            if (method_name == "take") {
                if (args.empty()) throw std::runtime_error("array.take() requires 1 argument (count)");
                int count = std::get<int>(args[0]->data);
                std::vector<std::shared_ptr<Value>> taken;
                for (int i = 0; i < count && i < static_cast<int>(arr.size()); i++) {
                    taken.push_back(arr[i]);
                }
                result_ = std::make_shared<Value>(taken);
                return;
            }
            if (method_name == "clone" || method_name == "copy") {
                auto new_arr = arr;
                result_ = std::make_shared<Value>(new_arr);
                return;
            }
            if (method_name == "remove" || method_name == "removeAt") {
                if (args.empty()) throw std::runtime_error("array.remove() requires 1 argument (index)");
                int idx = std::get<int>(args[0]->data);
                if (idx >= 0 && idx < static_cast<int>(arr.size())) {
                    arr.erase(arr.begin() + idx);
                }
                auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_call->getObject());
                if (obj_id && current_env_->has(obj_id->getName())) {
                    current_env_->set(obj_id->getName(), obj);
                }
                result_ = obj;
                return;
            }
            // Type-cast no-ops: .asList(), .toList(), .asArray(), .toArray()
            if (method_name == "asList" || method_name == "toList" ||
                method_name == "asArray" || method_name == "toArray") {
                result_ = obj; // Already an array, return as-is
                return;
            }
            // join(separator)
            if (method_name == "join") {
                std::string sep = args.empty() ? "," : args[0]->toString();
                std::string joined;
                for (size_t i = 0; i < arr.size(); i++) {
                    if (i > 0) joined += sep;
                    joined += arr[i]->toString();
                }
                result_ = std::make_shared<Value>(joined);
                return;
            }
            // reverse()
            if (method_name == "reverse" || method_name == "reversed") {
                std::vector<std::shared_ptr<Value>> rev(arr.rbegin(), arr.rend());
                result_ = std::make_shared<Value>(rev);
                return;
            }
            // indexOf(item)
            if (method_name == "indexOf" || method_name == "findIndex" || method_name == "index_of") {
                if (args.empty()) throw std::runtime_error("array.indexOf() requires 1 argument");
                for (int i = 0; i < static_cast<int>(arr.size()); i++) {
                    if (arr[i]->toString() == args[0]->toString()) {
                        result_ = std::make_shared<Value>(i);
                        return;
                    }
                }
                result_ = std::make_shared<Value>(-1);
                return;
            }
            // DOT-2: first, last, sort, shift, unshift, find, slice, for_each
            if (method_name == "first") {
                if (arr.empty()) throw std::runtime_error("array.first() called on empty array");
                result_ = arr[0];
                return;
            }
            if (method_name == "last") {
                if (arr.empty()) throw std::runtime_error("array.last() called on empty array");
                result_ = arr[arr.size() - 1];
                return;
            }
            if (method_name == "sort") {
                std::vector<std::shared_ptr<Value>> sorted = arr;
                std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
                    return a->toFloat() < b->toFloat();
                });
                result_ = std::make_shared<Value>(sorted);
                return;
            }
            if (method_name == "shift") {
                if (arr.empty()) throw std::runtime_error("array.shift() called on empty array");
                auto first_elem = arr[0];
                arr.erase(arr.begin());
                auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_call->getObject());
                if (obj_id && current_env_->has(obj_id->getName())) {
                    current_env_->set(obj_id->getName(), obj);
                }
                result_ = first_elem;
                return;
            }
            if (method_name == "unshift") {
                if (args.empty()) throw std::runtime_error("array.unshift() requires 1 argument");
                arr.insert(arr.begin(), args[0]);
                auto* obj_id = dynamic_cast<ast::IdentifierExpr*>(member_call->getObject());
                if (obj_id && current_env_->has(obj_id->getName())) {
                    current_env_->set(obj_id->getName(), obj);
                }
                result_ = obj;
                return;
            }
            if (method_name == "find") {
                if (args.empty()) throw std::runtime_error("array.find() requires 1 argument (predicate function)");
                for (const auto& item : arr) {
                    auto res = callFunction(args[0], {item});
                    if (res && res->toBool()) {
                        result_ = item;
                        return;
                    }
                }
                result_ = std::make_shared<Value>(); // null if not found
                return;
            }
            if (method_name == "for_each" || method_name == "forEach") {
                if (args.empty()) throw std::runtime_error("array.forEach() requires 1 argument (function)");
                for (const auto& item : arr) {
                    callFunction(args[0], {item});
                }
                result_ = std::make_shared<Value>(); // void
                return;
            }
            if (method_name == "slice") {
                if (args.empty()) throw std::runtime_error("array.slice() requires at least 1 argument (start)");
                int start = args[0]->toInt();
                int end = static_cast<int>(arr.size());
                if (args.size() >= 2) end = args[1]->toInt();
                if (start < 0) start = 0;
                if (end > static_cast<int>(arr.size())) end = static_cast<int>(arr.size());
                std::vector<std::shared_ptr<Value>> sliced(arr.begin() + start, arr.begin() + end);
                result_ = std::make_shared<Value>(sliced);
                return;
            }
            // HELPER-5: .len() -> .length()
            if (method_name == "len") {
                throw std::runtime_error(
                    "Unknown method: .len()\n  Did you mean: .length() or the builtin len(x)?"
                );
            }
            // Not a built-in array method - fall through to normal handling
        }

        // ===== Built-in STRING methods (skip module markers) =====
        if (auto* str_ptr = std::get_if<std::string>(&obj->data)) {
            auto& str = *str_ptr;

            if (str.substr(0, 18) == "__stdlib_module__:" ||
                str.substr(0, 10) == "__module__:") {
                // Fall through to normal member access
            } else

            if (method_name == "size" || method_name == "length") {
                result_ = std::make_shared<Value>(static_cast<int>(str.size()));
                return;
            }
            if (method_name == "isEmpty") {
                result_ = std::make_shared<Value>(str.empty());
                return;
            }
            if (method_name == "contains" || method_name == "includes") {
                if (args.empty()) throw std::runtime_error("string.contains() requires 1 argument");
                result_ = std::make_shared<Value>(str.find(args[0]->toString()) != std::string::npos);
                return;
            }
            if (method_name == "indexOf") {
                if (args.empty()) throw std::runtime_error("string.indexOf() requires 1 argument");
                auto pos = str.find(args[0]->toString());
                result_ = std::make_shared<Value>(pos != std::string::npos ? static_cast<int>(pos) : -1);
                return;
            }
            if (method_name == "lastIndexOf") {
                if (args.empty()) throw std::runtime_error("string.lastIndexOf() requires 1 argument");
                auto pos = str.rfind(args[0]->toString());
                result_ = std::make_shared<Value>(pos != std::string::npos ? static_cast<int>(pos) : -1);
                return;
            }
            if (method_name == "substring" || method_name == "substr" || method_name == "slice") {
                if (args.empty()) throw std::runtime_error("string.substring() requires at least 1 argument (start)");
                int start = std::get<int>(args[0]->data);
                if (start < 0) start = 0;
                if (start >= static_cast<int>(str.size())) {
                    result_ = std::make_shared<Value>(std::string(""));
                    return;
                }
                if (args.size() >= 2) {
                    int end = std::get<int>(args[1]->data);
                    if (end > static_cast<int>(str.size())) end = static_cast<int>(str.size());
                    result_ = std::make_shared<Value>(str.substr(start, end - start));
                } else {
                    result_ = std::make_shared<Value>(str.substr(start));
                }
                return;
            }
            if (method_name == "replace") {
                if (args.size() < 2) throw std::runtime_error("string.replace() requires 2 arguments (old, new)");
                std::string old_str = args[0]->toString();
                std::string new_str = args[1]->toString();
                std::string result = str;
                size_t pos = 0;
                while ((pos = result.find(old_str, pos)) != std::string::npos) {
                    result.replace(pos, old_str.length(), new_str);
                    pos += new_str.length();
                }
                result_ = std::make_shared<Value>(result);
                return;
            }
            if (method_name == "toUpperCase" || method_name == "upper") {
                std::string result = str;
                std::transform(result.begin(), result.end(), result.begin(), ::toupper);
                result_ = std::make_shared<Value>(result);
                return;
            }
            if (method_name == "toLowerCase" || method_name == "lower") {
                std::string result = str;
                std::transform(result.begin(), result.end(), result.begin(), ::tolower);
                result_ = std::make_shared<Value>(result);
                return;
            }
            if (method_name == "trim") {
                std::string result = str;
                result.erase(0, result.find_first_not_of(" \t\n\r"));
                result.erase(result.find_last_not_of(" \t\n\r") + 1);
                result_ = std::make_shared<Value>(result);
                return;
            }
            if (method_name == "split") {
                if (args.empty()) throw std::runtime_error("string.split() requires 1 argument (separator)");
                std::string sep = args[0]->toString();
                std::vector<std::shared_ptr<Value>> parts;
                if (sep.empty()) {
                    for (char c : str) parts.push_back(std::make_shared<Value>(std::string(1, c)));
                } else {
                    size_t start = 0, pos;
                    while ((pos = str.find(sep, start)) != std::string::npos) {
                        parts.push_back(std::make_shared<Value>(str.substr(start, pos - start)));
                        start = pos + sep.size();
                    }
                    parts.push_back(std::make_shared<Value>(str.substr(start)));
                }
                result_ = std::make_shared<Value>(parts);
                return;
            }
            if (method_name == "startsWith" || method_name == "starts_with") {
                if (args.empty()) throw std::runtime_error("string.startsWith() requires 1 argument");
                result_ = std::make_shared<Value>(str.find(args[0]->toString()) == 0);
                return;
            }
            if (method_name == "endsWith" || method_name == "ends_with") {
                if (args.empty()) throw std::runtime_error("string.endsWith() requires 1 argument");
                std::string suffix = args[0]->toString();
                result_ = std::make_shared<Value>(
                    str.size() >= suffix.size() &&
                    str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0
                );
                return;
            }
            if (method_name == "index_of") {
                if (args.empty()) throw std::runtime_error("string.index_of() requires 1 argument");
                auto pos = str.find(args[0]->toString());
                result_ = std::make_shared<Value>(pos != std::string::npos ? static_cast<int>(pos) : -1);
                return;
            }
            // DOT-1: char_at, reverse, repeat
            if (method_name == "char_at") {
                if (args.empty()) throw std::runtime_error("string.char_at() requires 1 argument (index)");
                int idx = args[0]->toInt();
                if (idx < 0 || idx >= static_cast<int>(str.size())) {
                    throw std::runtime_error("String index out of bounds: " + std::to_string(idx) + " (length: " + std::to_string(str.size()) + ")");
                }
                result_ = std::make_shared<Value>(std::string(1, str[static_cast<size_t>(idx)]));
                return;
            }
            if (method_name == "reverse") {
                std::string rev(str.rbegin(), str.rend());
                result_ = std::make_shared<Value>(rev);
                return;
            }
            if (method_name == "repeat") {
                if (args.empty()) throw std::runtime_error("string.repeat() requires 1 argument (count)");
                int count = args[0]->toInt();
                std::string repeated;
                for (int ri = 0; ri < count; ri++) repeated += str;
                result_ = std::make_shared<Value>(repeated);
                return;
            }
            if (method_name == "pad_right") {
                if (args.empty() || args.size() > 2) throw std::runtime_error("pad_right() takes 1-2 arguments (width[, fill_char])");
                int width = args[0]->toInt();
                char fill = ' ';
                if (args.size() == 2) {
                    auto fs = std::get<std::string>(args[1]->data);
                    if (fs.length() != 1) throw std::runtime_error("pad_right() fill_char must be exactly 1 character");
                    fill = fs[0];
                }
                std::string s = str;
                if (static_cast<int>(s.length()) < width) s.append(width - s.length(), fill);
                result_ = std::make_shared<Value>(s);
                return;
            }
            if (method_name == "pad_left") {
                if (args.empty() || args.size() > 2) throw std::runtime_error("pad_left() takes 1-2 arguments (width[, fill_char])");
                int width = args[0]->toInt();
                char fill = ' ';
                if (args.size() == 2) {
                    auto fs = std::get<std::string>(args[1]->data);
                    if (fs.length() != 1) throw std::runtime_error("pad_left() fill_char must be exactly 1 character");
                    fill = fs[0];
                }
                if (static_cast<int>(str.length()) < width) {
                    result_ = std::make_shared<Value>(std::string(width - str.length(), fill) + str);
                } else {
                    result_ = std::make_shared<Value>(str);
                }
                return;
            }
            // HELPER-5: .len() -> .length()
            if (method_name == "len") {
                throw std::runtime_error(
                    "Unknown method: .len()\n  Did you mean: .length() or the builtin len(x)?"
                );
            }
            // Not a built-in string method - fall through to normal handling
        }

        // ===== Normal member access call (functions stored in dicts, struct methods, etc.) =====
        // Evaluate the full member access
        member_call->accept(*this);
        auto func_value = result_;

        // Check if it's a function
        if (auto* func_ptr = std::get_if<std::shared_ptr<FunctionValue>>(&func_value->data)) {
            result_ = callFunction(func_value, args);
            return;
        }

        std::ostringstream oss;
        oss << "Type error: Member is not callable\n\n";
        oss << "  Member type: " << getTypeName(func_value) << "\n";
        oss << "  Expected: function\n\n";

        // Detect stdlib constant access with () - e.g., math.PI()
        auto* obj_id_err = dynamic_cast<ast::IdentifierExpr*>(member_call->getObject());
        if (obj_id_err && (method_name == "PI" || method_name == "E")) {
            std::string mod_name = obj_id_err->getName();
            oss << "  Help:\n";
            oss << "  - " << mod_name << "." << method_name << " is a constant, not a function\n";
            oss << "  - Access it without parentheses:\n\n";
            oss << "  Example:\n";
            oss << "    ✗ Wrong: " << mod_name << "." << method_name << "()\n";
            oss << "    ✓ Right: " << mod_name << "." << method_name << "\n";
        } else {
            oss << "  Help:\n";
            oss << "  - Only functions can be called with ()\n";
            oss << "  - If accessing a property or constant, don't use ()\n\n";
            oss << "  Example:\n";
            oss << "    ✗ Wrong: obj.value()    // value is not a function\n";
            oss << "    ✓ Right: obj.value       // access the property directly\n";
            oss << "    ✓ Right: obj.getValue()  // call a function instead\n";
        }
        throw std::runtime_error(oss.str());
    }

    // Try to get function name (for built-ins and named functions)
    auto* id_expr = dynamic_cast<ast::IdentifierExpr*>(node.getCallee());

    // If callee is not an identifier (e.g., array[0], higher-order function result),
    // evaluate it and check if it's a callable function
    if (!id_expr) {
        node.getCallee()->accept(*this);
        auto callee_value = result_;

        // Check if the result is a function
        if (auto* func_ptr = std::get_if<std::shared_ptr<FunctionValue>>(&callee_value->data)) {
            (void)func_ptr;  // Type check only, value not needed
            // Call the function using the general callFunction helper
            result_ = callFunction(callee_value, args);
            return;
        }

        std::ostringstream oss;
        oss << "Type error: Expression is not callable\n\n";
        oss << "  Tried to call: " << getTypeName(callee_value) << "\n";
        oss << "  Expected: function\n\n";
        oss << "  Help:\n";
        oss << "  - Only functions can be called with ()\n";
        oss << "  - If you're calling arr[i], make sure arr contains functions\n";
        oss << "  - If you're using higher-order functions, verify they return functions\n\n";
        oss << "  Example:\n";
        oss << "    ✗ Wrong: let arr = [1, 2, 3]; arr[0]()  // int isn't callable\n";
        oss << "    ✓ Right: let fns = [function() { ... }]; fns[0]()\n";
        throw std::runtime_error(oss.str());
    }

    std::string func_name = id_expr->getName();

    // Check if it's a user-defined function or loaded block
    if (current_env_->has(func_name)) {
        auto value = current_env_->get(func_name);

        // Check for user-defined function
        if (auto* func_ptr = std::get_if<std::shared_ptr<FunctionValue>>(&value->data)) {
            auto& func = *func_ptr;

            // Phase 5: If this is a generator function, return GeneratorValue
            if (func->is_generator) {
                auto gen = std::make_shared<GeneratorValue>();
                gen->func = func;
                gen->args = args;
                result_ = std::make_shared<Value>(gen);
                return;
            }

            // Check parameter count and defaults
            size_t min_args = 0;
            for (size_t i = 0; i < func->params.size(); i++) {
                if (!func->defaults[i]) {
                    min_args = i + 1;  // Last non-default parameter + 1
                }
            }

            if (args.size() < min_args || args.size() > func->params.size()) {
                // Build parameter list for error message
                std::ostringstream oss;
                oss << "Function " << func->name << " expects " << min_args << "-"
                    << func->params.size() << " arguments, got " << args.size() << "\n"
                    << "  Function: " << func->name << "(";

                // Show parameters
                for (size_t i = 0; i < func->params.size(); ++i) {
                    if (i > 0) oss << ", ";
                    oss << func->params[i];
                }
                oss << ")\n";

                // Show what was provided
                oss << "  Provided: " << args.size() << " argument(s)";

                throw std::runtime_error(oss.str());
            }

            // Governance v4: Check input contracts (direct call path)
            if (governance_ && governance_->isActive()) {
                std::vector<std::string> arg_types;
                for (const auto& arg : args) {
                    arg_types.push_back(arg ? getTypeName(arg) : "null");
                }
                std::string input_err = governance_->checkFunctionInputContract(
                    func->name, arg_types, node.getLocation().line);
                if (!input_err.empty()) {
                    governance_->logContractCheck(func->name, "FAIL", input_err,
                                                  current_file_, node.getLocation().line);
                    throw std::runtime_error(input_err);
                }
            }

            // Phase 2.4.4 Phase 3: Handle generic type arguments (explicit or inferred)
            std::map<std::string, ast::Type> type_substitutions;
            if (!func->type_parameters.empty()) {
                LOG_DEBUG("[INFO] Function {} is generic with type parameters: ", func->name);
                for (const auto& tp : func->type_parameters) {
                    fmt::print("{} ", tp);
                }
                fmt::print("\n");

                // Check if explicit type arguments were provided
                const auto& explicit_type_args = node.getTypeArguments();
                if (!explicit_type_args.empty()) {
                    // Use explicit type arguments
                    LOG_DEBUG("[INFO] Using {} explicit type argument(s)\n", explicit_type_args.size());

                    if (explicit_type_args.size() != func->type_parameters.size()) {
                        throw std::runtime_error(fmt::format(
                            "Function {} expects {} type parameter(s), got {}",
                            func->name, func->type_parameters.size(), explicit_type_args.size()));
                    }

                    for (size_t i = 0; i < func->type_parameters.size(); i++) {
                        type_substitutions.insert({func->type_parameters[i], explicit_type_args[i]});
                        LOG_DEBUG("[INFO] Type parameter {} = {}\n",
                                  func->type_parameters[i],
                                  formatTypeName(explicit_type_args[i]));
                    }
                } else {
                    // Infer type arguments from actual arguments
                    auto inferred_types = inferGenericArgs(func, args);

                    // Build substitution map
                    for (size_t i = 0; i < func->type_parameters.size() && i < inferred_types.size(); i++) {
                        type_substitutions.insert({func->type_parameters[i], inferred_types[i]});
                    }
                }
            }

            // Phase 2.4.2 & 2.4.5: Validate argument types (union types and null safety)
            for (size_t i = 0; i < args.size(); i++) {
                // Phase 2.4.4 Phase 3: Substitute type parameters in param type
                ast::Type param_type = func->param_types[i];
                if (!type_substitutions.empty()) {
                    param_type = substituteTypeParams(param_type, type_substitutions);
                }

                // Phase 2.4.5: Null safety - cannot pass null to non-nullable parameter
                if (!param_type.is_nullable && isNull(args[i])) {
                    throw std::runtime_error(
                        "Null safety error: Cannot pass null to non-nullable parameter '" +
                        func->params[i] + "' of function '" + func->name + "'" +
                        "\n  Expected: " + formatTypeName(param_type) +
                        "\n  Got: null" +
                        "\n  Help: Change parameter to nullable: " +
                        formatTypeName(param_type) + "?"
                    );
                }

                // Check union types
                if (param_type.kind == ast::TypeKind::Union) {
                    if (!valueMatchesUnion(args[i], param_type.union_types)) {
                        throw std::runtime_error(
                            "Type error: Parameter '" + func->params[i] +
                            "' of function '" + func->name +
                            "' expects " + formatTypeName(param_type) +
                            ", but got " + getValueTypeName(args[i])
                        );
                    }
                }
                // Check non-union types (if not Any)
                else if (param_type.kind != ast::TypeKind::Any) {
                    if (!valueMatchesType(args[i], param_type)) {
                        throw std::runtime_error(
                            "Type error: Parameter '" + func->params[i] +
                            "' of function '" + func->name +
                            "' expects " + formatTypeName(param_type) +
                            ", but got " + getValueTypeName(args[i])
                        );
                    }
                }
            }

            // ISS-022: Create new environment for function execution with closure as parent
            auto parent_env = func->closure ? func->closure : global_env_;
            auto func_env = std::make_shared<Environment>(parent_env);

            // Phase 2.1: Bind provided arguments (ref vs value semantics)
            for (size_t i = 0; i < args.size(); i++) {
                if (func->param_types[i].is_reference) {
                    // Reference parameter: pass the shared_ptr directly (share the value)
                    func_env->define(func->params[i], args[i]);
                } else {
                    // Value parameter: copy the value (default behavior)
                    func_env->define(func->params[i], copyValue(args[i]));
                }
            }

            // Bind default values for missing arguments
            for (size_t i = args.size(); i < func->params.size(); i++) {
                if (func->defaults[i]) {
                    // Evaluate default expression in current environment
                    auto saved_env = current_env_;
                    current_env_ = func_env;
                    func->defaults[i]->accept(*this);
                    auto default_val = result_;
                    current_env_ = saved_env;

                    // Phase 2.1: Apply ref vs value semantics to default parameters
                    if (func->param_types[i].is_reference) {
                        func_env->define(func->params[i], default_val);
                    } else {
                        func_env->define(func->params[i], copyValue(default_val));
                    }
                } else {
                    throw std::runtime_error(fmt::format(
                        "Function {} parameter {} has no default value",
                        func->name, func->params[i]));
                }
            }

            // Save current environment and function
            auto saved_env = current_env_;
            auto saved_returning = returning_;
            auto saved_function = current_function_;  // Phase 2.4.2: Save for return type validation
            auto saved_type_subst = current_type_substitutions_;  // Phase 2.4.4: Save type substitutions
            auto saved_file = current_file_;  // Phase 3.1: Save current file for cross-module calls
            env_stack_.push_back(current_env_);  // BUG-10 fix: Make caller env visible to GC
            current_env_ = func_env;
            returning_ = false;
            current_function_ = func;  // Phase 2.4.2: Track current function
            current_type_substitutions_ = type_substitutions;  // Phase 2.4.4: Set type substitutions for generics
            current_file_ = func->source_file;  // Phase 3.1: Set file to function's source file

            // Issue #3: Push file context for function's source file
            if (!func->source_file.empty()) {
                pushFileContext(func->source_file);
            }

            // Phase 4.1: Push stack frame for error reporting
            pushStackFrame(func->name, func->source_line);  // Phase 3.1: Use actual line number

            // Push call frame if debugger is active
            if (debugger_ && debugger_->isActive()) {
                debugger::CallFrame frame;
                frame.function_name = func->name;
                frame.source_location = "unknown:0:0";  // TODO: Get from AST node
                frame.env = func_env;
                frame.frame_depth = debugger_->getCurrentDepth();

                // Populate locals map
                for (size_t i = 0; i < args.size(); i++) {
                    frame.locals[func->params[i]] = args[i];
                }

                debugger_->pushFrame(frame);
            }

            // Governance: Check and track call depth for direct function calls
            if (governance_ && governance_->isActive()) {
                // BUG-AD: Reset lastReturnTainted before each function call (direct path)
                governance_->setLastReturnTainted(false);
                std::string depth_err = governance_->checkCallDepth(call_depth_ + 1);
                if (!depth_err.empty()) {
                    popStackFrame();
                    if (!func->source_file.empty()) popFileContext();
                    if (!env_stack_.empty()) env_stack_.pop_back();  // BUG-10 fix
                    current_env_ = saved_env;
                    returning_ = saved_returning;
                    current_function_ = saved_function;
                    current_type_substitutions_ = saved_type_subst;
                    current_file_ = saved_file;
                    throw std::runtime_error(depth_err);
                }
            }
            ++call_depth_;

            // Phase 4.1: Execute function body with proper error propagation
            try {
                func->body->accept(*this);
            } catch (...) {
                // Clean up before re-throwing
                --call_depth_;
                if (debugger_ && debugger_->isActive()) {
                    debugger_->popFrame();
                }
                popStackFrame();
                // Issue #3: Pop file context on error
                if (!func->source_file.empty()) {
                    popFileContext();
                }
                if (!env_stack_.empty()) env_stack_.pop_back();  // BUG-10 fix
                current_env_ = saved_env;
                returning_ = saved_returning;
                current_function_ = saved_function;  // Phase 2.4.2: Restore
                current_type_substitutions_ = saved_type_subst;  // Phase 2.4.4: Restore
                current_file_ = saved_file;  // Phase 3.1: Restore file
                throw;  // Re-throw with stack frame info already captured
            }

            --call_depth_;

            // Pop call frame if debugger is active
            if (debugger_ && debugger_->isActive()) {
                debugger_->popFrame();
            }

            // Phase 4.1: Pop stack frame
            popStackFrame();

            // Issue #3: Pop file context on success
            if (!func->source_file.empty()) {
                popFileContext();
            }

            // Restore environment and function
            if (!env_stack_.empty()) env_stack_.pop_back();  // BUG-10 fix
            current_env_ = saved_env;
            returning_ = saved_returning;
            current_function_ = saved_function;  // Phase 2.4.2: Restore
            current_type_substitutions_ = saved_type_subst;  // Phase 2.4.4: Restore
            current_file_ = saved_file;  // Phase 3.1: Restore file

            // Phase 3 Governance: Check function contracts (inline call path)
            if (governance_ && governance_->isActive() && !func->name.empty()) {
                auto return_value = result_;
                std::string result_str = return_value ? return_value->toString() : "null";
                std::string result_type = return_value ? getTypeName(return_value) : "null";
                std::string contract_err = governance_->checkFunctionContract(
                    func->name, result_str, result_type, func->source_line);
                if (!contract_err.empty()) {
                    // BUG-Q: Audit logging for contract check on direct call path
                    governance_->logContractCheck(func->name, "FAIL", contract_err,
                                                  current_file_, node.getLocation().line);
                    throw std::runtime_error(contract_err);
                }
                // BUG-Q: Log successful contract check too
                governance_->logContractCheck(func->name, "PASS", "return_type=" + result_type,
                                              current_file_, node.getLocation().line);
            }

            LOG_TRACE("[CALL] Function {} executed\n", func->name);
            return;
        }

        // Check for loaded block
        if (auto* block_ptr = std::get_if<std::shared_ptr<BlockValue>>(&value->data)) {
            auto& block = *block_ptr;

            LOG_TRACE("[CALL] Invoking block {} ({}) with {} args\n",
                      block->metadata.name, block->metadata.language, args.size());

            // Phase 7: Try executor-based calling first
            auto* executor = block->getExecutor();
            if (executor) {
                LOG_DEBUG("[INFO] Calling block via executor ({})...\n", block->metadata.language);

                // Determine function name to call:
                // - If member_path is set, this is a member accessor (e.g., block.method)
                // - Otherwise, use the function name being called
                std::string function_to_call = block->member_path.empty()
                    ? func_name
                    : block->member_path;

                LOG_DEBUG("[INFO] Calling function: {}\n", function_to_call);
                result_ = executor->callFunction(function_to_call, args);
                flushExecutorOutput(executor);  // Phase 11.1: Flush captured output

                if (result_) {
                    LOG_DEBUG("[SUCCESS] Block call completed\n");

                    // Phase 4.4: Record block usage for analytics
                    if (block_loader_) {
                        // Estimate tokens saved (use block's token_count or default to 50)
                        int tokens_saved = (block->metadata.token_count > 0)
                            ? block->metadata.token_count
                            : 50;
                        block_loader_->recordBlockUsage(block->metadata.block_id, tokens_saved);

                        // Record block pair if there was a previous block
                        if (!last_executed_block_id_.empty()) {
                            block_loader_->recordBlockPair(last_executed_block_id_, block->metadata.block_id);
                        }
                        last_executed_block_id_ = block->metadata.block_id;
                    }
                } else {
                    fmt::print("[WARN] Block call returned null\n");
                    result_ = std::make_shared<Value>();
                }
                return;
            }

            // Fallback: Legacy Python handling for blocks without executor
            if (block->metadata.language == "python") {
                // Python blocks: Execute using embedded Python interpreter
#ifdef NAAB_HAS_PYTHON
                LOG_DEBUG("[INFO] Executing Python block: {}\n", block->metadata.name);

                // Add common imports automatically
                PyRun_SimpleString("from typing import Dict, List, Optional, Any, Union\n"
                                  "import sys\n");

                // Execute the block code to define classes/functions using exec()
                // This handles multi-line code with proper indentation
                std::string exec_code = "exec('''" + block->code + "''')";
                PyRun_SimpleString(exec_code.c_str());

                // Handle member access calls
                if (!block->member_path.empty()) {
                    LOG_DEBUG("[INFO] Calling member: {}\n", block->member_path);

                    // Build argument list for Python
                    std::string args_str = "(";
                    for (size_t i = 0; i < args.size(); i++) {
                        if (i > 0) args_str += ", ";

                        // Convert Value to Python representation
                        if (auto* intval = std::get_if<int>(&args[i]->data)) {
                            args_str += std::to_string(*intval);
                        } else if (auto* floatval = std::get_if<double>(&args[i]->data)) {
                            args_str += std::to_string(*floatval);
                        } else if (auto* strval = std::get_if<std::string>(&args[i]->data)) {
                            args_str += "\"" + *strval + "\"";
                        } else if (auto* boolval = std::get_if<bool>(&args[i]->data)) {
                            args_str += *boolval ? "True" : "False";
                        } else {
                            args_str += "None";
                        }
                    }
                    args_str += ")";

                    // Call the member and capture return value
                    std::string call_expr = block->member_path + args_str;

                    // Get main module
                    PyObject* main_module = PyImport_AddModule("__main__");
                    PyObject* global_dict = PyModule_GetDict(main_module);

                    // Compile and evaluate the expression
                    PyObject* py_result = PyRun_String(call_expr.c_str(),
                                                       Py_eval_input,
                                                       global_dict,
                                                       global_dict);

                    if (py_result != nullptr) {
                        // Convert Python result to NAAb Value
                        if (PyLong_Check(py_result)) {
                            long val = PyLong_AsLong(py_result);
                            result_ = std::make_shared<Value>(static_cast<int>(val));
                            LOG_DEBUG("[SUCCESS] Returned int: {}\n", val);
                        } else if (PyFloat_Check(py_result)) {
                            double val = PyFloat_AsDouble(py_result);
                            result_ = std::make_shared<Value>(val);
                            LOG_DEBUG("[SUCCESS] Returned float: {}\n", val);
                        } else if (PyUnicode_Check(py_result)) {
                            const char* val = PyUnicode_AsUTF8(py_result);
                            result_ = std::make_shared<Value>(std::string(val));
                            LOG_DEBUG("[SUCCESS] Returned string: {}\n", val);
                        } else if (PyBool_Check(py_result)) {
                            bool val = py_result == Py_True;
                            result_ = std::make_shared<Value>(val);
                            LOG_DEBUG("[SUCCESS] Returned bool: {}\n", val);
                        } else if (py_result == Py_None) {
                            result_ = std::make_shared<Value>();
                            LOG_DEBUG("[SUCCESS] Returned None\n");
                            Py_DECREF(py_result);
                        } else {
                            // Complex object - wrap in PythonObjectValue for method chaining
                            auto py_obj = std::make_shared<PythonObjectValue>(py_result);
                            result_ = std::make_shared<Value>(py_obj);
                            LOG_DEBUG("[SUCCESS] Returned Python object: {}\n", py_obj->repr);
                            Py_DECREF(py_result);  // PythonObjectValue has its own reference
                        }
                    } else {
                        PyErr_Print();
                        fmt::print("[ERROR] Member call failed\n");
                        result_ = std::make_shared<Value>();
                    }

                    return;
                }

                // Regular block execution (no member access)
                // Inject arguments as Python variables
                if (!args.empty()) {
                    // Create args list
                    std::string args_setup = "args = [";
                    for (size_t i = 0; i < args.size(); i++) {
                        if (i > 0) args_setup += ", ";

                        // Convert Value to Python representation
                        if (auto* intval = std::get_if<int>(&args[i]->data)) {
                            args_setup += std::to_string(*intval);
                        } else if (auto* floatval = std::get_if<double>(&args[i]->data)) {
                            args_setup += std::to_string(*floatval);
                        } else if (auto* strval = std::get_if<std::string>(&args[i]->data)) {
                            args_setup += "\"" + *strval + "\"";
                        } else if (auto* boolval = std::get_if<bool>(&args[i]->data)) {
                            args_setup += *boolval ? "True" : "False";
                        } else {
                            args_setup += "None";
                        }
                    }
                    args_setup += "]\n";

                    PyRun_SimpleString(args_setup.c_str());
                    LOG_DEBUG("[INFO] Injected {} args into Python context\n", args.size());
                }

                // Execute Python code - for blocks that are classes/functions
                // Try to evaluate as expression first (for simple cases)
                PyObject* main_module = PyImport_AddModule("__main__");
                PyObject* global_dict = PyModule_GetDict(main_module);
                (void)global_dict;  // Reserved for future expression evaluation

                // For blocks that define classes, just execute and return success
                int result = PyRun_SimpleString(block->code.c_str());

                if (result == 0) {
                    LOG_DEBUG("[SUCCESS] Python block executed successfully\n");
                    result_ = std::make_shared<Value>();  // Return null for definition blocks
                } else {
                    fmt::print("[ERROR] Python block execution failed\n");
                    result_ = std::make_shared<Value>();
                }
#else
                fmt::print("[WARN] Python execution not available\n");
                result_ = std::make_shared<Value>();
#endif
                return;
            } else {
                fmt::print("[WARN] Unsupported block language: {}\n", block->metadata.language);
                result_ = std::make_shared<Value>();
                return;
            }
        }
    }

    // Built-in functions
    if (func_name == "print") {
        for (size_t i = 0; i < args.size(); i++) {
            if (i > 0) std::cout << " ";
            std::cout << args[i]->toString();
        }
        std::cout << std::endl;
        result_ = std::make_shared<Value>();
    }
    else if (func_name == "len") {
        if (args.empty()) {
            throw std::runtime_error("len() requires exactly 1 argument\n  Example: len([1, 2, 3])  // 3");
        }
        if (auto* str = std::get_if<std::string>(&args[0]->data)) {
            result_ = std::make_shared<Value>(static_cast<int>(str->length()));
        } else if (auto* list = std::get_if<std::vector<std::shared_ptr<Value>>>(&args[0]->data)) {
            result_ = std::make_shared<Value>(static_cast<int>(list->size()));
        } else if (auto* dict = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&args[0]->data)) {
            result_ = std::make_shared<Value>(static_cast<int>(dict->size()));
        } else {
            result_ = std::make_shared<Value>(0);
        }
    }
    else if (func_name == "type") {
        if (args.empty()) {
            throw std::runtime_error("type() requires exactly 1 argument\n  Example: type(42)  // \"int\"");
        }
        if (!args.empty()) {
            std::string type_name = std::visit([](auto&& arg) -> std::string {
                using T = std::decay_t<decltype(arg)>;
                if constexpr (std::is_same_v<T, int>) return "int";
                else if constexpr (std::is_same_v<T, double>) return "float";
                else if constexpr (std::is_same_v<T, bool>) return "bool";
                else if constexpr (std::is_same_v<T, std::string>) return "string";
                else if constexpr (std::is_same_v<T, std::vector<std::shared_ptr<Value>>>) return "array";
                else if constexpr (std::is_same_v<T, std::unordered_map<std::string, std::shared_ptr<Value>>>) return "dict";
                else if constexpr (std::is_same_v<T, std::shared_ptr<BlockValue>>) return "block";
                else if constexpr (std::is_same_v<T, std::shared_ptr<FunctionValue>>) return "function";
                else if constexpr (std::is_same_v<T, std::shared_ptr<PythonObjectValue>>) return "python_object";
                return "unknown";
            }, args[0]->data);
            result_ = std::make_shared<Value>(type_name);
        }
    }
    // Phase 2.4.2: typeof operator for union type checking
    else if (func_name == "typeof") {
        if (args.empty()) {
            throw std::runtime_error("typeof() requires exactly 1 argument");
        }
        if (args.size() > 1) {
            throw std::runtime_error("typeof() requires exactly 1 argument, got " + std::to_string(args.size()));
        }
        std::string type_name = getValueTypeName(args[0]);
        result_ = std::make_shared<Value>(type_name);
    }
    // range() builtin — range(end), range(start, end), range(start, end, step)
    else if (func_name == "range") {
        if (args.empty() || args.size() > 3) {
            throw std::runtime_error(
                "Argument error: range() takes 1-3 arguments (end), (start, end), or (start, end, step)\n\n"
                "  Example:\n"
                "    range(5)        // [0, 1, 2, 3, 4]\n"
                "    range(2, 6)     // [2, 3, 4, 5]\n"
                "    range(0, 10, 2) // [0, 2, 4, 6, 8]\n");
        }

        int start = 0, end = 0, step = 1;

        if (args.size() == 1) {
            if (auto* v = std::get_if<int>(&args[0]->data)) { end = *v; }
            else if (auto* d = std::get_if<double>(&args[0]->data)) { end = static_cast<int>(*d); }
            else { throw std::runtime_error("range() arguments must be numbers"); }
        } else if (args.size() >= 2) {
            if (auto* v = std::get_if<int>(&args[0]->data)) { start = *v; }
            else if (auto* d = std::get_if<double>(&args[0]->data)) { start = static_cast<int>(*d); }
            else { throw std::runtime_error("range() arguments must be numbers"); }

            if (auto* v = std::get_if<int>(&args[1]->data)) { end = *v; }
            else if (auto* d = std::get_if<double>(&args[1]->data)) { end = static_cast<int>(*d); }
            else { throw std::runtime_error("range() arguments must be numbers"); }
        }
        if (args.size() == 3) {
            if (auto* v = std::get_if<int>(&args[2]->data)) { step = *v; }
            else if (auto* d = std::get_if<double>(&args[2]->data)) { step = static_cast<int>(*d); }
            else { throw std::runtime_error("range() arguments must be numbers"); }
        }

        if (step == 0) {
            throw std::runtime_error("range() step cannot be zero");
        }

        std::vector<std::shared_ptr<Value>> result;
        if (step > 0) {
            for (int i = start; i < end; i += step) {
                result.push_back(std::make_shared<Value>(i));
            }
        } else {
            for (int i = start; i > end; i += step) {
                result.push_back(std::make_shared<Value>(i));
            }
        }

        result_ = std::make_shared<Value>(result);
    }
    // Type cast builtins
    else if (func_name == "int") {
        if (args.size() != 1) throw std::runtime_error("int() takes exactly 1 argument");
        if (auto* str = std::get_if<std::string>(&args[0]->data)) {
            try {
                // Try parsing as double first to handle "3.14" -> 3
                double d = std::stod(*str);
                result_ = std::make_shared<Value>(static_cast<int>(d));
            } catch (...) {
                throw std::runtime_error("int() cannot convert \"" + *str + "\" to int");
            }
        } else {
            result_ = std::make_shared<Value>(args[0]->toInt());
        }
    }
    else if (func_name == "float") {
        if (args.size() != 1) throw std::runtime_error("float() takes exactly 1 argument");
        if (auto* str = std::get_if<std::string>(&args[0]->data)) {
            try {
                result_ = std::make_shared<Value>(std::stod(*str));
            } catch (...) {
                throw std::runtime_error("float() cannot convert \"" + *str + "\" to float");
            }
        } else {
            result_ = std::make_shared<Value>(args[0]->toFloat());
        }
    }
    else if (func_name == "string") {
        if (args.size() != 1) throw std::runtime_error("string() takes exactly 1 argument");
        result_ = std::make_shared<Value>(args[0]->toString());
    }
    else if (func_name == "bool") {
        if (args.size() != 1) throw std::runtime_error("bool() takes exactly 1 argument");
        result_ = std::make_shared<Value>(args[0]->toBool());
    }
    // Phase 3.2: Manual garbage collection trigger
    else if (func_name == "gc_collect") {
        runGarbageCollection(current_env_);  // Pass current environment
        result_ = std::make_shared<Value>();  // Return void
    }
    // Array/string slicing: __slice(collection, start, end)
    else if (func_name == "__slice") {
        if (args.size() != 3) throw std::runtime_error("__slice() takes exactly 3 arguments");
        auto& collection = args[0];
        int start = 0, end = 0;
        if (auto* si = std::get_if<int>(&args[1]->data)) start = *si;
        else if (auto* sd = std::get_if<double>(&args[1]->data)) start = static_cast<int>(*sd);
        else throw std::runtime_error("Slice start index must be a number");
        if (auto* ei = std::get_if<int>(&args[2]->data)) end = *ei;
        else if (auto* ed = std::get_if<double>(&args[2]->data)) end = static_cast<int>(*ed);
        else throw std::runtime_error("Slice end index must be a number");

        if (auto* arr = std::get_if<std::vector<std::shared_ptr<Value>>>(&collection->data)) {
            int len = static_cast<int>(arr->size());
            if (start < 0) start = std::max(0, len + start);
            if (end < 0) end = std::max(0, len + end);
            if (start > len) start = len;
            if (end > len) end = len;
            if (start >= end) {
                result_ = std::make_shared<Value>(std::vector<std::shared_ptr<Value>>{});
            } else {
                std::vector<std::shared_ptr<Value>> sliced(arr->begin() + start, arr->begin() + end);
                result_ = std::make_shared<Value>(sliced);
            }
        } else if (auto* str = std::get_if<std::string>(&collection->data)) {
            int len = static_cast<int>(str->size());
            if (start < 0) start = std::max(0, len + start);
            if (end < 0) end = std::max(0, len + end);
            if (start > len) start = len;
            if (end > len) end = len;
            if (start >= end) {
                result_ = std::make_shared<Value>(std::string(""));
            } else {
                result_ = std::make_shared<Value>(str->substr(start, end - start));
            }
        } else {
            throw std::runtime_error("Slice operator requires an array or string");
        }
    }
    else {
        // Function not found - provide targeted hints for common mistakes
        std::ostringstream oss;
        oss << "Name error: Undefined function\n\n";
        oss << "  Function: " << func_name << "\n\n";

        // Targeted hints for commonly misused function names
        if (func_name == "sleep") {
            oss << "  'sleep' is in the time module, not a global function:\n";
            oss << "    import time\n";
            oss << "    time.sleep(1.0)  // sleep for 1 second\n";
        } else if (func_name == "exit") {
            oss << "  NAAb has no exit() function.\n";
            oss << "  To stop: return from functions, or let main block end.\n";
        } else if (func_name == "error") {
            oss << "  'error' is not a built-in. To print errors:\n";
            oss << "    print(\"ERROR: something went wrong\")\n";
        } else if (func_name == "callFunction") {
            oss << "  NAAb does not need callFunction(). Functions are first-class:\n";
            oss << "    let result = fn(arg1, arg2)   // call directly\n";
        } else if (func_name == "parseInt" || func_name == "parseFloat" || func_name == "Number") {
            oss << "  Use NAAb type conversion functions:\n";
            oss << "    int(\"42\")     // instead of parseInt(\"42\")\n";
            oss << "    float(\"3.14\") // instead of parseFloat(\"3.14\")\n";
        } else if (func_name == "toString" || func_name == "str") {
            oss << "  Use NAAb type conversion:\n";
            oss << "    string(42)    // instead of toString(42)\n";
        } else if (func_name == "keys" || func_name == "values") {
            oss << "  '" << func_name << "' is a method on dicts, not a global function:\n";
            oss << "    myDict." << func_name << "()  // correct\n";
        } else if (func_name == "push" || func_name == "append" || func_name == "pop") {
            oss << "  '" << func_name << "' is a method on arrays, not a global function:\n";
            oss << "    myArray." << func_name << "(item)  // correct\n";
            oss << "    // or: import array; array.push(myArray, item)\n";
        } else if (func_name == "forEach" || func_name == "map" || func_name == "filter" || func_name == "reduce") {
            if (func_name == "forEach") {
                oss << "  NAAb uses for-in loops instead of forEach:\n";
                oss << "    for item in myArray { print(item) }\n";
            } else {
                oss << "  NAAb has array." << func_name << "_fn (NOT a global function):\n";
                oss << "    array.map_fn(arr, fn(x) { return x * 2 })\n";
                oss << "    array.filter_fn(arr, fn(x) { return x > 5 })\n";
                oss << "    array.reduce_fn(arr, fn(acc, x) { return acc + x }, 0)\n";
                oss << "  Note: These do NOT work with dot notation — use array." << func_name << "_fn(arr, fn), not arr." << func_name << "_fn(fn)\n";
            }
        } else {
            oss << "  Help:\n";
            oss << "  - Check for typos in the function name\n";
            oss << "  - Make sure the function is defined before calling\n";
            oss << "  - For stdlib functions, use module.function() (e.g., array.push())\n";
        }
        oss << "\n  Common builtins: print, len, type, typeof, int, float, string, bool\n\n";
        oss << "  Example:\n";
        oss << "    ✗ Wrong: printt(\"hello\")  // typo\n";
        oss << "    ✓ Right: print(\"hello\")\n";
        oss << "    ✓ Right: array.length([1,2,3])  // stdlib module function\n";
        throw std::runtime_error(oss.str());
    }

    // Phase 3.2: Track allocation for automatic GC
    trackAllocation();
}


void Interpreter::visit(ast::MemberExpr& node) {
    std::string member_name = node.getMember();

    // Build full qualified name from nested MemberExpr chain (handles module.Enum.Variant)
    {
        ast::Expr* current = &node;
        std::vector<std::string> parts;
        while (auto* member = dynamic_cast<ast::MemberExpr*>(current)) {
            parts.push_back(member->getMember());
            current = member->getObject();
        }
        if (auto* id = dynamic_cast<ast::IdentifierExpr*>(current)) {
            parts.push_back(id->getName());
        }
        if (parts.size() >= 3) {
            std::reverse(parts.begin(), parts.end());
            std::string full_qualified;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) full_qualified += ".";
                full_qualified += parts[i];
            }
            if (current_env_->has(full_qualified)) {
                result_ = current_env_->get(full_qualified);
                return;
            }
        }
    }

    // Phase 2.4.3: Check for enum member access first (EnumName.VariantName)
    // If the object is a simple identifier, check if EnumName.VariantName exists
    auto* id_expr = dynamic_cast<ast::IdentifierExpr*>(node.getObject());
    if (id_expr) {
        std::string qualified_name = id_expr->getName() + "." + member_name;
        if (current_env_->has(qualified_name)) {
            result_ = current_env_->get(qualified_name);
            return;
        }
    }

    auto obj = eval(*node.getObject());

    // Optional chaining: if object is null and ?. was used, return null
    if (node.isOptional()) {
        if (!obj || std::holds_alternative<std::monostate>(obj->data)) {
            result_ = std::make_shared<Value>();  // null
            return;
        }
    }

    // Handle struct member access
    if (std::holds_alternative<std::shared_ptr<StructValue>>(obj->data)) {
        auto struct_val = std::get<std::shared_ptr<StructValue>>(obj->data);
        result_ = struct_val->getField(member_name);
        return;
    }

    // Check if object is a block
    if (auto* block_ptr = std::get_if<std::shared_ptr<BlockValue>>(&obj->data)) {
        auto& block = *block_ptr;

        // Phase 7: Executor-based member access
        auto* executor = block->getExecutor();
        if (executor) {
            // For blocks with executors, create a member accessor
            // Build member path (support chaining like obj.member1.member2)
            std::string full_member_path = block->member_path.empty()
                ? member_name
                : block->member_path + "." + member_name;

            // Create new BlockValue representing the member accessor
            // Copy executor reference (borrowed) or owned_executor (owned)
            std::shared_ptr<BlockValue> member_block;

            if (block->owned_executor_) {
                // Can't share owned executor - must be borrowed for member access
                // Store pointer to original block's executor
                member_block = std::make_shared<BlockValue>(
                    block->metadata,
                    block->code,
                    block->owned_executor_.get()
                );
            } else {
                // Borrowed executor - can share
                member_block = std::make_shared<BlockValue>(
                    block->metadata,
                    block->code,
                    block->executor_
                );
            }

            member_block->member_path = full_member_path;

            result_ = std::make_shared<Value>(member_block);
            LOG_DEBUG("[INFO] Created member accessor: {} ({})\n",
                      full_member_path, block->metadata.language);
            return;
        }

        // Fallback: Legacy Python handling for blocks without executor
        if (block->metadata.language == "python") {
#ifdef NAAB_HAS_PYTHON
            // Execute the block code using exec() to handle multi-line properly
            std::string exec_code = "exec('''" + block->code + "''')";
            PyRun_SimpleString(exec_code.c_str());

            // Build member path
            std::string full_member_path = block->member_path.empty()
                ? member_name
                : block->member_path + "." + member_name;

            // Create new BlockValue representing the member
            auto member_block = std::make_shared<BlockValue>(
                block->metadata,
                block->code,
                block->python_namespace,
                full_member_path
            );

            result_ = std::make_shared<Value>(member_block);
            LOG_DEBUG("[INFO] Created member accessor (legacy Python): {}\n", full_member_path);
#else
            throw std::runtime_error("Python support required for member access");
#endif
            return;
        } else {
            throw std::runtime_error("Member access not supported for " +
                                   block->metadata.language + " blocks without executor");
        }
    }

    // Check if object is a Python object (for method chaining)
    if (auto* py_obj_ptr = std::get_if<std::shared_ptr<PythonObjectValue>>(&obj->data)) {
        auto& py_obj = *py_obj_ptr;

#ifdef NAAB_HAS_PYTHON
        fmt::print("[MEMBER] Accessing .{} on Python object\n", member_name);

        // Get the member attribute from the Python object
        PyObject* py_member = PyObject_GetAttrString(py_obj->obj, member_name.c_str());

        if (py_member != nullptr) {
            // Wrap the member in a new PythonObjectValue
            auto member_obj = std::make_shared<PythonObjectValue>(py_member);
            result_ = std::make_shared<Value>(member_obj);
            Py_DECREF(py_member);  // PythonObjectValue has its own reference
            LOG_DEBUG("[INFO] Accessed Python object member: {}\n", member_name);
        } else {
            PyErr_Print();
            throw std::runtime_error("Python object has no attribute: " + member_name);
        }
#else
        throw std::runtime_error("Python support required for Python object member access");
#endif
        return;
    }

    // Check if object is a dictionary (for module imports)
    if (auto* dict_ptr = std::get_if<std::unordered_map<std::string, std::shared_ptr<Value>>>(&obj->data)) {
        auto it = dict_ptr->find(member_name);
        if (it != dict_ptr->end()) {
            result_ = it->second;
            return;
        }

        std::ostringstream oss;
        oss << "Name error: Member not found in module\n\n";
        oss << "  Member: " << member_name << "\n";

        if (dict_ptr->empty()) {
            oss << "  Module has no exported members\n";
        } else {
            oss << "  Available members: ";
            size_t count = 0;
            for (const auto& pair : *dict_ptr) {
                if (count > 0) oss << ", ";
                oss << pair.first;
                if (++count >= 10) {
                    oss << "...";
                    break;
                }
            }
            oss << "\n";
        }

        oss << "\n  Help:\n";
        oss << "  - Check spelling of member name\n";
        oss << "  - Verify the member is exported\n";
        oss << "  - Member names are case-sensitive\n\n";
        oss << "  Example:\n";
        oss << "    import mymodule\n";
        oss << "    ✗ Wrong: mymodule.MyFunc()  // case mismatch\n";
        oss << "    ✓ Right: mymodule.myFunc()\n";
        throw std::runtime_error(oss.str());
    }

    // Check if object is a stdlib module marker
    if (auto* str_ptr = std::get_if<std::string>(&obj->data)) {
        std::string marker = *str_ptr;

        // Check if it's a stdlib module marker
        if (marker.substr(0, 18) == "__stdlib_module__:") {
            std::string module_alias = marker.substr(18);

            // Look up the module
            auto it = imported_modules_.find(module_alias);
            if (it == imported_modules_.end()) {
                std::ostringstream oss;
                oss << "Import error: Module not found\n\n";
                oss << "  Module: " << module_alias << "\n\n";
                oss << "  Help:\n";
                oss << "  - Check if module is imported at top of file\n";
                oss << "  - Verify import statement: import " << module_alias << "\n";
                oss << "  - For stdlib: array, string, math, file, env, time, etc.\n\n";
                oss << "  Example:\n";
                oss << "    import array  // add at top of file\n";
                oss << "    let arr = [1, 2, 3]\n";
                oss << "    array.push(arr, 4)\n";
                throw std::runtime_error(oss.str());
            }

            auto module = it->second;

            // ISS-034 FIX: Check if this is a constant (zero-argument function like PI, E)
            // If so, invoke it immediately instead of creating a marker
            // This prevents constants from returning __stdlib_call__ markers
            static const std::unordered_set<std::string> math_constants = {"PI", "E", "pi", "e"};
            if (module_alias == "math" && math_constants.count(member_name) > 0) {
                // Invoke the constant immediately with no arguments
                std::vector<std::shared_ptr<Value>> no_args;
                result_ = module->call(member_name, no_args);
                return;
            }

            // Create a marker for the function call
            // Format: __stdlib_call__:module_alias:function_name
            std::string func_marker = "__stdlib_call__:" + module_alias + ":" + member_name;
            result_ = std::make_shared<Value>(func_marker);
            return;
        }

        // Phase 4.0: Check if it's a module marker (Rust-style use statements)
        if (marker.substr(0, 11) == "__module__:") {
            std::string module_path = marker.substr(11);

            // Look up the module in the registry
            modules::NaabModule* module = module_registry_->getModule(module_path);
            if (!module) {
                throw std::runtime_error("Module not found: " + module_path);
            }

            // Get module environment and look up member
            auto module_env = module->getEnvironment();
            if (!module_env) {
                throw std::runtime_error("Module not executed: " + module_path);
            }

            if (!module_env->has(member_name)) {
                throw std::runtime_error(
                    fmt::format("Module '{}' has no member '{}'\n\n", module_path, member_name) +
                    "  Help:\n"
                    "  - Did you forget 'export' on the function?\n"
                    "  - Only exported functions are visible from other files:\n"
                    "    export function " + member_name + "() { ... }\n\n"
                    "  - Check for typos in the member name\n"
                );
            }

            result_ = module_env->get(member_name);
            return;
        }
    }

    // Member access on other types — type-specific helpful errors
    std::ostringstream oss;
    std::string type_name = getTypeName(obj);

    if (type_name == "array") {
        oss << "Type error: Arrays don't support dot notation\n\n";
        oss << "  Tried to access: array." << member_name << "\n\n";
        oss << "  Help: Use the array module for array operations:\n";
        if (member_name == "length" || member_name == "size" || member_name == "count") {
            oss << "    ✗ Wrong: my_array.length\n";
            oss << "    ✓ Right: len(my_array)             // built-in\n";
            oss << "    ✓ Right: array.length(my_array)    // module function\n";
        } else if (member_name == "push" || member_name == "append" || member_name == "add") {
            oss << "    ✗ Wrong: my_array.push(item)\n";
            oss << "    ✓ Right: array.push(my_array, item)\n";
        } else if (member_name == "pop") {
            oss << "    ✗ Wrong: my_array.pop()\n";
            oss << "    ✓ Right: array.pop(my_array)\n";
        } else if (member_name == "map" || member_name == "filter" || member_name == "reduce") {
            oss << "    ✗ Wrong: my_array." << member_name << "(fn)\n";
            oss << "    ✓ Right: array." << member_name << "_fn(my_array, fn)\n";
        } else if (member_name == "sort") {
            oss << "    ✗ Wrong: my_array.sort()\n";
            oss << "    ✓ Right: array.sort(my_array)\n";
        } else if (member_name == "reverse") {
            oss << "    ✗ Wrong: my_array.reverse()\n";
            oss << "    ✓ Right: array.reverse(my_array)\n";
        } else {
            oss << "    ✗ Wrong: my_array." << member_name << "(...)\n";
            oss << "    ✓ Right: array." << member_name << "(my_array, ...)\n";
        }
    } else if (type_name == "string") {
        oss << "Type error: Strings don't support dot notation\n\n";
        oss << "  Tried to access: string." << member_name << "\n\n";
        oss << "  Help: Use the string module for string operations:\n";
        if (member_name == "length" || member_name == "size") {
            oss << "    ✗ Wrong: my_string.length\n";
            oss << "    ✓ Right: len(my_string)             // built-in\n";
            oss << "    ✓ Right: string.length(my_string)  // module function\n";
        } else if (member_name == "upper" || member_name == "toUpperCase" || member_name == "toUpper") {
            oss << "    ✗ Wrong: my_string." << member_name << "()\n";
            oss << "    ✓ Right: string.upper(my_string)\n";
        } else if (member_name == "lower" || member_name == "toLowerCase" || member_name == "toLower") {
            oss << "    ✗ Wrong: my_string." << member_name << "()\n";
            oss << "    ✓ Right: string.lower(my_string)\n";
        } else if (member_name == "trim") {
            oss << "    ✗ Wrong: my_string.trim()\n";
            oss << "    ✓ Right: string.trim(my_string)\n";
        } else if (member_name == "split") {
            oss << "    ✗ Wrong: my_string.split(delim)\n";
            oss << "    ✓ Right: string.split(my_string, delim)\n";
        } else {
            oss << "    ✗ Wrong: my_string." << member_name << "(...)\n";
            oss << "    ✓ Right: string." << member_name << "(my_string, ...)\n";
        }
    } else if (type_name == "dict") {
        oss << "Type error: Dictionaries don't support dot notation for data access\n\n";
        oss << "  Tried to access: dict." << member_name << "\n\n";
        oss << "  Help: Use bracket notation for dict values:\n";
        oss << "    ✗ Wrong: my_dict." << member_name << "\n";
        oss << "    ✓ Right: my_dict[\"" << member_name << "\"]\n\n";
        oss << "  For iterating keys: for key in my_dict.keys() { }\n";
    } else {
        oss << "Type error: Member access not supported\n\n";
        oss << "  Tried to access: " << type_name << "." << member_name << "\n";
        oss << "  Supported types: struct, dict (for modules), block\n\n";
        oss << "  Help:\n";
        oss << "  - Structs support dot notation: obj.field\n";
        oss << "  - Dictionaries use bracket notation: dict[\"key\"]\n";
        oss << "  - Modules support member access: module.function()\n";
    }
    throw std::runtime_error(oss.str());
}


} // namespace interpreter
} // namespace naab
