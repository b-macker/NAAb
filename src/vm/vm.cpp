#include "naab/vm.h"
#include "naab/compiler.h"
#include "naab/debugger.h"
#include "naab/governance.h"
#include "naab/interpreter.h"
#include "../interpreter/cycle_detector.h"  // V-RT-008: GC for VM
#include "naab/profiler.h"
#include "naab/json_result_parser.h"
#include "naab/language_registry.h"
#include "naab/module_resolver.h"
#include "naab/stdlib.h"
#include "naab/stdlib_new_modules.h"
#include "naab/sandbox.h"
#include "naab/error_helpers.h"
#ifndef _WIN32
#include "naab/js_executor_adapter.h"
#include "naab/cpp_executor_adapter.h"
#endif
#include "naab/resource_limits.h"
#include "naab/limits.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <chrono>
#include <unordered_set>

namespace naab {
namespace vm {

// ============================================================================
// Chunk
// ============================================================================

int Chunk::addConstant(interpreter::NaabVal value) {
    constants.push_back(std::move(value));
    return static_cast<int>(constants.size()) - 1;
}

void Chunk::emit(uint32_t instruction, int line) {
    code.push_back(instruction);
    // Run-length encode line info: only add if line changed
    if (lines.empty() || lines.back().line != line) {
        lines.push_back({static_cast<int>(code.size()) - 1, line});
    }
}

int Chunk::emitJump(OpCode op, int line) {
    // Emit jump with placeholder arg (will be patched)
    emit(encodeWide(op, 0xFFFFFF), line);
    return static_cast<int>(code.size()) - 1;
}

void Chunk::patchJump(int offset) {
    // Calculate jump distance: from instruction AFTER the jump to current position
    int jump = static_cast<int>(code.size()) - offset - 1;
    if (jump > 0xFFFFFF) {
        // This shouldn't happen for reasonable programs
        throw std::runtime_error("Jump offset too large");
    }
    // Preserve opcode, replace arg
    uint32_t op_bits = code[static_cast<size_t>(offset)] & 0xFF000000;
    code[static_cast<size_t>(offset)] = op_bits | (static_cast<uint32_t>(jump) & 0x00FFFFFF);
}

int Chunk::getLine(int offset) const {
    // Binary search through run-length encoded lines
    int result = 0;
    for (const auto& info : lines) {
        if (info.offset <= offset) {
            result = info.line;
        } else {
            break;
        }
    }
    return result;
}

// ============================================================================
// VM
// ============================================================================

VM::VM()
    : stack_(std::make_unique<interpreter::NaabVal[]>(STACK_MAX))
    , stack_top_(stack_.get())
    , taint_stack_(std::make_unique<bool[]>(STACK_MAX))
    , taint_top_(taint_stack_.get())
    , frames_(std::make_unique<CallFrame[]>(FRAMES_MAX))
    , gc_detector_(std::make_unique<interpreter::CycleDetector>())  // V-RT-008
{
    std::memset(taint_stack_.get(), 0, STACK_MAX * sizeof(bool));
}

VM::~VM() {
    // shared_ptr handles cleanup — just break the linked list to avoid deep recursion
    open_upvalues_.reset();
}

interpreter::NaabVal VM::execute(CompiledFunction* main_fn) {
    // Set up the initial call frame
    frame_count_ = 0;
    stack_top_ = stack_.get();
    taint_top_ = taint_stack_.get();
    run_depth_ = 0;

    // Wrap main function in a closure for uniform handling
    main_closure_ = std::make_shared<VMClosure>(main_fn, std::vector<std::shared_ptr<ObjUpvalue>>{});

    // Push the main function as first stack slot
    push(interpreter::NaabVal::makeNull());  // Placeholder for function slot

    CallFrame* frame = &frames_[frame_count_++];
    frame->function = main_fn;
    frame->closure = main_closure_.get();
    frame->ip = main_fn->chunk.code.data();
    frame->slots = stack_.get();
    frame->handler_count = 0;
    frame->source_file = main_fn->source_file;

    // Register built-in functions as globals
    static const char* builtins[] = {
        "print", "println", "len", "type", "typeof", "range",
        "int", "float", "string", "bool", "gc_collect", "__slice"
    };
    for (auto name : builtins) {
        globals_[name] = interpreter::NaabVal::makeString(
            std::string("__builtin__:") + name);
    }

    // Register stdlib prelude modules (same as tree-walker's auto-import)
    // Skip names that conflict with builtins (string is both a module and a function)
    static const char* prelude_modules[] = {
        "array", "dict", "io", "file", "debug", "bolo", "env",
        "math", "json", "http", "crypto", "time", "os"
    };
    for (auto mod : prelude_modules) {
        // Don't overwrite block imports injected via setGlobal()
        auto it = globals_.find(mod);
        if (it != globals_.end() && it->second.isBlock()) continue;
        globals_[mod] = interpreter::NaabVal::makeString(
            std::string("__stdlib_module__:") + mod);
    }

    // Wire up function evaluator for array.map_fn/filter_fn/reduce_fn callbacks
    if (stdlib_) {
        auto array_module = stdlib_->getModule("array");
        if (array_module) {
            auto* array_mod = dynamic_cast<stdlib::ArrayModule*>(array_module.get());
            if (array_mod) {
                array_mod->setFunctionEvaluator(
                    [this](interpreter::NaabVal fn,
                           const std::vector<interpreter::NaabVal>& args) -> interpreter::NaabVal {
                        return this->callNaabFunction(fn, args);
                    }
                );
            }
        }
    }

    // Pre-execution governance: validate scope patterns, require main block
    if (governance_ && governance_->isActive() && module_loading_depth_ == 0) {
        // Collect function names from globals for scope validation
        std::vector<std::string> func_names;
        for (auto& [gname, gval] : globals_) {
            if (gval.isVMClosure()) {
                func_names.push_back(gname);
            }
        }
        governance_->validateScopePatterns(func_names);

        // Check require_main_block — the compiler always has a main function
        // but if the source has no main{} block, the main function will be empty
        if (governance_->requiresMainBlock()) {
            // If the main function has only OP_RETURN_NULL (empty), there's no main block
            if (main_fn->chunk.code.size() <= 1) {
                auto level = governance_->getRules().main_block_level;
                std::string msg = "Governance error: Program requires a main { } block ["
                    + std::string(level == governance::EnforcementLevel::HARD ? "HARD-MANDATORY" :
                                  level == governance::EnforcementLevel::SOFT ? "SOFT-MANDATORY" : "ADVISORY")
                    + "]\n\n"
                    "  Rule (govern.json): requirements.main_block\n\n"
                    "  Help:\n"
                    "  - All programs must have a main { } block when governance requires it\n";
                if (level == governance::EnforcementLevel::HARD) {
                    throw std::runtime_error(msg);
                } else if (level == governance::EnforcementLevel::SOFT) {
                    if (!governance_->isOverrideEnabled()) {
                        throw std::runtime_error(msg + "\n  This is a soft-mandatory rule. Adjust govern.json to change enforcement.\n");
                    }
                    fprintf(stderr, "[governance] OVERRIDE requirements.main_block: %s\n", msg.c_str());
                } else {
                    fprintf(stderr, "[governance] WARNING requirements.main_block: Program has no main block\n");
                }
            }
        }
    }

    auto result = run();

    // Post-execution governance: flush advisories, print summary, write reports
    if (governance_ && governance_->isActive() && module_loading_depth_ == 0) {
        governance_->flushGroupedAdvisories();
        governance_->runGovernanceVoice();

        if (governance_verbose_) {
            std::string summary = governance_->formatSummary();
            if (!summary.empty()) {
                fprintf(stderr, "\n%s\n", summary.c_str());
            }
        } else {
            std::string oneline = governance_->formatSummaryOneLine();
            if (!oneline.empty()) {
                fprintf(stderr, "\n%s\n", oneline.c_str());
            }
        }
        governance_->writeReports();
    }

    return result;
}

interpreter::NaabVal VM::callBuiltinFunction(const std::string& name, int argc,
                                             interpreter::NaabVal* args) {
    if (name == "print" || name == "println") {
        for (int i = 0; i < argc; i++) {
            if (i > 0) std::cout << " ";
            std::cout << args[i].toString();
        }
        std::cout << std::endl;
        return interpreter::NaabVal::makeNull();
    }
    if (name == "len") {
        if (argc != 1) runtimeError("len() requires exactly 1 argument");
        if (args[0].isString())
            return interpreter::NaabVal::makeInt(static_cast<int>(args[0].asString().length()));
        if (args[0].isList())
            return interpreter::NaabVal::makeInt(static_cast<int>(args[0].asList().size()));
        if (args[0].isDict())
            return interpreter::NaabVal::makeInt(static_cast<int>(args[0].asDict().size()));
        return interpreter::NaabVal::makeInt(0);
    }
    if (name == "type") {
        if (argc != 1) runtimeError("type() requires exactly 1 argument");
        return interpreter::NaabVal::makeString(args[0].getTypeName());
    }
    if (name == "typeof") {
        if (argc != 1) runtimeError("typeof() requires exactly 1 argument");
        return interpreter::NaabVal::makeString(args[0].getTypeName());
    }
    if (name == "int") {
        if (argc != 1) runtimeError("int() takes exactly 1 argument");
        if (args[0].isString()) {
            try {
                double d = std::stod(args[0].asString());
                return interpreter::NaabVal::makeInt(static_cast<int>(d));
            } catch (...) {
                runtimeError("int() cannot convert \"%s\" to int", args[0].asString().c_str());
            }
        }
        return interpreter::NaabVal::makeInt(args[0].toInt());
    }
    if (name == "float") {
        if (argc != 1) runtimeError("float() takes exactly 1 argument");
        if (args[0].isString()) {
            try {
                return interpreter::NaabVal::makeDouble(std::stod(args[0].asString()));
            } catch (...) {
                runtimeError("float() cannot convert \"%s\" to float", args[0].asString().c_str());
            }
        }
        return interpreter::NaabVal::makeDouble(args[0].toFloat());
    }
    if (name == "string") {
        if (argc != 1) runtimeError("string() takes exactly 1 argument");
        return interpreter::NaabVal::makeString(args[0].toString());
    }
    if (name == "bool") {
        if (argc != 1) runtimeError("bool() takes exactly 1 argument");
        return interpreter::NaabVal::makeBool(args[0].toBool());
    }
    if (name == "range") {
        if (argc < 1 || argc > 3) runtimeError("range() takes 1-3 arguments");
        int start = 0, end = 0, step = 1;
        if (argc == 1) {
            end = args[0].isInt() ? args[0].asInt() : static_cast<int>(args[0].asDouble());
        } else {
            start = args[0].isInt() ? args[0].asInt() : static_cast<int>(args[0].asDouble());
            end = args[1].isInt() ? args[1].asInt() : static_cast<int>(args[1].asDouble());
        }
        if (argc == 3) {
            step = args[2].isInt() ? args[2].asInt() : static_cast<int>(args[2].asDouble());
        }
        if (step == 0) runtimeError("range() step cannot be zero");
        std::vector<interpreter::NaabVal> result;
        if (step > 0) {
            for (int i = start; i < end; i += step)
                result.push_back(interpreter::NaabVal::makeInt(i));
        } else {
            for (int i = start; i > end; i += step)
                result.push_back(interpreter::NaabVal::makeInt(i));
        }
        return interpreter::NaabVal::makeList(std::move(result));
    }
    if (name == "__slice") {
        if (argc != 3) runtimeError("__slice() takes exactly 3 arguments");
        int start = args[1].isInt() ? args[1].asInt() : static_cast<int>(args[1].asDouble());
        int end_val = args[2].isInt() ? args[2].asInt() : static_cast<int>(args[2].asDouble());
        if (args[0].isList()) {
            auto& list = args[0].asList();
            int len = static_cast<int>(list.size());
            if (start < 0) start += len;
            if (end_val < 0) end_val += len;
            if (start < 0) start = 0;
            if (end_val > len) end_val = len;
            std::vector<interpreter::NaabVal> result;
            for (int i = start; i < end_val; i++)
                result.push_back(list[i]);
            return interpreter::NaabVal::makeList(std::move(result));
        }
        if (args[0].isString()) {
            const std::string& s = args[0].asString();
            int len = static_cast<int>(s.size());
            if (start < 0) start += len;
            if (end_val < 0) end_val += len;
            if (start < 0) start = 0;
            if (end_val > len) end_val = len;
            return interpreter::NaabVal::makeString(s.substr(start, end_val - start));
        }
        runtimeError("__slice() requires a list or string");
    }
    if (name == "gc_collect") {
        // V-RT-008: collect VM stack values and globals as GC roots
        if (gc_detector_ && gc_threshold_ > 0) {
            std::vector<interpreter::NaabVal> roots;
            roots.reserve(static_cast<size_t>(stack_top_ - stack_.get()) + globals_.size());
            for (auto* p = stack_.get(); p < stack_top_; ++p) roots.push_back(*p);
            for (auto& [k, v] : globals_) roots.push_back(v);
            gc_detector_->detectAndCollect(nullptr, roots, {});
        }
        return interpreter::NaabVal::makeNull();
    }
    // Targeted hints for commonly misused function names
    if (name == "parseInt" || name == "parseFloat" || name == "Number") {
        runtimeError("Unknown function '%s'\n\n"
                     "  Use NAAb type conversion functions:\n"
                     "    int(\"42\")     // instead of parseInt(\"42\")\n"
                     "    float(\"3.14\") // instead of parseFloat(\"3.14\")\n", name.c_str());
    }
    if (name == "toString" || name == "str") {
        runtimeError("Unknown function '%s'\n\n"
                     "  Use NAAb type conversion:\n"
                     "    string(42)    // instead of toString(42)\n", name.c_str());
    }
    if (name == "sleep") {
        runtimeError("Unknown function 'sleep'\n\n"
                     "  'sleep' is in the time module, not a global function:\n"
                     "    import time\n"
                     "    time.sleep(1.0)  // sleep for 1 second\n");
    }
    if (name == "exit") {
        runtimeError("Unknown function 'exit'\n\n"
                     "  NAAb has no exit() function.\n"
                     "  To stop: return from functions, or let main block end.\n");
    }
    if (name == "error") {
        runtimeError("Unknown function 'error'\n\n"
                     "  To print errors: print(\"ERROR: something went wrong\")\n"
                     "  To throw an error: throw \"something went wrong\"\n");
    }
    if (name == "keys" || name == "values") {
        runtimeError("Unknown function '%s'\n\n"
                     "  '%s' is a method on dicts, not a global function:\n"
                     "    myDict.%s()  // correct\n", name.c_str(), name.c_str(), name.c_str());
    }
    if (name == "push" || name == "append" || name == "pop") {
        runtimeError("Unknown function '%s'\n\n"
                     "  '%s' is a method on arrays, not a global function:\n"
                     "    myArray.%s(item)  // correct\n", name.c_str(), name.c_str(), name.c_str());
    }
    if (name == "forEach") {
        runtimeError("Unknown function 'forEach'\n\n"
                     "  NAAb uses for-in loops instead of forEach:\n"
                     "    for item in myArray { print(item) }\n");
    }
    if (name == "map" || name == "filter" || name == "reduce") {
        runtimeError("Unknown function '%s'\n\n"
                     "  NAAb has array.%s_fn (NOT a global function):\n"
                     "    array.map_fn(arr, fn(x) { return x * 2 })\n"
                     "    array.filter_fn(arr, fn(x) { return x > 5 })\n"
                     "    array.reduce_fn(arr, fn(acc, x) { return acc + x }, 0)\n", name.c_str(), name.c_str());
    }
    runtimeError("Unknown built-in function '%s'\n\n"
                 "  Common builtins: print, len, type, typeof, int, float, string, bool\n"
                 "  For stdlib functions, use module.function() (e.g., array.push())\n", name.c_str());
}

// ============================================================================
// Polyglot Helpers (standalone — no Interpreter dependency)
// ============================================================================

static std::string serializeForLanguage(const interpreter::NaabVal& nval, const std::string& language, int depth = 0) {
    // V-VM-002: prevent unbounded recursion on deeply-nested structures
    if (depth > 64) {
        throw std::runtime_error(
            "Serialization error: nested structure exceeds maximum depth (64).\n\n"
            "  Polyglot blocks cannot serialize structures nested more than 64 levels.\n"
            "  Flatten the structure before passing it to a polyglot block.\n"
        );
    }
    if (nval.isNull()) {
        if (language == "python") return "None";
        if (language == "go") return "nil";
        if (language == "ruby") return "nil";
        if (language == "nim") return "\"\"";
        if (language == "shell" || language == "sh" || language == "bash") return "\"\"";
        return "null";
    }
    if (nval.isInt()) return std::to_string(nval.asInt());
    if (nval.isDouble()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", nval.asDouble());
        return std::string(buf);
    }
    if (nval.isString()) {
        const auto& str = nval.asString();
        if (language == "shell" || language == "sh" || language == "bash") {
            std::string escaped;
            for (char c : str) {
                if (c == '\'') escaped += "'\\''";
                else escaped += c;
            }
            return "'" + escaped + "'";
        }
        std::string escaped;
        for (char c : str) {
            if (c == '"') escaped += "\\\"";
            else if (c == '\\') escaped += "\\\\";
            else if (c == '\n') escaped += "\\n";
            else if (c == '\r') escaped += "\\r";
            else if (c == '\t') escaped += "\\t";
            else escaped += c;
        }
        return "\"" + escaped + "\"";
    }
    if (nval.isBool()) {
        bool b = nval.asBool();
        if (language == "python") return b ? "True" : "False";
        if (language == "shell" || language == "sh" || language == "bash") return b ? "1" : "0";
        return b ? "true" : "false";
    }
    if (nval.isList()) {
        const auto& list = nval.asListConst();
        std::string prefix = "[", suffix = "]";
        if (language == "php") { prefix = "array("; suffix = ")"; }
        else if (language == "rust") { prefix = "vec!["; suffix = "]"; }
        else if (language == "go") { prefix = "[]interface{}{"; suffix = "}"; }
        else if (language == "nim") { prefix = "@["; suffix = "]"; }
        std::string result = prefix;
        for (size_t i = 0; i < list.size(); i++) {
            if (i > 0) result += ", ";
            result += serializeForLanguage(list[i], language, depth + 1);
        }
        result += suffix;
        return result;
    }
    if (nval.isDict()) {
        const auto& dict = nval.asDictConst();
        auto escapeKey = [](const std::string& k) -> std::string {
            std::string escaped;
            for (char c : k) {
                if (c == '"') escaped += "\\\"";
                else if (c == '\\') escaped += "\\\\";
                else if (c == '\n') escaped += "\\n";
                else escaped += c;
            }
            return escaped;
        };
        std::string result = "{";
        bool first = true;
        for (const auto& [key, val] : dict) {
            if (!first) result += ", ";
            first = false;
            if (language == "ruby")
                result += "\"" + escapeKey(key) + "\" => " + serializeForLanguage(val, language, depth + 1);
            else if (language == "go")
                result += "\"" + escapeKey(key) + "\": " + serializeForLanguage(val, language, depth + 1);
            else
                result += "\"" + escapeKey(key) + "\": " + serializeForLanguage(val, language, depth + 1);
        }
        result += "}";
        if (language == "go") { result = "map[string]interface{}" + result; }
        else if (language == "php") { result = "array(" + result.substr(1, result.size()-2) + ")"; }
        return result;
    }
    return "null";
}

static std::string buildVarDeclarations(const std::string& language,
    const std::vector<std::string>& var_names,
    const std::vector<interpreter::NaabVal>& var_vals) {
    std::string decls;
    for (size_t i = 0; i < var_names.size() && i < var_vals.size(); i++) {
        std::string serialized = serializeForLanguage(var_vals[i], language);
        if (language == "python") {
            decls += var_names[i] + " = " + serialized + "\n";
        } else if (language == "shell" || language == "sh" || language == "bash") {
            decls += "export " + var_names[i] + "=" + serialized + "\n";
        } else if (language == "javascript" || language == "js") {
            decls += "const " + var_names[i] + " = " + serialized + ";\n";
        } else if (language == "go") {
            bool is_complex = var_vals[i].isList() || var_vals[i].isDict();
            decls += (is_complex ? "var " : "const ") + var_names[i] + " = " + serialized + "\n";
        } else if (language == "rust") {
            decls += "let " + var_names[i] + " = " + serialized + ";\n";
        } else if (language == "nim") {
            decls += "var " + var_names[i] + " = " + serialized + "\n";
        } else if (language == "cpp" || language == "c++") {
            decls += "const auto " + var_names[i] + " = " + serialized + ";\n";
        } else if (language == "ruby") {
            decls += var_names[i] + " = " + serialized + "\n";
        } else {
            decls += "let " + var_names[i] + " = " + serialized + ";\n";
        }
    }
    return decls;
}

static std::string stripCommonIndent(const std::string& raw_code) {
    std::vector<std::string> lines;
    std::istringstream stream(raw_code);
    std::string line;
    while (std::getline(stream, line)) lines.push_back(line);

    size_t min_indent = std::string::npos;
    for (auto& l : lines) {
        if (l.empty() || l.find_first_not_of(" \t") == std::string::npos) continue;
        size_t indent = l.find_first_not_of(" \t");
        if (indent < min_indent) min_indent = indent;
    }

    std::string result;
    for (auto& l : lines) {
        if (l.empty() || l.find_first_not_of(" \t") == std::string::npos)
            result += "\n";
        else if (min_indent != std::string::npos && l.length() > min_indent)
            result += l.substr(min_indent) + "\n";
        else
            result += l + "\n";
    }
    return result;
}

static std::string injectAfterHeaders(const std::string& decls, const std::string& code,
                                       const std::string& language) {
    if (language == "go") {
        // Go: inject after import block or package line
        auto pos = code.find("import");
        if (pos != std::string::npos) {
            // Find end of import block
            auto paren = code.find('(', pos);
            if (paren != std::string::npos) {
                auto close = code.find(')', paren);
                if (close != std::string::npos) {
                    auto nl = code.find('\n', close);
                    if (nl != std::string::npos) {
                        return code.substr(0, nl + 1) + decls + code.substr(nl + 1);
                    }
                }
            }
            auto nl = code.find('\n', pos);
            if (nl != std::string::npos)
                return code.substr(0, nl + 1) + decls + code.substr(nl + 1);
        }
        auto pkg = code.find("package ");
        if (pkg != std::string::npos) {
            auto nl = code.find('\n', pkg);
            if (nl != std::string::npos)
                return code.substr(0, nl + 1) + decls + code.substr(nl + 1);
        }
    }
    return decls + code;
}

interpreter::NaabVal VM::run() {
    CallFrame* frame = &frames_[frame_count_ - 1];

#define READ_INSTR() (*frame->ip++)
#define CURRENT_CHUNK() (frame->function->chunk)
#define READ_CONSTANT(idx) \
    (static_cast<size_t>(idx) < CURRENT_CHUNK().constants.size() \
        ? CURRENT_CHUNK().constants[idx] \
        : (runtimeError("Invalid constant index %d (pool size %d)", \
            static_cast<int>(idx), static_cast<int>(CURRENT_CHUNK().constants.size())), \
           interpreter::NaabVal::makeNull()))

    // Debugger check — extracted to lambda so computed goto DISPATCH macro stays small
    auto debugCheck = [&]() {
        if (debugger_ && debugger_->isActive()) {
            int ip_offset = static_cast<int>(frame->ip - CURRENT_CHUNK().code.data()) - 1;
            int line = CURRENT_CHUNK().getLine(ip_offset);
            if (line != last_debug_line_) {
                last_debug_line_ = line;
                std::string loc = (frame->source_file.empty() ? current_file_ : frame->source_file)
                    + ":" + std::to_string(line);
                debugger_->shouldBreak(loc);
            }
        }
    };

// Computed goto dispatch for GCC/Clang — eliminates branch prediction overhead
#if defined(__GNUC__) || defined(__clang__)
#define USE_COMPUTED_GOTO
#endif

#ifdef USE_COMPUTED_GOTO
    // Dispatch table: 80 entries, one per opcode. Unimplemented opcodes → vm_default.
    static void* dispatch_table[] = {
        &&vm_OP_CONST, &&vm_OP_NULL, &&vm_OP_TRUE, &&vm_OP_FALSE,        // 0-3
        &&vm_OP_POP, &&vm_OP_POPN, &&vm_OP_DUP, &&vm_OP_SWAP,           // 4-7
        &&vm_OP_ADD, &&vm_OP_SUB, &&vm_OP_MUL, &&vm_OP_DIV,             // 8-11
        &&vm_OP_MOD, &&vm_OP_NEG, &&vm_OP_NOT, &&vm_OP_EQ,              // 12-15
        &&vm_OP_NE, &&vm_OP_LT, &&vm_OP_LE, &&vm_OP_GT,                // 16-19
        &&vm_OP_GE, &&vm_OP_IN,                                          // 20-21
        &&vm_OP_GET_LOCAL, &&vm_OP_SET_LOCAL,                             // 22-23
        &&vm_OP_GET_UPVALUE, &&vm_OP_SET_UPVALUE,                        // 24-25
        &&vm_OP_GET_GLOBAL, &&vm_OP_SET_GLOBAL,                          // 26-27
        &&vm_OP_DEFINE_GLOBAL, &&vm_default,                              // 28-29 (29=GET_QUALIFIED stub)
        &&vm_OP_JUMP, &&vm_OP_JUMP_BACK,                                 // 30-31
        &&vm_OP_JUMP_IF_FALSE, &&vm_OP_JUMP_IF_TRUE, &&vm_OP_JUMP_IF_NULL, // 32-34
        &&vm_OP_CALL, &&vm_OP_CALL_METHOD,                               // 35-36
        &&vm_OP_RETURN, &&vm_OP_RETURN_NULL,                              // 37-38
        &&vm_OP_CLOSURE, &&vm_OP_CLOSE_UPVALUE,                          // 39-40
        &&vm_OP_LIST, &&vm_OP_DICT,                                       // 41-42
        &&vm_OP_GET_INDEX, &&vm_OP_SET_INDEX,                             // 43-44
        &&vm_OP_GET_MEMBER, &&vm_OP_SET_MEMBER,                           // 45-46
        &&vm_OP_RANGE,                                                     // 47
        &&vm_OP_STRUCT_NEW, &&vm_OP_STRUCT_INIT_FIELD, &&vm_OP_COPY_VALUE, // 48-50
        &&vm_OP_GET_ITER, &&vm_OP_ITER_NEXT,                              // 51-52
        &&vm_STUB_NOP, &&vm_STUB_NOP,                                      // 53-54 (DESTRUCTURE — compiler uses GET_INDEX)
        &&vm_OP_IMPORT, &&vm_OP_IMPORT_NAME,                              // 55-56
        &&vm_OP_IMPORT_WILDCARD, &&vm_OP_EXPORT_NAME,                     // 57-58
        &&vm_OP_TRY_BEGIN, &&vm_OP_TRY_END, &&vm_OP_THROW,               // 59-61
        &&vm_STUB_NOP, &&vm_STUB_NOP, &&vm_STUB_NOP,                      // 62-64 (CATCH/FINALLY — compiler inlines)
        &&vm_OP_POLYGLOT,                                                  // 65
        &&vm_STUB_NOP, &&vm_OP_GOV_TAINT_MARK, &&vm_STUB_NOP, &&vm_OP_GOV_TAINT_CHECK_ASSIGN, // 66-69: GOV_CHECK_FUNC(stub/TODO), TAINT_MARK, TAINT_CLEAR(stub), TAINT_CHECK_ASSIGN
        &&vm_STUB_NOP, &&vm_STUB_NOP, &&vm_STUB_NOP, &&vm_STUB_NOP,      // 70-73 (GOV — handled at AST level)
        &&vm_OP_YIELD, &&vm_OP_AWAIT,                                     // 74-75
        &&vm_STUB_NOP, &&vm_STUB_NOP,                                     // 76-77 (MAKE_GEN/ASYNC — reserved)
        &&vm_OP_RUNTIME_START, &&vm_STUB_NOP,                              // 78-79 (RUNTIME_START, EXEC via CALL_METHOD)
    };

    #define VM_CASE(opcode) vm_##opcode
    #define VM_NEXT() do { \
        if (naab::security::ResourceLimiter::isTimeoutTriggered()) { \
            runtimeError("Execution timeout: script exceeded the configured time limit"); \
        } \
        instruction = READ_INSTR(); \
        op = decodeOp(instruction); \
        arg = decodeArg(instruction); \
        debugCheck(); \
        goto *dispatch_table[static_cast<uint8_t>(op)]; \
    } while(0)
    #define VM_DEFAULT vm_default
#else
    #define VM_CASE(opcode) case OpCode::opcode
    #define VM_NEXT() break
    #define VM_DEFAULT default
#endif

