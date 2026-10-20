// What a stdlib call is to behavioural sequence detection: the event it
// records, if any, and whether the VM checks it BEFORE the call. One table,
// read by every VM route that calls a stdlib function.
//
// BSD sees a stdlib call only through this classification. It used to be two
// hand-written name lists in VM::run's OP_CALL_METHOD -- the pre-execution
// check and the post-call emission -- and they disagreed, while the
// zero-argument member route (`env.get_all` with no parentheses, which the VM
// CALLS) had neither. Measured at 2e9c120d (docs/open-investigations.md A22,
// tests/governance_v4/test_bsd_stdlib_events.sh): env.get_all -- the same code
// branch as env.list -- and env.set -- the same branch as env.set_var -- were
// recorded by neither list, nor were get_int/get_float/get_bool, delete_var,
// load_dotenv, file.write_lines, io.read_file/write_file or csv.read/write;
// file.read_lines was pre-checked but never emitted, so the pre-check's
// "already recorded" mark waited for an emit that never came and swallowed the
// next unrelated event; and a "process." prefix rule made process.getpid a
// process start.
//
// Coverage: tests/self-audit/test_bsd_event_coverage.sh fails when a covered
// module's hasFunction() names a function this table does not list, so an
// alias added to a module is classified the day it is added. A NEW module that
// reads or writes files, the environment, the network or processes is not
// covered until it is added to kCoveredModules -- the same limit the
// filesystem gate's allowlist (GovernanceEngine::filesystemAccessMode) has.

#pragma once

#include "naab/behavioral_sequence.h"

#include <string>
#include <unordered_map>

