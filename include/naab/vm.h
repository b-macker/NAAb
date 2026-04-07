#pragma once
// NAAb Bytecode VM — Stack-based virtual machine with NaN-boxed values
// Replaces tree-walking interpreter for faster execution.
// The tree-walker remains available via --tree-walk flag.

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>

#include "naab_val.h"
#include "ast.h"

namespace naab {

// Forward declarations
namespace governance { class GovernanceEngine; }
namespace debugger { class Debugger; }
namespace stdlib { class StdLib; }
namespace modules { class ModuleResolver; }
namespace runtime { class Executor; }
namespace interpreter { class CycleDetector; }  // V-RT-008: GC

namespace vm {

// Internal exception for routing runtime errors through try/catch handlers
struct VMException : public std::runtime_error {
    VMException(const std::string& msg) : std::runtime_error(msg) {}
};

// ============================================================================
// Opcodes — 8-bit, ~90 instructions
// ============================================================================
enum class OpCode : uint8_t {
    // Stack & value operations
    OP_CONST,           // [idx:24] Push constant pool[idx]
    OP_NULL,            // Push null
    OP_TRUE,            // Push true
    OP_FALSE,           // Push false
    OP_POP,             // Pop and discard TOS
    OP_POPN,            // [n:8] Pop n values
    OP_DUP,             // Duplicate TOS
    OP_SWAP,            // Swap top two

    // Arithmetic & comparison
    OP_ADD,             // a + b (int/double/string/list)
    OP_SUB,             // a - b
    OP_MUL,             // a * b
    OP_DIV,             // a / b
    OP_MOD,             // a % b
    OP_NEG,             // -a
    OP_NOT,             // !a
    OP_EQ,              // a == b
    OP_NE,              // a != b
    OP_LT,              // a < b
    OP_LE,              // a <= b
    OP_GT,              // a > b
    OP_GE,              // a >= b
    OP_IN,              // item in container

    // Variable access
    OP_GET_LOCAL,       // [slot:24] Push locals[slot]
    OP_SET_LOCAL,       // [slot:24] Peek -> locals[slot]
    OP_GET_UPVALUE,     // [idx:24] Push upvalues[idx]
    OP_SET_UPVALUE,     // [idx:24] Peek -> upvalues[idx]
    OP_GET_GLOBAL,      // [name_idx:24] Push global by name
    OP_SET_GLOBAL,      // [name_idx:24] Peek -> set global
    OP_DEFINE_GLOBAL,   // [name_idx:24] Pop -> define global
    OP_GET_QUALIFIED,   // [name_idx:24] Qualified name lookup

    // Control flow
    OP_JUMP,            // [offset:24] Unconditional forward jump
    OP_JUMP_BACK,       // [offset:24] Unconditional backward jump
    OP_JUMP_IF_FALSE,   // [offset:24] Pop; jump if falsy
    OP_JUMP_IF_TRUE,    // [offset:24] Pop; jump if truthy
    OP_JUMP_IF_NULL,    // [offset:24] Pop; jump if null

    // Function calls
    OP_CALL,            // [argc:8] Call function with argc args
    OP_CALL_METHOD,     // [name_idx:16, argc:8] Call method on TOS obj
    OP_RETURN,          // Return TOS from function
    OP_RETURN_NULL,     // Return null (implicit return)
    OP_CLOSURE,         // [fn_idx:24] Create closure
    OP_CLOSE_UPVALUE,   // [slot:24] Close upvalue at stack slot

    // Object operations
    OP_LIST,            // [count:24] Pop count values -> list
    OP_DICT,            // [count:24] Pop count k/v pairs -> dict
    OP_GET_INDEX,       // Pop index, obj -> obj[index]
    OP_SET_INDEX,       // Pop value, index, obj -> obj[index] = value
    OP_GET_MEMBER,      // [name_idx:24] Pop obj -> obj.member
    OP_SET_MEMBER,      // [name_idx:24] Pop value, obj -> obj.member = value
    OP_RANGE,           // [inclusive:8] Pop end, start -> range list
    OP_STRUCT_NEW,      // [type_idx:24] Create struct from def
    OP_STRUCT_INIT_FIELD, // [name_idx:24] Pop value -> init TOS struct field
    OP_COPY_VALUE,      // Deep copy TOS (value semantics)