    uint32_t instruction;
    OpCode op;
    uint32_t arg;

    for (;;) {
      try {
#ifdef USE_COMPUTED_GOTO
        VM_NEXT();  // Initial dispatch (and re-dispatch after exception handling)
#else
        if (naab::security::ResourceLimiter::isTimeoutTriggered()) {
            runtimeError("Execution timeout: script exceeded the configured time limit");
        }
        instruction = READ_INSTR();
        op = decodeOp(instruction);
        arg = decodeArg(instruction);
        debugCheck();

        switch (op) {
#endif
            VM_CASE(OP_CONST): {
                push(READ_CONSTANT(arg));
            }
                VM_NEXT();

            VM_CASE(OP_NULL): {
                push(interpreter::NaabVal::makeNull());
            }
                VM_NEXT();

            VM_CASE(OP_TRUE): {
                push(interpreter::NaabVal::makeBool(true));
            }
                VM_NEXT();

            VM_CASE(OP_FALSE): {
                push(interpreter::NaabVal::makeBool(false));
            }
                VM_NEXT();

            VM_CASE(OP_POP): {
                pop();
            }
                VM_NEXT();

            VM_CASE(OP_POPN): {
                uint8_t n = decodeA(instruction);
                // Release each value properly (prevents handle leaks)
                for (uint8_t i = 0; i < n; i++) {
                    pop();
                }
            }
                VM_NEXT();

            VM_CASE(OP_DUP): {
                bool t = peekTaint(0);
                push(peek(0));
                peekTaint(0) = t;
            }
                VM_NEXT();

            VM_CASE(OP_SWAP): {
                bool ta = peekTaint(0), tb = peekTaint(1);
                interpreter::NaabVal a = pop();
                interpreter::NaabVal b = pop();
                push(std::move(a));
                push(std::move(b));
                peekTaint(0) = tb;
                peekTaint(1) = ta;
            }
                VM_NEXT();

            VM_CASE(OP_COPY_VALUE): {
                // Deep copy TOS if it's a list or dict (value semantics)
                interpreter::NaabVal& top = peek(0);
                if (top.isList()) {
                    const auto& list = top.asListConst();
                    std::vector<interpreter::NaabVal> new_list(list.begin(), list.end());
                    top = interpreter::NaabVal::makeList(std::move(new_list));
                } else if (top.isDict()) {
                    const auto& dict = top.asDictConst();
                    std::unordered_map<std::string, interpreter::NaabVal> new_dict(dict.begin(), dict.end());
                    top = interpreter::NaabVal::makeDict(std::move(new_dict));
                }
                // Other types: no-op (ints, strings, bools are value types)
            }
                VM_NEXT();

            // Arithmetic
            VM_CASE(OP_ADD): {
                // Fast path: int + int — direct bits_ manipulation, no pop/push overhead
                interpreter::NaabVal& b_ref = *(stack_top_ - 1);
                interpreter::NaabVal& a_ref = *(stack_top_ - 2);
                if ((a_ref.bits_ & interpreter::NaabVal::TAG_MASK) == interpreter::NaabVal::TAG_INT &&
                    (b_ref.bits_ & interpreter::NaabVal::TAG_MASK) == interpreter::NaabVal::TAG_INT) {
                    int32_t ai = static_cast<int32_t>(static_cast<uint32_t>(a_ref.bits_));
                    int32_t bi = static_cast<int32_t>(static_cast<uint32_t>(b_ref.bits_));
                    // Overflow check
                    if ((bi > 0 && ai > INT32_MAX - bi) || (bi < 0 && ai < INT32_MIN - bi)) {
                        runtimeError("Math error: Integer overflow in addition\n\n"
                            "  Expression: %d + %d\n"
                            "  INT_MAX: %d, INT_MIN: %d\n\n"
                            "  Help:\n"
                            "  - Use float for larger numbers: %d.0 + %d.0\n",
                            ai, bi, INT32_MAX, INT32_MIN, ai, bi);
                    }
                    uint32_t ru; int32_t ri = ai + bi;
                    std::memcpy(&ru, &ri, sizeof(ru));
                    bool ta_f = *(taint_top_ - 2), tb_f = *(taint_top_ - 1);
                    bool combined_t = ta_f || tb_f;
                    // Lineage: inherit from tainted operand to result slot
                    if (combined_t && !taint_lineage_map_.empty()) {
                        size_t a_off = static_cast<size_t>((stack_top_ - 2) - stack_.get());
                        size_t b_off = static_cast<size_t>((stack_top_ - 1) - stack_.get());
                        auto lit = taint_lineage_map_.find(ta_f ? a_off : b_off);
                        if (lit == taint_lineage_map_.end()) lit = taint_lineage_map_.find(tb_f ? b_off : a_off);
                        if (lit != taint_lineage_map_.end()) taint_lineage_map_[a_off] = lit->second;
                        taint_lineage_map_.erase(b_off);
                    }
                    a_ref.bits_ = interpreter::NaabVal::TAG_INT | static_cast<uint64_t>(ru);
                    stack_top_--;
                    taint_top_--;
                    *(taint_top_ - 1) = combined_t;
                } else {
                bool tb = peekTaint(0), ta = peekTaint(1);
                // Capture offsets before pop for lineage propagation
                size_t a_off_slow = static_cast<size_t>((stack_top_ - 2) - stack_.get());
                size_t b_off_slow = static_cast<size_t>((stack_top_ - 1) - stack_.get());
                interpreter::NaabVal b = pop();
                interpreter::NaabVal a = pop();
                if (a.isString() || b.isString()) {
                    // String concatenation with auto-coercion (must be before double check)
                    push(interpreter::NaabVal::makeString(a.toString() + b.toString()));
                    allocation_count_++;
                } else if (a.isInt() && b.isInt()) {
                    int32_t av = static_cast<int32_t>(a.asInt()), bv = static_cast<int32_t>(b.asInt());
                    // Overflow check
                    if ((bv > 0 && av > INT32_MAX - bv) ||
                        (bv < 0 && av < INT32_MIN - bv)) {
                        runtimeError("Math error: Integer overflow in addition\n\n"
                            "  Expression: %d + %d\n"
                            "  Help: Use float for larger numbers: %d.0 + %d.0\n",
                            av, bv, av, bv);
                    }
                    push(interpreter::NaabVal::makeInt(av + bv));
                } else if (a.isDouble() || b.isDouble()) {
                    double av = a.isDouble() ? a.asDouble() : static_cast<double>(a.asInt());
                    double bv = b.isDouble() ? b.asDouble() : static_cast<double>(b.asInt());
                    push(interpreter::NaabVal::makeDouble(av + bv));
                } else if (a.isList() && b.isList()) {
                    auto result = a.asListConst();
                    auto& blist = b.asListConst();
                    result.insert(result.end(), blist.begin(), blist.end());
                    push(interpreter::NaabVal::makeList(std::move(result)));
                } else {
                    runtimeError("Type error: Cannot add %s and %s",
                                 a.getTypeName().c_str(), b.getTypeName().c_str());
                }
                peekTaint(0) = ta || tb;
                // Lineage: inherit from tainted operand to result
                if ((ta || tb) && !taint_lineage_map_.empty()) {
                    size_t res_off = static_cast<size_t>((stack_top_ - 1) - stack_.get());
                    auto lit = taint_lineage_map_.find(ta ? a_off_slow : b_off_slow);
                    if (lit == taint_lineage_map_.end()) lit = taint_lineage_map_.find(tb ? b_off_slow : a_off_slow);
                    if (lit != taint_lineage_map_.end()) taint_lineage_map_[res_off] = lit->second;
                    taint_lineage_map_.erase(b_off_slow);
                    if (a_off_slow != res_off) taint_lineage_map_.erase(a_off_slow);
                }
                } // end slow path else
            }
                VM_NEXT();

            VM_CASE(OP_SUB): {
                // Fast path: int - int
                interpreter::NaabVal& b_ref = *(stack_top_ - 1);
                interpreter::NaabVal& a_ref = *(stack_top_ - 2);
                if ((a_ref.bits_ & interpreter::NaabVal::TAG_MASK) == interpreter::NaabVal::TAG_INT &&
                    (b_ref.bits_ & interpreter::NaabVal::TAG_MASK) == interpreter::NaabVal::TAG_INT) {
                    int32_t ai = static_cast<int32_t>(static_cast<uint32_t>(a_ref.bits_));
                    int32_t bi = static_cast<int32_t>(static_cast<uint32_t>(b_ref.bits_));
                    // Overflow check
                    if ((bi < 0 && ai > INT32_MAX + bi) || (bi > 0 && ai < INT32_MIN + bi)) {
                        runtimeError("Math error: Integer overflow in subtraction\n\n"
                            "  Expression: %d - %d\n"
                            "  Help: Use float for larger numbers: %d.0 - %d.0\n",
                            ai, bi, ai, bi);
                    }
                    uint32_t ru; int32_t ri = ai - bi;
                    std::memcpy(&ru, &ri, sizeof(ru));
                    bool combined_t = *(taint_top_ - 2) || *(taint_top_ - 1);
                    a_ref.bits_ = interpreter::NaabVal::TAG_INT | static_cast<uint64_t>(ru);
                    stack_top_--;
                    taint_top_--;
                    *(taint_top_ - 1) = combined_t;
                } else {
                bool tb = peekTaint(0), ta = peekTaint(1);
                interpreter::NaabVal b = pop();
                interpreter::NaabVal a = pop();
                if (a.isInt() && b.isInt()) {
                    int32_t av = static_cast<int32_t>(a.asInt()), bv = static_cast<int32_t>(b.asInt());
                    if ((bv < 0 && av > INT32_MAX + bv) || (bv > 0 && av < INT32_MIN + bv)) {
                        runtimeError("Math error: Integer overflow in subtraction\n\n"
                            "  Expression: %d - %d\n"
                            "  Help: Use float for larger numbers: %d.0 - %d.0\n",
                            av, bv, av, bv);
                    }
                    push(interpreter::NaabVal::makeInt(av - bv));
                } else if (a.isDouble() || b.isDouble()) {
                    double av = a.isDouble() ? a.asDouble() : static_cast<double>(a.asInt());
                    double bv = b.isDouble() ? b.asDouble() : static_cast<double>(b.asInt());
                    push(interpreter::NaabVal::makeDouble(av - bv));
                } else {
                    runtimeError("Type error: Cannot subtract %s from %s",
                                 b.getTypeName().c_str(), a.getTypeName().c_str());
                }
                peekTaint(0) = ta || tb;
                } // end slow path
            }
                VM_NEXT();

            VM_CASE(OP_MUL): {
                bool tb = peekTaint(0), ta = peekTaint(1);
                interpreter::NaabVal b = pop();
                interpreter::NaabVal a = pop();
                if (a.isInt() && b.isInt()) {
                    int32_t av = static_cast<int32_t>(a.asInt()), bv = static_cast<int32_t>(b.asInt());
                    // Overflow check using 64-bit multiplication
                    int64_t result64 = static_cast<int64_t>(av) * static_cast<int64_t>(bv);
                    if (result64 > INT32_MAX || result64 < INT32_MIN) {
                        runtimeError("Math error: Integer overflow in multiplication\n\n"
                            "  Expression: %d * %d\n"
                            "  Help: Use float for larger numbers: %d.0 * %d.0\n",
                            av, bv, av, bv);
                    }
                    push(interpreter::NaabVal::makeInt(static_cast<int32_t>(result64)));
                } else if (a.isDouble() || b.isDouble()) {
                    double av = a.isDouble() ? a.asDouble() : static_cast<double>(a.asInt());
                    double bv = b.isDouble() ? b.asDouble() : static_cast<double>(b.asInt());
                    push(interpreter::NaabVal::makeDouble(av * bv));
                } else if (a.isString() && b.isInt()) {
                    // String repetition: "abc" * 3 -> "abcabcabc"
                    std::string result;
                    int64_t count = b.asInt();
                    if (count > 0) {
                        const std::string& s = a.asString();
                        result.reserve(s.size() * count);
                        for (int64_t i = 0; i < count; i++) result += s;
                    }
                    push(interpreter::NaabVal::makeString(std::move(result)));
                } else if (a.isInt() && b.isString()) {
                    // 3 * "abc" -> "abcabcabc"
                    std::string result;
                    int64_t count = a.asInt();
                    if (count > 0) {
                        const std::string& s = b.asString();
                        result.reserve(s.size() * count);
                        for (int64_t i = 0; i < count; i++) result += s;
                    }
                    push(interpreter::NaabVal::makeString(std::move(result)));
                } else {
                    runtimeError("Type error: Cannot multiply %s and %s",
                                 a.getTypeName().c_str(), b.getTypeName().c_str());
                }
                peekTaint(0) = ta || tb;
            }
                VM_NEXT();

            VM_CASE(OP_DIV): {
                bool tb = peekTaint(0), ta = peekTaint(1);
                interpreter::NaabVal b = pop();
                interpreter::NaabVal a = pop();
                if (a.isInt() && b.isInt()) {
                    if (b.asInt() == 0) runtimeError("Division by zero");
                    push(interpreter::NaabVal::makeInt(a.asInt() / b.asInt()));
                } else if (a.isDouble() || b.isDouble()) {
                    double bv = b.isDouble() ? b.asDouble() : static_cast<double>(b.asInt());
                    if (bv == 0.0) runtimeError("Division by zero");
                    double av = a.isDouble() ? a.asDouble() : static_cast<double>(a.asInt());
                    push(interpreter::NaabVal::makeDouble(av / bv));
                } else {
                    runtimeError("Type error: Cannot divide %s by %s",
                                 a.getTypeName().c_str(), b.getTypeName().c_str());
                }
                peekTaint(0) = ta || tb;
            }
                VM_NEXT();

            VM_CASE(OP_MOD): {
                bool tb = peekTaint(0), ta = peekTaint(1);
                interpreter::NaabVal b = pop();
                interpreter::NaabVal a = pop();
                if (a.isInt() && b.isInt()) {
                    if (b.asInt() == 0) runtimeError("Modulo by zero");
                    push(interpreter::NaabVal::makeInt(a.asInt() % b.asInt()));
                } else {
                    runtimeError("Type error: Modulo requires integers");
                }
                peekTaint(0) = ta || tb;
            }
                VM_NEXT();

            VM_CASE(OP_NEG): {
                bool t = peekTaint(0);
                interpreter::NaabVal a = pop();
                if (a.isInt()) {
                    int32_t av = static_cast<int32_t>(a.asInt());
                    if (av == INT32_MIN) {
                        runtimeError("Math error: Integer overflow in negation\n\n"
                            "  Expression: -(%d)\n"
                            "  -INT_MIN cannot be represented as a 32-bit integer\n"
                            "  Help: Use float: -(%d.0)\n",
                            av, av);
                    }
                    push(interpreter::NaabVal::makeInt(-av));
                } else if (a.isDouble()) {
                    push(interpreter::NaabVal::makeDouble(-a.asDouble()));
                } else {
                    runtimeError("Type error: Cannot negate %s", a.getTypeName().c_str());
                }
                peekTaint(0) = t;
            }
                VM_NEXT();

            VM_CASE(OP_NOT): {
                bool t = peekTaint(0);
                interpreter::NaabVal a = pop();
                push(interpreter::NaabVal::makeBool(!a.toBool()));
                peekTaint(0) = t;
            }
                VM_NEXT();

            // Comparison
            VM_CASE(OP_EQ): {
                bool tb = peekTaint(0), ta = peekTaint(1);
                interpreter::NaabVal b = pop();
                interpreter::NaabVal a = pop();
                bool eq = false;
                if (a.isNull() && b.isNull()) {
                    eq = true;
                } else if (a.isNull() || b.isNull()) {
                    eq = false;
                } else if (a.isBool() && b.isBool()) {
                    eq = a.toBool() == b.toBool();
                } else if ((a.isInt() || a.isDouble()) && (b.isInt() || b.isDouble())) {
                    eq = a.toFloat() == b.toFloat();
                } else if (a.isString() && b.isString()) {
                    eq = a.toString() == b.toString();
                }
                push(interpreter::NaabVal::makeBool(eq));
                peekTaint(0) = ta || tb;
            }
                VM_NEXT();

            VM_CASE(OP_NE): {
                bool tb = peekTaint(0), ta = peekTaint(1);
                interpreter::NaabVal b = pop();
                interpreter::NaabVal a = pop();
                bool eq = false;
                if (a.isNull() && b.isNull()) {
                    eq = true;
                } else if (a.isNull() || b.isNull()) {
                    eq = false;
                } else if (a.isBool() && b.isBool()) {
                    eq = a.toBool() == b.toBool();
                } else if ((a.isInt() || a.isDouble()) && (b.isInt() || b.isDouble())) {
                    eq = a.toFloat() == b.toFloat();
                } else if (a.isString() && b.isString()) {
                    eq = a.toString() == b.toString();
                }
                push(interpreter::NaabVal::makeBool(!eq));
                peekTaint(0) = ta || tb;
            }
                VM_NEXT();

            VM_CASE(OP_LT): {
                // Fast path: int < int
                interpreter::NaabVal& b_ref = *(stack_top_ - 1);
                interpreter::NaabVal& a_ref = *(stack_top_ - 2);
                if ((a_ref.bits_ & interpreter::NaabVal::TAG_MASK) == interpreter::NaabVal::TAG_INT &&
                    (b_ref.bits_ & interpreter::NaabVal::TAG_MASK) == interpreter::NaabVal::TAG_INT) {
                    int32_t ai = static_cast<int32_t>(static_cast<uint32_t>(a_ref.bits_));
                    int32_t bi = static_cast<int32_t>(static_cast<uint32_t>(b_ref.bits_));
                    bool combined_t = *(taint_top_ - 2) || *(taint_top_ - 1);
                    a_ref.bits_ = interpreter::NaabVal::TAG_BOOL | (ai < bi ? 1ULL : 0ULL);
                    stack_top_--;
                    taint_top_--;
                    *(taint_top_ - 1) = combined_t;
                } else {
                bool tb = peekTaint(0), ta = peekTaint(1);
                interpreter::NaabVal b = pop();
                interpreter::NaabVal a = pop();
                if (a.isInt() && b.isInt()) {
                    push(interpreter::NaabVal::makeBool(a.asInt() < b.asInt()));
                } else if ((a.isInt() || a.isDouble()) && (b.isInt() || b.isDouble())) {
                    double av = a.isDouble() ? a.asDouble() : static_cast<double>(a.asInt());
                    double bv = b.isDouble() ? b.asDouble() : static_cast<double>(b.asInt());
                    push(interpreter::NaabVal::makeBool(av < bv));
                } else if (a.isString() && b.isString()) {
                    push(interpreter::NaabVal::makeBool(a.asString() < b.asString()));
                } else {
                    runtimeError("Type error: Cannot compare %s < %s",
                                 a.getTypeName().c_str(), b.getTypeName().c_str());
                }
                peekTaint(0) = ta || tb;
                } // end slow path
            }
                VM_NEXT();

            VM_CASE(OP_LE): {
                bool tb = peekTaint(0), ta = peekTaint(1);
                interpreter::NaabVal b = pop();
                interpreter::NaabVal a = pop();
                if (a.isInt() && b.isInt()) {
                    push(interpreter::NaabVal::makeBool(a.asInt() <= b.asInt()));
                } else if ((a.isInt() || a.isDouble()) && (b.isInt() || b.isDouble())) {
                    double av = a.isDouble() ? a.asDouble() : static_cast<double>(a.asInt());
                    double bv = b.isDouble() ? b.asDouble() : static_cast<double>(b.asInt());
                    push(interpreter::NaabVal::makeBool(av <= bv));
                } else if (a.isString() && b.isString()) {
                    push(interpreter::NaabVal::makeBool(a.asString() <= b.asString()));
                } else {
                    runtimeError("Type error: Cannot compare %s <= %s",
                                 a.getTypeName().c_str(), b.getTypeName().c_str());
                }
                peekTaint(0) = ta || tb;
            }
                VM_NEXT();

            VM_CASE(OP_GT): {
                bool tb = peekTaint(0), ta = peekTaint(1);
                interpreter::NaabVal b = pop();
                interpreter::NaabVal a = pop();
                if (a.isInt() && b.isInt()) {
                    push(interpreter::NaabVal::makeBool(a.asInt() > b.asInt()));
                } else if ((a.isInt() || a.isDouble()) && (b.isInt() || b.isDouble())) {
                    double av = a.isDouble() ? a.asDouble() : static_cast<double>(a.asInt());
                    double bv = b.isDouble() ? b.asDouble() : static_cast<double>(b.asInt());
                    push(interpreter::NaabVal::makeBool(av > bv));
                } else if (a.isString() && b.isString()) {
                    push(interpreter::NaabVal::makeBool(a.asString() > b.asString()));
                } else {
                    runtimeError("Type error: Cannot compare %s > %s",
                                 a.getTypeName().c_str(), b.getTypeName().c_str());
                }
                peekTaint(0) = ta || tb;
            }
                VM_NEXT();

            VM_CASE(OP_GE): {
                bool tb = peekTaint(0), ta = peekTaint(1);
                interpreter::NaabVal b = pop();
                interpreter::NaabVal a = pop();
                if (a.isInt() && b.isInt()) {
                    push(interpreter::NaabVal::makeBool(a.asInt() >= b.asInt()));
                } else if ((a.isInt() || a.isDouble()) && (b.isInt() || b.isDouble())) {
                    double av = a.isDouble() ? a.asDouble() : static_cast<double>(a.asInt());
                    double bv = b.isDouble() ? b.asDouble() : static_cast<double>(b.asInt());
                    push(interpreter::NaabVal::makeBool(av >= bv));
                } else if (a.isString() && b.isString()) {
                    push(interpreter::NaabVal::makeBool(a.asString() >= b.asString()));
                } else {
                    runtimeError("Type error: Cannot compare %s >= %s",
                                 a.getTypeName().c_str(), b.getTypeName().c_str());
                }
                peekTaint(0) = ta || tb;
            }
                VM_NEXT();

            VM_CASE(OP_IN): {
                bool tb = peekTaint(0), ta = peekTaint(1);
                interpreter::NaabVal container = pop();
                interpreter::NaabVal item = pop();
                if (container.isList()) {
                    bool found = false;
                    for (auto& elem : container.asList()) {
                        if (elem.toString() == item.toString()) { found = true; break; }
                    }
                    push(interpreter::NaabVal::makeBool(found));
                } else if (container.isDict()) {
                    push(interpreter::NaabVal::makeBool(container.asDict().count(item.toString()) > 0));
                } else if (container.isString()) {
                    push(interpreter::NaabVal::makeBool(
                        container.asString().find(item.toString()) != std::string::npos));
                } else {
                    runtimeError("Type error: 'in' requires a list, dict, or string on the right side");
                }
                peekTaint(0) = ta || tb;
            }
                VM_NEXT();

            // Variable access
            VM_CASE(OP_GET_LOCAL): {
                // Local's taint is at the same offset in the taint stack
                size_t slot_offset = static_cast<size_t>(frame->slots - stack_.get()) + arg;
                push(frame->slots[arg]);
                peekTaint(0) = taint_stack_[slot_offset];
                // Lineage: copy from local slot to TOS
                if (peekTaint(0) && !taint_lineage_map_.empty()) {
                    auto lit = taint_lineage_map_.find(slot_offset);
                    if (lit != taint_lineage_map_.end()) {
                        size_t tos = static_cast<size_t>((stack_top_ - 1) - stack_.get());
                        taint_lineage_map_[tos] = lit->second;
                    }
                }
            }
                VM_NEXT();

            VM_CASE(OP_SET_LOCAL): {
                frame->slots[arg] = peek(0);
                // Copy taint to local's position
                size_t slot_offset = static_cast<size_t>(frame->slots - stack_.get()) + arg;
                taint_stack_[slot_offset] = peekTaint(0);
                // Copy lineage from TOS to local slot
                if (peekTaint(0) && !taint_lineage_map_.empty()) {
                    size_t tos_offset = static_cast<size_t>((stack_top_ - 1) - stack_.get());
                    auto lit = taint_lineage_map_.find(tos_offset);
                    if (lit != taint_lineage_map_.end()) {
                        taint_lineage_map_[slot_offset] = lit->second;
                    }
                } else {
                    taint_lineage_map_.erase(slot_offset);
                }
            }
                VM_NEXT();

            VM_CASE(OP_GET_UPVALUE): {
                auto& upvalue = frame->closure->upvalues[arg];
                if (upvalue->is_open) {
                    push(*upvalue->location);
                    // Get taint from the upvalue's stack position
                    size_t uv_offset = static_cast<size_t>(upvalue->location - stack_.get());
                    peekTaint(0) = taint_stack_[uv_offset];
                    // Copy lineage from upvalue's stack slot to TOS
                    if (peekTaint(0) && !taint_lineage_map_.empty()) {
                        auto lit = taint_lineage_map_.find(uv_offset);
                        if (lit != taint_lineage_map_.end()) {
                            size_t tos = static_cast<size_t>((stack_top_ - 1) - stack_.get());
                            taint_lineage_map_[tos] = lit->second;
                        }
                    }
                } else {
                    push(upvalue->closed);
                    peekTaint(0) = upvalue->tainted;
                    // Copy lineage from closed upvalue to TOS
                    if (upvalue->tainted && !upvalue->lineage_func.empty()) {
                        size_t tos = static_cast<size_t>((stack_top_ - 1) - stack_.get());
                        taint_lineage_map_[tos] = {upvalue->lineage_func, upvalue->lineage_arg, "", 0};
                    }
                }
            }
                VM_NEXT();

            VM_CASE(OP_SET_UPVALUE): {
                auto& upvalue = frame->closure->upvalues[arg];
                if (upvalue->is_open) {
                    *upvalue->location = peek(0);
                    size_t uv_offset = static_cast<size_t>(upvalue->location - stack_.get());
                    taint_stack_[uv_offset] = peekTaint(0);
                    // Copy lineage from TOS to upvalue's stack slot
                    if (peekTaint(0) && !taint_lineage_map_.empty()) {
                        size_t tos_offset = static_cast<size_t>((stack_top_ - 1) - stack_.get());
                        auto lit = taint_lineage_map_.find(tos_offset);
                        if (lit != taint_lineage_map_.end()) {
                            taint_lineage_map_[uv_offset] = lit->second;
                        }
                    } else {
                        taint_lineage_map_.erase(uv_offset);
                    }
                } else {
                    upvalue->closed = peek(0);
                    upvalue->tainted = peekTaint(0);
                    // Store lineage in closed upvalue
                    if (peekTaint(0) && !taint_lineage_map_.empty()) {
                        size_t tos_offset = static_cast<size_t>((stack_top_ - 1) - stack_.get());
                        auto lit = taint_lineage_map_.find(tos_offset);
                        if (lit != taint_lineage_map_.end()) {
                            upvalue->lineage_func = lit->second.source_func;
                            upvalue->lineage_arg = lit->second.source_arg;
                        }
                    } else {
                        upvalue->lineage_func.clear();
                        upvalue->lineage_arg.clear();
                    }
                }
            }
                VM_NEXT();

            VM_CASE(OP_GET_GLOBAL): {
                const std::string& name = READ_CONSTANT(arg).asString();
                auto it = globals_.find(name);
                if (it == globals_.end()) {
                    std::string helper = getVariableHelper(name);
                    if (helper.empty()) {
                        runtimeError("Undefined variable '%s'", name.c_str());
                    } else {
                        runtimeError("Undefined variable '%s'\n%s", name.c_str(), helper.c_str());
                    }
                }
                push(it->second);
                if (governance_ && governance_->isActive()) {
                    peekTaint(0) = governance_->isTainted(name);
                }
            }
                VM_NEXT();

            VM_CASE(OP_SET_GLOBAL): {
                const std::string& name = READ_CONSTANT(arg).asString();
                auto it = globals_.find(name);
                if (it == globals_.end()) {
                    std::string helper = getVariableHelper(name);
                    if (helper.empty()) {
                        runtimeError("Undefined variable '%s'", name.c_str());
                    } else {
                        runtimeError("Undefined variable '%s'\n%s", name.c_str(), helper.c_str());
                    }
                }
                it->second = peek(0);
                if (governance_ && governance_->isActive()) {
                    if (peekTaint(0)) {
                        int gov_line = CURRENT_CHUNK().getLine(
                            static_cast<int>(frame->ip - CURRENT_CHUNK().code.data()) - 2);
                        std::string origin_func = governance_->lastTaintSource();
                        std::string origin_arg;
                        // Prefer lineage map (more precise) over lastTaintSource
                        if (!taint_lineage_map_.empty()) {
                            size_t tos = static_cast<size_t>((stack_top_ - 1) - stack_.get());
                            auto lit = taint_lineage_map_.find(tos);
                            if (lit != taint_lineage_map_.end()) {
                                origin_func = lit->second.source_func;
                                origin_arg = lit->second.source_arg;
                            }
                        }
                        governance_->markTainted(name, origin_func, origin_arg,
                            current_file_, gov_line);
                    } else {
                        governance_->clearTaint(name);
                    }
                }
            }
                VM_NEXT();

            VM_CASE(OP_DEFINE_GLOBAL): {
                const std::string& name = READ_CONSTANT(arg).asString();
                bool t = peekTaint(0);
                // Capture lineage before pop destroys the stack slot
                std::string origin_func_dg;
                std::string origin_arg_dg;
                if (t && !taint_lineage_map_.empty()) {
                    size_t tos = static_cast<size_t>((stack_top_ - 1) - stack_.get());
                    auto lit = taint_lineage_map_.find(tos);
                    if (lit != taint_lineage_map_.end()) {
                        origin_func_dg = lit->second.source_func;
                        origin_arg_dg = lit->second.source_arg;
                    }
                }
                globals_[name] = pop();
                if (governance_ && governance_->isActive()) {
                    if (t) {
                        int gov_line = CURRENT_CHUNK().getLine(
                            static_cast<int>(frame->ip - CURRENT_CHUNK().code.data()) - 2);
                        if (origin_func_dg.empty()) origin_func_dg = governance_->lastTaintSource();
                        governance_->markTainted(name, origin_func_dg, origin_arg_dg,
                            current_file_, gov_line);
                    } else {
                        governance_->clearTaint(name);
                    }
                }
            }
                VM_NEXT();

            // Control flow
            VM_CASE(OP_JUMP): {
                frame->ip += arg;
            }
                VM_NEXT();

            VM_CASE(OP_JUMP_BACK): {
                frame->ip -= arg;
                // Governance: check loop iteration limits
                if (governance_ && governance_->isActive()) {
                    uint32_t* target = frame->ip;
                    int& count = loop_iter_counts_[target];
                    count++;
                    std::string err = governance_->checkLoopIterations(static_cast<size_t>(count));
                    if (!err.empty()) runtimeError("%s", err.c_str());
                }
                // V-RT-008: periodic GC at loop back-edges to reclaim cycle garbage
                if (gc_detector_ && gc_threshold_ > 0 &&
                    ++gc_instruction_count_ >= gc_threshold_) {
                    gc_instruction_count_ = 0;
                    std::vector<interpreter::NaabVal> roots;
                    roots.reserve(static_cast<size_t>(stack_top_ - stack_.get()) + globals_.size());
                    for (auto* p = stack_.get(); p < stack_top_; ++p) roots.push_back(*p);
                    for (auto& [k, v] : globals_) roots.push_back(v);
                    gc_detector_->detectAndCollect(nullptr, roots, {});
                }
            }
                VM_NEXT();

            VM_CASE(OP_JUMP_IF_FALSE): {
                // Peek — does NOT pop. Caller must emit OP_POP if needed.
                if (!peek(0).toBool()) {
                    frame->ip += arg;
                }
            }
                VM_NEXT();

            VM_CASE(OP_JUMP_IF_TRUE): {
                // Peek — does NOT pop. Caller must emit OP_POP if needed.
                if (peek(0).toBool()) {
                    frame->ip += arg;
                }
            }
                VM_NEXT();

            VM_CASE(OP_JUMP_IF_NULL): {
                bool t = peekTaint(0);
                interpreter::NaabVal val = pop();
                if (val.isNull()) {
                    frame->ip += arg;
                } else {
                    // Not null — push it back (it was consumed for the check)
                    push(std::move(val));
                    peekTaint(0) = t;
                }
            }
                VM_NEXT();

            VM_CASE(OP_RETURN): {
                // Debugger/Profiler: end of function
                if (debugger_ && debugger_->isActive()) debugger_->popFrame();
                if (profile_) {
                    profiling::Profiler::instance().endFunction(
                        frame->function->name.empty() ? "<anonymous>" : frame->function->name);
                }

                // Governance: track return taint + check function contract
                bool return_tainted = peekTaint(0);
                if (governance_ && governance_->isActive()) {
                    governance_->setLastReturnTainted(return_tainted);

                    // Function output contract check
                    const std::string& fn_name = frame->function->name;
                    if (!fn_name.empty()) {
                        std::string result_str = peek(0).toString();
                        std::string result_type = peek(0).getTypeName();
                        int line = CURRENT_CHUNK().getLine(
                            static_cast<int>(frame->ip - CURRENT_CHUNK().code.data()) - 1);
                        std::string err = governance_->checkFunctionContract(
                            fn_name, result_str, result_type, line,
                            frame->source_file.empty() ? current_file_ : frame->source_file);
                        if (!err.empty()) {
                            governance_->logContractCheck(fn_name, "FAIL", err,
                                frame->source_file.empty() ? current_file_ : frame->source_file, line);
                            runtimeError("%s", err.c_str());
                        }
                    }
                }

                interpreter::NaabVal result = pop();
                closeUpvalues(frame->slots);
                // Pop exception handlers belonging to this frame (e.g., return inside try block)
                {
                    int fi = frame_count_ - 1;
                    while (!exception_handlers_.empty() &&
                           exception_handlers_.back().frame_index == fi) {
                        exception_handlers_.pop_back();
                    }
                }
                frame_count_--;
                if (frame_count_ <= stop_frame_count_) {
                    // Clear stale slots to release handles
                    interpreter::NaabVal* old_top = stack_top_;
                    stack_top_ = frame->slots;
                    for (auto* p = stack_top_; p < old_top; p++)
                        *p = interpreter::NaabVal();
                    // Evict stale lineage entries for reclaimed stack slots
                    if (!taint_lineage_map_.empty()) {
                        size_t new_top = static_cast<size_t>(stack_top_ - stack_.get());
                        for (auto it = taint_lineage_map_.begin(); it != taint_lineage_map_.end(); ) {
                            if (it->first >= new_top) it = taint_lineage_map_.erase(it);
                            else ++it;
                        }
                    }
                    syncTaintTop();
                    push(result);
                    // Sanitizer functions clear taint on their return value
                    if (return_tainted && governance_ && governance_->isActive()) {
                        const std::string& fn_name = frame->function->name;
                        if (!fn_name.empty() && governance_->isSanitizer(fn_name)) {
                            return_tainted = false;
                        }
                    }
                    peekTaint(0) = return_tainted;
                    // Carry lineage to the return value's new position
                    if (return_tainted && governance_ && !governance_->lastTaintSource().empty()) {
                        size_t tos = static_cast<size_t>((stack_top_ - 1) - stack_.get());
                        taint_lineage_map_[tos] = {governance_->lastTaintSource(), "",
                            current_file_, 0};
                    }
                    return result;
                }
                {
                    // Clear stale slots to release handles
                    interpreter::NaabVal* old_top = stack_top_;
                    stack_top_ = frame->slots;
                    for (auto* p = stack_top_; p < old_top; p++)
                        *p = interpreter::NaabVal();
                    // Evict stale lineage entries for reclaimed stack slots
                    if (!taint_lineage_map_.empty()) {
                        size_t new_top = static_cast<size_t>(stack_top_ - stack_.get());
                        for (auto it = taint_lineage_map_.begin(); it != taint_lineage_map_.end(); ) {
                            if (it->first >= new_top) it = taint_lineage_map_.erase(it);
                            else ++it;
                        }
                    }
                }
                syncTaintTop();
                push(std::move(result));
                // Sanitizer functions clear taint on their return value
                if (return_tainted && governance_ && governance_->isActive()) {
                    const std::string& fn_name = frame->function->name;
                    if (!fn_name.empty() && governance_->isSanitizer(fn_name)) {
                        return_tainted = false;
                    }
                }
                peekTaint(0) = return_tainted;
                // Carry lineage to the return value's new position
                if (return_tainted && governance_ && !governance_->lastTaintSource().empty()) {
                    size_t tos = static_cast<size_t>((stack_top_ - 1) - stack_.get());
                    taint_lineage_map_[tos] = {governance_->lastTaintSource(), "",
                        current_file_, 0};
                }
                frame = &frames_[frame_count_ - 1];
            }
                VM_NEXT();

            VM_CASE(OP_RETURN_NULL): {
                // Debugger/Profiler: end of function
                if (debugger_ && debugger_->isActive()) debugger_->popFrame();
                if (profile_) {
                    profiling::Profiler::instance().endFunction(
                        frame->function->name.empty() ? "<anonymous>" : frame->function->name);
                }

                // Governance: clear return taint for null returns
                if (governance_ && governance_->isActive()) {
                    governance_->setLastReturnTainted(false);
                }

                closeUpvalues(frame->slots);
                // Pop exception handlers belonging to this frame (e.g., return inside try block)
                {
                    int fi = frame_count_ - 1;
                    while (!exception_handlers_.empty() &&
                           exception_handlers_.back().frame_index == fi) {
                        exception_handlers_.pop_back();
                    }
                }
                frame_count_--;
                if (frame_count_ <= stop_frame_count_) {
                    interpreter::NaabVal* old_top = stack_top_;
                    stack_top_ = frame->slots;
                    for (auto* p = stack_top_; p < old_top; p++)
                        *p = interpreter::NaabVal();
                    syncTaintTop();
                    push(interpreter::NaabVal::makeNull());
                    return interpreter::NaabVal::makeNull();
                }
                {
                    interpreter::NaabVal* old_top = stack_top_;
                    stack_top_ = frame->slots;
                    for (auto* p = stack_top_; p < old_top; p++)
                        *p = interpreter::NaabVal();
                }
                syncTaintTop();
                push(interpreter::NaabVal::makeNull());
                frame = &frames_[frame_count_ - 1];
            }
                VM_NEXT();

            VM_CASE(OP_CALL): {
                uint8_t argc = decodeA(instruction);
                interpreter::NaabVal& callee = peek(argc);
                if (!callValue(callee, argc)) {
                    runtimeError("Value is not callable");
                }
                frame = &frames_[frame_count_ - 1];
            }
                VM_NEXT();

            VM_CASE(OP_CALL_METHOD): {
                // Packed: [name_idx:16][argc:8] in the 24-bit arg
                uint32_t name_idx = (arg >> 8) & 0xFFFF;
                uint8_t argc = static_cast<uint8_t>(arg & 0xFF);
                const std::string& method = READ_CONSTANT(name_idx).asString();

                // Object is below the args on the stack
                interpreter::NaabVal& obj = peek(argc);

                // Build full method name for governance checks (mod.method)
                std::string full_method_name;

                // Check for stdlib module marker
                if (obj.isString()) {
                    const std::string& sv = obj.asString();
                    if (sv.size() >= 18 && sv.substr(0, 18) == "__stdlib_module__:") {
                        std::string mod = sv.substr(18);
                        full_method_name = mod + "." + method;

                        // Governance: check module-level permissions
                        if (governance_) {
                            if (mod == "file") {
                                std::string fs_mode = (method == "read" || method == "read_lines"
                                                       || method == "exists" || method == "size")
                                                      ? "read" : "write";
                                std::string err = governance_->checkFilesystemAllowed(fs_mode);
                                if (!err.empty()) runtimeError("%s", err.c_str());
                                // Path-level access check (first arg = file path)
                                if (argc > 0) {
                                    interpreter::NaabVal path_arg = stack_top_[-argc];
                                    if (path_arg.isString()) {
                                        std::string perr = governance_->checkPathAccess(path_arg.asString(), fs_mode);
                                        if (!perr.empty()) runtimeError("%s", perr.c_str());
                                    }
                                }
                                // copy/move: also check destination path (second arg)
                                if ((method == "copy" || method == "move") && argc > 1) {
                                    interpreter::NaabVal dest_arg = stack_top_[-argc + 1];
                                    if (dest_arg.isString()) {
                                        std::string perr = governance_->checkPathAccess(dest_arg.asString(), "write");
                                        if (!perr.empty()) runtimeError("%s", perr.c_str());
                                    }
                                }
                            } else if (mod == "http") {
                                std::string err = governance_->checkNetworkAllowed();
                                if (!err.empty()) runtimeError("%s", err.c_str());
                            } else if (mod == "env") {
                                // Finding E fix: enforce governance dangerous-call policy on env access
                                if (governance_->isActive()) {
                                    std::string full = "env." + method;
                                    std::string err = governance_->checkDangerousCall("naab", full, 0);
                                    if (!err.empty()) runtimeError("%s", err.c_str());
                                }
                            }

                            // Generic dangerous-calls policy check for ALL stdlib modules
                            // (mirrors tree-walker call_dispatch.cpp:1366 — Finding C parity)
                            if (governance_->isActive()) {
                                std::string full_call = mod + "." + method;
                                int gov_line = CURRENT_CHUNK().getLine(
                                    static_cast<int>(frame->ip - CURRENT_CHUNK().code.data()) - 4);
                                std::string derr = governance_->checkDangerousCall("naab", full_call, gov_line);
                                if (!derr.empty()) runtimeError("%s", derr.c_str());
                            }
                        }

                        // Governance: check if this is a taint sink (e.g., env.set_var)
                        // If any argument is tainted, temporarily mark it and call checkTaintedSink
                        if (governance_ && governance_->isActive()) {
                            for (int ai = 0; ai < static_cast<int>(argc); ai++) {
                                ptrdiff_t arg_offset = (stack_top_ - argc + ai) - stack_.get();
                                if (taint_stack_[arg_offset]) {
                                    std::string arg_label = "argument " + std::to_string(ai) + " of '" + full_method_name + "()'";
                                    governance_->markTainted(arg_label);
                                    int gov_line = CURRENT_CHUNK().getLine(
                                        static_cast<int>(frame->ip - CURRENT_CHUNK().code.data()) - 4);
                                    std::string terr = governance_->checkTaintedSink(
                                        arg_label, full_method_name, current_file_, gov_line);
                                    governance_->clearTaint(arg_label);
                                    if (!terr.empty()) runtimeError("%s", terr.c_str());
                                }
                            }
                        }

                        interpreter::NaabVal* args_ptr = stack_top_ - argc;
                        // Capture first argument for lineage before call
                        std::string first_arg_str;
                        if (argc > 0 && governance_ && governance_->getRules().taint_tracking.lineage) {
                            first_arg_str = args_ptr[0].toString();
                        }
                        interpreter::NaabVal result = callStdlibMethod(mod, method, argc, args_ptr);
                        // Clear stale slots before adjusting stack pointer
                        for (int si = 0; si < argc + 1; si++)
                            *(stack_top_ - 1 - si) = interpreter::NaabVal();
                        stack_top_ -= (argc + 1);
                        syncTaintTop();
                        push(std::move(result));
                        // Governance: check taint source/sanitizer
                        if (governance_ && governance_->isActive()) {
                            if (governance_->isTaintSource(full_method_name)) {
                                peekTaint(0) = true;
                                governance_->setLastReturnTainted(true, full_method_name);
                                // VM lineage: record origin at the result's stack offset
                                if (governance_->getRules().taint_tracking.lineage) {
                                    size_t result_offset = static_cast<size_t>((stack_top_ - 1) - stack_.get());
                                    int src_line = CURRENT_CHUNK().getLine(
                                        static_cast<int>(frame->ip - CURRENT_CHUNK().code.data()) - 4);
                                    taint_lineage_map_[result_offset] = {full_method_name, first_arg_str, current_file_, src_line};
                                }
                            } else if (governance_->isSanitizer(full_method_name)) {
                                peekTaint(0) = false;
                            } else if (governance_->lastReturnWasTainted()) {
                                peekTaint(0) = true;
                                governance_->setLastReturnTainted(false);
                            }
                        }
                        goto done_call_method;
                    }
                    // Also handle __builtin__:X where X is a stdlib module name
                    // (e.g., "string" is both a builtin function and stdlib module)
                    if (sv.size() >= 12 && sv.substr(0, 12) == "__builtin__:") {
                        std::string mod = sv.substr(12);
                        if (stdlib_ && stdlib_->hasModule(mod)) {
                            full_method_name = mod + "." + method;
                            interpreter::NaabVal* args_ptr = stack_top_ - argc;
                            interpreter::NaabVal result = callStdlibMethod(mod, method, argc, args_ptr);
                            for (int si = 0; si < argc + 1; si++)
                                *(stack_top_ - 1 - si) = interpreter::NaabVal();
                            stack_top_ -= (argc + 1);
                            syncTaintTop();
                            push(std::move(result));
                            if (governance_ && governance_->isActive()) {
                                if (governance_->isTaintSource(full_method_name)) {
                                    peekTaint(0) = true;
                                } else if (governance_->isSanitizer(full_method_name)) {
                                    peekTaint(0) = false;
                                }
                            }
                            goto done_call_method;
                        }
                    }
                    // Phase 12: Persistent runtime .exec() dispatch
                    if (sv.size() >= 17 && sv.substr(0, 17) == "__NAAB_RUNTIME__:" && method == "exec") {
                        std::string runtime_name = sv.substr(17);
                        auto rit = named_runtimes_.find(runtime_name);
                        if (rit == named_runtimes_.end()) {
                            runtimeError("Runtime error: Runtime '%s' not found", runtime_name.c_str());
                        }
                        if (argc < 1) {
                            runtimeError("Runtime error: %s.exec() requires a code argument.\n\n"
                                "  Example: %s.exec(<<python\n    your code here\n  >>)\n",
                                runtime_name.c_str(), runtime_name.c_str());
                        }

                        // Get code argument (string)
                        interpreter::NaabVal code_val = peek(0);
                        std::string code;
                        if (code_val.isString()) {
                            code = code_val.asString();
                        } else {
                            // Result of an already-executed polyglot block — use directly
                            for (int si = 0; si < argc + 1; si++)
                                *(stack_top_ - 1 - si) = interpreter::NaabVal();
                            stack_top_ -= (argc + 1);
                            syncTaintTop();
                            push(std::move(code_val));
                            goto done_call_method;
                        }

                        auto& rt = rit->second;

                        // Governance checks
                        if (governance_ && governance_->isActive()) {
                            int gov_line = CURRENT_CHUNK().getLine(
                                static_cast<int>(frame->ip - CURRENT_CHUNK().code.data()) - 4);
                            std::string gov_err = governance_->checkPolyglotBlock(
                                rt.language, code, current_file_, gov_line, 0);
                            if (!gov_err.empty()) runtimeError("%s", gov_err.c_str());
                        }

                        // For subprocess-based languages, accumulate code
                        bool is_embedded = (rt.language == "python" || rt.language == "javascript" ||
                                           rt.language == "js");
                        if (!is_embedded) {
                            rt.code_buffer += code + "\n";
                            code = rt.code_buffer;
                        }

                        // Detect if code is a statement (no return value)
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
                            interpreter::NaabVal result;
                            bool is_js = (rt.language == "javascript" || rt.language == "js");
                            if (is_js) {
#ifndef _WIN32
                                auto* js_adapter = dynamic_cast<runtime::JsExecutorAdapter*>(rt.executor);
                                if (js_adapter) {
                                    if (is_statement) {
                                        js_adapter->execute(code, runtime::JsExecutionMode::BLOCK_LIBRARY);
                                        result = interpreter::NaabVal::makeNull();
                                    } else {
                                        result = js_adapter->executeWithReturn(code);
                                    }
                                } else {
                                    result = rt.executor->executeWithReturn(code);
                                }
#else
                                result = rt.executor->executeWithReturn(code);
#endif
                            } else if (is_statement) {
                                rt.executor->execute(code);
                                result = interpreter::NaabVal::makeNull();
                            } else {
                                result = rt.executor->executeWithReturn(code);
                            }
                            for (int si = 0; si < argc + 1; si++)
                                *(stack_top_ - 1 - si) = interpreter::NaabVal();
                            stack_top_ -= (argc + 1);
                            syncTaintTop();
                            push(std::move(result));
                        } catch (const std::exception& e) {
                            runtimeError("Persistent runtime '%s' error: %s",
                                runtime_name.c_str(), e.what());
                        }
                        goto done_call_method;
                    }
                }

                // Check if dict has a callable value at this key
                if (obj.isDict()) {
                    auto& dict = obj.asDict();
                    auto it = dict.find(method);
                    if (it != dict.end() && it->second.isVMClosure()) {
                        // Replace obj on stack with the function, then call
                        peek(argc) = it->second;
                        if (!callValue(it->second, argc)) {
                            runtimeError("Dict value '%s' is not callable", method.c_str());
                        }
                        frame = &frames_[frame_count_ - 1];
                        goto done_call_method;
                    }
                    // Struct method lookup: "StructName.method" in globals
                    auto sn_it = dict.find("__struct_name__");
                    if (sn_it != dict.end() && sn_it->second.isString()) {
                        std::string qualified = sn_it->second.asString() + "." + method;
                        auto git = globals_.find(qualified);
                        if (git != globals_.end() && git->second.isVMClosure()) {
                            peek(argc) = git->second;
                            if (!callValue(git->second, argc)) {
                                runtimeError("Struct method '%s' is not callable", qualified.c_str());
                            }
                            frame = &frames_[frame_count_ - 1];
                            goto done_call_method;
                        }
                    }
                }

                // Block method calls: obj is a BlockValue, dispatch to executor
                if (obj.isBlock()) {
                    auto& block = obj.asBlock();
                    auto* executor = block->getExecutor();
                    if (executor) {
#ifndef _WIN32
                        // Restore C++ block ID for multi-block executor sharing
                        if (!block->cpp_block_id.empty()) {
                            auto* cpp_exec = dynamic_cast<runtime::CppExecutorAdapter*>(executor);
                            if (cpp_exec) cpp_exec->setCurrentBlockId(block->cpp_block_id);
                        }
#endif
                        std::vector<interpreter::NaabVal> arg_vec;
                        interpreter::NaabVal* args_ptr = stack_top_ - argc;
                        for (int i = 0; i < static_cast<int>(argc); i++) {
                            arg_vec.push_back(args_ptr[i]);
                        }
                        interpreter::NaabVal result = executor->callFunction(method, arg_vec);
                        for (int si = 0; si < argc + 1; si++)
                            *(stack_top_ - 1 - si) = interpreter::NaabVal();
                        stack_top_ -= (argc + 1);
                        syncTaintTop();
                        push(std::move(result));
                        goto done_call_method;
                    }
                }

                // Built-in methods on arrays, dicts, strings
                {
                    bool obj_tainted = peekTaint(argc);

                    // V-VM-003: capture taint for dict/list mutation methods BEFORE pop.
                    // dict.put(key, val): argc=2, val is args_ptr[1] = peekTaint(0)
                    // list.push(val):     argc=1, val is args_ptr[0] = peekTaint(0)
                    // list.insert(i,val): argc=2, val is args_ptr[1] = peekTaint(0)
                    // list.set(i,val):    argc=2, val is args_ptr[1] = peekTaint(0)
                    bool val_arg_tainted = false;
                    if (governance_ && (obj.isDict() || obj.isList())) {
                        // For single-arg mutations (push/append/add) the value IS the arg
                        // For two-arg mutations (put/insert/set) the value is the last arg
                        if (argc >= 1) {
                            val_arg_tainted = peekTaint(0);  // last arg = TOS = val
                        }
                    }

                    interpreter::NaabVal* args_ptr = stack_top_ - argc;
                    interpreter::NaabVal result = callBuiltinMethod(obj, method, argc, args_ptr);
                    // Clear stale slots, then pop args + object
                    for (int si = 0; si < argc + 1; si++)
                        *(stack_top_ - 1 - si) = interpreter::NaabVal();
                    stack_top_ -= (argc + 1);
                    syncTaintTop();
                    push(std::move(result));

                    // V-VM-003: propagate container-level taint for mutation methods.
                    if (governance_) {
                        bool is_mutation = (method == "put" || method == "push" ||
                                            method == "append" || method == "add" ||
                                            method == "insert" || method == "set");
                        bool is_read = (method == "get");
                        if (is_mutation && val_arg_tainted && (obj.isList() || obj.isDict())) {
                            tainted_containers_.insert(obj.toLegacy().get());
                        }
                        if (is_read && tainted_containers_.count(obj.toLegacy().get()) > 0) {
                            peekTaint(0) = true;
                        } else {
                            // Built-in methods propagate taint from object
                            peekTaint(0) = obj_tainted || (tainted_containers_.count(obj.toLegacy().get()) > 0);
                        }
                    }
                }
            }
            done_call_method:
                VM_NEXT();

            VM_CASE(OP_CLOSURE): {
                interpreter::NaabVal closure_val = READ_CONSTANT(arg);
                auto& closure = closure_val.asVMClosure();
                CompiledFunction* fn = closure->function;

                // Create a new closure with captured upvalues
                auto new_closure = std::make_shared<VMClosure>();
                new_closure->function = fn;
                new_closure->upvalues.resize(fn->upvalues.size());

                // Read upvalue descriptors following the OP_CLOSURE instruction
                for (size_t i = 0; i < fn->upvalues.size(); i++) {
                    uint32_t desc = READ_INSTR();
                    bool is_local = (desc >> 24) != 0;
                    uint8_t index = static_cast<uint8_t>(desc & 0xFF);

                    if (is_local) {
                        new_closure->upvalues[i] = captureUpvalue(frame->slots + index);
                    } else {
                        // Get from enclosing closure's upvalues
                        new_closure->upvalues[i] = frame->closure->upvalues[index];
                    }
                }

                push(interpreter::NaabVal::makeVMClosure(std::move(new_closure)));
                allocation_count_++;
            }
                VM_NEXT();

            VM_CASE(OP_CLOSE_UPVALUE): {
                closeUpvalues(stack_top_ - 1);
                pop();
            }
                VM_NEXT();

            // ====== Collections ======

            VM_CASE(OP_LIST): {
                int count = static_cast<int>(arg);
                // V-GOV-023: Enforce governance array size limit on literal lists
                naab::limits::checkArraySize(count);
                if (governance_ && governance_->isActive()) {
                    std::string gerr = governance_->checkArraySize(count);
                    if (!gerr.empty()) runtimeError("%s", gerr.c_str());
                }
                // Taint: list is tainted if ANY element is tainted
                bool list_taint = false;
                if (governance_) {
                    for (int i = count; i > 0; i--) {
                        if (peekTaint(i - 1)) { list_taint = true; break; }
                    }
                }
                std::vector<interpreter::NaabVal> elems;
                elems.reserve(count);
                for (int i = count; i > 0; i--) {
                    elems.push_back(peek(i - 1));
                }
                for (int i = 0; i < count; i++) pop();
                push(interpreter::NaabVal::makeList(std::move(elems)));
                allocation_count_++;
                if (governance_) peekTaint(0) = list_taint;
            }
                VM_NEXT();

            VM_CASE(OP_DICT): {
                int count = static_cast<int>(arg);
                // V-GOV-023: Enforce governance array size limit on literal dicts
                naab::limits::checkArraySize(count);
                if (governance_ && governance_->isActive()) {
                    std::string gerr = governance_->checkArraySize(count);
                    if (!gerr.empty()) runtimeError("%s", gerr.c_str());
                }
                // Taint: dict is tainted if ANY value is tainted
                bool dict_taint = false;
                if (governance_) {
                    for (int i = count - 1; i >= 0; i--) {
                        if (peekTaint(i * 2)) { dict_taint = true; break; }
                    }
                }
                std::unordered_map<std::string, interpreter::NaabVal> entries;
                for (int i = count - 1; i >= 0; i--) {
                    interpreter::NaabVal val = peek(i * 2);
                    interpreter::NaabVal key = peek(i * 2 + 1);
                    entries[key.toString()] = val;
                }
                for (int i = 0; i < count * 2; i++) pop();
                push(interpreter::NaabVal::makeDict(std::move(entries)));
                allocation_count_++;
                if (governance_) peekTaint(0) = dict_taint;
            }
                VM_NEXT();

            VM_CASE(OP_GET_INDEX): {
                // Taint: result inherits taint from container or index
                bool idx_taint = peekTaint(0);
                bool obj_taint = peekTaint(1);
                interpreter::NaabVal index = pop();
                interpreter::NaabVal obj = pop();
                if (obj.isList()) {
                    auto& list = obj.asList();
                    if (!index.isInt()) {
                        runtimeError("List index must be an integer, got %s",
                                     index.getTypeName().c_str());
                    }
                    int idx = static_cast<int>(index.asInt());
                    int size = static_cast<int>(list.size());
                    if (idx < 0) idx += size;
                    if (idx < 0 || idx >= size) {
                        runtimeError("Index %d out of bounds for list of size %d", idx, size);
                    }
                    push(list[idx]);
                } else if (obj.isDict()) {
                    auto& dict = obj.asDict();
                    std::string key = index.toString();
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
                                if (++count >= 10) { oss << "..."; break; }
                            }
                            oss << "\n";
                        }
                        oss << "\n  Help:\n";
                        oss << "  - Use dict.get(\"" << key << "\") for safe access (returns null if missing)\n";
                        oss << "  - Use dict.get(\"" << key << "\", default_value) to provide a default\n";
                        oss << "  - Use dict.has(\"" << key << "\") to check before accessing\n\n";
                        oss << "  Example:\n";
                        oss << "    \xE2\x9C\x97 Throws: d[\"" << key << "\"]\n";
                        oss << "    \xE2\x9C\x93 Safe:   d.get(\"" << key << "\", \"default\")\n";
                        runtimeError("%s", oss.str().c_str());
                    }
                    push(it->second);
                } else if (obj.isString()) {
                    auto& str = obj.asString();
                    // Allow string["message"] → returns self.
                    // This enables e["message"] when a plain string is thrown and caught.
                    if (index.isString() && index.asString() == "message") {
                        push(obj);
                    } else {
                        if (!index.isInt()) {
                            runtimeError("String index must be an integer");
                        }
                        int idx = static_cast<int>(index.asInt());
                        int size = static_cast<int>(str.size());
                        if (idx < 0) idx += size;
                        if (idx < 0 || idx >= size) {
                            runtimeError("Index %d out of bounds for string of length %d", idx, size);
                        }
                        push(interpreter::NaabVal::makeString(std::string(1, str[idx])));
                    }
                } else {
                    runtimeError("Cannot index into %s", obj.getTypeName().c_str());
                }
                if (governance_) {
                    // V-VM-003: also check side-table for containers that had
                    // tainted values stored into them via OP_SET_INDEX or dict.put.
                    bool container_tainted = tainted_containers_.count(obj.toLegacy().get()) > 0;
                    peekTaint(0) = obj_taint || idx_taint || container_tainted;

                }
            }
                VM_NEXT();