namespace naab {
namespace governance {

enum class StdlibEventSource {
    None,     // the call records no event
    Vm,       // the VM records it after the call returns
    Module,   // the stdlib module records it itself, inside the call
};

struct StdlibEventClass {
    const char* module;
    const char* method;
    StdlibEventSource source;
    RuntimeEventType type;     // meaningful unless source == None
    bool arg_in_detail;        // the event detail carries the first argument
};

// Is an event of this type checked BEFORE the call (checkPreExecution), so a
// call that would complete a pattern is refused before it runs? Decided per
// TYPE, never per name, so two names that record the same event cannot be
// treated differently -- file.read_lines was pre-checked while file.read's
// other spellings were not even recorded. Unchanged from the two lists this
// replaces: ENV_WRITE and DECODE are recorded after the call only, and
// tests/governance_v4/test_bsd_preexec_*.sh use DECODE as the type the
// pre-check never sees (their completed-path control and oracle). Whether an
// environment write should be refused before it happens is open (A22).
inline bool preCheckedEventType(RuntimeEventType t) {
    switch (t) {
        case RuntimeEventType::ENV_READ:
        case RuntimeEventType::FILE_READ:
        case RuntimeEventType::FILE_WRITE:
        case RuntimeEventType::NET_CONNECT:
        case RuntimeEventType::ENCODE:
        case RuntimeEventType::PROCESS_EXEC:
        case RuntimeEventType::AGENT_SEND:
            return true;
        default:
            return false;
    }
}

namespace bsd_table {

constexpr StdlibEventClass vmEvent(const char* m, const char* f, RuntimeEventType t) {
    return {m, f, StdlibEventSource::Vm, t, true};
}
constexpr StdlibEventClass noEvent(const char* m, const char* f) {
    return {m, f, StdlibEventSource::None, RuntimeEventType::ENV_READ, false};
}

using T = RuntimeEventType;

// Every function each covered module's hasFunction() accepts. An event is
// recorded for a call that performs the action its type names, with the path
// or variable as its first argument. Calls that only inspect (exists, has,
// size) record nothing: an event spends every pattern's max_gap budget, so a
// non-action shortens how far apart real steps may be.
inline const StdlibEventClass kTable[] = {
    // env
    vmEvent("env", "get",       T::ENV_READ),
    vmEvent("env", "get_int",   T::ENV_READ),
    vmEvent("env", "get_float", T::ENV_READ),
    vmEvent("env", "get_bool",  T::ENV_READ),
    vmEvent("env", "list",      T::ENV_READ),   // the whole environment
    vmEvent("env", "get_all",   T::ENV_READ),   // the same branch as list
    vmEvent("env", "set_var",   T::ENV_WRITE),
    vmEvent("env", "set",       T::ENV_WRITE),  // the same branch as set_var
    vmEvent("env", "delete_var", T::ENV_WRITE),
    // load_dotenv reads a file AND sets what it read; one event per call, and
    // the file read is the one a pattern over secrets files names.
    vmEvent("env", "load_dotenv", T::FILE_READ),
    noEvent("env", "has"),             // reads no value
    noEvent("env", "parse_env_file"),  // parses a string it is given
    noEvent("env", "get_args"),        // the command line, not the environment

    // file
    vmEvent("file", "read",        T::FILE_READ),
    vmEvent("file", "read_lines",  T::FILE_READ),
    vmEvent("file", "write",       T::FILE_WRITE),
    vmEvent("file", "append",      T::FILE_WRITE),
    vmEvent("file", "write_lines", T::FILE_WRITE),
    // Two paths, and the detail carries only the first: a copy or move cannot
    // be told apart by destination, so recording one would match path globs
    // against its SOURCE. Not recorded, as before (open, A22).
    noEvent("file", "copy"),
    noEvent("file", "move"),
    noEvent("file", "delete"),         // not a content write; as before (open, A22)
    noEvent("file", "create_dir"),
    noEvent("file", "exists"),
    noEvent("file", "size"),
    noEvent("file", "is_file"),
    noEvent("file", "is_dir"),
    noEvent("file", "list_dir"),
    noEvent("file", "basename"),
    noEvent("file", "dirname"),
    noEvent("file", "extension"),

    // io: a file surface and a console surface
    vmEvent("io", "read_file",  T::FILE_READ),
    vmEvent("io", "write_file", T::FILE_WRITE),
    noEvent("io", "exists"),
    noEvent("io", "list_dir"),
    noEvent("io", "write"),
    noEvent("io", "write_error"),
    noEvent("io", "output"),
    noEvent("io", "print"),
    noEvent("io", "println"),
    noEvent("io", "log"),
    noEvent("io", "read_line"),
    noEvent("io", "input"),

    // csv
    vmEvent("csv", "read",       T::FILE_READ),
    vmEvent("csv", "read_dict",  T::FILE_READ),
    vmEvent("csv", "write",      T::FILE_WRITE),
    vmEvent("csv", "write_dict", T::FILE_WRITE),
    noEvent("csv", "parse"),           // strings in, strings out
    noEvent("csv", "parse_dict"),
    noEvent("csv", "format_row"),
    noEvent("csv", "format_rows"),
    noEvent("csv", "stringify"),

    // http: every function connects
    vmEvent("http", "get",    T::NET_CONNECT),
    vmEvent("http", "post",   T::NET_CONNECT),
    vmEvent("http", "put",    T::NET_CONNECT),
    vmEvent("http", "delete", T::NET_CONNECT),
    vmEvent("http", "head",   T::NET_CONNECT),
    vmEvent("http", "patch",  T::NET_CONNECT),

    // process
    vmEvent("process", "run",  T::PROCESS_EXEC),
    vmEvent("process", "kill", T::PROCESS_EXEC),  // signals a process; needs SYS_EXEC, like run
    noEvent("process", "getpid"),  // was a process start under the "process." prefix rule
    noEvent("process", "exit"),    // ends the program; nothing follows to sequence

    // crypto
    vmEvent("crypto", "base64_encode", T::ENCODE),
    vmEvent("crypto", "hex_encode",    T::ENCODE),
    vmEvent("crypto", "base64_decode", T::DECODE),
    vmEvent("crypto", "hex_decode",    T::DECODE),
    noEvent("crypto", "md5"),          // one-way digests and randomness
    noEvent("crypto", "sha1"),
    noEvent("crypto", "sha256"),
    noEvent("crypto", "sha512"),
    noEvent("crypto", "hash"),
    noEvent("crypto", "hash_password"),
    noEvent("crypto", "compare_digest"),
    noEvent("crypto", "random_bytes"),
    noEvent("crypto", "random_string"),
    noEvent("crypto", "random_int"),
    noEvent("crypto", "generate_token"),

    // path: lexical, or metadata (exists stats, resolve canonicalises)
    noEvent("path", "join"),
    noEvent("path", "dirname"),
    noEvent("path", "basename"),
    noEvent("path", "extension"),
    noEvent("path", "is_absolute"),
    noEvent("path", "normalize"),
    noEvent("path", "exists"),
    noEvent("path", "resolve"),

    // log: set_output names a sink; the messages are not paths
    noEvent("log", "info"),
    noEvent("log", "warn"),
    noEvent("log", "error"),
    noEvent("log", "debug"),
    noEvent("log", "log"),
    noEvent("log", "set_level"),
    noEvent("log", "set_format"),
    noEvent("log", "set_output"),
    noEvent("log", "get_level"),

    // agent: the module records its own events (AGENT_SEND before each model
    // call -- send, run, batch, fan_out, pipeline and propose all reach one --
    // plus responses, tool calls and re-registrations). The VM only pre-checks
    // send, whose first argument is the HANDLE: it carries __nonce, the HMAC
    // tying the handle to server-side state, so it never enters the detail.
    {"agent", "send", StdlibEventSource::Module, T::AGENT_SEND, false},
    noEvent("agent", "create"),
    noEvent("agent", "run"),
    noEvent("agent", "messages"),
    noEvent("agent", "usage"),
    noEvent("agent", "batch"),
    noEvent("agent", "fan_out"),
    noEvent("agent", "pipeline"),
    noEvent("agent", "check"),
    noEvent("agent", "key_health"),
    noEvent("agent", "dispatch_status"),
    noEvent("agent", "environment"),
    noEvent("agent", "register_tool"),
    noEvent("agent", "extract_code"),
    noEvent("agent", "propose"),
    noEvent("agent", "commit"),
    noEvent("agent", "coherence"),
    noEvent("agent", "reset"),
    noEvent("agent", "record_validation"),
};

}  // namespace bsd_table

// The modules whose every function is in kTable (checked by the coverage test).
inline const char* const kCoveredModules[] = {
    "env", "file", "io", "csv", "http", "process", "crypto", "path", "log", "agent",
};

// The class of module.method, or nullptr when the table does not list it
// (a module that records nothing, or a name no covered module accepts).
inline const StdlibEventClass* stdlibEventClass(const std::string& module,
                                                const std::string& method) {
    static const std::unordered_map<std::string, const StdlibEventClass*> index = [] {
        std::unordered_map<std::string, const StdlibEventClass*> m;
        for (const auto& c : bsd_table::kTable)
            m.emplace(std::string(c.module) + "." + c.method, &c);
        return m;
    }();
    auto it = index.find(module + "." + method);
    return it == index.end() ? nullptr : it->second;
}

// The event detail: "module.method" plus the first argument (at most 64
// characters) when the class carries it and there is one.
inline std::string stdlibEventDetail(const StdlibEventClass& c,
                                     const std::string& full_name,
                                     const std::string& first_arg) {
    std::string d = full_name;
    if (c.arg_in_detail && !first_arg.empty()) d += "('" + first_arg.substr(0, 64) + "')";
    return d;
}

}  // namespace governance
}  // namespace naab