    // Iteration
    OP_GET_ITER,        // Pop iterable -> push iterator state
    OP_ITER_NEXT,       // [vars:8, jump:16] Advance or jump if done

    // Destructuring
    OP_DESTRUCTURE_LIST, // [count:8, rest_idx:8] Pop list -> push values
    OP_DESTRUCTURE_DICT, // [count:8] Pop dict -> push values

    // Module & import
    OP_IMPORT,          // [path_idx:24] Load module, push exports
    OP_IMPORT_NAME,     // [name_idx:24] Pop module -> extract export
    OP_IMPORT_WILDCARD, // [alias_idx:24] Pop module -> define all exports
    OP_EXPORT_NAME,     // [name_idx:24] Mark local as exported

    // Exception handling
    OP_TRY_BEGIN,       // [catch_offset:24] Push exception handler
    OP_TRY_END,         // Pop exception handler
    OP_THROW,           // Throw TOS as exception
    OP_CATCH_BEGIN,     // [var_slot:24] Store exception in local
    OP_FINALLY_BEGIN,   // Start finally block
    OP_FINALLY_END,     // End finally; re-throw if pending

    // Polyglot
    OP_POLYGLOT,        // [info_idx:24] Execute polyglot block

    // Governance
    OP_GOV_CHECK_FUNC,       // [name_idx:24] Check function governance
    OP_GOV_TAINT_MARK,       // [slot:24] Mark local as tainted
    OP_GOV_TAINT_CLEAR,      // [slot:24] Clear taint
    OP_GOV_TAINT_CHECK_ASSIGN, // [info:24] Check RHS taint
    OP_GOV_CHECK_POLYGLOT_VARS, // [info:24] Check bound vars taint
    OP_GOV_CHECK_CONTRACT_IN,   // [name_idx:24] Input contract
    OP_GOV_CHECK_CONTRACT_OUT,  // [name_idx:24] Output contract
    OP_GOV_INCREMENT_BLOCK,     // Increment polyglot block count

    // Async & generators
    OP_YIELD,           // Yield TOS; save VM state
    OP_AWAIT,           // Await future TOS
    OP_MAKE_GENERATOR,  // [fn_idx:24] Wrap as generator
    OP_MAKE_ASYNC,      // [fn_idx:24] Mark for async exec

    // Persistent runtime
    OP_RUNTIME_START,   // [name_idx:24, lang_idx:24] Create persistent runtime
    OP_RUNTIME_EXEC,    // [runtime_idx:24] Execute on persistent runtime
};

// ============================================================================
// Instruction encoding helpers
// ============================================================================

// Encode: [opcode:8][arg:24]
inline uint32_t encodeWide(OpCode op, uint32_t arg) {
    return (static_cast<uint32_t>(op) << 24) | (arg & 0x00FFFFFF);
}

// Encode: [opcode:8][a:8][b:8][c:8]
inline uint32_t encode3(OpCode op, uint8_t a, uint8_t b, uint8_t c) {
    return (static_cast<uint32_t>(op) << 24) |
           (static_cast<uint32_t>(a) << 16) |
           (static_cast<uint32_t>(b) << 8) |
           static_cast<uint32_t>(c);
}

// Decode
inline OpCode decodeOp(uint32_t instr) {
    return static_cast<OpCode>((instr >> 24) & 0xFF);
}

inline uint32_t decodeArg(uint32_t instr) {
    return instr & 0x00FFFFFF;
}

inline uint8_t decodeA(uint32_t instr) {
    return static_cast<uint8_t>((instr >> 16) & 0xFF);
}

inline uint8_t decodeB(uint32_t instr) {
    return static_cast<uint8_t>((instr >> 8) & 0xFF);
}

inline uint8_t decodeC(uint32_t instr) {
    return static_cast<uint8_t>(instr & 0xFF);
}

// ============================================================================
// Chunk — compiled bytecode for one function/scope
// ============================================================================

struct LineInfo {
    int offset;     // Bytecode offset
    int line;       // Source line number
};

struct Chunk {
    std::vector<uint32_t> code;
    std::vector<interpreter::NaabVal> constants;
    std::vector<LineInfo> lines;
    std::string source_file;