            VM_CASE(OP_SET_INDEX): {
                // Stack order from compiler: [val, obj, index] (top = index)
                bool val_taint_si = governance_ ? peekTaint(2) : false;
                interpreter::NaabVal index = pop();
                interpreter::NaabVal obj = pop();
                interpreter::NaabVal val = pop();
                if (obj.isList()) {
                    auto& list = obj.asList();
                    if (!index.isInt()) {
                        runtimeError("List index must be an integer");
                    }
                    int idx = static_cast<int>(index.asInt());
                    int size = static_cast<int>(list.size());
                    if (idx < 0) idx += size;
                    if (idx < 0 || idx >= size) {
                        runtimeError("Index %d out of bounds for list of size %d", idx, size);
                    }
                    list[idx] = val;
                } else if (obj.isDict()) {
                    auto& dict = obj.asDict();
                    dict[index.toString()] = val;
                } else {
                    runtimeError("Cannot set index on %s", obj.getTypeName().c_str());
                }
                // V-VM-003: if the stored value is tainted, record the container as
                // tainted in the side-table so future reads from it propagate taint.
                if (governance_ && val_taint_si && (obj.isList() || obj.isDict())) {
                    tainted_containers_.insert(obj.toLegacy().get());
                }
                push(val);
                if (governance_) peekTaint(0) = val_taint_si;
            }
                VM_NEXT();

            VM_CASE(OP_RANGE): {
                bool inclusive = (arg != 0);
                bool range_taint = governance_ ? (peekTaint(0) || peekTaint(1)) : false;
                interpreter::NaabVal end_val = pop();
                interpreter::NaabVal start_val = pop();
                if (!start_val.isInt() || !end_val.isInt()) {
                    runtimeError("Range bounds must be integers");
                }
                int64_t start = start_val.asInt();
                int64_t end = end_val.asInt();
                if (inclusive) end++;
                std::vector<interpreter::NaabVal> range_list;
                if (start <= end) {
                    range_list.reserve(end - start);
                    for (int64_t i = start; i < end; i++) {
                        range_list.push_back(interpreter::NaabVal::makeInt(i));
                    }
                }
                push(interpreter::NaabVal::makeList(std::move(range_list)));
                if (governance_) peekTaint(0) = range_taint;
            }
                VM_NEXT();

            VM_CASE(OP_GET_ITER): {
                bool iter_taint = governance_ ? peekTaint(0) : false;
                interpreter::NaabVal iterable = pop();
                if (iterable.isList()) {
                    push(iterable);
                    push(interpreter::NaabVal::makeInt(0));
                } else if (iterable.isDict()) {
                    // for (k, v) in dict is not supported in VM — use [k, v] instead
                    if (arg & 0x800000u) {
                        runtimeError("for (k, v) in dict is not supported — use for [k, v] in dict instead.\n\n"
                                     "  The bracket form [k, v] correctly binds key and value:\n"
                                     "    for [k, v] in my_dict {\n"
                                     "        print(k + \" = \" + string(v))\n"
                                     "    }");
                    }
                    uint32_t dict_arg = arg & 0x7FFFFFu;  // strip flag bit, keep lower 23 bits
                    auto& dict = iterable.asDictConst();
                    // Check for generator wrapper dict
                    auto gen_it = dict.find("__is_generator__");
                    if (gen_it != dict.end() && gen_it->second.isBool() && gen_it->second.asBool()) {
                        // Eagerly run generator function, collect yielded values
                        auto fn_it = dict.find("__generator__");
                        auto args_it = dict.find("__args__");
                        if (fn_it != dict.end() && fn_it->second.isVMClosure()) {
                            auto& gen_args = args_it->second.asListConst();
                            // Save and set generator collection target
                            auto* saved_gen = generator_values_;
                            std::vector<interpreter::NaabVal> collected;
                            generator_values_ = &collected;
                            try {
                                callNaabFunction(fn_it->second, gen_args);
                            } catch (...) {
                                generator_values_ = saved_gen;
                                throw;
                            }
                            generator_values_ = saved_gen;
                            // Now iterate over collected values
                            push(interpreter::NaabVal::makeList(std::move(collected)));
                            push(interpreter::NaabVal::makeInt(0));
                            if (governance_) peekTaint(1) = iter_taint;
                            goto done_get_iter;
                        }
                    }
                    // Regular dict iteration
                    // dict_arg > 0 means destructuring: produce [key, value] pairs
                    if (dict_arg > 0) {
                        std::vector<interpreter::NaabVal> pairs;
                        pairs.reserve(dict.size());
                        for (auto& kv : dict) {
                            std::vector<interpreter::NaabVal> pair;
                            pair.push_back(interpreter::NaabVal::makeString(kv.first));
                            pair.push_back(kv.second);
                            pairs.push_back(interpreter::NaabVal::makeList(std::move(pair)));
                        }
                        push(interpreter::NaabVal::makeList(std::move(pairs)));
                    } else {
                        std::vector<interpreter::NaabVal> keys;
                        keys.reserve(dict.size());
                        for (auto& kv : dict) {
                            keys.push_back(interpreter::NaabVal::makeString(kv.first));
                        }
                        push(interpreter::NaabVal::makeList(std::move(keys)));
                    }
                    push(interpreter::NaabVal::makeInt(0));
                } else if (iterable.isString()) {
                    auto& str = iterable.asString();
                    std::vector<interpreter::NaabVal> chars;
                    chars.reserve(str.size());
                    for (char c : str) {
                        chars.push_back(interpreter::NaabVal::makeString(std::string(1, c)));
                    }
                    push(interpreter::NaabVal::makeList(std::move(chars)));
                    push(interpreter::NaabVal::makeInt(0));
                } else {
                    runtimeError("Cannot iterate over %s", iterable.getTypeName().c_str());
                }
                // Taint: propagate iterable taint to the list on stack
                if (governance_) peekTaint(1) = iter_taint;
            }
            done_get_iter:
                VM_NEXT();

            VM_CASE(OP_ITER_NEXT): {
                int vars = (arg >> 16) & 0xFF;
                int jump_offset = arg & 0xFFFF;

                // Taint: iterable's taint propagates to extracted elements
                bool iterable_taint = governance_ ? peekTaint(1) : false;

                interpreter::NaabVal idx_val = peek(0);
                interpreter::NaabVal list = peek(1);
                int64_t idx = idx_val.asInt();
                auto& elems = list.asListConst();

                if (idx >= static_cast<int64_t>(elems.size())) {
                    frame->ip += jump_offset;
                } else {
                    *(stack_top_ - 1) = interpreter::NaabVal::makeInt(idx + 1);
                    if (vars == 0) {
                        push(elems[idx]);
                        if (governance_) peekTaint(0) = iterable_taint;
                    } else {
                        interpreter::NaabVal elem = elems[idx];
                        if (elem.isList()) {
                            auto& sub = elem.asListConst();
                            for (int v = 0; v < vars; v++) {
                                if (v < static_cast<int>(sub.size())) {
                                    push(sub[v]);
                                } else {
                                    push(interpreter::NaabVal::makeNull());
                                }
                                if (governance_) peekTaint(0) = iterable_taint;
                            }
                        } else {
                            push(elem);
                            if (governance_) peekTaint(0) = iterable_taint;
                            for (int v = 1; v < vars; v++) {
                                push(interpreter::NaabVal::makeNull());
                            }
                        }
                    }
                }
            }
                VM_NEXT();

            VM_CASE(OP_GET_MEMBER): {
                bool member_obj_taint = governance_ ? peekTaint(0) : false;
                interpreter::NaabVal obj = pop();
                std::string name = READ_CONSTANT(arg).toString();
                if (obj.isDict()) {
                    auto& dict = obj.asDict();
                    auto it = dict.find(name);
                    if (it != dict.end()) {
                        push(it->second);
                    } else {
                        // Struct method lookup: "StructName.method" in globals
                        auto sn_it = dict.find("__struct_name__");
                        if (sn_it != dict.end() && sn_it->second.isString()) {
                            std::string qualified = sn_it->second.asString() + "." + name;
                            auto git = globals_.find(qualified);
                            if (git != globals_.end()) {
                                push(git->second);
                            } else {
                                // Field not a method — suggest .get() for dict access
                                runtimeError("Dict has no method '%s'\n\n"
                                             "  This dict was originally a '%s' struct, but after JSON\n"
                                             "  serialization it became a plain dict.\n\n"
                                             "  Use .get() for field access on dicts:\n"
                                             "    ✗ Wrong: obj.%s\n"
                                             "    ✓ Right: obj.get(\"%s\")\n",
                                             name.c_str(), sn_it->second.asString().c_str(),
                                             name.c_str(), name.c_str());
                            }
                        } else {
                            runtimeError("Key '%s' not found in dict\n\n"
                                         "  Dicts use .get() for field access:\n"
                                         "    ✗ Wrong: obj.%s\n"
                                         "    ✓ Right: obj.get(\"%s\")\n"
                                         "    ✓ Also:  obj.get(\"%s\", default_value)\n",
                                         name.c_str(), name.c_str(), name.c_str(), name.c_str());
                        }
                    }
                } else if (obj.isString()) {
                    const std::string& sv = obj.asString();
                    // Stdlib module constant access: math.PI, math.E, etc.
                    if (sv.size() >= 18 && sv.substr(0, 18) == "__stdlib_module__:") {
                        std::string mod = sv.substr(18);
                        interpreter::NaabVal* no_args = nullptr;
                        push(callStdlibMethod(mod, name, 0, no_args));
                        // Finding D fix: apply taint source/sanitizer for zero-arg member access
                        if (governance_ && governance_->isActive()) {
                            std::string full_name = mod + "." + name;
                            if (governance_->isTaintSource(full_name)) {
                                peekTaint(0) = true;
                            } else if (governance_->isSanitizer(full_name)) {
                                peekTaint(0) = false;
                            }
                        }
                    } else if (sv.size() >= 12 && sv.substr(0, 12) == "__builtin__:") {
                        std::string mod = sv.substr(12);
                        if (stdlib_ && stdlib_->hasModule(mod)) {
                            interpreter::NaabVal* no_args = nullptr;
                            push(callStdlibMethod(mod, name, 0, no_args));
                            // Finding D fix: apply taint source/sanitizer for zero-arg member access
                            if (governance_ && governance_->isActive()) {
                                std::string full_name = mod + "." + name;
                                if (governance_->isTaintSource(full_name)) {
                                    peekTaint(0) = true;
                                } else if (governance_->isSanitizer(full_name)) {
                                    peekTaint(0) = false;
                                }
                            }
                        } else {
                            runtimeError("Cannot access member '%s' on %s",
                                         name.c_str(), obj.getTypeName().c_str());
                        }
                    } else {
                        runtimeError("Cannot access member '%s' on %s",
                                     name.c_str(), obj.getTypeName().c_str());
                    }
                } else if (obj.isBlock()) {
                    // Block member access: create sub-BlockValue with member_path
                    auto& block = obj.asBlock();
                    auto sub_block = std::make_shared<interpreter::BlockValue>(
                        block->metadata, block->code, block->getExecutor());
                    sub_block->member_path = name;
                    push(interpreter::NaabVal::makeBlock(sub_block));
                } else {
                    // DX: suggest function call form for common property-style access
                    if (name == "length" || name == "size") {
                        runtimeError("Cannot access member '%s' on %s\n\n"
                                     "  Help: Use %s() as a method call instead:\n"
                                     "    let n = my_value.length()\n",
                                     name.c_str(), obj.getTypeName().c_str(), name.c_str());
                    }
                    runtimeError("Cannot access member '%s' on %s",
                                 name.c_str(), obj.getTypeName().c_str());
                }
                // Finding D fix: propagate object taint only if not already set by taint source check
                if (governance_ && member_obj_taint && !peekTaint(0)) {
                    peekTaint(0) = true;
                }
            }
                VM_NEXT();