    // Add a constant, return its index
    int addConstant(interpreter::NaabVal value);

    // Emit an instruction
    void emit(uint32_t instruction, int line);

    // Emit a jump with placeholder, return offset to patch
    int emitJump(OpCode op, int line);

    // Backpatch jump target
    void patchJump(int offset);

    // Get source line for bytecode offset
    int getLine(int offset) const;

    // Current code size
    int codeSize() const { return static_cast<int>(code.size()); }
};

// ============================================================================
// Upvalue descriptor (for closures)
// ============================================================================

struct UpvalueDesc {
    uint8_t index;      // Stack slot or parent upvalue index
    bool is_local;      // true = from enclosing stack, false = from enclosing upvalue
};

// ============================================================================
// CompiledFunction — bytecode + metadata for one function
// ============================================================================

struct CompiledFunction {
    std::string name;
    int arity = 0;                  // Required params (min args)
    int max_arity = 0;              // With defaults (max args)
    int local_count = 0;            // Local variable slots needed
    Chunk chunk;
    std::vector<UpvalueDesc> upvalues;
    std::vector<ast::Type> param_types;
    ast::Type return_type{ast::TypeKind::Any};
    bool is_generator = false;
    bool is_async = false;
    std::string source_file;
    int source_line = 0;

    // Default parameter expressions (compiled as sub-chunks)
    std::vector<std::optional<Chunk>> default_chunks;

    // Debug info: slot index → variable name (for debugger variable inspection)
    std::vector<std::string> local_names;
};

// ============================================================================
// CallFrame — one frame on the call stack
// ============================================================================

struct CallFrame {
    CompiledFunction* function = nullptr;
    uint32_t* ip = nullptr;             // Instruction pointer
    interpreter::NaabVal* slots = nullptr;  // Base into value stack
    VMClosure* closure = nullptr;        // Closure with upvalues (if any)
    int handler_count = 0;               // Exception handlers in this frame
    std::string source_file;
};

// ============================================================================
// ObjUpvalue — heap-stored captured variable
// ============================================================================

struct ObjUpvalue {
    interpreter::NaabVal* location = nullptr;  // Points into stack while open
    interpreter::NaabVal closed;               // Value after closing
    std::shared_ptr<ObjUpvalue> next;          // Linked list (shared for safe cleanup)
    bool is_open = true;
    bool tainted = false;                      // Governance taint tracking
};

// ============================================================================
// ExceptionHandler — for try/catch/finally
// ============================================================================

struct ExceptionHandler {
    int frame_index = 0;
    uint32_t* catch_ip = nullptr;
    uint32_t* finally_ip = nullptr;
    interpreter::NaabVal* stack_base = nullptr;
    int handler_slot = -1;
};

// ============================================================================
// PolyglotBlockInfo — stored in constant pool
// ============================================================================

struct PolyglotBlockInfo {
    std::string language;
    std::string code;
    std::string return_type;
    std::vector<int> bound_var_slots;
    std::vector<std::string> bound_var_names;
};

// ============================================================================
// VMClosure — compiled function + captured upvalues
// ============================================================================

struct VMClosure {
    CompiledFunction* function = nullptr;
    std::vector<std::shared_ptr<ObjUpvalue>> upvalues;

    VMClosure() = default;
    VMClosure(CompiledFunction* fn, std::vector<std::shared_ptr<ObjUpvalue>> ups)
        : function(fn), upvalues(std::move(ups)) {}
};

// ============================================================================
// VM — the bytecode virtual machine
// ============================================================================

class VM {
public:
    VM();
    ~VM();