            VM_CASE(OP_SET_MEMBER): {
                bool val_taint_sm = governance_ ? peekTaint(1) : false;
                interpreter::NaabVal obj = pop();
                interpreter::NaabVal val = pop();
                std::string name = READ_CONSTANT(arg).toString();
                if (obj.isDict()) {
                    obj.asDict()[name] = val;
                } else {
                    runtimeError("Cannot set member '%s' on %s",
                                 name.c_str(), obj.getTypeName().c_str());
                }
                push(val);
                if (governance_) peekTaint(0) = val_taint_sm;
            }
                VM_NEXT();

            // ====== Exception Handling ======

            VM_CASE(OP_TRY_BEGIN): {
                // Push exception handler; arg = offset to catch block
                ExceptionHandler handler;
                handler.frame_index = frame_count_ - 1;
                handler.catch_ip = frame->ip + arg; // arg is relative offset from next instruction
                handler.finally_ip = nullptr;
                handler.stack_base = stack_top_;
                exception_handlers_.push_back(handler);
            }
                VM_NEXT();

            VM_CASE(OP_TRY_END): {
                if (!exception_handlers_.empty()) {
                    exception_handlers_.pop_back();
                }
            }
                VM_NEXT();

            VM_CASE(OP_THROW): {
                interpreter::NaabVal exception = pop();
                // Unwind to nearest exception handler
                if (exception_handlers_.empty()) {
                    runtimeError("Uncaught exception: %s", exception.toString().c_str());
                }
                ExceptionHandler handler = exception_handlers_.back();
                exception_handlers_.pop_back();
                // Restore stack and frame
                while (frame_count_ - 1 > handler.frame_index) {
                    frame_count_--;
                }
                frame = &frames_[frame_count_ - 1];
                stack_top_ = handler.stack_base;
                syncTaintTop();
                frame->ip = handler.catch_ip;
                // Push exception value for catch clause (untainted)
                push(exception);
            }
                VM_NEXT();

            // ====== Structs ======

            VM_CASE(OP_STRUCT_NEW): {
                // TOS = struct definition dict; clone it as a new instance
                interpreter::NaabVal def = pop();
                if (!def.isDict()) {
                    runtimeError("OP_STRUCT_NEW: expected struct definition dict");
                }
                auto& src = def.asDictConst();
                // V-GOV-023: Enforce governance array size limit on struct creation
                naab::limits::checkArraySize(src.size());
                if (governance_ && governance_->isActive()) {
                    std::string gerr = governance_->checkArraySize(src.size());
                    if (!gerr.empty()) runtimeError("%s", gerr.c_str());
                }
                std::unordered_map<std::string, interpreter::NaabVal> instance(src.begin(), src.end());
                push(interpreter::NaabVal::makeDict(std::move(instance)));
            }
                VM_NEXT();

            VM_CASE(OP_STRUCT_INIT_FIELD): {
                // Pop value, set field on TOS struct instance
                // Taint: if value is tainted, struct instance becomes tainted
                bool field_taint = governance_ ? peekTaint(0) : false;
                interpreter::NaabVal val = pop();
                std::string name = READ_CONSTANT(arg).toString();
                interpreter::NaabVal& instance = peek(0);
                if (!instance.isDict()) {
                    runtimeError("OP_STRUCT_INIT_FIELD: expected struct instance");
                }
                instance.asDict()[name] = val;
                if (governance_ && field_taint) peekTaint(0) = true;
            }
                VM_NEXT();

            // ====== Polyglot ======

            VM_CASE(OP_POLYGLOT): {
                // arg: [info_idx:16][num_vars:8]
                int info_idx = (arg >> 8) & 0xFFFF;
                int num_vars = arg & 0xFF;
                interpreter::NaabVal block_info = READ_CONSTANT(info_idx);
                // Collect bound variable values and taint from stack
                std::vector<interpreter::NaabVal> bound_vals;
                std::vector<bool> bound_taints;
                for (int i = num_vars - 1; i >= 0; i--) {
                    bound_vals.push_back(peek(i));
                    bound_taints.push_back(governance_ ? peekTaint(i) : false);
                }
                for (int i = 0; i < num_vars; i++) pop();

                // Extract info
                auto& info = block_info.asDictConst();
                std::string language = info.at("language").toString();
                std::string raw_code = info.at("code").toString();
                std::string return_type;
                auto rt_it = info.find("return_type");
                if (rt_it != info.end()) return_type = rt_it->second.toString();

                // Get bound variable names
                std::vector<std::string> bound_var_names;
                auto bv_it = info.find("bound_vars");
                if (bv_it != info.end() && bv_it->second.isList()) {
                    for (auto& v : bv_it->second.asListConst()) {
                        bound_var_names.push_back(v.toString());
                    }
                }

                // Governance: check polyglot block
                int polyglot_gov_line = CURRENT_CHUNK().getLine(
                    static_cast<int>(frame->ip - CURRENT_CHUNK().code.data()) - 4);
                if (governance_) {
                    int gov_line = polyglot_gov_line;
                    governance_->setCheckContext(current_file_, gov_line);
                    std::string err = governance_->checkPolyglotBlock(
                        language, raw_code, current_file_, gov_line,
                        bound_var_names.size());
                    if (!err.empty()) runtimeError("%s", err.c_str());
                    std::string count_err = governance_->incrementAndCheckPolyglotBlockCount();
                    if (!count_err.empty()) runtimeError("%s", count_err.c_str());

                    // Taint sink check: block tainted variables from reaching polyglot blocks
                    // Use shadow taint stack data (bound_taints) for locals, AND governance taint set for globals
                    if (governance_->isActive()) {
                        std::string sink_type;
                        if (language == "shell" || language == "sh" || language == "bash") {
                            sink_type = "shell_exec";
                        } else if (language == "js" || language == "javascript") {
                            sink_type = "javascript_exec";
                        } else {
                            sink_type = language + "_exec";
                        }
                        for (size_t bi = 0; bi < bound_var_names.size(); bi++) {
                            // Check both shadow taint stack AND governance taint set
                            bool is_tainted = (bi < bound_taints.size() && bound_taints[bi])
                                              || governance_->isTainted(bound_var_names[bi]);
                            if (is_tainted) {
                                // Look up lineage from the bound variable's stack offset
                                std::string origin_func;
                                std::string origin_arg;
                                if (!taint_lineage_map_.empty()) {
                                    // bound vars are at stack positions: stack_top_ - num_vars + bi
                                    size_t var_offset = static_cast<size_t>(
                                        (stack_top_ - num_vars + static_cast<int>(bi)) - stack_.get());
                                    auto lit = taint_lineage_map_.find(var_offset);
                                    if (lit != taint_lineage_map_.end()) {
                                        origin_func = lit->second.source_func;
                                        origin_arg = lit->second.source_arg;
                                    }
                                }
                                // Mark tainted with lineage so checkTaintedSink has origin info
                                governance_->markTainted(bound_var_names[bi],
                                    origin_func, origin_arg, current_file_, gov_line);
                                std::string terr = governance_->checkTaintedSink(
                                    bound_var_names[bi], sink_type, current_file_, gov_line);
                                if (!terr.empty()) runtimeError("%s", terr.c_str());
                            }
                        }
                    }
                }

                // Enterprise Security: Activate sandbox for polyglot execution
                auto& sandbox_manager = security::SandboxManager::instance();
                security::SandboxConfig sandbox_config = sandbox_manager.getDefaultConfig();
                if (governance_ && governance_->getTimeoutSeconds() > 0) {
                    sandbox_config.max_cpu_seconds = governance_->getTimeoutSeconds();
                }
                security::ScopedSandbox scoped_sandbox(sandbox_config);

                // Get executor from LanguageRegistry
                auto& registry = runtime::LanguageRegistry::instance();
                auto* executor = registry.getExecutor(language);
                if (!executor) {
                    std::string msg = "No executor found for language: " + language + "\n\n";
                    if (language == "python") msg += "  Ensure python3 is in PATH\n";
                    else if (language == "go") msg += "  Install: pkg install golang\n";
                    else if (language == "javascript" || language == "js") msg += "  Ensure node is in PATH\n";
                    else if (language == "nim") msg += "  Install: pkg install nim\n";
                    msg += "  Verify: which " + language + "\n";
                    runtimeError("%s", msg.c_str());
                }

                // Build variable declarations
                std::string var_declarations = buildVarDeclarations(
                    language, bound_var_names, bound_vals);

                // Strip common indentation from code
                std::string code = stripCommonIndent(raw_code);

                // Inject declarations (header-aware for Go/PHP)
                std::string final_code;
                if (!var_declarations.empty() &&
                    (language == "go" || language == "php")) {
                    final_code = injectAfterHeaders(var_declarations, code, language);
                } else {
                    final_code = var_declarations + code;
                }

                // Phase 12: Inject naab_return() helper function per language
                // (mirrors polyglot.cpp:251-292)
                bool code_uses_naab_return = (raw_code.find("naab_return") != std::string::npos);
                if (code_uses_naab_return) {
                    std::string helper;
                    if (language == "python") {
                        if (!return_type.empty()) {
                            helper = "def naab_return(data):\n    import json as __nrj\n    print(__nrj.dumps(data) if not isinstance(data, str) else data)\n";
                        } else {
                            helper = "def naab_return(data):\n    return data\n";
                        }
                    } else if (language == "javascript" || language == "js") {
                        helper = "function naab_return(data) { return data; }\n";
                    } else if (language == "typescript" || language == "ts") {
                        helper = "function naab_return(data) { return data; }\n";
                    } else if (language == "shell" || language == "sh" || language == "bash") {
                        helper = "naab_return() { echo \"__NAAB_RETURN__:$1\"; }\n";
                    } else if (language == "ruby") {
                        helper = "require 'json'\ndef naab_return(data); puts \"__NAAB_RETURN__:\" + data.to_json; end\n";
                    } else if (language == "php") {
                        if (final_code.find("<?php") == std::string::npos) helper = "<?php\n";
                        helper += "function naab_return($data) { echo \"__NAAB_RETURN__:\" . json_encode($data) . \"\\n\"; }\n";
                    } else if (language == "rust") {
                        helper = "macro_rules! naab_return { ($val:expr) => { println!(\"__NAAB_RETURN__:{}\", $val); }; }\n";
                    } else if (language == "cpp" || language == "c++") {
                        helper = "#include <sstream>\n#define naab_return(val) do { std::ostringstream __os; __os << \"__NAAB_RETURN__:\" << (val); std::cout << __os.str() << std::endl; } while(0)\n";
                    }
                    if (!helper.empty()) {
                        final_code = helper + final_code;
                    }
                }

                // Handle -> JSON return type for Python
                if (!return_type.empty() && language == "python") {
                    // Auto-wrap last bare expression in print()
                    // Supports multi-line expressions (e.g., print(json.dumps({\n...\n})))
                    auto bracketDepth = [](const std::string& s) -> int {
                        int depth = 0;
                        bool in_string = false;
                        char string_char = 0;
                        for (size_t j = 0; j < s.size(); ++j) {
                            char c = s[j];
                            if (in_string) {
                                if (c == '\\' && j + 1 < s.size()) { ++j; continue; }
                                if (c == string_char) in_string = false;
                                continue;
                            }
                            if (c == '"' || c == '\'') { in_string = true; string_char = c; continue; }
                            if (c == '#') break;
                            if (c == '(' || c == '[' || c == '{') ++depth;
                            else if (c == ')' || c == ']' || c == '}') --depth;
                        }
                        return depth;
                    };

                    std::istringstream iss(final_code);
                    std::string line;
                    std::vector<std::string> fc_lines;
                    while (std::getline(iss, line)) fc_lines.push_back(line);
                    for (int li = static_cast<int>(fc_lines.size()) - 1; li >= 0; --li) {
                        std::string trimmed = fc_lines[li];
                        size_t start = trimmed.find_first_not_of(" \t");
                        if (start == std::string::npos) continue;
                        trimmed = trimmed.substr(start);
                        if (trimmed[0] == '#') continue;
                        if (trimmed.substr(0, 7) == "import " || trimmed.substr(0, 5) == "from ") continue;
                        if (trimmed.substr(0, 6) == "print(" || trimmed.substr(0, 7) == "print (") break;
                        if (trimmed.find('=') != std::string::npos && trimmed.find("==") == std::string::npos
                            && trimmed.find("!=") == std::string::npos && trimmed.find(">=") == std::string::npos
                            && trimmed.find("<=") == std::string::npos) break;
                        if (trimmed.substr(0, 3) == "if " || trimmed.substr(0, 4) == "for "
                            || trimmed.substr(0, 6) == "while " || trimmed.substr(0, 4) == "def "
                            || trimmed.substr(0, 6) == "class " || trimmed.substr(0, 4) == "try:"
                            || trimmed.substr(0, 7) == "except:" || trimmed.substr(0, 7) == "except "
                            || trimmed.substr(0, 4) == "with ") break;

                        int end_depth = bracketDepth(trimmed);
                        if (end_depth < 0) {
                            // Multi-line expression — walk backwards to find start
                            int cumulative_depth = end_depth;
                            int expr_start = li;
                            for (int j = li - 1; j >= 0 && cumulative_depth < 0; --j) {
                                std::string jtrimmed = fc_lines[j];
                                size_t jstart = jtrimmed.find_first_not_of(" \t");
                                if (jstart == std::string::npos) continue;
                                cumulative_depth += bracketDepth(jtrimmed.substr(jstart));
                                expr_start = j;
                            }
                            // Check if expr_start already has print()
                            std::string es_trimmed = fc_lines[expr_start];
                            size_t es_start = es_trimmed.find_first_not_of(" \t");
                            if (es_start != std::string::npos) es_trimmed = es_trimmed.substr(es_start);
                            if (es_trimmed.substr(0, 6) != "print(" && es_trimmed.substr(0, 7) != "print (") {
                                size_t sstart = fc_lines[expr_start].find_first_not_of(" \t");
                                std::string leading = (sstart != std::string::npos)
                                    ? fc_lines[expr_start].substr(0, sstart) : "";
                                std::string strimmed = (sstart != std::string::npos)
                                    ? fc_lines[expr_start].substr(sstart) : fc_lines[expr_start];
                                fc_lines[expr_start] = leading + "print(" + strimmed;
                                fc_lines[li] = fc_lines[li] + ")";
                            }
                        } else {
                            std::string leading = fc_lines[li].substr(0, start);
                            fc_lines[li] = leading + "print(" + trimmed + ")";
                        }
                        break;
                    }
                    final_code.clear();
                    for (size_t li = 0; li < fc_lines.size(); ++li) {
                        if (li > 0) final_code += "\n";
                        final_code += fc_lines[li];
                    }
                    final_code += "\n";

#ifdef HAVE_PYBIND11
                    // Embedded Python (pybind11): redirect stdout to buffer, parse JSON, return via C API
                    std::string preamble =
                        "import sys as __naab_sys, io as __naab_io, json as __naab_json\n"
                        "__naab_buf = __naab_io.StringIO()\n"
                        "__naab_orig = __naab_sys.stdout\n"
                        "__naab_sys.stdout = __naab_buf\n";
                    std::string postamble =
                        "\n__naab_sys.stdout = __naab_orig\n"
                        "__naab_captured = __naab_buf.getvalue().strip().split('\\n')\n"
                        "__naab_result = None\n"
                        "for __naab_l in reversed(__naab_captured):\n"
                        "    __naab_l = __naab_l.strip()\n"
                        "    if not __naab_l:\n"
                        "        continue\n"
                        "    try:\n"
                        "        __naab_result = __naab_json.loads(__naab_l)\n"
                        "        break\n"
                        "    except:\n"
                        "        __naab_sys.stdout.write(__naab_l + '\\n')\n"
                        "__naab_result\n";
                    final_code = preamble + final_code + postamble;
#endif
                    // Subprocess Python (no pybind11): auto-wrap above added print() to last expression,
                    // which writes directly to subprocess stdout. No preamble/postamble needed.
                }

                // Execute via language executor
                auto polyglot_exec_start = std::chrono::steady_clock::now();
                try {
                    interpreter::NaabVal result = executor->executeWithReturn(final_code);

                    // Post-execution: parse JSON output for -> JSON blocks
                    // Strategy 1: Check captured output buffer
                    std::string captured = executor->getCapturedOutput();
                    bool json_parsed = false;
                    if (!captured.empty()) {
                        auto polyglot_result = runtime::parsePolyglotOutput(captured, return_type);
                        if (!polyglot_result.return_value.isNull()) {
                            result = polyglot_result.return_value;
                            json_parsed = true;
                        }
                        if (!polyglot_result.log_output.empty()) {
                            std::cout << polyglot_result.log_output << std::flush;
                        }
                    }

                    // Strategy 2: If result is a string, check for sentinel or try JSON parse
                    if (!json_parsed && !result.isNull() && result.isString()) {
                        const auto& str_val = result.asString();
                        if (str_val.find("__NAAB_RETURN__:") != std::string::npos) {
                            auto polyglot_result = runtime::parsePolyglotOutput(str_val, return_type);
                            if (!polyglot_result.return_value.isNull()) {
                                result = polyglot_result.return_value;
                                json_parsed = true;
                            }
                        } else if (!return_type.empty()) {
                            auto polyglot_result = runtime::parsePolyglotOutput(str_val, return_type);
                            if (!polyglot_result.return_value.isNull()) {
                                result = polyglot_result.return_value;
                                json_parsed = true;
                            }
                        }
                    }

                    // Phase 12: Contract violation — -> JSON declared but no JSON produced
                    if (!return_type.empty() && return_type == "JSON") {
                        bool has_valid_result = json_parsed || (!result.isNull() && !result.isString());
                        if (!has_valid_result) {
                            std::string err = "Block contract violation: <<" + language +
                                " -> JSON>> expected a JSON return value, "
                                "but no valid JSON was found in stdout.\n\n"
                                "  The '-> JSON' header means the block MUST output valid JSON.\n"
                                "  The last printed line must be a JSON string.\n\n"
                                "  Common fixes:\n"
                                "  - Use naab_return({key: value}) for structured data\n"
                                "  - Use print(json.dumps(data)) for Python\n"
                                "  - Use JSON.stringify(data) for JavaScript\n";
                            runtimeError("%s", err.c_str());
                        }
                    }

                    push(result);
                    // V-GOV-006: All polyglot block outputs are unconditionally tainted when
                    // governance is active. Foreign-language data is untrusted by definition —
                    // the marshalling layer (JSON/subprocess) cannot preserve taint metadata,
                    // so we conservatively taint all cross-language return values.
                    if (governance_ && governance_->isActive()) {
                        peekTaint(0) = true;

                        // Pass 2: Record polyglot execution for post-execution audit
                        auto polyglot_exec_end = std::chrono::steady_clock::now();
                        int64_t duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            polyglot_exec_end - polyglot_exec_start).count();
                        governance::PolyglotExecutionRecord rec;
                        rec.language = language;
                        rec.runtime_version = executor->getRuntimeVersion();
                        rec.source_line = polyglot_gov_line;
                        rec.duration_us = duration_us;
                        rec.file = current_file_;
                        rec.bound_vars = bound_var_names;
                        rec.captured_output = captured;
                        rec.final_code = final_code;
                        rec.contract_verified = json_parsed;
                        rec.exit_code = executor->getLastExitCode();
                        governance_->addPolyglotExecution(rec);
                    }
                } catch (const std::exception& e) {
                    runtimeError("Polyglot %s error: %s", language.c_str(), e.what());
                }
            }
                VM_NEXT();

            VM_CASE(OP_RUNTIME_START): {
                // Phase 12: Persistent runtime declaration
                // Packed: name_idx in upper 12 bits, lang_idx in lower 12 bits
                uint32_t name_idx = (arg >> 12) & 0xFFF;
                uint32_t lang_idx = arg & 0xFFF;
                const std::string& name = READ_CONSTANT(name_idx).asString();
                const std::string& language = READ_CONSTANT(lang_idx).asString();

                if (named_runtimes_.count(name)) {
                    runtimeError("Runtime error: Runtime '%s' already exists.\n\n"
                        "  Each runtime name must be unique. Use a different name:\n"
                        "    runtime %s2 = %s.start()\n",
                        name.c_str(), name.c_str(), language.c_str());
                }

                auto& registry = runtime::LanguageRegistry::instance();
                auto* executor = registry.getExecutor(language);
                if (!executor) {
                    runtimeError("Runtime error: Unknown language '%s' for persistent runtime.\n\n"
                        "  Supported languages: python, javascript, js, shell, bash, sh,\n"
                        "    rust, go, cpp, csharp, cs, ruby, php, typescript, ts\n\n"
                        "  Example: runtime py = python.start()\n", language.c_str());
                }

                PersistentRuntime rt;
                rt.language = language;
                rt.executor = executor;
                rt.code_buffer = "";
                named_runtimes_[name] = std::move(rt);

                // Define as global string marker for .exec() dispatch
                globals_[name] = interpreter::NaabVal::makeString("__NAAB_RUNTIME__:" + name);
            }
                VM_NEXT();

            VM_CASE(OP_IMPORT): {
                // arg = constant index of module path string
                std::string module_path = frame->function->chunk.constants[arg].asString();

                // Check module cache first
                auto cache_it = module_cache_.find(module_path);
                if (cache_it != module_cache_.end()) {
                    for (auto& [ename, eval] : *cache_it->second) {
                        globals_[ename] = eval;
                    }
                    push(interpreter::NaabVal::makeDict(
                        std::unordered_map<std::string, interpreter::NaabVal>(*cache_it->second)));
                    goto done_import;
                }

                // Try stdlib fallback
                std::string bare_name = module_path;
                if (bare_name.size() > 5 && bare_name.substr(bare_name.size() - 5) == ".naab") {
                    bare_name = bare_name.substr(0, bare_name.size() - 5);
                }
                auto last_slash = bare_name.rfind('/');
                if (last_slash != std::string::npos) {
                    bare_name = bare_name.substr(last_slash + 1);
                }
                if (stdlib_ && stdlib_->hasModule(bare_name)) {
                    // Push stdlib marker as module
                    push(interpreter::NaabVal::makeString("__stdlib_module__:" + bare_name));
                    goto done_import;
                }

                // Resolve file path
                if (!module_resolver_) {
                    runtimeError("Module imports require a module resolver (use setModuleResolver())");
                }

                std::filesystem::path current_dir;
                if (!current_file_.empty()) {
                    current_dir = std::filesystem::path(current_file_).parent_path();
                } else {
                    current_dir = std::filesystem::current_path();
                }

                auto resolved = module_resolver_->resolve(module_path, current_dir);
                if (!resolved) {
                    // Check common aliases from other languages
                    static const std::unordered_map<std::string, std::string> module_aliases = {
                        {"fs", "file"}, {"os", "env"}, {"re", "regex"},
                        {"console", "io"}, {"sys", "env"}, {"system", "process"},
                        {"random", "math"}, {"datetime", "time"}, {"net", "http"},
                    };
                    auto alias_it = module_aliases.find(bare_name);
                    if (alias_it != module_aliases.end()) {
                        runtimeError("Module not found: %s\n\n  Did you mean: use %s\n"
                            "  NAAb's '%s' module provides this functionality.",
                            module_path.c_str(), alias_it->second.c_str(), alias_it->second.c_str());
                    }
                    runtimeError("Module not found: %s", module_path.c_str());
                }

                std::string canonical = modules::ModuleResolver::canonicalizePath(*resolved);

                // Check cache with canonical path
                cache_it = module_cache_.find(canonical);
                if (cache_it != module_cache_.end()) {
                    for (auto& [ename, eval] : *cache_it->second) {
                        globals_[ename] = eval;
                    }
                    push(interpreter::NaabVal::makeDict(
                        std::unordered_map<std::string, interpreter::NaabVal>(*cache_it->second)));
                    goto done_import;
                }

                // Load, compile, and execute the module
                auto exports = importModule(canonical);
                module_cache_[canonical] = exports;
                module_cache_[module_path] = exports;  // Cache under original path too

                // Re-register function evaluator — module VM may have overwritten it
                // (module VM shares stdlib_ and its execute() sets the evaluator to
                //  capture module VM's 'this', which becomes dangling after module VM dies)
                if (stdlib_) {
                    auto array_module = stdlib_->getModule("array");
                    if (array_module) {
                        auto* array_mod = dynamic_cast<stdlib::ArrayModule*>(array_module.get());
                        if (array_mod) {
                            array_mod->setFunctionEvaluator(
                                [this](interpreter::NaabVal fn,
                                       const std::vector<interpreter::NaabVal>& args) -> interpreter::NaabVal {
                                    return this->callNaabFunction(fn, args);
                                }
                            );
                        }
                    }
                }

                // Define all module exports as globals in caller VM
                // This ensures imported functions can find their enum/struct dependencies
                // Always override — module exports are filtered to exclude default preludes
                for (auto& [ename, eval] : *exports) {
                    globals_[ename] = eval;
                }

                push(interpreter::NaabVal::makeDict(
                    std::unordered_map<std::string, interpreter::NaabVal>(*exports)));
            }
            done_import:
                VM_NEXT();

            VM_CASE(OP_IMPORT_NAME): {
                // arg = constant index of name to extract
                // Module dict stays on stack — we push the extracted value on top
                std::string name = frame->function->chunk.constants[arg].asString();
                auto& module_val = peek(0);  // Module is on top of stack

                if (module_val.isDict()) {
                    auto& dict = module_val.asDict();
                    auto it = dict.find(name);
                    if (it != dict.end()) {
                        // Push extracted value on top (module stays below)
                        push(it->second);
                    } else {
                        runtimeError("Module does not export '%s'", name.c_str());
                    }
                } else if (module_val.isString()) {
                    std::string marker = module_val.asString();
                    if (marker.substr(0, 18) == "__stdlib_module__:") {
                        push(interpreter::NaabVal::makeString(marker + "." + name));
                    } else {
                        runtimeError("Cannot extract '%s' from non-module value", name.c_str());
                    }
                } else {
                    runtimeError("Cannot extract '%s' from non-module value", name.c_str());
                }
            }
                VM_NEXT();

            VM_CASE(OP_IMPORT_WILDCARD): {
                // arg = constant index (unused for now, could be alias)
                auto module_val = pop();
                if (module_val.isDict()) {
                    auto& dict = module_val.asDict();
                    for (auto& [name, val] : dict) {
                        // Skip internal metadata keys
                        if (name.substr(0, 2) == "__") continue;
                        globals_[name] = val;
                    }
                } else {
                    runtimeError("Wildcard import requires a module dict");
                }
            }
                VM_NEXT();

            VM_CASE(OP_EXPORT_NAME): {
                // No-op for now — all globals are accessible
            }
                VM_NEXT();

            VM_CASE(OP_AWAIT): {
                interpreter::NaabVal val = pop();
                if (val.isFuture()) {
                    auto& future_val = val.asFuture();
                    try {
                        // Deep-copy result so main VM owns all handles
                        // (async VM's handles may be freed after it exits)
                        push(future_val->future.get().deepCopy());
                    } catch (const std::exception& e) {
                        runtimeError("Await error: %s failed\n  Cause: %s",
                                     future_val->description.c_str(), e.what());
                    }
                } else {
                    // await on non-future = identity (pass through)
                    push(val);
                }
            }
                VM_NEXT();

            VM_CASE(OP_YIELD): {
                interpreter::NaabVal val = pop();
                if (generator_values_) {
                    generator_values_->push_back(val);
                } else {
                    runtimeError("'yield' used outside of a generator function");
                }
                // yield is an expression — push null as its result value
                // (ExprStmt will OP_POP this)
                push(interpreter::NaabVal::makeNull());
            }
                VM_NEXT();

            // Pre-flight taint mark: compiler detected static taint in RHS expression
            vm_OP_GOV_TAINT_MARK:
            {
                // Mark TOS as tainted on the shadow taint stack
                if (governance_) peekTaint(0) = true;
            }
                VM_NEXT();

            // Finding G: Check if tainted TOS value reaches a sink at assignment point
            vm_OP_GOV_TAINT_CHECK_ASSIGN:
            {
                if (governance_ && governance_->isActive() && peekTaint(0)) {
                    std::string var_name = READ_CONSTANT(arg).toString();
                    int gov_line = CURRENT_CHUNK().getLine(
                        static_cast<int>(frame->ip - CURRENT_CHUNK().code.data()) - 4);
                    std::string terr = governance_->checkTaintedSink(
                        var_name, "assignment", current_file_, gov_line);
                    if (!terr.empty()) runtimeError("%s", terr.c_str());
                }
            }
                VM_NEXT();

            // Reserved opcodes — no-op (compiler doesn't emit these).
            // OP_GOV_CHECK_FUNC: reserved for future per-function governance
            // policy (e.g. govern.json "function_rules"). Not emitted by compiler.
            // VM already enforces function contracts via checkFunctionContract()
            // and checkFunctionInputContract() at call sites. Post-v1.0.
            vm_STUB_NOP:
                VM_NEXT();

            // Truly unimplemented opcodes
            VM_DEFAULT: {
                runtimeError("Unimplemented opcode: %d", static_cast<int>(op));
            }
#ifndef USE_COMPUTED_GOTO
        }  // end switch
#endif
      } catch (const VMException& e) {
        // Route VM runtime errors through the exception handler system
        if (!exception_handlers_.empty()) {
            ExceptionHandler handler = exception_handlers_.back();
            exception_handlers_.pop_back();
            while (frame_count_ - 1 > handler.frame_index) {
                frame_count_--;
            }
            frame = &frames_[frame_count_ - 1];
            stack_top_ = handler.stack_base;
            syncTaintTop();
            frame->ip = handler.catch_ip;
            // Push error as dict with "message" key (matching tree-walker)
            std::unordered_map<std::string, interpreter::NaabVal> err;
            err["message"] = interpreter::NaabVal::makeString(e.what());
            push(interpreter::NaabVal::makeDict(std::move(err)));
        } else {
            throw std::runtime_error(e.what());
        }
      } catch (const std::runtime_error& e) {
        // Route C++ exceptions (from stdlib, polyglot, etc.) through exception handlers
        if (!exception_handlers_.empty()) {
            ExceptionHandler handler = exception_handlers_.back();
            exception_handlers_.pop_back();
            while (frame_count_ - 1 > handler.frame_index) {
                frame_count_--;
            }
            frame = &frames_[frame_count_ - 1];
            stack_top_ = handler.stack_base;
            syncTaintTop();
            frame->ip = handler.catch_ip;
            // Push error as dict with "message" key (matching tree-walker)
            std::unordered_map<std::string, interpreter::NaabVal> err;
            err["message"] = interpreter::NaabVal::makeString(e.what());
            push(interpreter::NaabVal::makeDict(std::move(err)));
        } else {
            throw;  // Re-throw if no handler
        }
      }
    }

#undef READ_INSTR
#undef CURRENT_CHUNK
#undef VM_CASE
#undef VM_NEXT
#undef VM_DEFAULT
#ifdef USE_COMPUTED_GOTO
#undef USE_COMPUTED_GOTO
#endif
}

// ============================================================================
// Async thread isolation: deep-copy CompiledFunction tree
// ============================================================================

// Recursively deep-copies a CompiledFunction and all nested functions
// referenced by VMClosure constants. This ensures the async thread has
// its own handles for all constants, preventing cross-thread handle sharing.
static void deepCopyFunctionTree(
    CompiledFunction* src,
    std::unordered_map<CompiledFunction*, CompiledFunction*>& cache,
    std::vector<std::unique_ptr<CompiledFunction>>& owned)
{
    if (cache.count(src)) return;
    auto copy = std::make_unique<CompiledFunction>();
    CompiledFunction* copy_ptr = copy.get();
    cache[src] = copy_ptr;

    // Copy plain data fields
    copy->name = src->name;
    copy->arity = src->arity;
    copy->max_arity = src->max_arity;
    copy->local_count = src->local_count;
    copy->upvalues = src->upvalues;  // UpvalueDesc is plain data
    copy->param_types = src->param_types;
    copy->return_type = src->return_type;
    copy->is_generator = src->is_generator;
    copy->is_async = src->is_async;
    copy->source_file = src->source_file;
    copy->source_line = src->source_line;
    copy->local_names = src->local_names;

    // Copy chunk: opcodes and line info are plain data
    copy->chunk.code = src->chunk.code;
    copy->chunk.lines = src->chunk.lines;
    copy->chunk.source_file = src->chunk.source_file;

    // Deep-copy default parameter chunks (constants need deep copy)
    copy->default_chunks.reserve(src->default_chunks.size());
    for (const auto& dc : src->default_chunks) {
        if (dc.has_value()) {
            Chunk dc_copy;
            dc_copy.code = dc->code;
            dc_copy.lines = dc->lines;
            dc_copy.source_file = dc->source_file;
            dc_copy.constants.reserve(dc->constants.size());
            for (const auto& c : dc->constants) {
                dc_copy.constants.push_back(c.deepCopy());
            }
            copy->default_chunks.push_back(std::move(dc_copy));
        } else {
            copy->default_chunks.push_back(std::nullopt);
        }
    }

    // First pass: recursively copy nested functions found in constants
    for (const auto& c : src->chunk.constants) {
        if (c.isVMClosure()) {
            auto& closure = c.asVMClosureConst();
            if (closure && closure->function) {
                deepCopyFunctionTree(closure->function, cache, owned);
            }
        }
    }

    // Second pass: deep-copy constants, replacing CompiledFunction* refs
    copy->chunk.constants.reserve(src->chunk.constants.size());
    for (const auto& c : src->chunk.constants) {
        if (c.isVMClosure()) {
            auto& orig_cl = c.asVMClosureConst();
            if (orig_cl && orig_cl->function && cache.count(orig_cl->function)) {
                auto new_cl = std::make_shared<VMClosure>(
                    cache[orig_cl->function],
                    orig_cl->upvalues);  // upvalues are shared_ptr, refcounted
                copy->chunk.constants.push_back(
                    interpreter::NaabVal::makeVMClosure(std::move(new_cl)));
            } else {
                copy->chunk.constants.push_back(c.deepCopy());
            }
        } else {
            copy->chunk.constants.push_back(c.deepCopy());
        }
    }

    owned.push_back(std::move(copy));
}

// ============================================================================
// Function calls
// ============================================================================

bool VM::callValue(interpreter::NaabVal callee, int argc) {
    if (callee.isVMClosure()) {
        auto& closure = callee.asVMClosure();
        CompiledFunction* fn = closure->function;

        // Async function: spawn on separate thread, return FutureValue
        if (fn->is_async) {
            // Collect args from stack — deep-copy for thread isolation
            interpreter::NaabVal* args_start = stack_top_ - argc;
            std::vector<interpreter::NaabVal> args;
            args.reserve(argc);
            for (int i = 0; i < argc; i++) {
                args.push_back(args_start[i].deepCopy());
            }

            // Deep-copy closure with pre-closed upvalues for the async thread.
            // Upvalue locations point into THIS VM's stack, which is invalid
            // in the async VM's separate stack. Close them now so the values
            // are self-contained in the ObjUpvalue::closed field.
            auto orig_closure = callee.asVMClosure();
            std::vector<std::shared_ptr<ObjUpvalue>> closed_upvalues;
            closed_upvalues.reserve(orig_closure->upvalues.size());
            for (auto& uv : orig_closure->upvalues) {
                auto new_uv = std::make_shared<ObjUpvalue>();
                // Deep-copy the current value for thread isolation
                new_uv->closed = uv->location->deepCopy();
                new_uv->location = &new_uv->closed;  // self-referencing
                new_uv->is_open = false;              // Mark as closed!
                new_uv->tainted = uv->tainted;
                closed_upvalues.push_back(std::move(new_uv));
            }
            // Deep-copy the CompiledFunction tree so async thread has its own
            // handles for all constants — prevents cross-thread handle sharing.
            std::unordered_map<CompiledFunction*, CompiledFunction*> fn_cache;
            std::vector<std::unique_ptr<CompiledFunction>> async_owned_fns;
            deepCopyFunctionTree(orig_closure->function, fn_cache, async_owned_fns);
            CompiledFunction* fn_copy = fn_cache[orig_closure->function];

            auto closure_copy = std::make_shared<VMClosure>(
                fn_copy, std::move(closed_upvalues));
            // V-CONC-007: Async VMs get their own StdLib instance to avoid
            // accessing the main thread's stack-local StdLib after scope exit.
            // Governance and module_resolver are also not shared (not thread-safe).
            auto async_stdlib = std::make_shared<stdlib::StdLib>();
            std::string file = fn->source_file;
            // V-CONC-006: Deep-copy globals to isolate async thread from shared containers
            std::unordered_map<std::string, interpreter::NaabVal> globals_copy;
            for (auto& [k, v] : globals_) {
                globals_copy[k] = v.deepCopy();
            }

            auto future_val = std::make_shared<interpreter::FutureValue>();
            future_val->description = "async fn " + fn->name;
            future_val->func_name = fn->name;

            // Thread-local handle allocators ensure each async thread gets its
            // own handle range — no cross-thread handle reuse, no TOCTOU race.
            auto shared_future = std::async(std::launch::async,
                [closure_copy, args, async_stdlib, file, globals_copy,
                 owned_fns = std::move(async_owned_fns)]() mutable -> interpreter::NaabVal {
                    interpreter::NaabVal safe_result;
                    {
                        VM async_vm;
                        async_vm.setStdlib(async_stdlib.get());
                        async_vm.setCurrentFile(file);
                        async_vm.setGlobals(globals_copy);
                        auto result = async_vm.callNaabFunction(
                            interpreter::NaabVal::makeVMClosure(closure_copy), args);
                        safe_result = result.deepCopy();
                    }
                    // async_vm fully destructed — safe_result has independent handles
                    return safe_result;
                }).share();

            future_val->future = shared_future;
            for (int si = 0; si < argc + 1; si++)
                *(stack_top_ - 1 - si) = interpreter::NaabVal();
            stack_top_ -= (argc + 1);  // Pop args + callee
            syncTaintTop();
            push(interpreter::NaabVal::makeFuture(future_val));
            return true;
        }

        // Generator function: return GeneratorValue (eagerly evaluated when iterated)
        if (fn->is_generator) {
            interpreter::NaabVal* args_start = stack_top_ - argc;
            std::vector<interpreter::NaabVal> args(args_start, stack_top_);
            auto closure_copy = callee.asVMClosure();

            auto gen = std::make_shared<interpreter::GeneratorValue>();
            // Store the VM closure and args for later execution
            // We'll use a shared_ptr<FunctionValue> wrapper for compatibility
            gen->args = std::move(args);
            // Store the closure in the generator for later use
            // We'll eagerly run it at iteration time via callNaabFunction

            for (int si = 0; si < argc + 1; si++)
                *(stack_top_ - 1 - si) = interpreter::NaabVal();
            stack_top_ -= (argc + 1);  // Pop args + callee
            syncTaintTop();
            // Return the callee+args bundled — we'll eagerly collect yields when iterated
            // Use a dict to carry the closure + args
            std::unordered_map<std::string, interpreter::NaabVal> gen_info;
            gen_info["__generator__"] = interpreter::NaabVal::makeVMClosure(closure_copy);
            std::vector<interpreter::NaabVal> gen_args_list(gen->args.begin(), gen->args.end());
            gen_info["__args__"] = interpreter::NaabVal::makeList(std::move(gen_args_list));
            gen_info["__is_generator__"] = interpreter::NaabVal::makeBool(true);
            push(interpreter::NaabVal::makeDict(std::move(gen_info)));
            return true;
        }

        return callFunction(closure.get(), argc);
    }
    // Built-in functions stored as "__builtin__:name" marker strings
    if (callee.isString() && callee.asString().substr(0, 12) == "__builtin__:") {
        std::string name = callee.asString().substr(12);
        // Check arg taint for sanitizer detection and sink enforcement
        bool any_arg_tainted = false;
        for (int i = 0; i < argc; i++) {
            if (peekTaint(i)) { any_arg_tainted = true; break; }
        }
        // Taint sink check for print/println — governance may list "print" as a sink
        if (any_arg_tainted && governance_ && governance_->isActive() &&
            (name == "print" || name == "println")) {
            auto& cf = frames_[frame_count_-1];
            int gov_line = cf.function->chunk.getLine(
                static_cast<int>(cf.ip - cf.function->chunk.code.data()));
            std::string terr = governance_->checkTaintedSink(
                "(print-arg)", "print", current_file_, gov_line);
            if (!terr.empty()) runtimeError("%s", terr.c_str());
        }
        interpreter::NaabVal* args_ptr = stack_top_ - argc;
        interpreter::NaabVal result = callBuiltinFunction(name, argc, args_ptr);
        // Clear stale slots, then pop args + callee
        for (int si = 0; si < argc + 1; si++)
            *(stack_top_ - 1 - si) = interpreter::NaabVal();
        stack_top_ -= (argc + 1);
        syncTaintTop();
        push(std::move(result));
        // Governance: sanitizer builtins (int, float, string, bool) clear taint
        if (governance_ && governance_->isActive() && any_arg_tainted) {
            if (governance_->isSanitizer(name)) {
                peekTaint(0) = false;
            } else {
                peekTaint(0) = true;  // Propagate taint through non-sanitizer calls
            }
        }
        return true;
    }
    // Stdlib module markers called as functions: "use string" then "string(42)"
    // Dispatch to builtin function of the same name (e.g., string, int, float, bool)
    if (callee.isString() && callee.asString().substr(0, 18) == "__stdlib_module__:") {
        std::string mod_name = callee.asString().substr(18);
        // Finding C fix: governance checks for direct stdlib function references via OP_CALL
        if (governance_) {
            if (mod_name == "file") {
                // Conservative: treat direct file module call as "write" (most restrictive)
                // since the specific function name is not available from the callee string
                std::string err = governance_->checkFilesystemAllowed("write");
                if (!err.empty()) runtimeError("%s", err.c_str());
            } else if (mod_name == "http") {
                std::string err = governance_->checkNetworkAllowed();
                if (!err.empty()) runtimeError("%s", err.c_str());
            }
            // Generic dangerous-calls policy check for ALL modules (Finding C parity)
            if (governance_->isActive()) {
                std::string full_call = mod_name + "." + "call";
                std::string derr = governance_->checkDangerousCall("naab", full_call, 0);
                if (!derr.empty()) runtimeError("%s", derr.c_str());
            }
            if (governance_->isActive()) {
                for (int ai = 0; ai < argc; ai++) {
                    ptrdiff_t arg_offset = (stack_top_ - argc + ai) - stack_.get();
                    if (taint_stack_[arg_offset]) {
                        std::string arg_label = "argument " + std::to_string(ai) + " of '" + mod_name + "()'";
                        governance_->markTainted(arg_label);
                        std::string terr = governance_->checkTaintedSink(
                            arg_label, mod_name, current_file_, 0);
                        governance_->clearTaint(arg_label);
                        if (!terr.empty()) runtimeError("%s", terr.c_str());
                    }
                }
            }
        }
        interpreter::NaabVal* args_ptr = stack_top_ - argc;
        interpreter::NaabVal result = callBuiltinFunction(mod_name, argc, args_ptr);
        for (int si = 0; si < argc + 1; si++)
            *(stack_top_ - 1 - si) = interpreter::NaabVal();
        stack_top_ -= (argc + 1);
        syncTaintTop();
        push(std::move(result));
        // Taint source/sanitizer post-call check
        if (governance_ && governance_->isActive()) {
            if (governance_->isTaintSource(mod_name)) {
                peekTaint(0) = true;
            } else if (governance_->isSanitizer(mod_name)) {
                peekTaint(0) = false;
            }
        }
        return true;
    }
    // Block method calls: dispatch to executor->callFunction(member_path, args)
    if (callee.isBlock()) {
        auto& block = callee.asBlock();
        auto* executor = block->getExecutor();
        if (executor && !block->member_path.empty()) {
            std::vector<interpreter::NaabVal> args;
            interpreter::NaabVal* args_start = stack_top_ - argc;
            for (int i = 0; i < argc; i++) {
                args.push_back(args_start[i]);
            }
            interpreter::NaabVal result = executor->callFunction(block->member_path, args);
            for (int si = 0; si < argc + 1; si++)
                *(stack_top_ - 1 - si) = interpreter::NaabVal();
            stack_top_ -= (argc + 1);
            syncTaintTop();
            push(std::move(result));
            return true;
        }
    }
    return false;
}

bool VM::callFunction(VMClosure* closure, int argc) {
    CompiledFunction* fn = closure->function;
    // Validate arg count
    if (argc < fn->arity) {
        runtimeError("Expected at least %d arguments but got %d", fn->arity, argc);
    }
    if (argc > fn->max_arity) {
        runtimeError("Expected at most %d arguments but got %d", fn->max_arity, argc);
    }

    // Governance: check call depth + input contracts
    if (governance_) {
        std::string err = governance_->checkCallDepth(static_cast<size_t>(frame_count_ + 1));
        if (!err.empty()) runtimeError("%s", err.c_str());

        // Check function input contracts
        if (governance_->isActive() && !fn->name.empty()) {
            std::vector<std::string> arg_types;
            interpreter::NaabVal* args_start = stack_top_ - argc;
            for (int i = 0; i < argc; i++) {
                arg_types.push_back(args_start[i].getTypeName());
            }
            std::string cerr = governance_->checkFunctionInputContract(fn->name, arg_types);
            if (!cerr.empty()) runtimeError("%s", cerr.c_str());
        }
    }

    if (frame_count_ >= static_cast<int>(FRAMES_MAX)) {
        runtimeError("Stack overflow (call depth exceeded)");
    }

    CallFrame* new_frame = &frames_[frame_count_++];
    new_frame->function = fn;
    new_frame->closure = closure;
    new_frame->ip = fn->chunk.code.data();
    // The callee + args are already on the stack
    // slots points to the callee's slot (slot 0 = function itself)
    new_frame->slots = stack_top_ - argc - 1;
    new_frame->handler_count = 0;
    new_frame->source_file = fn->source_file;

    // Debugger: push call frame for backtrace
    if (debugger_ && debugger_->isActive()) {
        int line = fn->chunk.getLine(0);
        std::string loc = fn->source_file + ":" + std::to_string(line);
        naab::debugger::CallFrame dbg_frame(
            fn->name.empty() ? "<anonymous>" : fn->name, loc, frame_count_ - 1);
        debugger_->pushFrame(dbg_frame);
    }

    // Profiler: start timing this function
    if (profile_) {
        profiling::Profiler::instance().startFunction(
            fn->name.empty() ? "<anonymous>" : fn->name);
    }

    return true;
}

interpreter::NaabVal VM::callNaabFunction(interpreter::NaabVal fn,
                                          const std::vector<interpreter::NaabVal>& args) {
    if (!fn.isVMClosure()) {
        runtimeError("callNaabFunction: value is not a function");
    }

    if (++run_depth_ > MAX_RUN_DEPTH) {
        --run_depth_;
        runtimeError("Maximum callback nesting depth exceeded (%d). "
                     "This usually indicates infinite recursion in stdlib callbacks.",
                     MAX_RUN_DEPTH);
    }

    // Save stack position — we'll restore it after the call so the
    // caller's stack state isn't affected (important for re-entrant calls
    // from stdlib callbacks like array.map_fn)
    interpreter::NaabVal* saved_stack_top = stack_top_;

    // Push function + args onto stack
    push(fn);  // callee slot
    for (auto& arg : args) {
        push(arg);
    }

    // Set up the call
    auto& closure = fn.asVMClosure();
    if (!callFunction(closure.get(), static_cast<int>(args.size()))) {
        interpreter::NaabVal* old_top = stack_top_;
        stack_top_ = saved_stack_top;
        for (auto* p = stack_top_; p < old_top; p++)
            *p = interpreter::NaabVal();
        runtimeError("callNaabFunction: failed to call function");
    }

    // Run dispatch loop for this call (stops when OP_RETURN pops back)
    // Use frame_count_ - 1 so that inner function returns (which bring
    // frame_count_ back to this level) don't prematurely exit run().
    // Only when THIS function's own OP_RETURN fires (frame_count_ drops
    // below this level) does run() return.
    int saved_stop = stop_frame_count_;
    stop_frame_count_ = frame_count_ - 1;
    interpreter::NaabVal result = run();
    stop_frame_count_ = saved_stop;

    // Restore stack to before the call — the result is returned by value,
    // not left on the stack. Clear stale slots to release handles.
    {
        interpreter::NaabVal* old_top = stack_top_;
        stack_top_ = saved_stack_top;
        for (auto* p = stack_top_; p < old_top; p++)
            *p = interpreter::NaabVal();
    }
    syncTaintTop();
    --run_depth_;
    return result;
}