    // Main entry point: execute a compiled function
    interpreter::NaabVal execute(CompiledFunction* main_fn);

    // Configuration (mirrors Interpreter)
    void setGovernance(governance::GovernanceEngine* gov) { governance_ = gov; }
    void setStdlib(stdlib::StdLib* stdlib) { stdlib_ = stdlib; }
    void setDebugger(debugger::Debugger* dbg) { debugger_ = dbg; }
    void setVerboseMode(bool v) { verbose_ = v; }
    void setProfileMode(bool p) { profile_ = p; }
    void setExplainMode(bool e) { explain_ = e; }
    void setSourceCode(const std::string& src) { source_code_ = src; }
    void setScriptArgs(const std::vector<std::string>& args) { script_args_ = args; }
    void setCurrentFile(const std::string& f) { current_file_ = f; }
    void setGlobals(const std::unordered_map<std::string, interpreter::NaabVal>& g) { globals_ = g; }
    void setGlobal(const std::string& name, interpreter::NaabVal val) { globals_[name] = std::move(val); }
    void setModuleResolver(modules::ModuleResolver* mr) { module_resolver_ = mr; }
    void setGovernanceVerbose(bool v) { governance_verbose_ = v; }
    governance::GovernanceEngine* getGovernance() const { return governance_; }
    void setGCThreshold(size_t t) { gc_threshold_ = t; }  // V-RT-008

    // Debugger: get current scope variables (slot → name mapping)
    std::map<std::string, interpreter::NaabVal> getCurrentScopeVariables() const;

private:
    // Value stack (heap-allocated to avoid 560KB on C++ stack)
    static constexpr size_t STACK_MAX = 65536;
    std::unique_ptr<interpreter::NaabVal[]> stack_;
    interpreter::NaabVal* stack_top_;

    // Shadow taint stack (mirrors value stack 1:1 for governance taint tracking)
    std::unique_ptr<bool[]> taint_stack_;
    bool* taint_top_;

    // Call frames (heap-allocated)
    static constexpr size_t FRAMES_MAX = 1024;
    std::unique_ptr<CallFrame[]> frames_;
    int frame_count_ = 0;

    // Main closure wrapper (keeps it alive)
    std::shared_ptr<VMClosure> main_closure_;

    // Open upvalues (shared_ptr for automatic cleanup when closures are destroyed)
    std::shared_ptr<ObjUpvalue> open_upvalues_;

    // Exception handlers
    std::vector<ExceptionHandler> exception_handlers_;

    // Globals
    std::unordered_map<std::string, interpreter::NaabVal> globals_;

    // Module cache
    std::unordered_map<std::string, std::shared_ptr<std::unordered_map<std::string, interpreter::NaabVal>>> module_cache_;
    // Keep compiled functions alive so VMClosure pointers remain valid after module compilation
    std::vector<std::unique_ptr<CompiledFunction>> owned_functions_;
    int module_loading_depth_ = 0;

    // Recursive run() depth guard (callNaabFunction re-enters run())
    int run_depth_ = 0;
    static constexpr int MAX_RUN_DEPTH = 64;

    // Integration
    governance::GovernanceEngine* governance_ = nullptr;
    stdlib::StdLib* stdlib_ = nullptr;
    debugger::Debugger* debugger_ = nullptr;
    modules::ModuleResolver* module_resolver_ = nullptr;

    // State
    int stop_frame_count_ = 0;  // run() stops when frame_count_ drops to this
    std::string current_file_;
    bool verbose_ = false;
    bool profile_ = false;
    bool explain_ = false;
    bool governance_verbose_ = false;
    int last_debug_line_ = -1;  // Track line changes for debugger (avoid multi-break per line)
    std::string source_code_;
    std::vector<std::string> script_args_;

    // Persistent runtimes (Phase 12: runtime py = python.start())
    struct PersistentRuntime {
        std::string language;
        runtime::Executor* executor;
        std::string code_buffer;
    };
    std::unordered_map<std::string, PersistentRuntime> named_runtimes_;

    // Generator state (for yield collection)
    std::vector<interpreter::NaabVal>* generator_values_ = nullptr;

    // Loop iteration counters for governance (keyed by back-edge target IP)
    std::unordered_map<uint32_t*, int> loop_iter_counts_;

    // V-VM-003: container-level taint side-table. When a tainted value is stored
    // into a dict or list (OP_SET_INDEX or dict.put via CALL_METHOD), the container's
    // underlying Value* identity is inserted here. OP_GET_INDEX and dict.get check
    // this set so taint survives the container's pop/push cycle.
    // NOTE: rawBits() is NOT used because NaabVal uses a handle table on ARM64 —
    // each fromLegacy() call allocates a new handle even for the same Value object.
    // toLegacy().get() returns the stable raw Value* shared by all NaabVal copies.
    std::unordered_set<const void*> tainted_containers_;

    // V-RT-008: GC cycle detector + instruction-count trigger
    // gc_detector_ runs mark-and-sweep on the VM stack + globals as roots.
    // gc_instruction_count_ increments at each OP_JUMP_BACK (loop back-edge).
    // gc_threshold_ matches the tree-walker's global_gc_threshold (default 5000).
    std::unique_ptr<interpreter::CycleDetector> gc_detector_;
    size_t gc_instruction_count_ = 0;
    size_t gc_threshold_ = 5000;

    // Core dispatch loop
    interpreter::NaabVal run();

    // Stack operations (inline for speed)
    void push(interpreter::NaabVal val) {
        if (stack_top_ >= stack_.get() + STACK_MAX) {
            runtimeError("Stack overflow");
        }
        *stack_top_++ = std::move(val);
        *taint_top_++ = false;  // default untainted
    }

    interpreter::NaabVal pop() {
        --taint_top_;
        return std::move(*--stack_top_);
    }

    interpreter::NaabVal& peek(int distance = 0) {
        return *(stack_top_ - 1 - distance);
    }

    // Taint stack access (mirrors peek)
    bool& peekTaint(int distance = 0) {
        return *(taint_top_ - 1 - distance);
    }

    // Sync taint_top_ after direct stack_top_ manipulation
    void syncTaintTop() {
        taint_top_ = taint_stack_.get() + (stack_top_ - stack_.get());
    }

    // Function calls
    bool callValue(interpreter::NaabVal callee, int argc);
    bool callFunction(VMClosure* closure, int argc);
    interpreter::NaabVal callBuiltinMethod(interpreter::NaabVal& obj, const std::string& method,
                                           int argc, interpreter::NaabVal* args);
    interpreter::NaabVal callStdlibMethod(const std::string& module, const std::string& method,
                                          int argc, interpreter::NaabVal* args);
    interpreter::NaabVal callBuiltinFunction(const std::string& name, int argc,
                                             interpreter::NaabVal* args);

    // Call a NAAb function from C++ (for stdlib callbacks like array.map_fn)
    interpreter::NaabVal callNaabFunction(interpreter::NaabVal fn,
                                          const std::vector<interpreter::NaabVal>& args);

    // Upvalue management
    std::shared_ptr<ObjUpvalue> captureUpvalue(interpreter::NaabVal* local);
    void closeUpvalues(interpreter::NaabVal* last);

    // Module import
    std::shared_ptr<std::unordered_map<std::string, interpreter::NaabVal>>
        importModule(const std::string& module_path);

    // Helper error messages (DX parity with tree-walker)
    std::string getVariableHelper(const std::string& name) const;

    // Error
    [[noreturn]] void runtimeError(const char* format, ...);
    std::vector<std::string> getStackTrace() const;
};

// ============================================================================
// Disassembler — human-readable bytecode output
// ============================================================================

void disassembleChunk(const Chunk& chunk, const std::string& name);
int disassembleInstruction(const Chunk& chunk, int offset);

} // namespace vm
} // namespace naab