std::shared_ptr<std::unordered_map<std::string, interpreter::NaabVal>>
VM::importModule(const std::string& module_path) {
    // Check cache
    auto cache_it = module_cache_.find(module_path);
    if (cache_it != module_cache_.end()) {
        return cache_it->second;
    }

    // Load the module file via ModuleResolver
    auto module = module_resolver_->loadModule(std::filesystem::path(module_path));
    if (!module || !module->ast) {
        runtimeError("Failed to load module: %s", module_path.c_str());
    }

    // Compile the module (skip main blocks)
    Compiler module_compiler;
    auto* module_fn = module_compiler.compileModule(*module->ast, module_path);
    if (!module_fn) {
        runtimeError("Failed to compile module: %s (%s)",
                     module_path.c_str(), module_compiler.getLastError().c_str());
    }

    // Execute the module in a fresh VM
    VM module_vm;
    module_vm.setStdlib(stdlib_);
    module_vm.setModuleResolver(module_resolver_);
    module_vm.setCurrentFile(module_path);
    module_vm.setGovernance(governance_);  // Propagate governance to modules
    module_vm.module_loading_depth_ = module_loading_depth_ + 1;

    // Share the module cache so sub-imports can find already-loaded modules
    module_vm.module_cache_ = module_cache_;

    module_vm.execute(module_fn);

    // Take ownership of compiled functions so VMClosure pointers remain valid
    auto compiled_fns = module_compiler.takeCompiledFunctions();
    for (auto& fn : compiled_fns) {
        owned_functions_.push_back(std::move(fn));
    }
    // Also take functions from the sub-VM (from any sub-imports it did)
    for (auto& fn : module_vm.owned_functions_) {
        owned_functions_.push_back(std::move(fn));
    }

    // Collect module exports: all user-defined globals
    // Skip names that still have their default prelude values (not modified by module code)
    auto exports = std::make_shared<std::unordered_map<std::string, interpreter::NaabVal>>();
    for (auto& [name, val] : module_vm.globals_) {
        // Skip globals whose values are builtin/stdlib markers (prelude defaults)
        if (val.isString()) {
            const std::string& sv = val.asString();
            if (sv.size() >= 12 && sv.substr(0, 12) == "__builtin__:") continue;
            if (sv.size() >= 18 && sv.substr(0, 18) == "__stdlib_module__:") {
                    // Only filter prelude modules (every VM already has them)
                    // Let non-prelude use bindings (csv, regex, uuid, etc.) propagate
                    static const std::unordered_set<std::string> prelude = {
                        "array", "dict", "io", "file", "debug", "bolo", "env",
                        "math", "json", "http", "crypto", "time", "os"
                    };
                    std::string mod_name = sv.substr(18);
                    if (prelude.count(mod_name)) continue;
                }
        }
        (*exports)[name] = val;
    }

    // Merge sub-module caches back
    for (auto& [path, cached] : module_vm.module_cache_) {
        module_cache_[path] = cached;
    }

    module_cache_[module_path] = exports;
    return exports;
}

interpreter::NaabVal VM::callBuiltinMethod(interpreter::NaabVal& obj, const std::string& method,
                                           int argc, interpreter::NaabVal* args) {
    // String methods
    if (obj.isString()) {
        const std::string& s = obj.asString();
        if (method == "length" || method == "size") {
            return interpreter::NaabVal::makeInt(static_cast<int>(s.size()));
        }
        if (method == "upper" || method == "toUpperCase") {
            std::string result = s;
            for (auto& c : result) c = static_cast<char>(toupper(c));
            return interpreter::NaabVal::makeString(std::move(result));
        }
        if (method == "lower" || method == "toLowerCase") {
            std::string result = s;
            for (auto& c : result) c = static_cast<char>(tolower(c));
            return interpreter::NaabVal::makeString(std::move(result));
        }
        if (method == "trim") {
            std::string result = s;
            auto start = result.find_first_not_of(" \t\n\r");
            auto end = result.find_last_not_of(" \t\n\r");
            if (start == std::string::npos) return interpreter::NaabVal::makeString("");
            return interpreter::NaabVal::makeString(result.substr(start, end - start + 1));
        }
        if (method == "contains" || method == "includes") {
            if (argc < 1) runtimeError("contains() requires 1 argument");
            return interpreter::NaabVal::makeBool(s.find(args[0].toString()) != std::string::npos);
        }
        if (method == "split") {
            if (argc < 1) runtimeError("split() requires 1 argument");
            std::string delim = args[0].toString();
            std::vector<interpreter::NaabVal> parts;
            size_t start = 0, end;
            while ((end = s.find(delim, start)) != std::string::npos) {
                parts.push_back(interpreter::NaabVal::makeString(s.substr(start, end - start)));
                start = end + delim.size();
            }
            parts.push_back(interpreter::NaabVal::makeString(s.substr(start)));
            return interpreter::NaabVal::makeList(std::move(parts));
        }
        if (method == "isEmpty") {
            return interpreter::NaabVal::makeBool(s.empty());
        }
        if (method == "indexOf" || method == "index_of") {
            if (argc < 1) runtimeError("indexOf() requires 1 argument");
            auto pos = s.find(args[0].toString());
            return interpreter::NaabVal::makeInt(pos == std::string::npos ? -1 : static_cast<int>(pos));
        }
        if (method == "lastIndexOf") {
            if (argc < 1) runtimeError("lastIndexOf() requires 1 argument");
            auto pos = s.rfind(args[0].toString());
            return interpreter::NaabVal::makeInt(pos == std::string::npos ? -1 : static_cast<int>(pos));
        }
        if (method == "substring" || method == "substr") {
            if (argc < 1) runtimeError("substring() requires at least 1 argument");
            int start = args[0].toInt();
            if (start < 0) start = 0;
            if (start >= static_cast<int>(s.size())) return interpreter::NaabVal::makeString("");
            if (argc >= 2) {
                int end_val = args[1].toInt();
                if (end_val < start) return interpreter::NaabVal::makeString("");
                return interpreter::NaabVal::makeString(s.substr(start, end_val - start));
            }
            return interpreter::NaabVal::makeString(s.substr(start));
        }
        if (method == "slice") {
            if (argc < 1) runtimeError("slice() requires at least 1 argument");
            int len = static_cast<int>(s.size());
            int start = args[0].toInt();
            if (start < 0) start += len;
            if (start < 0) start = 0;
            int end_val = argc >= 2 ? args[1].toInt() : len;
            if (end_val < 0) end_val += len;
            if (end_val > len) end_val = len;
            if (start >= end_val) return interpreter::NaabVal::makeString("");
            return interpreter::NaabVal::makeString(s.substr(start, end_val - start));
        }
        if (method == "replace") {
            if (argc < 2) runtimeError("replace() requires 2 arguments");
            std::string result = s;
            std::string from = args[0].toString();
            std::string to = args[1].toString();
            size_t pos = result.find(from);
            if (pos != std::string::npos) result.replace(pos, from.size(), to);
            return interpreter::NaabVal::makeString(std::move(result));
        }
        if (method == "startsWith" || method == "starts_with") {
            if (argc < 1) runtimeError("startsWith() requires 1 argument");
            std::string prefix = args[0].toString();
            return interpreter::NaabVal::makeBool(s.substr(0, prefix.size()) == prefix);
        }
        if (method == "endsWith" || method == "ends_with") {
            if (argc < 1) runtimeError("endsWith() requires 1 argument");
            std::string suffix = args[0].toString();
            if (suffix.size() > s.size()) return interpreter::NaabVal::makeBool(false);
            return interpreter::NaabVal::makeBool(s.substr(s.size() - suffix.size()) == suffix);
        }
        if (method == "char_at") {
            if (argc < 1) runtimeError("char_at() requires 1 argument");
            int idx = args[0].toInt();
            if (idx < 0) idx += static_cast<int>(s.size());
            if (idx < 0 || idx >= static_cast<int>(s.size()))
                runtimeError("String index %d out of bounds (length %d)", idx, static_cast<int>(s.size()));
            return interpreter::NaabVal::makeString(std::string(1, s[idx]));
        }
        if (method == "reverse") {
            std::string result(s.rbegin(), s.rend());
            return interpreter::NaabVal::makeString(std::move(result));
        }
        if (method == "repeat") {
            if (argc < 1) runtimeError("repeat() requires 1 argument");
            int count = args[0].toInt();
            std::string result;
            for (int i = 0; i < count; i++) result += s;
            return interpreter::NaabVal::makeString(std::move(result));
        }
        if (method == "find") {
            if (argc < 1) runtimeError("find() requires 1 argument");
            auto pos = s.find(args[0].toString());
            return interpreter::NaabVal::makeInt(pos == std::string::npos ? -1 : static_cast<int>(pos));
        }
        if (method == "pad_left") {
            if (argc < 1 || argc > 2) runtimeError("pad_left() takes 1-2 arguments (width[, fill_char])");
            int width = args[0].toInt();
            char fill = ' ';
            if (argc == 2) {
                auto fs = args[1].toString();
                if (fs.length() != 1) runtimeError("pad_left() fill_char must be exactly 1 character");
                fill = fs[0];
            }
            if (static_cast<int>(s.length()) < width) {
                return interpreter::NaabVal::makeString(std::string(width - s.length(), fill) + s);
            }
            return interpreter::NaabVal::makeString(s);
        }
        if (method == "pad_right") {
            if (argc < 1 || argc > 2) runtimeError("pad_right() takes 1-2 arguments (width[, fill_char])");
            int width = args[0].toInt();
            char fill = ' ';
            if (argc == 2) {
                auto fs = args[1].toString();
                if (fs.length() != 1) runtimeError("pad_right() fill_char must be exactly 1 character");
                fill = fs[0];
            }
            std::string result = s;
            if (static_cast<int>(result.length()) < width) result.append(width - result.length(), fill);
            return interpreter::NaabVal::makeString(std::move(result));
        }
        // Common misspellings and camelCase→snake_case hints
        if (method == "len") {
            runtimeError("Unknown method: .len()\n  Did you mean: .length() or the builtin len(x)?");
        }
        if (method == "charAt") {
            runtimeError("Unknown method: .charAt()\n  Did you mean: .char_at()? NAAb uses snake_case.");
        }
        if (method == "startsWith") {
            runtimeError("Unknown method: .startsWith()\n  Did you mean: .starts_with()? NAAb uses snake_case.");
        }
        if (method == "endsWith") {
            runtimeError("Unknown method: .endsWith()\n  Did you mean: .ends_with()? NAAb uses snake_case.");
        }
        if (method == "toUpperCase") {
            runtimeError("Unknown method: .toUpperCase()\n  Did you mean: .upper()?");
        }
        if (method == "toLowerCase") {
            runtimeError("Unknown method: .toLowerCase()\n  Did you mean: .lower()?");
        }
        if (method == "strip") {
            runtimeError("Unknown method: .strip()\n  Did you mean: .trim()?");
        }
        runtimeError("String has no method '%s'\n\n"
                     "  Available string methods:\n"
                     "    .length(), .upper(), .lower(), .trim(), .split(delim),\n"
                     "    .contains(str), .starts_with(str), .ends_with(str),\n"
                     "    .indexOf(str), .index_of(str), .find(str),\n"
                     "    .substring(start, end), .slice(start, end),\n"
                     "    .replace(old, new), .char_at(i), .repeat(n), .reverse(),\n"
                     "    .pad_left(width, char), .pad_right(width, char), .isEmpty()\n", method.c_str());
    }

    // List methods
    if (obj.isList()) {
        auto& list = obj.asList();
        if (method == "length" || method == "size") {
            return interpreter::NaabVal::makeInt(static_cast<int>(list.size()));
        }
        if (method == "push" || method == "add" || method == "append") {
            if (argc < 1) runtimeError("push() requires 1 argument");
            list.push_back(args[0]);
            return interpreter::NaabVal::makeNull();
        }
        if (method == "pop") {
            if (list.empty()) runtimeError("Cannot pop from empty list");
            interpreter::NaabVal val = std::move(list.back());
            list.pop_back();
            return val;
        }
        if (method == "get") {
            if (argc < 1) runtimeError("get() requires 1 argument");
            int idx = args[0].toInt();
            if (idx < 0 || idx >= static_cast<int>(list.size())) {
                // Return default if provided, else null (safe access like dict.get)
                if (argc >= 2) return args[1];
                return interpreter::NaabVal::makeNull();
            }
            return list[static_cast<size_t>(idx)];
        }
        if (method == "isEmpty") {
            return interpreter::NaabVal::makeBool(list.empty());
        }
        if (method == "contains" || method == "includes") {
            if (argc < 1) runtimeError("contains() requires 1 argument");
            for (auto& item : list) {
                if (item.toString() == args[0].toString()) return interpreter::NaabVal::makeBool(true);
            }
            return interpreter::NaabVal::makeBool(false);
        }
        if (method == "join") {
            std::string delim = argc > 0 ? args[0].toString() : ",";
            std::string result;
            for (size_t i = 0; i < list.size(); i++) {
                if (i > 0) result += delim;
                result += list[i].toString();
            }
            return interpreter::NaabVal::makeString(std::move(result));
        }
        if (method == "reverse" || method == "reversed") {
            std::vector<interpreter::NaabVal> reversed(list.rbegin(), list.rend());
            return interpreter::NaabVal::makeList(std::move(reversed));
        }
        if (method == "take") {
            if (argc < 1) runtimeError("take() requires 1 argument");
            int count = args[0].toInt();
            if (count > static_cast<int>(list.size())) count = static_cast<int>(list.size());
            if (count < 0) count = 0;
            std::vector<interpreter::NaabVal> result(list.begin(), list.begin() + count);
            return interpreter::NaabVal::makeList(std::move(result));
        }
        if (method == "clone" || method == "copy") {
            std::vector<interpreter::NaabVal> result(list.begin(), list.end());
            return interpreter::NaabVal::makeList(std::move(result));
        }
        if (method == "indexOf" || method == "findIndex" || method == "index_of") {
            if (argc < 1) runtimeError("indexOf() requires 1 argument");
            for (size_t i = 0; i < list.size(); i++) {
                if (list[i].toString() == args[0].toString())
                    return interpreter::NaabVal::makeInt(static_cast<int>(i));
            }
            return interpreter::NaabVal::makeInt(-1);
        }
        if (method == "first") {
            if (list.empty()) return interpreter::NaabVal::makeNull();
            return list[0];
        }
        if (method == "last") {
            if (list.empty()) return interpreter::NaabVal::makeNull();
            return list[list.size() - 1];
        }
        if (method == "sort") {
            // Sort in-place via shared_ptr reference
            auto& arr = obj.asList();
            if (argc >= 1 && args[0].isVMClosure()) {
                // Comparator sort: fn(a, b) returns <0, 0, >0
                auto comp_fn = args[0];
                std::sort(arr.begin(), arr.end(), [this, &comp_fn](const interpreter::NaabVal& a, const interpreter::NaabVal& b) {
                    auto res = callNaabFunction(comp_fn, {a, b});
                    if (res.isInt()) return res.asInt() < 0;
                    if (res.isDouble()) return res.asDouble() < 0;
                    return false;
                });
            } else {
                // Default sort: numeric then string
                std::sort(arr.begin(), arr.end(), [](const interpreter::NaabVal& a, const interpreter::NaabVal& b) {
                    bool a_num = a.isInt() || a.isDouble();
                    bool b_num = b.isInt() || b.isDouble();
                    if (a_num && b_num) {
                        double av = a.isInt() ? static_cast<double>(a.asInt()) : a.asDouble();
                        double bv = b.isInt() ? static_cast<double>(b.asInt()) : b.asDouble();
                        return av < bv;
                    }
                    return a.toString() < b.toString();
                });
            }
            return obj;
        }
        if (method == "slice") {
            if (argc < 1) runtimeError("slice() requires at least 1 argument");
            int len = static_cast<int>(list.size());
            int start = args[0].toInt();
            if (start < 0) start += len;
            if (start < 0) start = 0;
            int end_val = argc >= 2 ? args[1].toInt() : len;
            if (end_val < 0) end_val += len;
            if (end_val > len) end_val = len;
            if (start >= end_val) return interpreter::NaabVal::makeList({});
            std::vector<interpreter::NaabVal> result(list.begin() + start, list.begin() + end_val);
            return interpreter::NaabVal::makeList(std::move(result));
        }
        if (method == "remove" || method == "removeAt") {
            if (argc < 1) runtimeError("remove() requires 1 argument");
            int idx = args[0].toInt();
            if (idx < 0 || idx >= static_cast<int>(list.size()))
                runtimeError("Index %d out of bounds", idx);
            interpreter::NaabVal val = list[idx];
            list.erase(list.begin() + idx);
            return val;
        }
        if (method == "shift") {
            if (list.empty()) runtimeError("Cannot shift from empty list");
            interpreter::NaabVal val = list[0];
            list.erase(list.begin());
            return val;
        }
        if (method == "unshift") {
            if (argc < 1) runtimeError("unshift() requires 1 argument");
            list.insert(list.begin(), args[0]);
            return interpreter::NaabVal::makeInt(static_cast<int>(list.size()));
        }
        if (method == "find") {
            if (argc < 1) runtimeError("find() requires 1 argument (predicate function)");
            for (const auto& item : list) {
                auto res = callNaabFunction(args[0], {item});
                if (res.toBool()) return item;
            }
            return interpreter::NaabVal::makeNull();
        }
        if (method == "for_each" || method == "forEach") {
            if (argc < 1) runtimeError("forEach() requires 1 argument (function)");
            for (const auto& item : list) {
                callNaabFunction(args[0], {item});
            }
            return interpreter::NaabVal::makeNull();
        }
        if (method == "insert") {
            if (argc < 2) runtimeError("insert() requires 2 arguments (index, value)");
            int idx = args[0].toInt();
            if (idx < 0) idx += static_cast<int>(list.size());
            if (idx < 0 || idx > static_cast<int>(list.size()))
                runtimeError("Insert index %d out of bounds", idx);
            list.insert(list.begin() + idx, args[1]);
            return interpreter::NaabVal::makeNull();
        }
        // Common misspellings
        if (method == "len") {
            runtimeError("Unknown method: .len()\n  Did you mean: .length() or the builtin len(x)?");
        }
        if (method == "append") {
            runtimeError("Unknown method: .append()\n  Did you mean: .push()?");
        }
        if (method == "includes") {
            runtimeError("Unknown method: .includes()\n  Did you mean: .contains()?");
        }
        if (method == "findIndex") {
            runtimeError("Unknown method: .findIndex()\n  Did you mean: .indexOf()?");
        }
        runtimeError("List has no method '%s'\n\n"
                     "  Available list methods:\n"
                     "    .length(), .size(), .push(item), .pop(), .shift(), .unshift(item),\n"
                     "    .get(i), .contains(item), .indexOf(item), .join(sep),\n"
                     "    .reverse(), .sort(), .slice(start, end), .first(), .last(),\n"
                     "    .find(fn), .forEach(fn), .insert(i, item),\n"
                     "    .isEmpty(), .flat(), .unique(), .clone()\n", method.c_str());
    }

    // Dict methods
    if (obj.isDict()) {
        auto& dict = obj.asDict();
        if (method == "size" || method == "length") {
            return interpreter::NaabVal::makeInt(static_cast<int>(dict.size()));
        }
        if (method == "get") {
            if (argc < 1) runtimeError("get() requires 1 argument");
            std::string key = args[0].toString();
            auto it = dict.find(key);
            if (it != dict.end()) return it->second;
            if (argc >= 2) return args[1]; // default value
            // "Did you mean?" hint when key not found and similar keys exist
            // Deduplicate: only show each (key, suggestion) pair once per execution
            if (!dict.empty()) {
                std::vector<std::string> keys;
                keys.reserve(dict.size());
                for (const auto& [k, v] : dict) { (void)v; keys.push_back(k); }
                auto suggestion = naab::error::suggestDictKey(key, keys);
                if (!suggestion.empty()) {
                    static std::unordered_set<std::string> seen_hints;
                    auto hint_key = key + "→" + suggestion;
                    if (seen_hints.insert(hint_key).second) {
                        fprintf(stderr, "[hint] dict.get(\"%s\") returned null — did you mean \"%s\"?\n",
                                key.c_str(), suggestion.c_str());
                    }
                }
            }
            return interpreter::NaabVal::makeNull();
        }
        if (method == "has" || method == "has_key" || method == "contains" || method == "containsKey") {
            if (argc < 1) runtimeError("has() requires 1 argument");
            return interpreter::NaabVal::makeBool(dict.count(args[0].toString()) > 0);
        }
        if (method == "keys") {
            std::vector<interpreter::NaabVal> keys;
            for (auto& [k, v] : dict) {
                (void)v;
                keys.push_back(interpreter::NaabVal::makeString(k));
            }
            return interpreter::NaabVal::makeList(std::move(keys));
        }
        if (method == "values") {
            std::vector<interpreter::NaabVal> values;
            for (auto& [k, v] : dict) {
                (void)k;
                values.push_back(v);
            }
            return interpreter::NaabVal::makeList(std::move(values));
        }
        if (method == "put" || method == "set") {
            if (argc < 2) runtimeError("put() requires 2 arguments");
            dict[args[0].toString()] = args[1];
            return interpreter::NaabVal::makeNull();
        }
        if (method == "remove" || method == "delete") {
            if (argc < 1) runtimeError("remove() requires 1 argument");
            dict.erase(args[0].toString());
            return interpreter::NaabVal::makeNull();
        }
        if (method == "isEmpty") {
            return interpreter::NaabVal::makeBool(dict.empty());
        }
        if (method == "clone" || method == "copy") {
            std::unordered_map<std::string, interpreter::NaabVal> copy(dict.begin(), dict.end());
            return interpreter::NaabVal::makeDict(std::move(copy));
        }
        if (method == "getString" || method == "getInt" || method == "getFloat" || method == "getBool") {
            if (argc < 1) runtimeError("%s() requires at least 1 argument", method.c_str());
            std::string key = args[0].toString();
            auto it = dict.find(key);
            if (it != dict.end()) return it->second;
            if (argc >= 2) return args[1];
            return interpreter::NaabVal::makeNull();
        }
        if (method == "entries") {
            std::vector<interpreter::NaabVal> entries;
            for (auto& [k, v] : dict) {
                std::vector<interpreter::NaabVal> entry;
                entry.push_back(interpreter::NaabVal::makeString(k));
                entry.push_back(v);
                entries.push_back(interpreter::NaabVal::makeList(std::move(entry)));
            }
            return interpreter::NaabVal::makeList(std::move(entries));
        }
        if (method == "merge") {
            if (argc < 1) runtimeError("merge() requires 1 argument");
            if (!args[0].isDict()) runtimeError("merge() argument must be a dict");
            auto copy = dict;
            for (auto& [k, v] : args[0].asDict()) copy[k] = v;
            return interpreter::NaabVal::makeDict(std::move(copy));
        }
        // Common misspellings
        if (method == "len") {
            runtimeError("Unknown method: .len()\n  Did you mean: .size()?");
        }
        if (method == "set") {
            runtimeError("Unknown method: .set()\n  Did you mean: .put(key, value)?");
        }
        if (method == "delete") {
            runtimeError("Unknown method: .delete()\n  Did you mean: .remove(key)?");
        }
        if (method == "containsKey") {
            runtimeError("Unknown method: .containsKey()\n  Did you mean: .has(key)?");
        }
        runtimeError("Dict has no method '%s'\n\n"
                     "  Available dict methods:\n"
                     "    .size(), .get(key), .get(key, default), .has(key),\n"
                     "    .put(key, value), .remove(key), .keys(), .values(),\n"
                     "    .entries(), .isEmpty(), .clone(), .merge(other)\n", method.c_str());
    }

    // Block method calls: dispatch to executor->callFunction
    if (obj.isBlock()) {
        auto& block = obj.asBlock();
        auto* executor = block->getExecutor();
        if (executor) {
            std::vector<interpreter::NaabVal> arg_vec(args, args + argc);
            return executor->callFunction(method, arg_vec);
        }
    }

    if (obj.isNull()) {
        runtimeError("Cannot call method '%s' on null\n\n"
                     "  Hint: The value may be null from env.get() or dict access.\n"
                     "  Use ?? to provide a default: value ?? \"fallback\"\n"
                     "  Or use env.get(key, default) for environment variables.",
                     method.c_str());
    }
    runtimeError("Cannot call method '%s' on %s", method.c_str(), obj.getTypeName().c_str());
}

interpreter::NaabVal VM::callStdlibMethod(const std::string& module, const std::string& method,
                                          int argc, interpreter::NaabVal* args) {
    if (!stdlib_) {
        runtimeError("Stdlib not available");
    }
    // Convert args to vector for stdlib call
    std::vector<interpreter::NaabVal> arg_vec(args, args + argc);
    auto mod = stdlib_->getModule(module);
    if (!mod) {
        runtimeError("Unknown stdlib module '%s'", module.c_str());
    }
    auto result = mod->call(method, arg_vec);
    // Write back mutated args to stack (stdlib functions like array.pop mutate args[0])
    // This updates the stack copy, which we then propagate to locals below
    for (int i = 0; i < argc; i++) {
        args[i] = arg_vec[i];
    }
    return result;
}


// ============================================================================
// Upvalues
// ============================================================================

std::shared_ptr<ObjUpvalue> VM::captureUpvalue(interpreter::NaabVal* local) {
    // Walk the open upvalue list to see if we already have one for this slot
    std::shared_ptr<ObjUpvalue> prev;
    std::shared_ptr<ObjUpvalue> current = open_upvalues_;

    while (current && current->location > local) {
        prev = current;
        current = current->next;
    }

    if (current && current->location == local) {
        return current;  // Reuse existing upvalue
    }

    // Create new upvalue
    auto upvalue = std::make_shared<ObjUpvalue>();
    upvalue->location = local;
    upvalue->is_open = true;
    upvalue->next = current;

    if (!prev) {
        open_upvalues_ = upvalue;
    } else {
        prev->next = upvalue;
    }

    return upvalue;
}

void VM::closeUpvalues(interpreter::NaabVal* last) {
    while (open_upvalues_ && open_upvalues_->location >= last) {
        auto upvalue = open_upvalues_;
        // Copy taint from stack position to upvalue before closing
        if (governance_) {
            size_t stack_offset = static_cast<size_t>(upvalue->location - stack_.get());
            upvalue->tainted = taint_stack_[stack_offset];
            // Copy lineage from stack slot to closed upvalue
            if (upvalue->tainted && !taint_lineage_map_.empty()) {
                auto lit = taint_lineage_map_.find(stack_offset);
                if (lit != taint_lineage_map_.end()) {
                    upvalue->lineage_func = lit->second.source_func;
                    upvalue->lineage_arg = lit->second.source_arg;
                }
            }
        }
        upvalue->closed = *upvalue->location;
        upvalue->location = &upvalue->closed;
        upvalue->is_open = false;
        open_upvalues_ = upvalue->next;
    }
}

// ============================================================================
// Helper Error Messages (DX parity with tree-walker interpreter)
// ============================================================================

std::string VM::getVariableHelper(const std::string& name) const {
    // Known non-NAAb identifiers — mirrors interpreter.cpp:293-388 exactly
    if (name == "Sys" || name == "System" || name == "sys") {
        return "\n  NAAb does not have a 'Sys' object. Use built-in functions directly:\n"
               "    print(\"hello\")          // instead of Sys.print(\"hello\")\n"
               "    print(\"error: oops\")    // instead of Sys.error(\"oops\")\n\n"
               "  IMPORTANT - Sys.callFunction is NOT needed in NAAb:\n"
               "    Functions are first-class values. Call them directly:\n"
               "      let fn = someDict.get(\"myFunc\")\n"
               "      let result = fn(arg1, arg2)    // NOT Sys.callFunction(fn, arg1, arg2)\n"
               "      // or: someDict.myFunc(arg1, arg2)\n\n"
               "  Common replacements:\n"
               "    Sys.callFunction(fn, a, b) -> fn(a, b)\n"
               "    Sys.print(msg)             -> print(msg)\n"
               "    Sys.exit(code)             -> // just return or end the block\n\n"
               "  Built-in functions: print, len, type, typeof, int, float, string, bool\n"
               "  For sleep: import time; time.sleep(seconds)\n"
               "  For exit:  NAAb has no exit(). End the main block or return from functions.";
    }
    if (name == "Console" || name == "console") {
        return "\n  NAAb does not have a 'Console' object. Use:\n"
               "    print(\"hello\")          // instead of Console.log(\"hello\")\n"
               "    print(\"error: oops\")    // instead of Console.error(\"oops\")";
    }
    if (name == "Math") {
        return "\n  NAAb math functions are in the 'math' module (lowercase):\n"
               "    import math\n"
               "    let x = math.sqrt(16)   // instead of Math.sqrt(16)\n"
               "    let pi = math.PI";
    }
    if (name == "Array") {
        return "\n  NAAb array functions are in the 'array' module (lowercase):\n"
               "    import array\n"
               "    array.push(myArr, item) // instead of Array.push(...)";
    }
    if (name == "String") {
        return "\n  NAAb string functions are in the 'string' module (lowercase):\n"
               "    import string\n"
               "    string.upper(myStr)     // instead of String.toUpperCase(...)";
    }
    if (name == "File" || name == "fs" || name == "FS") {
        return "\n  NAAb file functions are in the 'file' module:\n"
               "    import file\n"
               "    let content = file.read(\"path.txt\")";
    }
    if (name == "sleep") {
        return "\n  'sleep' is not a global built-in. It's in the time module:\n"
               "    import time\n"
               "    time.sleep(1.0)          // sleep for 1 second";
    }
    if (name == "exit") {
        return "\n  NAAb has no exit() function. To stop execution:\n"
               "    return              // from a function\n"
               "    // or just let the main block end naturally";
    }
    if (name == "error") {
        return "\n  'error' is not a built-in function. To print errors:\n"
               "    print(\"ERROR: something went wrong\")\n"
               "  To throw an error:\n"
               "    throw \"something went wrong\"";
    }
    if (name == "require" || name == "include") {
        return "\n  NAAb uses 'import' for modules, not 'require':\n"
               "    import \"path/to/module.naab\" as MyModule\n"
               "    import math        // stdlib module";
    }
    if (name == "callFunction") {
        return "\n  NAAb does not need callFunction(). Functions are first-class:\n"
               "    let fn = myDict.get(\"funcName\")\n"
               "    let result = fn(arg1, arg2)   // call directly\n"
               "    // or: myDict.funcName(arg1, arg2)";
    }
    if (name == "args" || name == "argv" || name == "arguments" || name == "ARGV") {
        return "\n  NAAb does not have a global '" + name + "' variable.\n"
               "    Use env.get_args() to access command-line arguments:\n\n"
               "    let args = env.get_args()\n"
               "    let cmd = args.get(0) ?? \"help\"\n";
    }
    if (name == "process" || name == "os" || name == "OS") {
        return "\n  NAAb does not have a '" + name + "' object.\n"
               "    For environment variables: env.get(\"VAR_NAME\")\n"
               "    For command-line arguments: let args = env.get_args()";
    }
    if (name == "this" || name == "self") {
        return "\n  NAAb does not use '" + name + "'. In structs, access fields directly:\n"
               "    struct Point { x: Int, y: Int }\n"
               "  In closures/dicts, capture variables from the enclosing scope.";
    }
    if (name == "True" || name == "False") {
        std::string correct = (name == "True") ? "true" : "false";
        return "\n  NAAb uses '" + correct + "' (lowercase), not Python's '" + name + "':\n"
               "    let x = " + correct + "\n"
               "  Inside <<python>> blocks, use Python's " + name + ".";
    }
    if (name == "None" || name == "nil" || name == "undefined") {
        return "\n  NAAb uses 'null' (not '" + name + "'):\n"
               "    let x = null\n"
               "  Inside <<python>> blocks, use Python's None.";
    }
    if (name == "Object" || name == "Map") {
        return "\n  NAAb dicts are created with literal syntax:\n"
               "    let d = {\"key\": \"value\"}\n"
               "    d.get(\"key\")    // access values\n"
               "    d.put(\"k\", v)   // set values";
    }
    if (name == "JSON") {
        return "\n  NAAb does not have a JSON object. Use the json module (lowercase):\n"
               "    let data = json.parse(str)      // instead of JSON.parse(str)\n"
               "    let str = json.stringify(data)   // instead of JSON.stringify(data)\n"
               "  Dicts are native: let d = {\"key\": \"value\"}";
    }
    if (name == "round" || name == "abs" || name == "floor" || name == "ceil" ||
        name == "sqrt" || name == "pow" || name == "min" || name == "max" ||
        name == "random" || name == "sin" || name == "cos") {
        return "\n  '" + name + "' is not a builtin. It's in the math module:\n"
               "    use math\n"
               "    let x = math." + name + "(...)\n";
    }
    if (name == "PI" || name == "pi" || name == "E") {
        std::string correct = (name == "E") ? "E" : "PI";
        return "\n  '" + name + "' is not a builtin. It's a math constant:\n"
               "    use math\n"
               "    let x = math." + correct + "\n";
    }
    if (name == "keys" || name == "values" || name == "entries") {
        return "\n  '" + name + "' is not a global function. Use dot-notation on a dict:\n"
               "    let k = my_dict." + name + "()\n";
    }
    if (name == "push" || name == "pop" || name == "shift" || name == "unshift" ||
        name == "sort" || name == "reverse" || name == "contains" || name == "find") {
        return "\n  '" + name + "' is not a global function. Use dot-notation:\n"
               "    my_array." + name + "(...)\n";
    }
    if (name == "upper" || name == "lower" || name == "trim" || name == "split" ||
        name == "replace" || name == "starts_with" || name == "ends_with") {
        return "\n  '" + name + "' is not a global function. Use dot-notation:\n"
               "    my_string." + name + "(...)\n"
               "  Or use the string module:\n"
               "    use string\n"
               "    string." + name + "(my_string, ...)\n";
    }
    if (name == "parseInt" || name == "parseFloat" || name == "Number") {
        return "\n  NAAb uses type cast builtins instead:\n"
               "    let x = int(\"42\")      // parseInt\n"
               "    let y = float(\"3.14\")  // parseFloat\n";
    }
    if (name == "toString" || name == "str") {
        return "\n  NAAb uses the string() cast builtin:\n"
               "    let s = string(42)   // \"42\"\n";
    }
    if (name == "sorted" || name == "reversed") {
        std::string fn = (name == "sorted") ? "sort" : "reverse";
        return "\n  '" + name + "' is not a builtin. Arrays have mutable methods:\n"
               "    my_array." + fn + "()   // mutates in place\n";
    }
    if (name == "enumerate" || name == "zip") {
        return "\n  NAAb does not have '" + name + "'. Use index-based loops:\n"
               "    for i in 0..len(arr) {\n"
               "        let item = arr[i]\n"
               "    }\n";
    }
    if (name == "map" || name == "filter" || name == "reduce") {
        return "\n  '" + name + "' is not a global function. Use the array module:\n"
               "    use array\n"
               "    array." + name + "_fn(arr, fn(x) { return x })\n";
    }
    if (name == "append" || name == "extend") {
        return "\n  NAAb arrays use push() instead of '" + name + "':\n"
               "    my_array.push(item)\n";
    }
    if (name == "spawn" || name == "task") {
        return "\n  NAAb uses 'async function' for concurrent tasks:\n"
               "    async function myTask() { ... }\n"
               "    let future = myTask()\n"
               "    let result = await future\n";
    }
    if (name == "wait" || name == "wait_all" || name == "waitAll") {
        return "\n  NAAb uses 'await' keyword, not wait()/wait_all():\n"
               "    let result = await future\n";
    }
    if (name == "format" || name == "sprintf" || name == "printf") {
        return "\n  NAAb uses string concatenation with + operator:\n"
               "    let msg = \"Hello \" + name + \"!\"\n"
               "  Or use string() for type conversion:\n"
               "    let msg = \"Count: \" + string(count)\n";
    }

    // Fall back to fuzzy matching via error_helpers (stdlib modules, Python operators,
    // common typos, Levenshtein distance)
    std::vector<std::string> candidates;
    candidates.reserve(globals_.size());
    for (const auto& [gname, _] : globals_) {
        // Skip internal markers
        if (gname.find("__stdlib_module__:") == 0) continue;
        if (gname.find("__builtin__:") == 0) continue;
        candidates.push_back(gname);
    }
    return naab::error::suggestForUndefinedVariable(name, candidates);
}

// ============================================================================
// Error reporting
// ============================================================================

void VM::runtimeError(const char* format, ...) {
    va_list args;
    va_start(args, format);
    va_list args_copy;
    va_copy(args_copy, args);
    int needed = vsnprintf(nullptr, 0, format, args_copy);
    va_end(args_copy);

    std::string msg(needed + 1, '\0');
    vsnprintf(msg.data(), msg.size(), format, args);
    va_end(args);
    msg.resize(needed);

    // If there's an exception handler, route through it instead of crashing
    if (!exception_handlers_.empty()) {
        // Use longjmp-like approach: throw a special exception that run() catches
        throw VMException(msg);
    }

    // No handlers — add stack trace and throw C++ exception
    for (int i = frame_count_ - 1; i >= 0; i--) {
        CallFrame* f = &frames_[i];
        CompiledFunction* fn = f->function;
        int offset = static_cast<int>(f->ip - fn->chunk.code.data()) - 1;
        int line = fn->chunk.getLine(offset);
        msg += "\n  at ";
        if (fn->name.empty()) {
            msg += "<script>";
        } else {
            msg += fn->name + "()";
        }
        msg += " [" + fn->source_file + ":" + std::to_string(line) + "]";
    }

    throw std::runtime_error(msg);
}

std::vector<std::string> VM::getStackTrace() const {
    std::vector<std::string> trace;
    for (int i = frame_count_ - 1; i >= 0; i--) {
        const CallFrame* f = &frames_[i];
        const CompiledFunction* fn = f->function;
        int offset = static_cast<int>(f->ip - fn->chunk.code.data()) - 1;
        int line = fn->chunk.getLine(offset);
        std::string entry = fn->name.empty() ? "<script>" : fn->name + "()";
        entry += " [" + fn->source_file + ":" + std::to_string(line) + "]";
        trace.push_back(std::move(entry));
    }
    return trace;
}

// ============================================================================
// Disassembler
// ============================================================================

static const char* opcodeName(OpCode op) {
    switch (op) {
        case OpCode::OP_CONST: return "OP_CONST";
        case OpCode::OP_NULL: return "OP_NULL";
        case OpCode::OP_TRUE: return "OP_TRUE";
        case OpCode::OP_FALSE: return "OP_FALSE";
        case OpCode::OP_POP: return "OP_POP";
        case OpCode::OP_POPN: return "OP_POPN";
        case OpCode::OP_DUP: return "OP_DUP";
        case OpCode::OP_SWAP: return "OP_SWAP";
        case OpCode::OP_ADD: return "OP_ADD";
        case OpCode::OP_SUB: return "OP_SUB";
        case OpCode::OP_MUL: return "OP_MUL";
        case OpCode::OP_DIV: return "OP_DIV";
        case OpCode::OP_MOD: return "OP_MOD";
        case OpCode::OP_NEG: return "OP_NEG";
        case OpCode::OP_NOT: return "OP_NOT";
        case OpCode::OP_EQ: return "OP_EQ";
        case OpCode::OP_NE: return "OP_NE";
        case OpCode::OP_LT: return "OP_LT";
        case OpCode::OP_LE: return "OP_LE";
        case OpCode::OP_GT: return "OP_GT";
        case OpCode::OP_GE: return "OP_GE";
        case OpCode::OP_IN: return "OP_IN";
        case OpCode::OP_GET_LOCAL: return "OP_GET_LOCAL";
        case OpCode::OP_SET_LOCAL: return "OP_SET_LOCAL";
        case OpCode::OP_GET_UPVALUE: return "OP_GET_UPVALUE";
        case OpCode::OP_SET_UPVALUE: return "OP_SET_UPVALUE";
        case OpCode::OP_GET_GLOBAL: return "OP_GET_GLOBAL";
        case OpCode::OP_SET_GLOBAL: return "OP_SET_GLOBAL";
        case OpCode::OP_DEFINE_GLOBAL: return "OP_DEFINE_GLOBAL";
        case OpCode::OP_GET_QUALIFIED: return "OP_GET_QUALIFIED";
        case OpCode::OP_JUMP: return "OP_JUMP";
        case OpCode::OP_JUMP_BACK: return "OP_JUMP_BACK";
        case OpCode::OP_JUMP_IF_FALSE: return "OP_JUMP_IF_FALSE";
        case OpCode::OP_JUMP_IF_TRUE: return "OP_JUMP_IF_TRUE";
        case OpCode::OP_JUMP_IF_NULL: return "OP_JUMP_IF_NULL";
        case OpCode::OP_CALL: return "OP_CALL";
        case OpCode::OP_CALL_METHOD: return "OP_CALL_METHOD";
        case OpCode::OP_RETURN: return "OP_RETURN";
        case OpCode::OP_RETURN_NULL: return "OP_RETURN_NULL";
        case OpCode::OP_CLOSURE: return "OP_CLOSURE";
        case OpCode::OP_CLOSE_UPVALUE: return "OP_CLOSE_UPVALUE";
        case OpCode::OP_LIST: return "OP_LIST";
        case OpCode::OP_DICT: return "OP_DICT";
        case OpCode::OP_GET_INDEX: return "OP_GET_INDEX";
        case OpCode::OP_SET_INDEX: return "OP_SET_INDEX";
        case OpCode::OP_GET_MEMBER: return "OP_GET_MEMBER";
        case OpCode::OP_SET_MEMBER: return "OP_SET_MEMBER";
        case OpCode::OP_RANGE: return "OP_RANGE";
        case OpCode::OP_STRUCT_NEW: return "OP_STRUCT_NEW";
        case OpCode::OP_STRUCT_INIT_FIELD: return "OP_STRUCT_INIT_FIELD";
        case OpCode::OP_COPY_VALUE: return "OP_COPY_VALUE";
        case OpCode::OP_GET_ITER: return "OP_GET_ITER";
        case OpCode::OP_ITER_NEXT: return "OP_ITER_NEXT";
        case OpCode::OP_DESTRUCTURE_LIST: return "OP_DESTRUCTURE_LIST";
        case OpCode::OP_DESTRUCTURE_DICT: return "OP_DESTRUCTURE_DICT";
        case OpCode::OP_IMPORT: return "OP_IMPORT";
        case OpCode::OP_IMPORT_NAME: return "OP_IMPORT_NAME";
        case OpCode::OP_IMPORT_WILDCARD: return "OP_IMPORT_WILDCARD";
        case OpCode::OP_EXPORT_NAME: return "OP_EXPORT_NAME";
        case OpCode::OP_TRY_BEGIN: return "OP_TRY_BEGIN";
        case OpCode::OP_TRY_END: return "OP_TRY_END";
        case OpCode::OP_THROW: return "OP_THROW";
        case OpCode::OP_CATCH_BEGIN: return "OP_CATCH_BEGIN";
        case OpCode::OP_FINALLY_BEGIN: return "OP_FINALLY_BEGIN";
        case OpCode::OP_FINALLY_END: return "OP_FINALLY_END";
        case OpCode::OP_POLYGLOT: return "OP_POLYGLOT";
        case OpCode::OP_GOV_CHECK_FUNC: return "OP_GOV_CHECK_FUNC";
        case OpCode::OP_GOV_TAINT_MARK: return "OP_GOV_TAINT_MARK";
        case OpCode::OP_GOV_TAINT_CLEAR: return "OP_GOV_TAINT_CLEAR";
        case OpCode::OP_GOV_TAINT_CHECK_ASSIGN: return "OP_GOV_TAINT_CHECK_ASSIGN";
        case OpCode::OP_GOV_CHECK_POLYGLOT_VARS: return "OP_GOV_CHECK_POLYGLOT_VARS";
        case OpCode::OP_GOV_CHECK_CONTRACT_IN: return "OP_GOV_CHECK_CONTRACT_IN";
        case OpCode::OP_GOV_CHECK_CONTRACT_OUT: return "OP_GOV_CHECK_CONTRACT_OUT";
        case OpCode::OP_GOV_INCREMENT_BLOCK: return "OP_GOV_INCREMENT_BLOCK";
        case OpCode::OP_YIELD: return "OP_YIELD";
        case OpCode::OP_AWAIT: return "OP_AWAIT";
        case OpCode::OP_MAKE_GENERATOR: return "OP_MAKE_GENERATOR";
        case OpCode::OP_MAKE_ASYNC: return "OP_MAKE_ASYNC";
        case OpCode::OP_RUNTIME_START: return "OP_RUNTIME_START";
        case OpCode::OP_RUNTIME_EXEC: return "OP_RUNTIME_EXEC";
    }
    return "OP_UNKNOWN";
}

// ============================================================================
// Debugger: variable inspection
// ============================================================================

std::map<std::string, interpreter::NaabVal> VM::getCurrentScopeVariables() const {
    std::map<std::string, interpreter::NaabVal> vars;
    if (frame_count_ <= 0) return vars;
    const CallFrame& cf = frames_[frame_count_ - 1];
    const auto& names = cf.function->local_names;
    for (size_t i = 0; i < names.size(); i++) {
        if (names[i].empty()) continue;
        // Only include slots that are within the current stack range
        if (&cf.slots[i] < stack_top_) {
            vars[names[i]] = cf.slots[i];
        }
    }
    return vars;
}

// ============================================================================
// Disassembler
// ============================================================================

void disassembleChunk(const Chunk& chunk, const std::string& name) {
    printf("== %s ==\n", name.c_str());
    int offset = 0;
    while (offset < static_cast<int>(chunk.code.size())) {
        offset = disassembleInstruction(chunk, offset);
    }
}

int disassembleInstruction(const Chunk& chunk, int offset) {
    printf("%04d ", offset);

    int line = chunk.getLine(offset);
    if (offset > 0 && line == chunk.getLine(offset - 1)) {
        printf("   | ");
    } else {
        printf("%4d ", line);
    }

    uint32_t instruction = chunk.code[static_cast<size_t>(offset)];
    OpCode op = decodeOp(instruction);
    uint32_t arg = decodeArg(instruction);

    const char* name_str = opcodeName(op);

    // Determine if this opcode uses a wide arg
    switch (op) {
        case OpCode::OP_CONST:
        case OpCode::OP_GET_LOCAL:
        case OpCode::OP_SET_LOCAL:
        case OpCode::OP_GET_UPVALUE:
        case OpCode::OP_SET_UPVALUE:
        case OpCode::OP_GET_GLOBAL:
        case OpCode::OP_SET_GLOBAL:
        case OpCode::OP_DEFINE_GLOBAL:
        case OpCode::OP_GET_QUALIFIED:
        case OpCode::OP_JUMP:
        case OpCode::OP_JUMP_BACK:
        case OpCode::OP_JUMP_IF_FALSE:
        case OpCode::OP_JUMP_IF_TRUE:
        case OpCode::OP_JUMP_IF_NULL:
        case OpCode::OP_CLOSURE:
        case OpCode::OP_CLOSE_UPVALUE:
        case OpCode::OP_LIST:
        case OpCode::OP_DICT:
        case OpCode::OP_GET_MEMBER:
        case OpCode::OP_SET_MEMBER:
        case OpCode::OP_STRUCT_NEW:
        case OpCode::OP_STRUCT_INIT_FIELD:
        case OpCode::OP_IMPORT:
        case OpCode::OP_IMPORT_NAME:
        case OpCode::OP_IMPORT_WILDCARD:
        case OpCode::OP_EXPORT_NAME:
        case OpCode::OP_TRY_BEGIN:
        case OpCode::OP_CATCH_BEGIN:
        case OpCode::OP_POLYGLOT:
            printf("%-24s %d", name_str, arg);
            // For constant references, show the value
            if (op == OpCode::OP_CONST && arg < chunk.constants.size()) {
                printf(" '");
                printf("%s", chunk.constants[arg].toString().c_str());
                printf("'");
            }
            if ((op == OpCode::OP_GET_GLOBAL || op == OpCode::OP_SET_GLOBAL ||
                 op == OpCode::OP_DEFINE_GLOBAL) && arg < chunk.constants.size()) {
                printf(" '%s'", chunk.constants[arg].toString().c_str());
            }
            break;

        case OpCode::OP_CALL: {
            uint8_t argc = decodeA(instruction);
            printf("%-24s %d args", name_str, argc);
            break;
        }

        case OpCode::OP_CALL_METHOD: {
            uint16_t name_idx = static_cast<uint16_t>((arg >> 8) & 0xFFFF);
            uint8_t argc = static_cast<uint8_t>(arg & 0xFF);
            printf("%-24s %d '%s' %d args", name_str, name_idx,
                   name_idx < chunk.constants.size() ? chunk.constants[name_idx].toString().c_str() : "?",
                   argc);
            break;
        }

        case OpCode::OP_POPN: {
            printf("%-24s %d", name_str, decodeA(instruction));
            break;
        }

        default:
            printf("%-24s", name_str);
            break;
    }

    printf("\n");
    return offset + 1;
}

} // namespace vm
} // namespace naab
