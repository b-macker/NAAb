// NAAb Governance Engine Implementation
// Runtime enforcement of project governance rules via govern.json
//
// Three-tier enforcement model (inspired by HashiCorp Sentinel):
//   HARD      - Block execution. No override possible.
//   SOFT      - Block execution. Override with --governance-override flag.
//   ADVISORY  - Warn only. Execution continues.

#include "naab/governance.h"
#include "naab/stdlib_new_modules.h"
#include "naab/agent_review.h"
#include "naab/agent_provider.h"
#include "naab/crypto_utils.h"
#include "naab/trust_store.h"
#include "naab/secure_file.h"
#include "naab/subprocess_helpers.h"
#include "naab/ast.h"
#include "naab/interpreter.h"
#include "naab/analyzer/syntactic_analyzer.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <regex>
#include <chrono>
#include <ctime>
#include <algorithm>
#include <cmath>
#ifndef _WIN32
#  include <sys/file.h>
#endif
#include <sys/stat.h>
#ifdef _WIN32
#  include <direct.h>
#endif
#include <fmt/core.h>

namespace naab {

namespace governance {
    // Feature 1: Process-level flag for exit code determination
    // V-CONC-F6: std::atomic for thread-safe access from concurrent polyglot executors
    std::atomic<bool> g_governance_hard_block{false};

    // Thread-local governance engine for stdlib module access
    static thread_local GovernanceEngine* t_current_engine = nullptr;

    // Thread-local decision trace — avoids data races from concurrent check threads
    static thread_local std::vector<std::string> t_current_decision_trace;

    GovernanceEngine* GovernanceEngine::getCurrent() { return t_current_engine; }
    void GovernanceEngine::setCurrent(GovernanceEngine* engine) { t_current_engine = engine; }
}

// Extern: thread_local interpreter pointer set in interpreter.cpp
namespace interpreter {
    extern thread_local Interpreter* g_current_interpreter;
}

// Feature 3: CWE/OWASP mapping for standard compliance reporting
static const std::unordered_map<std::string, std::pair<std::vector<std::string>, std::vector<std::string>>>
    g_cwe_owasp_map = {
    // rule_name_suffix -> {cwe_ids, owasp_ids}
    // --- Injection / Input Validation ---
    {"no_sql_injection",        {{"CWE-89"},  {"A03:2021"}}},
    {"sql_string_concat",       {{"CWE-89"},  {"A03:2021"}}},
    {"no_path_traversal",       {{"CWE-22"},  {"A01:2021"}}},
    {"path_traversal",          {{"CWE-22"},  {"A01:2021"}}},
    {"shell_injection",         {{"CWE-78"},  {"A03:2021"}}},
    {"code_injection",          {{"CWE-94"},  {"A03:2021"}}},
    {"no_unsafe_deserialization",{{"CWE-502"}, {"A08:2021"}}},
    {"insecure_deserialization", {{"CWE-502"}, {"A08:2021"}}},
    {"eval_usage",              {{"CWE-95"},  {"A03:2021"}}},
    {"sink_violation",          {{"CWE-20"},  {"A03:2021"}}},
    // --- Secrets / Credentials ---
    {"no_secrets",              {{"CWE-798"}, {"A07:2021"}}},
    {"hardcoded_credentials",   {{"CWE-798"}, {"A07:2021"}}},
    {"no_hardcoded_urls",       {{"CWE-798"}, {}}},
    {"no_hardcoded_ips",        {{"CWE-798"}, {}}},
    {"vcs_secret_extraction",   {{"CWE-522"}, {"A07:2021"}}},
    // --- Access Control ---
    {"privilege_escalation",    {{"CWE-269"}, {"A04:2021"}}},
    {"imports",                 {{"CWE-829"}, {"A08:2021"}}},
    // --- Information Exposure ---
    {"data_exfiltration",       {{"CWE-200"}, {"A01:2021"}}},
    {"information_disclosure",  {{"CWE-200"}, {"A01:2021"}}},
    {"no_pii",                  {{"CWE-359"}, {}}},
    // --- Cryptography ---
    {"crypto",                  {{"CWE-327"}, {"A02:2021"}}},
    {"weak_crypto",             {{"CWE-327"}, {"A02:2021"}}},
    {"insecure_random",         {{"CWE-330"}, {"A02:2021"}}},
    // --- Resource Exhaustion ---
    {"resource_abuse",          {{"CWE-400"}, {}}},
    {"loop_iterations",         {{"CWE-834"}, {}}},
    {"nesting_depth",           {{"CWE-1124"},{}}},
    {"string_length",           {{"CWE-400"}, {}}},
    {"output_size",             {{"CWE-400"}, {}}},
    {"dict_size",               {{"CWE-400"}, {}}},
    {"polyglot_blocks",         {{"CWE-400"}, {}}},
    // --- Code Quality (LLM-specific) ---
    {"no_hallucinated_apis",    {{"CWE-477"}, {}}},
    {"no_dead_code",            {{"CWE-561"}, {}}},
    {"no_debug_artifacts",      {{"CWE-489"}, {"A05:2021"}}},
    {"no_incomplete_logic",     {{"CWE-480"}, {}}},
    {"no_mock_data",            {{"CWE-489"}, {"A05:2021"}}},
    {"no_oversimplification",   {{"CWE-393"}, {}}},
    {"no_simulation_markers",   {{"CWE-489"}, {"A05:2021"}}},
    {"no_temporary_code",       {{"CWE-489"}, {"A05:2021"}}},
    {"no_apologetic_language",  {{"CWE-1078"},{}}},
    {"intent_validation",       {{"CWE-754"}, {}}},
    {"semantic_checks",         {{"CWE-670"}, {}}},
    // --- Complexity ---
    {"max_complexity",          {{"CWE-1121"},{}}},
    {"complexity_floor",        {{"CWE-1121"},{}}},
    // --- Encoding / Other ---
    {"encoding",                {{"CWE-116"}, {}}},
    {"prototype_pollution",     {{"CWE-1321"},{}}},
    {"dangerous_calls",         {{"CWE-676"}, {}}},
};

std::pair<std::vector<std::string>, std::vector<std::string>>
lookupCweOwasp(const std::string& rule_name) {
    auto it = g_cwe_owasp_map.find(rule_name);
    if (it != g_cwe_owasp_map.end()) return it->second;
    // Try suffix after last dot (e.g., "code_quality.no_sql_injection" -> "no_sql_injection")
    auto dot = rule_name.rfind('.');
    if (dot != std::string::npos) {
        auto suffix = rule_name.substr(dot + 1);
        it = g_cwe_owasp_map.find(suffix);
        if (it != g_cwe_owasp_map.end()) return it->second;
    }
    return {{}, {}};
}

namespace governance {

// ============================================================================
// Pattern Databases (used by legacy check methods below)
// ============================================================================

static const std::vector<SecretPattern> SECRET_PATTERNS = {
    {"sk-[a-zA-Z0-9\\-_]{32,}",         "OpenAI API Key", "critical"},
    {"sk-ant-[a-zA-Z0-9\\-]{20,}",      "Anthropic API Key", "critical"},
    {"ghp_[a-zA-Z0-9]{36,}",            "GitHub Personal Access Token", "critical"},
    {"gho_[a-zA-Z0-9]{36,}",            "GitHub OAuth Token", "critical"},
    {"AKIA[0-9A-Z]{16}",                "AWS Access Key ID", "critical"},
    {"-----BEGIN[\\s\\S]*PRIVATE KEY-----", "Private Key", "critical"},
    {"xox[bpsa]-[0-9]{10,13}-[0-9]{10,13}-[a-zA-Z0-9]{24}", "Slack Token", "critical"},
    {"(?:sk|pk)_(?:test|live)_[a-zA-Z0-9]{24,}", "Stripe Key", "critical"},
    {"SG\\.[a-zA-Z0-9_-]{22}\\.[a-zA-Z0-9_-]{43}", "SendGrid Key", "critical"},
    {"AIza[0-9A-Za-z\\-_]{35}",         "Google API Key", "high"},
    {"eyJ[a-zA-Z0-9_-]*\\.[a-zA-Z0-9_-]*\\.[a-zA-Z0-9_-]*", "JWT Token", "high"},
    {"(?:mongodb|postgres|mysql|redis)://[^\\s]+", "Connection String", "high"},
    {"Bearer\\s+[A-Za-z0-9\\-._~+/]+=*", "Bearer Token", "high"},
    {"password\\s*=\\s*['\"][^'\"]{8,}['\"]", "Hardcoded Password", "high"},
    {"api[_-]?key\\s*=\\s*['\"][^'\"]{20,}['\"]", "API Key Assignment", "high"},
    {"token\\s*=\\s*['\"][^'\"]{20,}['\"]", "Hardcoded Token", "high"},
    {"secret\\s*=\\s*['\"][^'\"]{8,}['\"]", "Hardcoded Secret", "high"},
    {"aws_secret_access_key\\s*=\\s*['\"][^'\"]{40}['\"]", "AWS Secret Key", "critical"},
};

static const std::vector<DangerousPattern> DANGEROUS_PATTERNS_DB = {
    // Python
    {"python", "os\\.system\\s*\\(",      "os.system() call",
     "Use subprocess.run() with shell=False, or NAAb stdlib"},
    {"python", "subprocess\\.call\\s*\\(.*shell\\s*=\\s*True",
     "subprocess.call() with shell=True",
     "Use subprocess.run() with shell=False"},
    {"python", "\\beval\\s*\\(",           "eval() call",
     "Use json.loads() for data parsing, ast.literal_eval() for literals"},
    {"python", "\\bexec\\s*\\(",           "exec() call",
     "Restructure code to avoid dynamic execution"},
    {"python", "__import__\\s*\\(",        "__import__() call",
     "Use standard import statements"},
    {"python", "pickle\\.loads?\\s*\\(",   "pickle.load() call",
     "Use json.loads() — pickle can execute arbitrary code"},
    {"python", "yaml\\.load\\s*\\([^)]*(?!Loader)", "yaml.load() without SafeLoader",
     "Use yaml.safe_load() instead"},
    // EVA-EXTRA-2: Additional Python dangerous patterns
    {"python", "\\bcompile\\s*\\(.*\\bexec\\b",  "compile()+exec (dynamic code execution)",
     "Use direct function calls instead of dynamic code generation"},
    {"python", "\\bgetattr\\s*\\(",  "getattr() call (dynamic attribute access)",
     "Use direct attribute access instead of dynamic lookup"},
    {"python", "\\bimportlib\\.",    "importlib usage (dynamic imports)",
     "Use standard import statements"},
    // EVA-EXTRA-3: Python obfuscation hardening
    {"python", "subprocess\\.(?:Popen|run|check_output|check_call)\\s*\\(.*shell\\s*=\\s*True",
     "subprocess with shell=True",
     "Use subprocess.run() with shell=False and argument lists"},
    {"python", "subprocess\\.Popen\\s*\\(",
     "subprocess.Popen() call",
     "Use NAAb stdlib process.run()"},
    {"python", "ctypes\\.(?:CDLL|cdll|windll)\\s*\\(",
     "ctypes dynamic library loading (can execute native code)",
     "Use NAAb stdlib instead"},
    // Reflection-based function lookup (evasion vectors)
    {"python", "\\bglobals\\s*\\(\\s*\\)\\s*\\[",
     "globals() dict access (reflection-based function lookup)",
     "Call functions directly instead of through globals()"},
    {"python", "\\blocals\\s*\\(\\s*\\)\\s*\\[",
     "locals() dict access (reflection-based function lookup)",
     "Call functions directly instead of through locals()"},
    {"python", "\\bvars\\s*\\(\\s*\\)\\s*\\[",
     "vars() dict access (reflection-based function lookup)",
     "Call functions directly instead of through vars()"},
    {"python", "\\b__builtins__\\s*[\\[.]",
     "__builtins__ access (direct builtin manipulation)",
     "Use standard function calls instead of __builtins__ access"},
    {"python", "\\.__dict__\\s*\\[",
     "__dict__ attribute access (reflection-based function lookup)",
     "Access attributes directly instead of through __dict__"},
    // Class hierarchy sandbox escape vectors
    {"python", "\\.__class__\\s*\\.\\s*__bases__",
     "__class__.__bases__ access (class hierarchy traversal for sandbox escape)",
     "Avoid class hierarchy introspection in governed code"},
    {"python", "\\.__class__\\s*\\.\\s*__mro__",
     "__class__.__mro__ access (method resolution order traversal)",
     "Avoid class hierarchy introspection in governed code"},
    {"python", "\\btype\\s*\\([^)]*\\)\\s*\\.\\s*__subclasses__",
     "type().__subclasses__() (sandbox escape via class hierarchy)",
     "Avoid class hierarchy introspection in governed code"},
    {"python", "\\.__reduce__",
     "__reduce__ access (pickle-like arbitrary execution)",
     "Avoid __reduce__ — it enables arbitrary code execution during deserialization"},
    // Missing os functions
    {"python", "os\\.popen\\s*\\(",
     "os.popen() call (command execution)",
     "Use subprocess.run() with shell=False"},
    {"python", "os\\.exec[lv]p?e?\\s*\\(",
     "os.exec*() call (process replacement)",
     "Use subprocess.run() with shell=False"},
    {"python", "os\\.spawn[lv]p?e?\\s*\\(",
     "os.spawn*() call (process spawning)",
     "Use subprocess.run() with shell=False"},

    // JavaScript
    {"javascript", "\\beval\\s*\\(",       "eval() call",
     "Parse data with JSON.parse() instead"},
    {"javascript", "\\bFunction\\s*\\(",   "Function() constructor",
     "Define functions statically"},
    {"javascript", "require\\s*\\(\\s*['\"]child_process['\"]\\s*\\)",
     "child_process import",
     "Use NAAb stdlib for subprocess execution"},
    // EVA-EXTRA-3: JavaScript hardening
    {"javascript", "import\\s*\\(\\s*['\"]child_process['\"]",
     "dynamic import of child_process",
     "Use NAAb stdlib process.run()"},
    {"javascript", "process\\.env\\b",
     "process.env access (environment variable exposure)",
     "Use NAAb stdlib env.get()"},
    {"javascript", "vm\\.run(?:InNewContext|InThisContext)\\s*\\(",
     "vm.run*() (sandbox escape risk)",
     "Avoid Node.js vm module"},
    // Reflection-based function lookup (evasion vectors)
    // Note: string literals are stripped before pattern check, so window["eval"]
    // becomes window[]. Match bracket access without requiring quote characters.
    {"javascript", "\\bwindow\\s*\\[",
     "window[] bracket access (reflection-based function lookup)",
     "Call functions directly instead of through window[]"},
    {"javascript", "\\bglobalThis\\s*\\[",
     "globalThis[] bracket access (reflection-based function lookup)",
     "Call functions directly instead of through globalThis[]"},
    {"javascript", "\\bglobal\\s*\\[(?!\\s*\\d)",
     "global[] bracket access (reflection-based function lookup)",
     "Call functions directly instead of through global[]"},
    {"javascript", "\\bReflect\\.apply\\s*\\(",
     "Reflect.apply() (reflection-based function invocation)",
     "Call functions directly instead of through Reflect.apply()"},
    {"javascript", "\\bReflect\\.construct\\s*\\(",
     "Reflect.construct() (reflection-based constructor invocation)",
     "Use new Constructor() directly"},
    // Constructor chain sandbox escape
    {"javascript", "\\.constructor\\s*\\.\\s*constructor\\s*\\(",
     "constructor.constructor() chain (Function constructor access via prototype)",
     "Define functions statically — constructor chains can access Function()"},
    {"javascript", "\\.constructor\\s*\\[",
     "constructor[] bracket access (prototype-based function lookup)",
     "Access properties directly instead of through constructor[]"},
    // setTimeout/setInterval with string argument (acts as eval)
    {"javascript", "setTimeout\\s*\\(\\s*[^(,]*\\+",
     "setTimeout() with string concatenation (acts as eval)",
     "Use a function: setTimeout(() => { ... }, ms)"},
    {"javascript", "setInterval\\s*\\(\\s*[^(,]*\\+",
     "setInterval() with string concatenation (acts as eval)",
     "Use a function: setInterval(() => { ... }, ms)"},

    // Shell
    {"shell", "rm\\s+-rf\\s+/",           "rm -rf / (recursive root delete)",
     "Specify exact paths, never recursive from root"},
    {"shell", "\\bdd\\s+if=",             "dd command (disk destroyer)",
     "Use NAAb file module for safe file operations"},
    {"shell", "\\bmkfs\\.",               "mkfs (format filesystem)",
     "Extremely dangerous — do not format filesystems in polyglot blocks"},
    {"shell", ">\\s*/dev/(?!null\\b)",     "Writing to device files (excluding /dev/null)",
     "Avoid writing to device files (> /dev/null is safe and common)"},
    {"shell", "chmod\\s+777",             "chmod 777 (world-writable)",
     "Use specific permissions (644 for files, 755 for executables)"},
    {"shell", "curl[^|]*\\|\\s*(?:ba)?sh\\b", "curl | sh (remote code execution)",
     "Download and inspect scripts before executing"},
    {"shell", "wget[^|]*\\|\\s*(?:ba)?sh\\b", "wget | sh (remote code execution)",
     "Download and inspect scripts before executing"},
    // EVA-EXTRA-3: Shell hardening
    {"shell", "\\bsource\\s+",
     "source command (loads external script)",
     "Inline the script content directly"},
    {"shell", "\\bIFS\\s*=",
     "IFS manipulation (word splitting evasion)",
     "Avoid modifying IFS in governed code"},

    // Go
    {"go", "exec\\.Command\\s*\\(",
     "os/exec.Command() (command execution)",
     "Use NAAb stdlib process.run()"},
    {"go", "syscall\\.(?:Exec|ForkExec|RawSyscall)",
     "syscall direct call",
     "Avoid raw syscalls in polyglot blocks"},
    {"go", "\\bunsafe\\.Pointer",
     "unsafe.Pointer (memory unsafe)",
     "Avoid unsafe package in governed code"},
    {"go", "plugin\\.Open\\s*\\(",
     "plugin.Open() (dynamic code loading)",
     "Avoid dynamic plugin loading"},
    // Reflection-based evasion vectors
    {"go", "reflect\\.ValueOf\\s*\\(",
     "reflect.ValueOf() (reflection-based invocation)",
     "Call functions directly instead of through reflect"},
    {"go", "reflect\\.(?:New|MakeFunc)\\s*\\(",
     "reflect.New/MakeFunc (dynamic type/function creation)",
     "Use static types and functions"},

    // Rust
    {"rust", "Command::new\\s*\\(",
     "std::process::Command (command execution)",
     "Use NAAb stdlib process.run()"},
    {"rust", "\\bunsafe\\s*\\{",
     "unsafe block",
     "Avoid unsafe blocks in governed polyglot code"},
    {"rust", "\\blibc::",
     "libc FFI calls",
     "Avoid raw libc calls in governed code"},

    // C++
    {"cpp", "\\bsystem\\s*\\(",
     "system() call (command execution)",
     "Use NAAb stdlib process.run()"},
    {"cpp", "\\bpopen\\s*\\(",
     "popen() call",
     "Use NAAb stdlib process.run()"},
    {"cpp", "\\bexecl?p?\\s*\\(",
     "exec*() call (process replacement)",
     "Use NAAb stdlib process.run()"},
    {"cpp", "\\bdlopen\\s*\\(",
     "dlopen() (dynamic library loading)",
     "Avoid dynamic library loading"},
    {"cpp", "\\bsocket\\s*\\(",
     "socket() (raw network access)",
     "Use NAAb stdlib http module"},
    {"cpp", "\\b__asm__\\b|\\basm\\s*\\(",
     "inline assembly",
     "Avoid inline assembly in governed code"},

    // C#
    {"csharp", "Process\\.Start\\s*\\(",
     "Process.Start() (command execution)",
     "Use NAAb stdlib process.run()"},
    {"csharp", "Reflection\\.Emit",
     "Reflection.Emit (dynamic code gen)",
     "Avoid dynamic code generation"},
    {"csharp", "\\bDllImport\\b",
     "DllImport (native interop)",
     "Avoid P/Invoke in governed code"},
    {"csharp", "\\bunsafe\\s*\\{",
     "unsafe block (raw pointers)",
     "Avoid unsafe code in governed blocks"},
    // Reflection-based evasion vectors
    {"csharp", "Type\\.GetType\\s*\\(",
     "Type.GetType() (reflection-based type lookup)",
     "Use static types instead of runtime reflection"},
    {"csharp", "\\.GetMethod\\s*\\(",
     "GetMethod() (reflection-based method lookup)",
     "Call methods directly instead of through reflection"},
    {"csharp", "\\.Invoke\\s*\\(",
     "MethodInfo.Invoke() (reflection-based method invocation)",
     "Call methods directly instead of through reflection"},
    {"csharp", "Activator\\.CreateInstance\\s*\\(",
     "Activator.CreateInstance() (dynamic object creation)",
     "Use static constructors instead of dynamic creation"},
    {"csharp", "Assembly\\.Load(?:From|File)?\\s*\\(",
     "Assembly.Load() (dynamic assembly loading)",
     "Avoid loading assemblies dynamically"},

    // Ruby
    {"ruby", "\\bsystem\\s*\\(",
     "system() call (command execution)",
     "Use NAAb stdlib process.run()"},
    {"ruby", "\\beval\\s*\\(",
     "eval() call",
     "Avoid dynamic code execution"},
    {"ruby", "\\binstance_eval\\b",
     "instance_eval (arbitrary code in context)",
     "Use direct method calls"},
    {"ruby", "\\bsend\\s*\\(",
     "send() (dynamic dispatch)",
     "Use direct method calls"},
    // Reflection-based evasion vectors
    {"ruby", "\\bpublic_send\\s*\\(",
     "public_send() (dynamic dispatch)",
     "Use direct method calls"},
    {"ruby", "\\bmethod\\s*\\(\\s*:",
     "method(:sym) (method object extraction)",
     "Call methods directly instead of extracting method objects"},
    {"ruby", "\\.call\\s*\\(",
     "Method#call (invoke extracted method object)",
     "Call methods directly"},
    {"ruby", "\\bclass_eval\\b",
     "class_eval (arbitrary code in class context)",
     "Define methods statically"},
    {"ruby", "\\bmodule_eval\\b",
     "module_eval (arbitrary code in module context)",
     "Define methods statically"},
    {"ruby", "\\bconst_get\\s*\\(",
     "const_get (dynamic constant/class lookup)",
     "Reference classes directly by name"},
    // Shell execution syntax
    {"ruby", "`[^`]+`",
     "Backtick execution (shell command)",
     "Use NAAb stdlib process.run() instead of backtick shell execution"},
    {"ruby", "%x[({]",
     "%x() execution (shell command)",
     "Use NAAb stdlib process.run() instead of %x shell execution"},

    // PHP
    {"php", "\\bsystem\\s*\\(",
     "system() call",
     "Use NAAb stdlib process.run()"},
    {"php", "\\bexec\\s*\\(",
     "exec() call",
     "Use NAAb stdlib process.run()"},
    {"php", "\\bshell_exec\\s*\\(",
     "shell_exec() call",
     "Use NAAb stdlib process.run()"},
    {"php", "\\bpassthru\\s*\\(",
     "passthru() call",
     "Use NAAb stdlib process.run()"},
    {"php", "\\beval\\s*\\(",
     "eval() (arbitrary code execution)",
     "Avoid dynamic code execution"},
    {"php", "preg_replace\\s*\\(.*['\"/].*e",
     "preg_replace with /e modifier",
     "Use preg_replace_callback()"},
    // Reflection-based evasion vectors
    {"php", "\\bcall_user_func\\s*\\(",
     "call_user_func() (dynamic function invocation)",
     "Call functions directly instead of through call_user_func()"},
    {"php", "\\bcall_user_func_array\\s*\\(",
     "call_user_func_array() (dynamic function invocation with array args)",
     "Call functions directly"},
    {"php", "\\bcreate_function\\s*\\(",
     "create_function() (dynamic function creation, deprecated)",
     "Use closures (anonymous functions) instead"},
    {"php", "\\bassert\\s*\\(",
     "assert() (can execute strings as code)",
     "Use explicit conditionals instead of assert()"},
    {"php", "\\bReflectionMethod\\b",
     "ReflectionMethod (reflection-based method invocation)",
     "Call methods directly"},
    {"php", "\\bReflectionFunction\\b",
     "ReflectionFunction (reflection-based function invocation)",
     "Call functions directly"},
    {"php", "\\$\\{\\s*\\$",
     "Variable variables ($$var — dynamic variable access)",
     "Use arrays or explicit variable names"},
    {"php", "\\$\\w+\\s*=\\s*['\"]\\w*['\"]\\s*\\.\\s*['\"]\\w*['\"]",
     "String concatenation to build function name (evasion technique)",
     "Use direct function calls instead of concatenating function names"},

    // Nim
    {"nim", "\\bexecProcess\\s*\\(",
     "execProcess() (command execution)",
     "Use NAAb stdlib process.run()"},
    {"nim", "\\bstartProcess\\s*\\(",
     "startProcess() (command execution)",
     "Use NAAb stdlib process.run()"},
    {"nim", "\\{\\s*\\.importc\\b",
     "importc pragma (FFI)",
     "Avoid FFI in governed code"},
    {"nim", "\\{\\s*\\.emit\\b",
     "emit pragma (inline code)",
     "Avoid emit in governed code"},

    // Any language
    {"any", "\\bsudo\\s",                 "sudo (privilege escalation)",
     "Avoid privilege escalation in polyglot blocks"},
};

// Network library import patterns (for capabilities.network enforcement in polyglot blocks)
static const std::vector<DangerousPattern> NETWORK_IMPORT_PATTERNS = {
    // Python
    {"python", "\\bimport\\s+urllib",            "urllib import (network access)", ""},
    {"python", "\\bfrom\\s+urllib",              "urllib import (network access)", ""},
    {"python", "\\bimport\\s+requests",          "requests import (network access)", ""},
    {"python", "\\bimport\\s+http\\.client",     "http.client import (network access)", ""},
    {"python", "\\bimport\\s+aiohttp",           "aiohttp import (network access)", ""},
    {"python", "\\bimport\\s+socket",            "socket import (network access)", ""},
    {"python", "\\bimport\\s+httpx",             "httpx import (network access)", ""},
    // JavaScript
    {"javascript", "require\\s*\\(\\s*['\"]https?['\"]", "http/https require (network access)", ""},
    {"javascript", "require\\s*\\(\\s*['\"]node-fetch['\"]", "node-fetch require (network access)", ""},
    {"javascript", "\\bfetch\\s*\\(",            "fetch() call (network access)", ""},
    {"javascript", "XMLHttpRequest",             "XMLHttpRequest (network access)", ""},
    // Ruby
    {"ruby", "require\\s+['\"]net/http['\"]",    "net/http require (network access)", ""},
    {"ruby", "require\\s+['\"]open-uri['\"]",    "open-uri require (network access)", ""},
    // Go
    {"go", "\"net/http\"",                        "net/http import (network access)", ""},
    // C++
    {"cpp", "#include\\s*<.*(?:curl|socket|netdb|arpa|netinet).*>",
     "network header include", ""},
    // Rust
    {"rust", "std::net::",                  "std::net (network access)", ""},
    {"rust", "\\breqwest::",               "reqwest (HTTP client)", ""},
    // C#
    {"csharp", "System\\.Net\\b",          "System.Net (network access)", ""},
    {"csharp", "HttpClient",               "HttpClient (HTTP access)", ""},
    // PHP
    {"php", "\\bcurl_init\\s*\\(",         "curl_init() (network access)", ""},
    {"php", "\\bfsockopen\\s*\\(",         "fsockopen() (network access)", ""},
    // Nim
    {"nim", "HttpClient",                  "HttpClient (network access)", ""},
    {"nim", "\\bnewSocket\\b",             "newSocket() (network access)", ""},
};

// Filesystem operation patterns (for capabilities.filesystem enforcement in polyglot blocks)
static const std::vector<DangerousPattern> FILESYSTEM_IMPORT_PATTERNS = {
    // Python — builtin file I/O
    {"python", "\\bopen\\s*\\(",
     "open() call (direct filesystem access)",
     "Use NAAb stdlib file.read()/file.write() instead"},
    {"python", "\\bio\\.open\\s*\\(",
     "io.open() call (direct filesystem access)",
     "Use NAAb stdlib file.read()/file.write() instead"},
    // Python — pathlib
    {"python", "\\bpathlib\\b",
     "pathlib import/usage (filesystem access)",
     "Use NAAb stdlib file module instead of pathlib"},
    {"python", "\\bPath\\s*\\(",
     "pathlib.Path() constructor (filesystem access)",
     "Use NAAb stdlib file module instead of pathlib"},
    // Python — os filesystem operations
    {"python", "\\bos\\.listdir\\s*\\(",
     "os.listdir() (filesystem enumeration)",
     "Use NAAb stdlib file.list() instead"},
    {"python", "\\bos\\.walk\\s*\\(",
     "os.walk() (recursive filesystem traversal)",
     "Use NAAb stdlib file.list() instead"},
    {"python", "\\bos\\.remove\\s*\\(",
     "os.remove() (file deletion)",
     "Use NAAb stdlib file.delete() instead"},
    {"python", "\\bos\\.unlink\\s*\\(",
     "os.unlink() (file deletion)",
     "Use NAAb stdlib file.delete() instead"},
    {"python", "\\bos\\.rename\\s*\\(",
     "os.rename() (file move/rename)",
     "Avoid direct filesystem mutations in polyglot blocks"},
    {"python", "\\bos\\.makedirs?\\s*\\(",
     "os.makedirs() (directory creation)",
     "Use NAAb stdlib file.create_dir() instead"},
    {"python", "\\bos\\.path\\b",
     "os.path (filesystem path operations)",
     "Use NAAb stdlib path helpers instead"},
    // Python — shutil
    {"python", "\\bshutil\\.",
     "shutil (high-level filesystem operations)",
     "Use NAAb stdlib file module for safe file operations"},
    // Python — glob
    {"python", "\\bglob\\.(?:i)?glob\\s*\\(",
     "glob.glob() (filesystem pattern matching)",
     "Use NAAb stdlib file.list() instead"},
    // JavaScript — Node.js fs
    {"javascript",
     "\\bfs\\.(?:read|write|open|unlink|rmdir|mkdir|readdir|exists|stat|copyFile|rename)\\b",
     "Node.js fs module call (direct filesystem access)",
     "Use NAAb stdlib file module instead of fs"},
    {"javascript", "require\\s*\\(\\s*['\"]fs['\"]",
     "require('fs') (filesystem module import)",
     "Use NAAb stdlib file module instead of Node.js fs"},
    // Shell — recursive/forced deletion
    {"shell", "\\brm\\s+-[rRf]",
     "rm -r/-f (recursive/forced file deletion)",
     "Use NAAb stdlib file.delete() for single-file deletion"},
    // Go
    {"go", "os\\.(?:Open|Create|ReadFile|WriteFile|Remove|Mkdir)\\s*\\(",
     "os file operation", "Use NAAb stdlib file module"},
    // Rust
    {"rust", "std::fs::",                   "std::fs (filesystem access)", "Use NAAb stdlib file module"},
    {"rust", "File::(?:open|create)\\s*\\(", "File::open/create", "Use NAAb stdlib file module"},
    // C++
    {"cpp", "\\bfopen\\s*\\(",             "fopen() (filesystem access)", "Use NAAb stdlib file module"},
    {"cpp", "std::(?:i|o)?fstream",        "fstream (filesystem access)", "Use NAAb stdlib file module"},
    // C#
    {"csharp", "System\\.IO\\.",           "System.IO (filesystem access)", "Use NAAb stdlib file module"},
    // Ruby
    {"ruby", "File\\.(?:open|read|write|delete)", "File operation", "Use NAAb stdlib file module"},
    // PHP
    {"php", "\\bf(?:open|read|write|close)\\s*\\(",
     "file I/O function", "Use NAAb stdlib file module"},
    {"php", "file_(?:get|put)_contents\\s*\\(",
     "file_get/put_contents()", "Use NAAb stdlib file module"},
    // Nim
    {"nim", "\\breadFile\\s*\\(",          "readFile() (filesystem)", "Use NAAb stdlib file module"},
    {"nim", "\\bwriteFile\\s*\\(",         "writeFile() (filesystem)", "Use NAAb stdlib file module"},
};

static const std::vector<std::string> PLACEHOLDER_PATTERNS_DB = {
    "TODO", "FIXME", "STUB", "PLACEHOLDER", "XXX", "TBD",
    "HACK", "IMPLEMENT_ME", "RUNTIME_COMPUTED",
    "NOT_IMPLEMENTED", "UNFINISHED", "INCOMPLETE", "TEMPORARY",
    "PROTOTYPE", "DRAFT", "WIP", "WORK_IN_PROGRESS",
    "NEEDS_IMPLEMENTATION", "IMPLEMENT_LATER", "NEEDS_WORK",
    "NOT_YET_IMPLEMENTED", "UNIMPLEMENTED", "SKELETON",
    "BOILERPLATE", "SAMPLE_DATA", "DUMMY_DATA", "FAKE_DATA",
    "MOCK_RESULT", "SIMULATED", "HARDCODED_RESPONSE"
};

struct HardcodedResultPattern {
    std::string pattern;
    std::string description;
};

static const std::vector<HardcodedResultPattern> HARDCODED_RESULT_PATTERNS_DB = {
    // Return-with-comment patterns (both # and // comment styles)
    {"return\\s+True\\s*(?:#|//)",    "Hardcoded return True with comment"},
    {"return\\s+False\\s*(?:#|//)",   "Hardcoded return False with comment"},
    {"return\\s+0\\s*(?:#|//)",       "Hardcoded return 0 with comment"},
    {"return\\s+None\\s*(?:#|//)",    "Hardcoded return None with comment"},
    // EVA-EXTRA-4: Comment markers — both # and // styles
    {"(?:#|//)\\s*for now",           "Temporary implementation marker"},
    {"(?:#|//)\\s*simplified",        "Simplified implementation marker"},
    {"(?:#|//)\\s*placeholder",       "Placeholder implementation marker"},
    {"(?:#|//)\\s*stub",              "Stub implementation marker"},
    {"(?:#|//)\\s*not implemented",   "Not implemented marker"},
    {"(?:#|//)\\s*basic implementation", "Basic implementation marker"},
    {"(?:#|//)\\s*minimal",           "Minimal implementation marker"},
    // Hardcoded computation constants — variables that should be computed from input
    {"(?:max_possible|max_score|max_total|min_score|total_possible|total_weight)\\s*=\\s*\\d{2,}",
     "Hardcoded computation constant (should be derived from input)"},
    {"(?:denominator|divisor|normalizer|scale_factor)\\s*=\\s*\\d{2,}",
     "Hardcoded normalization constant (should be computed)"},
};

// --- Core Engine Implementation (extracted from governance.cpp) ---

bool GovernanceEngine::discoverAndLoad(const std::string& start_dir) {
    namespace fs = std::filesystem;

    last_error_.clear();
    fs::path dir(start_dir);
    while (true) {
        fs::path candidate = dir / "govern.json";
        if (fs::exists(candidate)) {
            govern_json_dir_ = dir.string();
            bool loaded = loadFromFile(candidate.string());
            if (!loaded) return false;

            // Project Context Awareness — load supplemental rules from project files
            if (rules_.project_context.enabled) {
                ProjectContextLoader loader;
                auto extractions = loader.loadContext(start_dir, rules_.project_context);

                if (!extractions.empty()) {
                    if (!rules_.project_context.dry_run) {
                        // Parse enforcement level
                        EnforcementLevel ctx_level = EnforcementLevel::ADVISORY;
                        if (rules_.project_context.enforcement_level == "soft")
                            ctx_level = EnforcementLevel::SOFT;
                        else if (rules_.project_context.enforcement_level == "hard")
                            ctx_level = EnforcementLevel::HARD;

                        loader.applyToRules(rules_, extractions, ctx_level);

                        if (rules_.project_context.feed_optimization) {
                            loader.applyOptimizationHints(
                                rules_.polyglot_optimization, extractions);
                        }
                    } else {
                        // Dry run: mark all as dry_run status
                        for (auto& ext : extractions) {
                            if (ext.status.empty()) ext.status = "dry_run";
                        }
                    }

                    if (rules_.project_context.show_extractions) {
                        std::string report = loader.formatReport(extractions);
                        if (!report.empty()) {
                            fprintf(stderr, "%s", report.c_str());
                        }
                    }
                }
            }

            return true;
        }

        fs::path parent = dir.parent_path();
        if (parent == dir) break;  // Reached root
        dir = parent;
    }
    last_error_ = "not_found";
    return false;
}

// ============================================================================
// Core Enforcement Logic
// ============================================================================

void GovernanceEngine::setCheckContext(const std::string& file, int line) {
    current_check_file_ = file;
    current_check_line_ = line;
}

// --- Decision trace accumulator ---
void GovernanceEngine::addTrace(const std::string& step) {
    t_current_decision_trace.push_back(step);
}

void GovernanceEngine::clearTrace() {
    t_current_decision_trace.clear();
}

std::string GovernanceEngine::lookupRationale(const std::string& rule_name) const {
    // Capabilities
    if (rule_name == "capabilities.network") return rules_.capabilities.network.rationale;
    if (rule_name == "capabilities.filesystem") return rules_.capabilities.filesystem.rationale;
    if (rule_name == "capabilities.shell") return rules_.capabilities.shell.rationale;
    if (rule_name == "capabilities.env_vars") return rules_.capabilities.env_vars.rationale;
    if (rule_name == "capabilities.process") return rules_.capabilities.process.rationale;
    // Requirements
    if (rule_name.rfind("requirements.", 0) == 0) {
        if (rule_name.find("main_block") != std::string::npos) return rules_.requirements.main_block.rationale;
        if (rule_name.find("error_handling") != std::string::npos) return rules_.requirements.error_handling.rationale;
        return "";
    }
    // Restrictions
    if (rule_name.rfind("restrictions.", 0) == 0) {
        std::string suffix = rule_name.substr(13);
        if (suffix == "dangerous_calls") return rules_.restrictions.dangerous_calls.rationale;
        if (suffix == "shell_injection") return rules_.restrictions.shell_injection.rationale;
        if (suffix == "privilege_escalation") return rules_.restrictions.privilege_escalation.rationale;
        if (suffix == "code_injection") return rules_.restrictions.code_injection.rationale;
        if (suffix == "crypto") return rules_.restrictions.crypto.rationale;
        if (suffix == "vcs_secret_extraction") return rules_.restrictions.vcs_secret_extraction.rationale;
        if (suffix == "data_exfiltration") return rules_.restrictions.data_exfiltration.rationale;
        if (suffix == "resource_abuse") return rules_.restrictions.resource_abuse.rationale;
        if (suffix == "information_disclosure") return rules_.restrictions.information_disclosure.rationale;
        if (suffix == "imports") return rules_.restrictions.imports.rationale;
        return "";
    }
    // Code quality
    if (rule_name.rfind("code_quality.", 0) == 0) {
        std::string suffix = rule_name.substr(13);
        if (suffix == "no_secrets") return rules_.code_quality.no_secrets.rationale;
        if (suffix == "no_placeholders") return rules_.code_quality.no_placeholders.rationale;
        if (suffix == "no_hardcoded_results") return rules_.code_quality.no_hardcoded_results.rationale;
        if (suffix == "no_pii") return rules_.code_quality.no_pii.rationale;
        if (suffix == "no_temporary_code") return rules_.code_quality.no_temporary_code.rationale;
        if (suffix == "no_simulation_markers") return rules_.code_quality.no_simulation_markers.rationale;
        if (suffix == "no_mock_data") return rules_.code_quality.no_mock_data.rationale;
        if (suffix == "no_apologetic_language") return rules_.code_quality.no_apologetic_language.rationale;
        if (suffix == "no_dead_code") return rules_.code_quality.no_dead_code.rationale;
        if (suffix == "no_debug_artifacts") return rules_.code_quality.no_debug_artifacts.rationale;
        if (suffix == "no_unsafe_deserialization") return rules_.code_quality.no_unsafe_deserialization.rationale;
        if (suffix == "no_sql_injection") return rules_.code_quality.no_sql_injection.rationale;
        if (suffix == "no_path_traversal") return rules_.code_quality.no_path_traversal.rationale;
        if (suffix == "no_hardcoded_urls") return rules_.code_quality.no_hardcoded_urls.rationale;
        if (suffix == "no_hardcoded_ips") return rules_.code_quality.no_hardcoded_ips.rationale;
        if (suffix == "max_complexity") return rules_.code_quality.max_complexity.rationale;
        if (suffix == "complexity_floor") return rules_.code_quality.complexity_floor.rationale;
        if (suffix == "encoding") return rules_.code_quality.encoding.rationale;
        if (suffix == "no_oversimplification") return rules_.code_quality.no_oversimplification.rationale;
        if (suffix == "no_incomplete_logic") return rules_.code_quality.no_incomplete_logic.rationale;
        if (suffix == "no_hallucinated_apis") return rules_.code_quality.no_hallucinated_apis.rationale;
        if (suffix == "intent_validation" || suffix.rfind("intent_validation.", 0) == 0) return rules_.code_quality.intent_validation.rationale;
        if (suffix == "duplicate_calls") return rules_.code_quality.duplicate_calls.rationale;
        if (suffix == "drift_detection") return rules_.code_quality.drift_detection.rationale;
        if (suffix == "semantic_checks") return rules_.code_quality.semantic_checks.rationale;
        return "";
    }
    // Taint tracking
    if (rule_name.rfind("taint", 0) == 0) return rules_.taint_tracking.rationale;
    // Context drift
    if (rule_name == "context_drift.coherence_loss") return rules_.context_drift.rationale;
    if (rule_name == "context_drift.reality_checkpoint") return rules_.context_drift.reality_checkpoint.rationale;
    // Exposure tracking
    if (rule_name == "exposure_tracking") return rules_.exposure_tracking.rationale;
    // Scoring
    if (rule_name.rfind("scoring", 0) == 0) return rules_.scoring.rationale;
    // Contracts — look up per-function rationale
    if (rule_name.rfind("contract.", 0) == 0) {
        // rule_name is "contract.<function_name>"
        std::string fn = rule_name.substr(9);
        auto it = rules_.contracts.functions.find(fn);
        if (it != rules_.contracts.functions.end()) return it->second.rationale;
    }
    // Codegen
    if (rule_name.rfind("codegen", 0) == 0) return rules_.codegen.rationale;
    // Custom rules
    if (rule_name.rfind("custom.", 0) == 0) {
        std::string id = rule_name.substr(7);
        for (const auto& cr : rules_.custom_rules) {
            if (cr.id == id) return cr.rationale;
        }
    }
    return "";  // No rationale configured
}

void GovernanceEngine::recordPass(const std::string& rule_name,
                                   EnforcementLevel level) {
    std::string cat = rule_name.substr(0, rule_name.find('.'));
    auto [cwes, owasps] = lookupCweOwasp(rule_name);
    std::string rationale = lookupRationale(rule_name);
    // V-CONC-007: mutex-guard concurrent access from async threads
    std::lock_guard<std::mutex> lock(results_mutex_);
    check_results_.push_back({rule_name, level, true, "", cat, "",
                              current_check_line_, current_check_file_, cwes, owasps,
                              false, rationale, std::move(t_current_decision_trace), ""});
    t_current_decision_trace.clear();
    // V-GOV-024: cap telemetry to prevent unbounded memory growth
    if (check_results_.size() > MAX_CHECK_RESULTS) {
        check_results_.erase(check_results_.begin());
    }
    // Pulse: track governance liveness
    pulse_.total_checks++;
    pulse_.consecutive_passes++;
    pulse_.last_check_epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string GovernanceEngine::enforce(
    const std::string& rule_name,
    EnforcementLevel level,
    const std::string& violation_message) {

    // Record the failing check with full context
    std::string cat = rule_name.substr(0, rule_name.find('.'));
    std::string sev = (level == EnforcementLevel::HARD ||
                       level == EnforcementLevel::APPROVAL_REQUIRED) ? "critical" :
                      (level == EnforcementLevel::SOFT) ? "high" : "medium";
    auto [cwes, owasps] = lookupCweOwasp(rule_name);
    std::string rationale = lookupRationale(rule_name);
    std::string explanation = generateExplanation(rule_name, level, false, rationale);
    {
        // V-CONC-007: mutex-guard concurrent access from async threads
        std::lock_guard<std::mutex> lock(results_mutex_);
        check_results_.push_back({rule_name, level, false, violation_message, cat, sev,
                                  current_check_line_, current_check_file_, cwes, owasps,
                                  preflight_mode_, rationale, std::move(t_current_decision_trace),
                                  explanation});
        t_current_decision_trace.clear();
        // V-GOV-024: cap telemetry — F8: skip preflight entries during eviction
        if (check_results_.size() > MAX_CHECK_RESULTS) {
            auto it = std::find_if(check_results_.begin(), check_results_.end(),
                [](const CheckResult& cr) { return !cr.preflight; });
            if (it != check_results_.end()) {
                check_results_.erase(it);
            } else {
                check_results_.erase(check_results_.begin());
            }
        }

        // Cumulative risk scoring — ADVISORY findings only
        // MONOTONIC: weight >= 0 guaranteed (clamped), score can only increase
        if (rules_.scoring.enabled && level == EnforcementLevel::ADVISORY) {
            int weight = rules_.scoring.default_weight;
            std::string weight_source = "default";
            auto wit = rules_.scoring.rule_weights.find(rule_name);
            if (wit != rules_.scoring.rule_weights.end()) {
                weight = wit->second;
                weight_source = "rule_weights";
            } else if (rule_name == "code_quality.intent_validation.self_declared") {
                weight = 1;  // supporting functions: reduced weight
                weight_source = "self_declared reduction";
            }
            weight = std::max(0, weight);
            // Advisory Escalation: multiply weight on 2nd+ occurrence
            if (rules_.advisory_escalation.enabled) {
                int occ = emitted_advisories_[rule_name];  // already incremented above
                if (occ > 1) {
                    weight = static_cast<int>(weight * rules_.advisory_escalation.weight_multiplier);
                    weight_source += " (escalated x" + std::to_string(occ) + ")";
                }
            }
            int prev_score = cumulative_score_;
            if (cumulative_score_ <= SCORE_SATURATION_LIMIT - weight) {
                cumulative_score_ += weight;
            } else {
                cumulative_score_ = SCORE_SATURATION_LIMIT;
            }
            score_contributions_[rule_name] += weight;
            // Append scoring trace to the just-pushed CheckResult
            if (!check_results_.empty()) {
                auto& last = check_results_.back();
                last.decision_trace.push_back(fmt::format(
                    "scoring: weight={} ({}), cumulative: {} → {}",
                    weight, weight_source, prev_score, cumulative_score_));
                std::string zone = cumulative_score_ >= rules_.scoring.red_threshold ? "RED" :
                                   cumulative_score_ >= rules_.scoring.yellow_threshold ? "YELLOW" : "GREEN";
                last.decision_trace.push_back(fmt::format("risk zone: {}", zone));
            }
        }
        // Pulse: reset consecutive passes on any enforcement
        pulse_.total_checks++;
        pulse_.consecutive_passes = 0;
        pulse_.last_check_epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (level == EnforcementLevel::ADVISORY) {
            pulse_.advisory_count++;
        }
    }

    // Audit mode: never block, just log — except safety-critical checks
    if (rules_.mode == GovernanceMode::AUDIT) {
        // F3: no_secrets and no_pii always enforce, even in AUDIT mode
        if (rule_name != "code_quality.no_secrets" &&
            rule_name != "code_quality.no_pii") {
            fprintf(stderr, "[governance] AUDIT %s: %s\n",
                    rule_name.c_str(),
                    violation_message.substr(0, violation_message.find('\n')).c_str());
            return "";  // Don't block
        }
        // Fall through to normal enforcement for secrets/PII
    }

    switch (level) {
        case EnforcementLevel::NONE:
            return "";  // Not enforced

        case EnforcementLevel::HARD:
            g_governance_hard_block = true;
            return violation_message;

        case EnforcementLevel::APPROVAL_REQUIRED: {
            std::string approver_id;
            if (hasValidApproval(rule_name, approver_id)) {
                addTrace("approval_required: valid token found — permitted");
                logAuditEvent("approval_used", rule_name,
                    fmt::format("approved by '{}'", approver_id));
                return "";
            }
            g_governance_hard_block = true;
            return violation_message +
                "\n\n  This rule requires explicit approval.\n"
                "  The project owner can provide a signed token.\n";
        }

        case EnforcementLevel::SOFT:
            if (override_enabled_) {
                // Require reason if configured — block silently if missing
                if (rules_.require_override_reason && override_reason_.empty()) {
                    g_governance_hard_block = true;
                    return violation_message;
                }
                // Log override with provenance
                if (!override_reason_.empty()) {
                    logAuditEvent("override", rule_name,
                        fmt::format("agent='{}' reason='{}'", agent_id_, override_reason_));
                } else {
                    logAuditEvent("override", rule_name,
                        fmt::format("agent='{}'", agent_id_));
                }
                override_counts_[agent_id_]++;
                fprintf(stderr, "[governance] OVERRIDE %s\n", rule_name.c_str());
                return "";  // Don't block
            }
            // naab-29 L-09: SOFT without override is a governance block (exit 3)
            g_governance_hard_block = true;
            return violation_message;

        case EnforcementLevel::ADVISORY: {
            // V-CONC-F7: mutex-guard emitted_advisories_ and score_yellow_warned_
            std::lock_guard<std::mutex> adv_lock(results_mutex_);
            int& occurrence = emitted_advisories_[rule_name];
            occurrence++;

            // Advisory Escalation: repeated advisories harden
            // 1st: warn. 2nd+: increase weight. N-th (soft_after): escalate to SOFT block
            const auto& esc = rules_.advisory_escalation;
            if (esc.enabled && occurrence >= esc.soft_after) {
                // Escalate to SOFT — release lock, recurse with SOFT level
                // (can't call enforce recursively under same lock — set flag and return)
                g_governance_hard_block = true;
                fprintf(stderr, "[governance] ESCALATED %s (occurrence %d >= %d)\n",
                    rule_name.c_str(), occurrence, esc.soft_after);
                return violation_message +
                    "\n\n  This advisory was escalated after repeated occurrences.\n";
            }

            if (occurrence == 1) {
                // First occurrence — standard advisory warning
                if (rule_name.rfind("agent_review.", 0) != 0) {
                    fprintf(stderr, "[governance] WARNING %s\n", rule_name.c_str());
                }
            } else if (esc.enabled) {
                // 2nd+ occurrence — warn with count
                fprintf(stderr, "[governance] WARNING %s (occurrence %d/%d)\n",
                    rule_name.c_str(), occurrence, esc.soft_after);
            }

            // Yellow-zone: warn ONCE when score first enters yellow zone
            if (rules_.scoring.enabled && !score_yellow_warned_ &&
                cumulative_score_ >= rules_.scoring.yellow_threshold) {
                score_yellow_warned_ = true;
                fprintf(stderr,
                    "[governance] Risk score %d reached yellow threshold %d "
                    "(red threshold: %d)\n",
                    cumulative_score_, rules_.scoring.yellow_threshold,
                    rules_.scoring.red_threshold);
                // Show top contributor so LLM knows what's causing the score
                if (!score_contributions_.empty()) {
                    auto top = std::max_element(score_contributions_.begin(),
                        score_contributions_.end(),
                        [](const auto& a, const auto& b) { return a.second < b.second; });
                    fprintf(stderr, "[governance] Top contributor: %s (+%d)\n",
                        top->first.c_str(), top->second);
                }
            }
            return "";  // Don't block
        }
    }
    return "";
}

// ============================================================================
// Explanation Generation
// ============================================================================

std::string GovernanceEngine::generateExplanation(
    const std::string& rule_name,
    EnforcementLevel level,
    bool passed,
    const std::string& rationale) const {
    if (passed || !rules_.explanations_enabled) return "";

    std::string action;
    switch (level) {
        case EnforcementLevel::HARD:
            action = "Blocked";
            break;
        case EnforcementLevel::APPROVAL_REQUIRED:
            action = "Requires approval";
            break;
        case EnforcementLevel::SOFT:
            action = override_enabled_ ? "Overridden" : "Blocked";
            break;
        case EnforcementLevel::ADVISORY:
            action = "Warning";
            break;
        default:
            return "";
    }

    std::string reason = rationale.empty() ? "project governance policy" : rationale;
    return fmt::format("{}: {} — {}.", action, rule_name, reason);
}

// ============================================================================
// Approval Token Verification
// ============================================================================

bool GovernanceEngine::hasValidApproval(const std::string& rule_name,
                                         std::string& approver_id_out) const {
    if (rules_.approval.store_path.empty()) return false;

    // Resolve store path relative to govern.json directory
    std::string store_path = rules_.approval.store_path;
    if (!store_path.empty() && store_path[0] != '/') {
        store_path = govern_json_dir_ + "/" + store_path;
    }

    std::lock_guard<std::mutex> lock(approval_mutex_);

    // Lazy-reload: check file mtime
    struct stat st;
    if (stat(store_path.c_str(), &st) != 0) return false;
    int64_t mtime = static_cast<int64_t>(st.st_mtime);

    if (mtime != approval_mtime_) {
        approval_mtime_ = mtime;
        approval_cache_.clear();
        try {
            std::ifstream ifs(store_path);
            if (!ifs.is_open()) return false;
            auto j = nlohmann::json::parse(ifs);
            if (!j.is_object()) return false;
            for (auto& [key, val] : j.items()) {
                if (!val.is_object()) continue;
                ApprovalToken token;
                token.rule_name = key;
                token.approver_id = val.value("approver_id", "");
                token.reason = val.value("reason", "");
                token.expiry_timestamp = val.value("expiry_timestamp", (int64_t)0);
                token.signature_b64 = val.value("signature", "");
                approval_cache_[key] = std::move(token);
            }
        } catch (...) {
            return false;
        }
    }

    auto it = approval_cache_.find(rule_name);
    if (it == approval_cache_.end()) return false;
    const auto& token = it->second;

    // Check expiry
    if (token.expiry_timestamp > 0 &&
        time(nullptr) > token.expiry_timestamp) return false;

    // Verify Ed25519 signature if approver_keys are configured
    if (!rules_.approval.approver_keys.empty() && !token.signature_b64.empty()) {
        // Length-prefixed canonical encoding — prevents pipe injection in reason field.
        // Format: "len:rule_name""len:approver_id""len:reason""expiry_timestamp"
        // Breaking change: pre-existing signed tokens will fail verification.
        auto lpEncode = [](const std::string& s) -> std::string {
            return std::to_string(s.size()) + ":" + s;
        };
        std::string canonical = lpEncode(token.rule_name) + lpEncode(token.approver_id)
            + lpEncode(token.reason) + std::to_string(token.expiry_timestamp);
        // Load trusted keys and check if any matching fingerprint verifies
        auto keys = security::TrustStore::loadKeys();
        for (const auto& key_fp : rules_.approval.approver_keys) {
            for (const auto& [fp, pem] : keys) {
                if (fp == key_fp) {
                    if (security::CryptoUtils::ed25519Verify(canonical, token.signature_b64, pem)) {
                        approver_id_out = token.approver_id;
                        return true;
                    }
                    break;
                }
            }
        }
        return false;  // Signature required but didn't verify
    }

    // H6 fix: fail-closed when approver_keys not configured.
    // Without configured keys, approval system is not meaningful.
    if (rules_.approval.approver_keys.empty()) {
        return false;
    }
    // Signature required but not provided
    return false;
}

// ============================================================================
// Enforcement Checks
// ============================================================================

std::string GovernanceEngine::checkCodegenAllowed(
    const std::string& language, size_t code_size, int line) {
    clearTrace();

    if (!rules_.codegen.enabled) {
        addTrace("codegen.enabled = false → blocked");
        return enforce("codegen.enabled", rules_.codegen.level,
            "Codegen error: dynamic code execution is not enabled\n\n"
            "  codegen.run() requires explicit enablement in governance configuration.\n"
            "  Use static polyglot blocks instead: <<" + language + " ... >>\n");
    }

    // Check codegen-specific language restrictions
    if (!rules_.codegen.allowed_languages.empty()) {
        bool found = false;
        for (const auto& l : rules_.codegen.allowed_languages) {
            if (l == language) { found = true; break; }
        }
        if (!found) {
            addTrace("codegen.allowed_languages does not contain '" + language + "' → blocked");
            return enforce("codegen.allowed_languages", rules_.codegen.level,
                "Codegen error: language '" + language + "' is not allowed for dynamic code\n");
        }
    }
    if (!rules_.codegen.blocked_languages.empty()) {
        for (const auto& l : rules_.codegen.blocked_languages) {
            if (l == language) {
                addTrace("codegen.blocked_languages contains '" + language + "' → blocked");
                return enforce("codegen.blocked_languages", rules_.codegen.level,
                    "Codegen error: language '" + language + "' is blocked for dynamic code\n");
            }
        }
    }

    // Check per-call size
    if (rules_.codegen.max_code_size_bytes > 0 &&
        static_cast<int>(code_size) > rules_.codegen.max_code_size_bytes) {
        addTrace("code_size " + std::to_string(code_size) + " > max " +
                 std::to_string(rules_.codegen.max_code_size_bytes) + " → blocked");
        return enforce("codegen.max_code_size_bytes", rules_.codegen.level,
            "Codegen error: code exceeds maximum size\n\n"
            "  Got: " + std::to_string(code_size) + " bytes\n"
            "  Limit: " + std::to_string(rules_.codegen.max_code_size_bytes) + " bytes\n");
    }

    addTrace("codegen allowed: " + language + ", " + std::to_string(code_size) + " bytes");
    return "";
}

std::string GovernanceEngine::checkLanguageAllowed(
    const std::string& language, int line) {
    clearTrace();

    // Check blocked list first
    if (rules_.blocked_languages.count(language)) {
        std::string location = line > 0
            ? fmt::format("line {}: <<{}", line, language)
            : fmt::format("<<{}", language);

        std::string blocked_list;
        for (auto& l : rules_.blocked_languages) {
            if (!blocked_list.empty()) blocked_list += ", ";
            blocked_list += l;
        }

        return enforce("languages.blocked", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Language \"{}\" is blocked", language),
                location,
                fmt::format("languages.blocked contains \"{}\"", language),
                fmt::format("The \"{}\" language is explicitly blocked in governance", language),
                fmt::format("let result = <<{}\n...\n>>", language),
                !rules_.allowed_languages.empty()
                    ? fmt::format("let result = <<{}\n...\n>>",
                        *rules_.allowed_languages.begin())
                    : ""));
    }

    // Check allowed list (only if non-empty — empty means all allowed)
    if (!rules_.allowed_languages.empty() &&
        !rules_.allowed_languages.count(language)) {

        std::string location = line > 0
            ? fmt::format("line {}: <<{}", line, language)
            : fmt::format("<<{}", language);

        std::string allowed_list;
        for (auto& l : rules_.allowed_languages) {
            if (!allowed_list.empty()) allowed_list += ", ";
            allowed_list += l;
        }

        return enforce("languages.allowed", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Language \"{}\" is not allowed", language),
                location,
                fmt::format("languages.allowed = [{}]", allowed_list),
                fmt::format("Only {} polyglot blocks are permitted\n"
                    "To allow {}, add it to the \"allowed\" array in govern.json",
                    allowed_list, language),
                fmt::format("let result = <<{}\n...\n>>", language),
                fmt::format("let result = <<{}\n...\n>>",
                    *rules_.allowed_languages.begin())));
    }

    // V-GOV-020: Per-agent language enforcement (defense-in-depth beyond applyAgentRole)
    for (const auto& role : rules_.agents) {
        if (role.name == agent_id_) {
            // Check per-agent blocked languages
            for (const auto& bl : role.blocked_languages) {
                if (bl == language) {
                    return enforce("agent_role.language", EnforcementLevel::HARD,
                        formatError(EnforcementLevel::HARD,
                            fmt::format("Agent '{}' is blocked from using language \"{}\"",
                                agent_id_, language),
                            "",
                            fmt::format("agents.{}.blocked_languages contains \"{}\"",
                                agent_id_, language),
                            fmt::format("Your agent role does not permit the \"{}\" language.\n"
                                "Check your agent's allowed_languages in govern.json.", language),
                            fmt::format("let result = <<{}\n...\n>>", language),
                            !rules_.allowed_languages.empty()
                                ? fmt::format("let result = <<{}\n...\n>>",
                                    *rules_.allowed_languages.begin())
                                : "Use an allowed language for this agent"));
                }
            }
            // Check per-agent allowed languages (if non-empty, must be in list)
            if (!role.allowed_languages.empty()) {
                bool found = false;
                for (const auto& al : role.allowed_languages) {
                    if (al == language) { found = true; break; }
                }
                if (!found) {
                    std::string al_list;
                    for (const auto& al : role.allowed_languages) {
                        if (!al_list.empty()) al_list += ", ";
                        al_list += al;
                    }
                    return enforce("agent_role.language", EnforcementLevel::HARD,
                        formatError(EnforcementLevel::HARD,
                            fmt::format("Agent '{}' is not allowed to use language \"{}\"",
                                agent_id_, language),
                            "",
                            fmt::format("agents.{}.allowed_languages = [{}]",
                                agent_id_, al_list),
                            fmt::format("Your agent role only permits: {}", al_list),
                            fmt::format("let result = <<{}\n...\n>>", language),
                            fmt::format("let result = <<{}\n...\n>>",
                                role.allowed_languages.front())));
                }
            }
            break;
        }
    }

    recordPass("languages", EnforcementLevel::HARD);
    return "";
}

std::string GovernanceEngine::checkNetworkAllowed() {
    clearTrace();
    if (!rules_.network_allowed) {
        return enforce("capabilities.network", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                "Network access is not allowed",
                "",
                "capabilities.network = false",
                "Network operations are disabled by governance\n"
                "This prevents outbound connections from polyglot blocks",
                "http.get(\"https://api.example.com\")",
                "let data = json.parse(file.read(\"cached_data.json\"))"));
    }
    // Per-agent network enforcement
    for (const auto& role : rules_.agents) {
        if (role.name == agent_id_) {
            if (role.network_allowed_set && !role.network_allowed) {
                return enforce("agent_role.network", EnforcementLevel::HARD,
                    formatError(EnforcementLevel::HARD,
                        "Agent '" + agent_id_ + "' is not allowed network access",
                        "",
                        "agents." + agent_id_ + ".network_allowed = false",
                        "Your agent role does not permit network operations.\n"
                        "Use file-based data or NAAb stdlib instead.",
                        "http.get(\"https://api.example.com\")",
                        "let data = json.parse(file.read(\"cached_data.json\"))"));
            }
            // Per-agent action matrix: check NET_CONNECT
            if (!role.allowed_actions.empty()) {
                bool allowed = false;
                for (const auto& a : role.allowed_actions) {
                    if (a == "NET_CONNECT") { allowed = true; break; }
                }
                if (!allowed) {
                    return enforce("agent_role.action_matrix", EnforcementLevel::HARD,
                        formatError(EnforcementLevel::HARD,
                            "Agent '" + agent_id_ + "' action matrix does not include NET_CONNECT",
                            "",
                            "agents." + agent_id_ + ".allowed_actions",
                            "Your agent's allowed_actions list does not include NET_CONNECT.\n"
                            "Add NET_CONNECT to the allowed_actions list to permit network access.",
                            "http.get(\"https://api.example.com\")",
                            "let data = json.parse(file.read(\"cached_data.json\"))"));
                }
            }
            break;
        }
    }
    recordPass("capabilities.network", EnforcementLevel::HARD);
    return "";
}

std::string GovernanceEngine::checkNetworkImports(
    const std::string& language, const std::string& code, int line) {
    clearTrace();
    if (rules_.network_allowed) {
        recordPass("capabilities.network", EnforcementLevel::HARD);
        return "";
    }
    // Scan polyglot code for network library usage patterns
    for (const auto& pat : NETWORK_IMPORT_PATTERNS) {
        if (pat.language != language && pat.language != "any") continue;
        try {
            std::regex re(pat.pattern);
            if (std::regex_search(code, re)) {
                return enforce("capabilities.network", EnforcementLevel::HARD,
                    formatError(EnforcementLevel::HARD,
                        fmt::format("Network access blocked: {} in {} block",
                                    pat.description, language),
                        fmt::format("line {}", line),
                        "capabilities.network = false",
                        "Network operations are disabled by governance.\n"
                        "This prevents outbound connections from polyglot blocks.\n"
                        "Use cached/local data or NAAb stdlib instead.",
                        fmt::format("{} in <<{}>> block", pat.description, language),
                        "let data = json.parse(file.read(\"data/cached.json\"))"));
            }
        } catch (...) {}
    }
    recordPass("capabilities.network", EnforcementLevel::HARD);
    return "";
}

std::string GovernanceEngine::checkFilesystemImports(
    const std::string& language, const std::string& code, int line) {
    clearTrace();
    // Only enforce when filesystem access is restricted (not the default "write" mode)
    if (rules_.filesystem_mode == "write" || rules_.filesystem_mode.empty()) {
        recordPass("capabilities.filesystem", EnforcementLevel::HARD);
        return "";
    }
    for (const auto& pat : FILESYSTEM_IMPORT_PATTERNS) {
        if (pat.language != language && pat.language != "any") continue;
        try {
            std::regex re(pat.pattern, std::regex::icase);
            if (std::regex_search(code, re)) {
                return enforce("capabilities.filesystem", EnforcementLevel::HARD,
                    formatError(EnforcementLevel::HARD,
                        fmt::format("Filesystem access blocked: {} in {} block",
                                    pat.description, language),
                        fmt::format("line {}", line),
                        fmt::format("capabilities.filesystem = \"{}\"",
                                    rules_.filesystem_mode),
                        "Filesystem operations are restricted by governance policy.\n"
                        "Use NAAb stdlib file module for controlled file access.",
                        fmt::format("{} in <<{}>> block", pat.description, language),
                        pat.safe_alternative.empty()
                            ? "let data = file.read(\"./data/input.txt\")  // NAAb stdlib"
                            : pat.safe_alternative));
            }
        } catch (const std::regex_error&) {
            // Invalid pattern — skip
        }
    }
    recordPass("capabilities.filesystem", EnforcementLevel::HARD);
    return "";
}

std::string GovernanceEngine::checkFilesystemAllowed(const std::string& mode) {
    clearTrace();
    if (rules_.filesystem_mode == "none") {
        return enforce("capabilities.filesystem", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                "Filesystem access is not allowed",
                "",
                "capabilities.filesystem = \"none\"",
                "All filesystem operations are disabled by governance",
                "file.write(\"output.txt\", data)",
                "print(data)  // Use stdout instead"));
    }
    if (rules_.filesystem_mode == "read" && mode == "write") {
        return enforce("capabilities.filesystem", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                "Filesystem write access is not allowed",
                "",
                "capabilities.filesystem = \"read\"",
                "Only read operations are allowed\n"
                "Writing files is disabled by governance",
                "file.write(\"output.txt\", data)",
                "let data = file.read(\"input.txt\")"));
    }
    // Per-agent action matrix: check FS_READ/FS_WRITE
    for (const auto& role : rules_.agents) {
        if (role.name == agent_id_ && !role.allowed_actions.empty()) {
            std::string required = (mode == "write") ? "FS_WRITE" : "FS_READ";
            bool allowed = false;
            for (const auto& a : role.allowed_actions) {
                if (a == required) { allowed = true; break; }
            }
            if (!allowed) {
                return enforce("agent_role.action_matrix", EnforcementLevel::HARD,
                    formatError(EnforcementLevel::HARD,
                        "Agent '" + agent_id_ + "' action matrix does not include " + required,
                        "",
                        "agents." + agent_id_ + ".allowed_actions",
                        "Your agent's allowed_actions list does not include " + required + ".\n"
                        "Add " + required + " to the allowed_actions list to permit this operation.",
                        mode == "write" ? "file.write(\"output.txt\", data)" : "file.read(\"input.txt\")",
                        "Use an agent with the appropriate permissions"));
            }
            break;
        }
    }
    recordPass("capabilities.filesystem", EnforcementLevel::HARD);
    return "";
}

std::string GovernanceEngine::checkPathAccess(const std::string& filepath, const std::string& mode) {
    clearTrace();
    // Canonicalize path for consistent prefix matching
    std::string canon;
    try {
        canon = std::filesystem::weakly_canonical(filepath).string();
    } catch (...) {
        canon = filepath;
    }

    // Normalize path separators: replace backslashes with forward slashes so that
    // govern.json paths (which may use either separator) compare correctly on Windows.
    auto normSep = [](std::string p) {
        std::replace(p.begin(), p.end(), '\\', '/');
        return p;
    };
    const std::string canon_n = normSep(canon);

    // V-GOV-025: Canonicalize config paths the same way as the filepath.
    // Without this, relative paths like "./output" in govern.json won't match
    // the canonicalized absolute filepath, causing false rejections.
    auto canonAndNorm = [&normSep](const std::string& p) -> std::string {
        try {
            return normSep(std::filesystem::weakly_canonical(p).string());
        } catch (...) {
            return normSep(p);
        }
    };

    // V-GOV-022: Path prefix match with directory boundary validation.
    // Ensures /data/safe doesn't match /data/safe_malicious.
    auto pathPrefixMatch = [](const std::string& path, const std::string& prefix) -> bool {
        if (path.find(prefix) != 0) return false;
        // Exact match or next char is '/' (directory boundary)
        return path.size() == prefix.size() ||
               prefix.back() == '/' ||
               path[prefix.size()] == '/';
    };

    // Layer 1: Check allowed_paths first (specific allow beats broad deny)
    bool explicitly_allowed = false;
    if (!rules_.capabilities.filesystem.allowed_paths.empty()) {
        for (const auto& ap : rules_.capabilities.filesystem.allowed_paths) {
            if (pathPrefixMatch(canon_n, canonAndNorm(ap))) {
                explicitly_allowed = true;
                break;
            }
        }
        if (!explicitly_allowed) {
            // V-GOV-025: Show resolved paths to help diagnose mismatches
            std::string resolved_list;
            std::string raw_list;
            for (const auto& ap : rules_.capabilities.filesystem.allowed_paths) {
                if (!resolved_list.empty()) { resolved_list += ", "; raw_list += ", "; }
                resolved_list += canonAndNorm(ap);
                raw_list += ap;
            }
            // Show first allowed path as example
            std::string example_path = rules_.capabilities.filesystem.allowed_paths.empty()
                ? "allowed/path/file.txt"
                : rules_.capabilities.filesystem.allowed_paths[0] + "file.txt";
            return enforce("capabilities.filesystem.path", EnforcementLevel::HARD,
                formatError(EnforcementLevel::HARD,
                    "File path not in allowed paths: " + filepath,
                    "Resolved to: " + canon_n + "\n  Allowed (resolved): " + resolved_list,
                    "capabilities.filesystem.allowed_paths does not match",
                    "Only paths under these directories are accessible: " + raw_list,
                    "file." + mode + "(\"" + filepath + "\", ...)",
                    "file." + mode + "(\"" + example_path + "\", ...)"));
        }
    }

    // Layer 2: blocked_paths — but skip if path is explicitly allowed
    // (specific allow like "./data/" beats broad deny like "/")
    if (!explicitly_allowed) {
        for (const auto& bp : rules_.capabilities.filesystem.blocked_paths) {
            if (pathPrefixMatch(canon_n, canonAndNorm(bp))) {
                return enforce("capabilities.filesystem.path", EnforcementLevel::HARD,
                    formatError(EnforcementLevel::HARD,
                        "File path blocked by governance: " + filepath,
                        "",
                        "capabilities.filesystem.blocked_paths contains \"" + bp + "\"",
                        "This path is blocked by the project's governance configuration.\n"
                        "Use a path under an allowed directory (e.g., ./data or ./output).",
                        "file." + mode + "(\"" + filepath + "\", ...)",
                        "file." + mode + "(\"./data/my_file.txt\", ...)"));
            }
        }
    }

    // Layer 3+4: Agent role path restrictions
    for (const auto& role : rules_.agents) {
        if (role.name == agent_id_) {
            // Agent blocked_paths
            for (const auto& bp : role.blocked_paths) {
                if (pathPrefixMatch(canon_n, canonAndNorm(bp))) {
                    return enforce("agent_role.path", EnforcementLevel::HARD,
                        formatError(EnforcementLevel::HARD,
                            "Agent '" + agent_id_ + "' blocked from path: " + filepath,
                            "",
                            "agents." + agent_id_ + ".blocked_paths contains \"" + bp + "\"",
                            "Your agent role does not permit access to this path.\n"
                            "Use a path within your agent's allowed directories.",
                            "file." + mode + "(\"" + filepath + "\", ...)",
                            !role.allowed_paths.empty()
                                ? "file." + mode + "(\"" + role.allowed_paths[0] + "/my_file.txt\", ...)"
                                : "file." + mode + "(\"./data/my_file.txt\", ...)"));
                }
            }
            // Agent allowed_paths (if non-empty, must match one)
            if (!role.allowed_paths.empty()) {
                bool agent_allowed = false;
                for (const auto& ap : role.allowed_paths) {
                    if (pathPrefixMatch(canon_n, canonAndNorm(ap))) {
                        agent_allowed = true;
                        break;
                    }
                }
                if (!agent_allowed) {
                    return enforce("agent_role.path", EnforcementLevel::HARD,
                        formatError(EnforcementLevel::HARD,
                            "Agent '" + agent_id_ + "' not allowed to access: " + filepath,
                            "",
                            "agents." + agent_id_ + ".allowed_paths",
                            fmt::format("Your agent role restricts file access to: {}",
                                [&]() { std::string l; for (const auto& p : role.allowed_paths) {
                                    if (!l.empty()) l += ", "; l += p; } return l; }()),
                            "file." + mode + "(\"" + filepath + "\", ...)",
                            "file." + mode + "(\"" + role.allowed_paths[0] + "/my_file.txt\", ...)"));
                }
            }
            break;
        }
    }

    recordPass("capabilities.filesystem.path", EnforcementLevel::HARD);
    return "";
}

std::string GovernanceEngine::checkShellAllowed() {
    clearTrace();
    if (!rules_.shell_allowed) {
        return enforce("capabilities.shell", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                "Shell execution is not allowed",
                "",
                "capabilities.shell = false",
                "Shell/bash polyglot blocks are disabled by governance\n"
                "Use NAAb stdlib or other allowed languages instead",
                "let result = <<shell\nls -la\n>>",
                "let files = file.list(\".\")"));
    }
    // V-GOV-020: Per-agent shell enforcement (defense-in-depth beyond applyAgentRole)
    for (const auto& role : rules_.agents) {
        if (role.name == agent_id_) {
            if (role.shell_allowed_set && !role.shell_allowed) {
                return enforce("agent_role.shell", EnforcementLevel::HARD,
                    formatError(EnforcementLevel::HARD,
                        "Agent '" + agent_id_ + "' is not allowed to execute shell blocks",
                        "",
                        "agents." + agent_id_ + ".shell_allowed = false",
                        "Your agent role does not permit shell execution.\n"
                        "Use NAAb stdlib or another allowed language instead.",
                        "let result = <<shell\nls -la\n>>",
                        "let files = file.list(\".\")  // NAAb stdlib"));
            }
            // Action matrix: check SHELL_EXEC
            if (!role.allowed_actions.empty()) {
                bool allowed = false;
                for (const auto& a : role.allowed_actions) {
                    if (a == "SHELL_EXEC") { allowed = true; break; }
                }
                if (!allowed) {
                    return enforce("agent_role.action_matrix", EnforcementLevel::HARD,
                        formatError(EnforcementLevel::HARD,
                            "Agent '" + agent_id_ + "' action matrix does not include SHELL_EXEC",
                            "",
                            "agents." + agent_id_ + ".allowed_actions",
                            "Your agent's allowed_actions list does not include SHELL_EXEC.\n"
                            "Add SHELL_EXEC to the allowed_actions list to permit shell execution.",
                            "let result = <<shell\nls -la\n>>",
                            "let files = file.list(\".\")  // NAAb stdlib"));
                }
            }
            break;
        }
    }
    recordPass("capabilities.shell", EnforcementLevel::HARD);
    return "";
}

std::string GovernanceEngine::checkCallDepth(size_t current_depth) {
    clearTrace();
    if (rules_.max_call_depth > 0 &&
        static_cast<int>(current_depth) > rules_.max_call_depth) {
        return enforce("limits.call_depth", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Call depth {} exceeds limit of {}",
                    current_depth, rules_.max_call_depth),
                "",
                fmt::format("limits.call_depth = {}", rules_.max_call_depth),
                "Function call depth exceeded — likely infinite recursion.\n"
                "Add a base case to recursive functions, or convert to iterative logic.",
                "fn count(n) { return count(n + 1) }  // no base case",
                "fn count(n) {\n    if n >= 100 { return n }\n    return count(n + 1)\n}"));
    }
    return "";
}

std::string GovernanceEngine::checkArraySize(size_t size) {
    clearTrace();
    if (rules_.max_array_size > 0 &&
        size > static_cast<size_t>(rules_.max_array_size)) {
        return enforce("limits.array_size", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Array size {} exceeds limit of {}",
                    size, rules_.max_array_size),
                "",
                fmt::format("limits.array_size = {}", rules_.max_array_size),
                "Array exceeds maximum element count — process data in batches.\n"
                "Split large collections into smaller chunks for processing.",
                "let all = range(0, 1000000)  // million-element array",
                "for batch in 0..10 {\n    let chunk = load_batch(batch, 1000)\n    process(chunk)\n}"));
    }
    return "";
}

std::string GovernanceEngine::checkPolyglotOutput(const std::string& output) {
    clearTrace();
    if (rules_.polyglot_output == "json") {
        // Try to parse as JSON
        try {
            (void)nlohmann::json::parse(output);
        } catch (...) {
            return enforce("restrictions.polyglot_output", EnforcementLevel::HARD,
                formatError(EnforcementLevel::HARD,
                    "Polyglot block must return valid JSON",
                    "",
                    "restrictions.polyglot_output = \"json\"",
                    "All polyglot blocks must return valid JSON output\n"
                    "Use json.dumps() or JSON.stringify() to format output",
                    "print(\"hello world\")",
                    "import json\nprint(json.dumps({\"message\": \"hello world\"}))"));
        }
    }
    return "";
}

std::string GovernanceEngine::checkDangerousCall(
    const std::string& language, const std::string& code, int line) {
    clearTrace();

    if (!rules_.restrict_dangerous_calls) return "";

    for (const auto& pattern : DANGEROUS_PATTERNS_DB) {
        // Check if pattern applies to this language
        if (pattern.language != "any" && pattern.language != language) continue;

        try {
            std::regex re(pattern.pattern, std::regex::icase);
            std::smatch match;
            if (std::regex_search(code, match, re)) {
                std::string location = line > 0
                    ? fmt::format("line {}: {} block", line, language)
                    : fmt::format("{} block", language);

                // FIX 29: Include matched text for easier debugging
                std::string matched_text = match[0].str();
                if (matched_text.size() > 60) matched_text = matched_text.substr(0, 60) + "...";

                return enforce("restrictions.dangerous_calls",
                    rules_.dangerous_calls_level,
                    formatError(rules_.dangerous_calls_level,
                        fmt::format("Dangerous pattern in {} block: {}",
                            language, pattern.description),
                        location,
                        fmt::format("restrictions.dangerous_calls = \"{}\"",
                            levelToString(rules_.dangerous_calls_level)),
                        fmt::format("{}\n  Matched: \"{}\"\n{}",
                            pattern.description, matched_text,
                            pattern.safe_alternative),
                        "", ""));
            }
        } catch (const std::regex_error&) {
            // Skip invalid patterns silently
        }
    }

    recordPass("restrictions.dangerous_calls", rules_.dangerous_calls_level);
    return "";
}

std::string GovernanceEngine::checkSecrets(
    const std::string& code, int line) {
    clearTrace();

    if (!rules_.no_secrets) return "";

    for (const auto& pattern : SECRET_PATTERNS) {
        try {
            std::regex re(pattern.pattern, std::regex::icase);
            std::smatch match;
            if (std::regex_search(code, match, re)) {
                std::string matched = match[0].str();
                // Mask the secret (show first 4, mask middle, show last 4)
                std::string masked;
                if (matched.size() > 10) {
                    masked = matched.substr(0, 4) +
                             std::string(matched.size() - 8, '*') +
                             matched.substr(matched.size() - 4);
                } else {
                    masked = std::string(matched.size(), '*');
                }

                std::string location = line > 0
                    ? fmt::format("line {}: {}", line, masked)
                    : masked;

                return enforce("code_quality.no_secrets",
                    rules_.no_secrets_level,
                    formatError(rules_.no_secrets_level,
                        fmt::format("Secret detected: {}", pattern.description),
                        location,
                        fmt::format("code_quality.no_secrets = \"{}\"",
                            levelToString(rules_.no_secrets_level)),
                        "Never hardcode secrets in source code\n"
                        "Use environment variables instead",
                        fmt::format("{} = \"{}\"",
                            pattern.description, masked),
                        "import os\n"
                        "key = os.environ[\"YOUR_KEY_NAME\"]\n\n"
                        "  In NAAb:\n"
                        "    let key = env.get_var(\"YOUR_KEY_NAME\")"));
            }
        } catch (const std::regex_error&) {
            // Skip invalid patterns
        }
    }

    recordPass("code_quality.no_secrets", rules_.no_secrets_level);
    return "";
}

std::string GovernanceEngine::checkPlaceholders(
    const std::string& code, int line) {
    clearTrace();

    if (!rules_.no_placeholders) return "";

    for (const auto& placeholder : PLACEHOLDER_PATTERNS_DB) {
        // Case-insensitive word boundary search
        try {
            std::regex re("\\b" + placeholder + "\\b", std::regex::icase);
            std::smatch match;
            if (std::regex_search(code, match, re)) {
                // Find the line containing the match
                std::string matched_line;
                std::istringstream stream(code);
                std::string l;
                auto offset = static_cast<int>(match.position());
                int pos = 0;
                while (std::getline(stream, l)) {
                    if (pos + static_cast<int>(l.size()) >= offset) {
                        matched_line = l;
                        break;
                    }
                    pos += l.size() + 1;
                }

                // Trim the matched line
                size_t start = matched_line.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    matched_line = matched_line.substr(start);
                }

                // Skip false positives: match arms (e.g. "Draft => ...") and
                // enum declarations (e.g. "Draft,") — these are identifiers, not placeholders
                {
                    std::regex arm_pat("\\b" + placeholder + "\\s*=>",
                                      std::regex::icase);
                    std::regex enum_pat("^\\s*" + placeholder + "\\s*[,}]",
                                        std::regex::icase);
                    if (std::regex_search(matched_line, arm_pat) ||
                        std::regex_search(matched_line, enum_pat)) {
                        continue;  // Not a real placeholder — skip
                    }
                }
                if (matched_line.size() > 80) {
                    matched_line = matched_line.substr(0, 80) + "...";
                }

                std::string location = line > 0
                    ? fmt::format("line {}: {}", line, matched_line)
                    : matched_line;

                return enforce("code_quality.no_placeholders",
                    rules_.no_placeholders_level,
                    formatError(rules_.no_placeholders_level,
                        fmt::format("Placeholder \"{}\" found in code", placeholder),
                        location,
                        fmt::format("code_quality.no_placeholders = \"{}\"",
                            levelToString(rules_.no_placeholders_level)),
                        "Code must be complete — no placeholder markers allowed.\n"
                        "Implement the actual functionality instead of deferring.\n"
                        "The most common fix: delete the comment and write real logic.",
                        "// TODO: implement validation\nfn validate(x) { return true }",
                        "fn validate(x) {\n    if type(x) != \"string\" { return false }\n    return x.length() > 0\n}"));
            }
        } catch (const std::regex_error&) {
            // Skip invalid patterns
        }
    }

    recordPass("code_quality.no_placeholders", rules_.no_placeholders_level);
    return "";
}

std::string GovernanceEngine::checkHardcodedResults(
    const std::string& code, int line) {
    clearTrace();

    if (!rules_.no_hardcoded_results) return "";

    for (const auto& pattern : HARDCODED_RESULT_PATTERNS_DB) {
        try {
            std::regex re(pattern.pattern, std::regex::icase);
            std::smatch match;
            if (std::regex_search(code, match, re)) {
                std::string matched = match[0].str();
                if (matched.size() > 60) {
                    matched = matched.substr(0, 60) + "...";
                }

                std::string location = line > 0
                    ? fmt::format("line {}: {}", line, matched)
                    : matched;

                return enforce("code_quality.no_hardcoded_results",
                    rules_.no_hardcoded_results_level,
                    formatError(rules_.no_hardcoded_results_level,
                        fmt::format("Hardcoded result: {}", pattern.description),
                        location,
                        fmt::format("code_quality.no_hardcoded_results = \"{}\"",
                            levelToString(rules_.no_hardcoded_results_level)),
                        "Code must contain real logic, not hardcoded return values\n"
                        "Implement actual validation/processing instead",
                        "def validate(data):\n    return True  # for now",
                        "def validate(data):\n"
                        "    if not isinstance(data, dict):\n"
                        "        return False\n"
                        "    return \"name\" in data and \"value\" in data"));
            }
        } catch (const std::regex_error&) {
            // Skip invalid patterns
        }
    }

    recordPass("code_quality.no_hardcoded_results",
        rules_.no_hardcoded_results_level);
    return "";
}

// ============================================================================
// Advisory Output Control
// ============================================================================

void GovernanceEngine::emitAdvisory(const std::string& msg) {
    int max = rules_.output.max_advisories;
    if (max > 0 && advisory_count_ >= max) {
        advisory_suppressed_++;
        return;
    }
    advisory_count_++;
    fmt::print(stderr, "{}\n", msg);
}

void GovernanceEngine::flushGroupedAdvisories() {
    // 1. Grouped duplicate call warnings
    if (!dup_call_summary_.empty()) {
        std::string msg = "[ADVISORY] Duplicate calls (store results in variables):";
        int shown = 0;
        for (const auto& [call, entries] : dup_call_summary_) {
            if (shown >= rules_.code_quality.duplicate_calls.max_entries) {
                msg += fmt::format("\n  ... and {} more unique calls",
                    static_cast<int>(dup_call_summary_.size()) - shown);
                break;
            }
            // Build compact location list
            std::string locs;
            for (size_t i = 0; i < entries.size() && i < 3; i++) {
                if (i > 0) locs += ", ";
                locs += fmt::format("{}:{}", entries[i].function_name, entries[i].line);
            }
            if (entries.size() > 3) {
                locs += fmt::format(", +{} more", entries.size() - 3);
            }
            msg += fmt::format("\n  {}  — {}x in: {}", call, entries[0].count, locs);
            shown++;
        }
        emitAdvisory(msg);
    }

    // 2. Grouped polyglot try/catch warnings
    if (!ptc_functions_.empty()) {
        std::string msg = "[ADVISORY] Polyglot blocks without try/catch:";
        int shown = 0;
        for (const auto& [name, line] : ptc_functions_) {
            if (shown >= rules_.code_quality.polyglot_try_catch.max_entries) {
                msg += fmt::format(", +{} more",
                    static_cast<int>(ptc_functions_.size()) - shown);
                break;
            }
            msg += (shown == 0 ? " " : ", ") + fmt::format("{}:{}", name, line);
            shown++;
        }
        msg += "\n  Wrap polyglot blocks in try/catch for graceful error handling.";
        emitAdvisory(msg);
    }

    // 3. Suppression summary
    if (advisory_suppressed_ > 0 && rules_.output.advisory_summary) {
        fmt::print(stderr, "[ADVISORY] ... and {} more advisories suppressed "
                   "(increase output.max_advisories to see all)\n", advisory_suppressed_);
    }

    // Reset
    dup_call_summary_.clear();
    ptc_functions_.clear();
    emitted_advisories_.clear();
    advisory_count_ = 0;
    advisory_suppressed_ = 0;
}

// ============================================================================
// Execution Summary
// ============================================================================

std::string GovernanceEngine::formatSummary() const {
    if (check_results_.empty()) return "";

    int passed = 0, warned = 0, blocked = 0;
    for (const auto& r : check_results_) {
        if (r.passed) {
            passed++;
        } else if (r.level == EnforcementLevel::ADVISORY) {
            warned++;
        } else {
            blocked++;
        }
    }

    std::ostringstream oss;
    std::string mode_str = "enforce";
    if (rules_.mode == GovernanceMode::AUDIT) mode_str = "audit";
    else if (rules_.mode == GovernanceMode::OFF) mode_str = "off";

    oss << "[governance] Summary (mode: " << mode_str << "): "
        << passed << " passed, "
        << warned << " warning" << (warned != 1 ? "s" : "") << ", "
        << blocked << " blocked\n";

    // Voice summary replaces individual violation details
    if (governance_voiced_ && !governance_voice_summary_.empty()) {
        oss << "\n" << governance_voice_summary_ << "\n";
        if (rules_.scoring.enabled && cumulative_score_ > 0) {
            const char* zone = cumulative_score_ >= rules_.scoring.red_threshold ? "RED" :
                               cumulative_score_ >= rules_.scoring.yellow_threshold ? "YELLOW" : "green";
            oss << fmt::format("  Risk score: {} ({})\n", cumulative_score_, zone);
        }
        return oss.str();
    }

    // Deduplicate results by rule_name (show only unique rules)
    std::unordered_map<std::string, const CheckResult*> unique_results;
    for (const auto& r : check_results_) {
        auto it = unique_results.find(r.rule_name);
        if (it == unique_results.end()) {
            unique_results[r.rule_name] = &r;
        } else if (!r.passed) {
            // Prefer showing failures over passes
            unique_results[r.rule_name] = &r;
        }
    }

    for (const auto& [name, r] : unique_results) {
        if (r->passed) {
            oss << fmt::format("  ✓ {:<35} [{}]  PASS\n",
                name, levelToString(r->level));
        } else if (r->level == EnforcementLevel::ADVISORY) {
            // Extract first line of message
            std::string first_line = r->message.substr(
                0, r->message.find('\n'));
            oss << fmt::format("  ⚠ {:<35} [{}]  WARN\n",
                name, levelToString(r->level));
        } else {
            oss << fmt::format("  ✗ {:<35} [{}]  BLOCKED\n",
                name, levelToString(r->level));
        }
    }

    if (rules_.scoring.enabled && cumulative_score_ > 0) {
        const char* zone = cumulative_score_ >= rules_.scoring.red_threshold ? "RED" :
                           cumulative_score_ >= rules_.scoring.yellow_threshold ? "YELLOW" : "green";
        oss << fmt::format("  Risk score: {} ({})\n", cumulative_score_, zone);
    }

    return oss.str();
}

// ============================================================================
// Agent Review — LLM-based governance phase
// ============================================================================

void GovernanceEngine::runAgentReview(const std::string& source) {
    if (!rules_.agent_review.enabled) return;

    // Build AgentReviewConfig from govern.json section
    runtime::AgentReviewConfig config;
    config.enabled = true;
    config.detection_agents = rules_.agent_review.detection;
    config.validation_agent = rules_.agent_review.validation;
    config.voice_agent = rules_.agent_review.voice;
    config.scorer_name = rules_.agent_review.scorer;
    config.enforcement = rules_.agent_review.enforcement;
    config.cache = rules_.agent_review.cache;
    config.hints = rules_.agent_review.hints;
    config.dispatch_mode = rules_.agent_review.dispatch_mode;
    config.max_parallel = rules_.agent_review.max_parallel;
    config.fail_strategy = rules_.agent_review.fail_strategy;

    // Include govern.json hash in cache key so config changes invalidate cache
    std::string govern_path = govern_json_dir_ + "/govern.json";
    std::ifstream gf(govern_path);
    if (gf.is_open()) {
        std::string govern_content((std::istreambuf_iterator<char>(gf)),
                                    std::istreambuf_iterator<char>());
        config.config_hash = security::CryptoUtils::sha256(govern_content);
    }

    auto result = runtime::runAgentReview(config, rules_, source, govern_json_dir_);

    if (result.cache_hit) {
        fprintf(stderr, "[governance] Agent review: cache hit (source + config unchanged)\n");
    }
    if (!result.error.empty()) {
        fprintf(stderr, "[governance] Agent review error: %s\n", result.error.c_str());
        // F10: fail_policy controls behavior on agent review errors
        if (rules_.agent_review.fail_policy == "closed") {
            fprintf(stderr, "[governance] Agent review fail_policy:closed — blocking execution.\n");
            g_governance_hard_block = true;
        }
        return;
    }
    if (!result.executed) return;

    // Determine enforcement level from zone
    std::string level_str = "advisory";
    auto zone_it = config.enforcement.find(result.zone);
    if (zone_it != config.enforcement.end()) level_str = zone_it->second;

    EnforcementLevel level = EnforcementLevel::ADVISORY;
    if (level_str == "hard") level = EnforcementLevel::HARD;
    else if (level_str == "soft") level = EnforcementLevel::SOFT;

    // Feed deduplicated findings through enforce() (for violation counting + quality gate)
    for (const auto& f : result.findings) {
        enforce("agent_review." + f.category, level, f.message);
    }

    agent_review_count_ = result.confirmed_count;

    // Print the voice summary — one cohesive remediation guide
    if (!result.findings.empty()) {
        fprintf(stderr, "\n[governance] Agent Review — %zu issue(s) found (score=%d, %s/%s):\n",
                result.findings.size(), result.score, result.zone.c_str(), level_str.c_str());

        if (!result.voice_summary.empty()) {
            // Voice agent produced a synthesized guide
            fprintf(stderr, "\n%s\n\n", result.voice_summary.c_str());
            agent_review_voiced_ = true;
        } else {
            // Fallback: compact table (no voice agent configured or it failed)
            for (const auto& f : result.findings) {
                fprintf(stderr, "  %-30s (%s) %s\n",
                        f.category.c_str(), f.source_agent.c_str(), f.message.c_str());
            }
            fprintf(stderr, "\n");
        }
    }

    // Print rejected findings as hints (when enabled in govern.json)
    if (rules_.agent_review.hints && !result.rejected_findings.empty()) {
        fprintf(stderr, "[hint] Agent review rejected %zu finding(s):\n",
                result.rejected_findings.size());
        for (const auto& f : result.rejected_findings) {
            fprintf(stderr, "[hint]   [%s] (%s) %s\n",
                    f.category.c_str(), f.source_agent.c_str(),
                    f.message.substr(0, 120).c_str());
        }
    }
}

// ============================================================================
// Governance Voice — unified output for blocking errors
// ============================================================================

void GovernanceEngine::setSource(const std::string& source) {
    source_ = source;
}

void GovernanceEngine::runGovernanceVoice() {
    if (rules_.output.voice.empty() || source_.empty()) return;

    // Collect non-passing results
    std::vector<const CheckResult*> violations;
    for (const auto& r : check_results_) {
        if (!r.passed) violations.push_back(&r);
    }
    if (violations.empty()) return;

    // Find voice agent config
    const AgentConfig* voice_cfg = nullptr;
    for (const auto& a : rules_.agents) {
        if (a.name == rules_.output.voice) { voice_cfg = &a; break; }
    }
    if (!voice_cfg) {
        fprintf(stderr, "[governance] Voice agent '%s' not found in agents config\n",
                rules_.output.voice.c_str());
        return;
    }

    std::string voice_key = runtime::resolveApiKey(voice_cfg->api_key_env);
    if (voice_key.empty()) return;

    // Cache key: sha256(source_hash + config_hash)
    std::string source_hash = security::CryptoUtils::sha256(source_);
    std::string config_hash;
    if (!govern_json_dir_.empty()) {
        std::string govern_path = govern_json_dir_ + "/govern.json";
        std::ifstream gf(govern_path);
        if (gf.is_open()) {
            std::string content((std::istreambuf_iterator<char>(gf)),
                                 std::istreambuf_iterator<char>());
            config_hash = security::CryptoUtils::sha256(content);
        }
    }
    std::string cache_key = security::CryptoUtils::sha256(source_hash + config_hash);

    // Cache check
    if (rules_.output.voice_cache && !govern_json_dir_.empty()) {
        std::string cpath = govern_json_dir_ + "/.naab_cache/" + cache_key + ".voice.json";
        std::ifstream cf(cpath);
        if (cf.is_open()) {
            try {
                std::string raw((std::istreambuf_iterator<char>(cf)),
                                 std::istreambuf_iterator<char>());
                auto wrapper = nlohmann::json::parse(raw);
                if (wrapper.contains("data") && wrapper.contains("hmac")) {
                    std::string data_str = wrapper["data"].get<std::string>();
                    std::string stored_hmac = wrapper["hmac"].get<std::string>();
                    std::string hmac_key = security::CryptoUtils::sha256(
                        config_hash + ":" + source_hash);
                    std::string computed_hmac = security::CryptoUtils::hmacSha256(
                        data_str, hmac_key);
                    if (computed_hmac == stored_hmac) {
                        auto data = nlohmann::json::parse(data_str);
                        governance_voice_summary_ = data.value("voice_summary", "");
                        if (!governance_voice_summary_.empty()) {
                            governance_voiced_ = true;
                            fprintf(stderr,
                                "[governance] Voice: cache hit (source + config unchanged)\n");
                            fprintf(stderr,
                                "\n[governance] Voice Summary (%zu violation%s):\n\n%s\n\n",
                                violations.size(), violations.size() != 1 ? "s" : "",
                                governance_voice_summary_.c_str());
                            return;
                        }
                    }
                }
            } catch (...) {
                // Cache corrupt — fall through to re-generate
            }
        }
    }

    // Build finding list from violations
    std::string finding_list;
    int num = 1;
    for (const auto* v : violations) {
        finding_list += std::to_string(num++) + ". [" + v->rule_name + "] "
                     + levelToString(v->level) + " — " + v->message + "\n";
    }

    // Voice prompt
    std::string prompt;
    prompt += "You are a governance advisor for NAAb scripts. ";
    prompt += "Static governance checks found these violations:\n\n";
    prompt += "Violations:\n" + finding_list + "\n";
    prompt += "Script:\n" + source_ + "\n\n";
    prompt += "Respond with ONLY a numbered list of fixes. Nothing else.\n";
    prompt += "Rules:\n";
    prompt += "- One line per fix. Reference the function name and what to change.\n";
    prompt += "- No thinking, no reasoning, no preamble, no summary, no explanation.\n";
    prompt += "- Do NOT include bypass instructions or governance flags.\n";
    prompt += "- Maximum 2 sentences per fix.\n";
    prompt += "Example output format:\n";
    prompt += "1. In func_name(), replace X with Y.\n";
    prompt += "2. In other_func(), remove Z and use W instead.\n";

    auto resp = runtime::callAgentSimple(*voice_cfg, voice_key, prompt);
    if (resp.success && !resp.content.empty()) {
        // Strip model thinking/reasoning lines (lines starting with * or whitespace+*)
        std::string cleaned;
        std::istringstream stream(resp.content);
        std::string line;
        while (std::getline(stream, line)) {
            // Skip thinking lines: start with *, whitespace+*, or are empty between thinking blocks
            std::string trimmed = line;
            size_t pos = trimmed.find_first_not_of(" \t");
            if (pos != std::string::npos && trimmed[pos] == '*') continue;
            if (!trimmed.empty()) {
                if (!cleaned.empty()) cleaned += "\n";
                cleaned += line;
            }
        }
        governance_voice_summary_ = cleaned.empty() ? resp.content : cleaned;
        governance_voiced_ = true;

        // Cache save (HMAC wrapper, same pattern as agent_review)
        if (rules_.output.voice_cache && !govern_json_dir_.empty()) {
            std::string cache_dir = govern_json_dir_ + "/.naab_cache";
#ifdef _WIN32
            _mkdir(cache_dir.c_str());
#else
            mkdir(cache_dir.c_str(), 0755);
#endif
            nlohmann::json data;
            data["voice_summary"] = governance_voice_summary_;
            data["violation_count"] = static_cast<int>(violations.size());
            data["source_hash"] = source_hash;

            std::string data_str = data.dump(2);
            std::string hmac_key = security::CryptoUtils::sha256(
                config_hash + ":" + source_hash);
            std::string hmac = security::CryptoUtils::hmacSha256(data_str, hmac_key);

            nlohmann::json wrapper;
            wrapper["data"] = data_str;
            wrapper["hmac"] = hmac;

            std::string cpath = govern_json_dir_ + "/.naab_cache/" + cache_key + ".voice.json";
            std::ofstream out(cpath);
            if (out.is_open()) {
                out << wrapper.dump(2);
            }
        }
    }

    // Print voice summary immediately — on error paths the summary formatter may not run
    if (governance_voiced_ && !governance_voice_summary_.empty()) {
        fprintf(stderr, "\n[governance] Voice Summary (%zu violation%s):\n\n%s\n\n",
                violations.size(), violations.size() != 1 ? "s" : "",
                governance_voice_summary_.c_str());
    }
}

bool GovernanceEngine::hasIntentBlock() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    for (const auto& r : check_results_) {
        if (!r.passed &&
            (r.rule_name == "agent_review.intent_mismatch" ||
             r.rule_name == "agent_review.intent_evasion") &&
            r.level != EnforcementLevel::ADVISORY) {
            return true;
        }
    }
    return false;
}

std::string GovernanceEngine::formatSummaryOneLine() const {
    if (check_results_.empty()) return "";

    int passed = 0, warned = 0, blocked = 0;
    for (const auto& r : check_results_) {
        if (r.passed) passed++;
        else if (r.level == EnforcementLevel::ADVISORY) warned++;
        else blocked++;
    }

    // Silent when all passed — clean output
    if (warned == 0 && blocked == 0) return "";

    std::ostringstream oss;
    std::string mode_str = "enforce";
    if (rules_.mode == GovernanceMode::AUDIT) mode_str = "audit";
    else if (rules_.mode == GovernanceMode::OFF) mode_str = "off";

    oss << "[governance] Summary (mode: " << mode_str << "): "
        << passed << " passed, "
        << warned << " warning" << (warned != 1 ? "s" : "") << ", "
        << blocked << " blocked\n";

    // Voice summary replaces individual violation details
    if (governance_voiced_ && !governance_voice_summary_.empty()) {
        oss << "\n" << governance_voice_summary_ << "\n";
        return oss.str();
    }

    // Show details only for non-passing rules
    // Skip agent_review.* rules when voice summary was already printed (redundant)
    std::unordered_map<std::string, const CheckResult*> unique_results;
    for (const auto& r : check_results_) {
        if (r.passed) continue;
        if (agent_review_voiced_ && r.rule_name.rfind("agent_review.", 0) == 0) continue;
        auto it = unique_results.find(r.rule_name);
        if (it == unique_results.end()) unique_results[r.rule_name] = &r;
        else if (!r.passed) unique_results[r.rule_name] = &r;
    }
    for (const auto& [name, r] : unique_results) {
        if (r->level == EnforcementLevel::ADVISORY) {
            oss << fmt::format("  ⚠ {:<35} [{}]  WARN\n", name, levelToString(r->level));
        } else {
            oss << fmt::format("  ✗ {:<35} [{}]  BLOCKED\n", name, levelToString(r->level));
        }
    }
    return oss.str();
}

// ============================================================================
// Dashboard Summary (--governance-dashboard)
// ============================================================================

void GovernanceEngine::printDashboard() const {
    int passed = 0, blocked = 0;
    std::map<std::string, int> block_counts;
    for (const auto& r : check_results_) {
        if (r.passed) {
            passed++;
        } else {
            blocked++;
            block_counts[r.rule_name]++;
        }
    }

    // Find top violation
    std::string top_rule;
    int top_count = 0;
    for (const auto& [rule, count] : block_counts) {
        if (count > top_count) {
            top_count = count;
            top_rule = rule;
        }
    }

    fprintf(stderr, "\n─── Agent Governance Summary ───\n");
    fprintf(stderr, "Agent:      %s\n", agent_id_.c_str());
    // Show active config context
    std::string mode_str = (rules_.mode == GovernanceMode::ENFORCE) ? "enforce"
                         : (rules_.mode == GovernanceMode::AUDIT) ? "audit" : "off";
    std::string config_line = "Mode:       " + mode_str;
    if (!active_env_.empty()) config_line += " | Env: " + active_env_;
    if (!rules_.sandbox_level_config.empty() && rules_.sandbox_level_config != "unrestricted")
        config_line += " | Sandbox: " + rules_.sandbox_level_config;
    fprintf(stderr, "%s\n", config_line.c_str());
    if (reload_count_ > 0)
        fprintf(stderr, "Reloads:    %d mid-run reload(s) applied\n", reload_count_);
    fprintf(stderr, "Checks:     %d passed, %d blocked\n", passed, blocked);
    if (!top_rule.empty())
        fprintf(stderr, "Top block:  %s (%d violation%s)\n",
                top_rule.c_str(), top_count, top_count != 1 ? "s" : "");
    // Show explanations for blocked checks (up to 3), fallback to rationale
    {
        int shown = 0;
        for (const auto& r : check_results_) {
            if (!r.passed && shown < 3) {
                if (!r.explanation.empty()) {
                    fprintf(stderr, "  Detail:   %s\n", r.explanation.c_str());
                    shown++;
                } else if (!r.rationale.empty()) {
                    fprintf(stderr, "  Why:      %s — %s\n", r.rule_name.c_str(), r.rationale.c_str());
                    shown++;
                }
            }
        }
    }
    if (rules_.scoring.enabled && cumulative_score_ > 0) {
        const char* zone = cumulative_score_ >= rules_.scoring.red_threshold ? "RED" :
                           cumulative_score_ >= rules_.scoring.yellow_threshold ? "YELLOW" : "green";
        fprintf(stderr, "Risk score: %d (%s) [%d/%d]\n",
                cumulative_score_, zone,
                rules_.scoring.yellow_threshold, rules_.scoring.red_threshold);
        // Count occurrences per rule for display
        std::unordered_map<std::string, int> occ_count;
        for (const auto& r : check_results_) {
            if (!r.passed && r.level == EnforcementLevel::ADVISORY &&
                r.rule_name.compare(0, 6, "pass2.") != 0) {
                occ_count[r.rule_name]++;
            }
        }
        std::vector<std::pair<std::string, int>> sorted(
            score_contributions_.begin(), score_contributions_.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        int shown = 0;
        for (const auto& [rule, score] : sorted) {
            if (shown++ >= 3) break;
            int count = occ_count.count(rule) ? occ_count[rule] : 1;
            int per = count > 0 ? score / count : score;
            if (count > 1) {
                fprintf(stderr, "  +%d  %s (%dx @%d)\n", score, rule.c_str(), count, per);
            } else {
                fprintf(stderr, "  +%d  %s\n", score, rule.c_str());
            }
        }
        if (!verifyScoreIntegrity()) {
            fprintf(stderr, "  INTEGRITY: score mismatch (incremental vs recomputed)\n");
        }
    }
    if (rules_.telemetry_output.enabled)
        fprintf(stderr, "Telemetry:  %zu events → %s\n",
                check_results_.size(), rules_.telemetry_output.output_file.c_str());
    // BSD/CDD feature summary
    if (bsd_enabled_.load(std::memory_order_relaxed)) {
        size_t evicted = sequence_detector_.totalEventsEvicted();
        if (evicted > 0) {
            fprintf(stderr, "BSD:        %zu events (%zu evicted), %zu patterns matched\n",
                    sequence_detector_.totalEventsProcessed(), evicted,
                    sequence_detector_.totalPatternsMatched());
        } else {
            fprintf(stderr, "BSD:        %zu events, %zu patterns matched\n",
                    sequence_detector_.totalEventsProcessed(),
                    sequence_detector_.totalPatternsMatched());
        }
    }
    if (cdd_enabled_.load(std::memory_order_relaxed)) {
        auto handle = current_agent_handle_.load(std::memory_order_relaxed);
        auto state = drift_analyzer_.getDriftState(handle);
        if (state) {
            fprintf(stderr, "CDD:        coherence=%.2f vel=%.3f accel=%.3f (%zu turns analyzed)\n",
                    state->coherence_score, state->coherence_velocity,
                    state->coherence_acceleration, drift_analyzer_.totalTurnsAnalyzed());
            if (rules_.context_drift.reality_checkpoint.enabled &&
                state->last_pressure_score > 0.0) {
                fprintf(stderr, "Checkpoint: pressure=%.2f (%d consecutive)\n",
                        state->last_pressure_score,
                        state->consecutive_high_pressure_turns);
            }
        } else {
            fprintf(stderr, "CDD:        enabled, %zu turns analyzed\n",
                    drift_analyzer_.totalTurnsAnalyzed());
        }
    }
    // Exposure tracking summary
    {
        int actions = autonomous_actions_.load(std::memory_order_relaxed);
        if (actions > 0) {
            size_t unique;
            {
                std::lock_guard<std::mutex> lock(exposure_mutex_);
                unique = unique_agents_.size();
            }
            fprintf(stderr, "Exposure:   %d autonomous actions, %zu unique agents\n",
                    actions, unique);
            // F8: Show risk budget status per agent
            {
                std::lock_guard<std::mutex> lock(risk_budget_mutex_);
                for (const auto& [name, consumed] : agent_risk_consumed_) {
                    int remaining = getRemainingBudget(name);
                    if (remaining >= 0) {
                        fprintf(stderr, "Budget:     %s: %d remaining (consumed %d)\n",
                                name.c_str(), remaining, consumed);
                    }
                }
            }
        }
    }
    if (rules_.taint_tracking.enabled && rules_.taint_tracking.lineage) {
        size_t sources = taint_lineage_.size();
        size_t sinks = taint_flows_.size();
        fprintf(stderr, "Lineage:    %zu tainted values, %zu sink flows\n", sources, sinks);
    }
    // Agent dispatch stats (key rotation, retry, fallback)
    {
        auto ds = stdlib::getAgentDispatchStats();
        if (ds.total_calls > 0) {
            fprintf(stderr, "Dispatch:   %d calls (%d retries, %d tokens, %lldms)\n",
                    ds.total_calls, ds.total_retries, ds.total_tokens,
                    static_cast<long long>(ds.total_agent_time_ms));
            if (!ds.dead_keys.empty()) {
                std::string dead_str;
                for (const auto& k : ds.dead_keys) {
                    if (!dead_str.empty()) dead_str += ", ";
                    dead_str += k;
                }
                fprintf(stderr, "Dead keys:  %s\n", dead_str.c_str());
            }
            if (ds.hard_stopped) {
                fprintf(stderr, "Hard stop:  %s\n", ds.stop_reason.c_str());
            }
            if (ds.total_tool_calls > 0) {
                fprintf(stderr, "Tools:      %d calls (%d blocked, %lldms)\n",
                        ds.total_tool_calls, ds.total_tool_calls_blocked,
                        static_cast<long long>(ds.total_tool_latency_ms));
            }
        }
    }
    // Codegen stats
    {
        auto cs = stdlib::getCodegenStats();
        if (cs.total_calls > 0 || cs.total_blocked > 0) {
            fprintf(stderr, "Codegen:    %d calls (%d blocked, %lldms)\n",
                    cs.total_calls, cs.total_blocked,
                    static_cast<long long>(cs.total_duration_ms));
        }
    }
    // Governance Pulse (no lock — printDashboard runs post-execution, same as check_results_)
    if (rules_.governance_health.enabled) {
        const char* verdict_str = "HEALTHY";
        if (pulse_.verdict == PulseVerdict::DEGRADED) verdict_str = "DEGRADED";
        else if (pulse_.verdict == PulseVerdict::IMPAIRED) verdict_str = "IMPAIRED";
        fprintf(stderr, "Pulse:      %s (%d checks, %d consecutive passes, epoch %d)\n",
                verdict_str, pulse_.total_checks, pulse_.consecutive_passes,
                governance_epoch_);
    }
    fprintf(stderr, "────────────────────────────────\n");
}

// ============================================================================
// Feature 1: wasBlocked()
// ============================================================================

bool GovernanceEngine::wasBlocked() const {
    for (const auto& r : check_results_) {
        if (!r.passed && r.level == EnforcementLevel::HARD) return true;
    }
    return false;
}

// ============================================================================
// Feature 2: Quality Gate Evaluation
// ============================================================================

std::string GovernanceEngine::evaluateQualityGate() const {
    bool audit_mode = (rules_.mode == GovernanceMode::AUDIT);

    // Quality gate conditions (only when enabled)
    if (rules_.quality_gate.enabled) {
        int hard = 0, soft = 0, advisory = 0, security = 0, total_violations = 0;
        for (const auto& r : check_results_) {
            if (r.passed) continue;
            total_violations++;
            if (r.level == EnforcementLevel::HARD) hard++;
            else if (r.level == EnforcementLevel::SOFT) soft++;
            else advisory++;
            if (r.severity == "critical" || r.severity == "high") security++;
        }

        for (const auto& cond : rules_.quality_gate.conditions) {
            int value = 0;
            if (cond.metric == "hard_violations") value = hard;
            else if (cond.metric == "soft_violations") value = soft;
            else if (cond.metric == "advisory_violations") value = advisory;
            else if (cond.metric == "security_findings") value = security;
            else if (cond.metric == "total_violations") value = total_violations;
            else if (cond.metric == "total_checks") value = static_cast<int>(check_results_.size());
            else if (cond.metric == "cumulative_risk_score") value = cumulative_score_;
            else continue;

            // Quality gate conditions use FAIL-WHEN semantics for inequality
            // operators: "advisory_violations > 0" means "fail when violations
            // exceed 0". The == operator is special: "hard_violations == 0"
            // means "require exactly 0" (fail when NOT equal).
            bool failed = false;
            if (cond.op == ">" && value > cond.threshold) failed = true;
            else if (cond.op == ">=" && value >= cond.threshold) failed = true;
            else if (cond.op == "<" && value < cond.threshold) failed = true;
            else if (cond.op == "<=" && value <= cond.threshold) failed = true;
            else if (cond.op == "==" && value != cond.threshold) failed = true;
            else if (cond.op == "!=" && value == cond.threshold) failed = true;

            if (failed) {
                if (audit_mode) {
                    fprintf(stderr, "[governance] AUDIT: Quality gate WOULD fail: %s %s %d (actual: %d)\n",
                            cond.metric.c_str(), cond.op.c_str(), cond.threshold, value);
                } else if (cond.metric == "cumulative_risk_score") {
                    int deficit = value - cond.threshold;
                    return fmt::format(
                        "[governance] Quality gate FAILED: cumulative risk score {} exceeded threshold {}\n\n"
                        "  {} points over — fix the highest-weight items first:\n\n"
                        "  Score breakdown:\n{}\n"
                        "  Help:\n"
                        "  - Fix the top contributor to drop below threshold fastest\n"
                        "  - Each fixed violation removes its weight from the score\n"
                        "  - Target: reduce score by at least {} points\n",
                        value, cond.threshold, deficit, formatScoreBreakdown(), deficit);
                } else {
                    return fmt::format(
                        "[governance] Quality gate FAILED: {} {} {} (actual: {})\n",
                        cond.metric, cond.op, cond.threshold, value);
                }
            }
        }
    }

    // Cumulative risk scoring gate (independent of quality_gate.enabled)
    if (rules_.scoring.enabled && cumulative_score_ >= rules_.scoring.red_threshold) {
        if (audit_mode) {
            fprintf(stderr, "[governance] AUDIT: Scoring gate WOULD block — score %d >= threshold %d\n",
                    cumulative_score_, rules_.scoring.red_threshold);
        } else {
            int deficit = cumulative_score_ - rules_.scoring.red_threshold;
            std::string breakdown = formatScoreBreakdown();
            return fmt::format(
                "Governance error: Cumulative risk score {} reached threshold {}\n\n"
                "  {} points over threshold — fix the highest-weight items first:\n\n"
                "  Score breakdown:\n{}\n"
                "  Help:\n"
                "  - Fix the top contributor to drop below threshold fastest\n"
                "  - Each fixed violation removes its weight from the score\n"
                "  - Target: reduce score by at least {} points\n",
                cumulative_score_, rules_.scoring.red_threshold,
                deficit, breakdown, deficit);
        }
    }

    return "";
}

// ============================================================================
// Cumulative Risk Scoring — Helpers
// ============================================================================

std::string GovernanceEngine::formatScoreBreakdown() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    // Count occurrences per rule from check_results_
    std::unordered_map<std::string, int> occurrence_count;
    for (const auto& r : check_results_) {
        if (!r.passed && r.level == EnforcementLevel::ADVISORY &&
            r.rule_name.compare(0, 6, "pass2.") != 0) {
            occurrence_count[r.rule_name]++;
        }
    }
    std::vector<std::pair<std::string, int>> sorted(
        score_contributions_.begin(), score_contributions_.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::ostringstream oss;
    bool first = true;
    for (const auto& [rule, total] : sorted) {
        int count = occurrence_count.count(rule) ? occurrence_count[rule] : 1;
        int per = count > 0 ? total / count : total;
        std::string marker = first ? "  << fix first" : "";
        first = false;
        std::string label = rule;
        if (rule == "code_quality.intent_validation.self_declared") {
            label = rule + " [supporting fns]";
        }
        if (count > 1) {
            oss << fmt::format("    +{:<4} {} ({}x @{}){}\n", total, label, count, per, marker);
        } else {
            oss << fmt::format("    +{:<4} {}{}\n", total, label, marker);
        }
    }
    return oss.str();
}

bool GovernanceEngine::verifyScoreIntegrity() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    if (!rules_.scoring.enabled) return true;
    int recomputed = 0;
    for (const auto& r : check_results_) {
        if (r.passed || r.level != EnforcementLevel::ADVISORY) continue;
        // Pass 2 entries bypass enforce() — exclude from integrity recomputation
        if (r.rule_name.compare(0, 6, "pass2.") == 0) continue;
        int weight = rules_.scoring.default_weight;
        auto wit = rules_.scoring.rule_weights.find(r.rule_name);
        if (wit != rules_.scoring.rule_weights.end()) {
            weight = wit->second;
        } else if (r.rule_name == "code_quality.intent_validation.self_declared") {
            weight = 1;  // supporting functions: reduced weight
        }
        weight = std::max(0, weight);
        if (recomputed <= SCORE_SATURATION_LIMIT - weight) {
            recomputed += weight;
        } else {
            recomputed = SCORE_SATURATION_LIMIT;
        }
    }
    return recomputed == cumulative_score_;
}

// ============================================================================
// Feature 4: Governance Baseline
// ============================================================================

void GovernanceEngine::saveGovernanceBaseline() const {
    if (rules_.governance_baseline.path.empty()) return;

    int hard = 0, soft = 0, advisory = 0, security = 0;
    for (const auto& r : check_results_) {
        if (r.passed) continue;
        if (r.level == EnforcementLevel::HARD) hard++;
        else if (r.level == EnforcementLevel::SOFT) soft++;
        else advisory++;
        if (r.severity == "critical" || r.severity == "high") security++;
    }

    std::filesystem::path p(rules_.governance_baseline.path);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path());

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    nlohmann::json baseline;
    baseline["version"] = 1;
    baseline["timestamp"] = std::string(ts);
    baseline["hard_violations"] = hard;
    baseline["soft_violations"] = soft;
    baseline["advisory_violations"] = advisory;
    baseline["security_findings"] = security;
    baseline["total_checks"] = static_cast<int>(check_results_.size());

    std::ofstream ofs(rules_.governance_baseline.path);
    if (ofs.is_open()) {
        ofs << baseline.dump(2) << "\n";
        fprintf(stderr, "[governance] Baseline saved to %s\n",
                rules_.governance_baseline.path.c_str());
    }
}

std::string GovernanceEngine::checkGovernanceBaseline() const {
    if (!rules_.governance_baseline.enabled) return "";

    std::ifstream ifs(rules_.governance_baseline.path);
    if (!ifs.is_open()) return "";  // No baseline yet — skip

    nlohmann::json prev;
    try { prev = nlohmann::json::parse(ifs); }
    catch (...) { return ""; }

    int hard = 0, soft = 0, advisory = 0, security = 0;
    for (const auto& r : check_results_) {
        if (r.passed) continue;
        if (r.level == EnforcementLevel::HARD) hard++;
        else if (r.level == EnforcementLevel::SOFT) soft++;
        else advisory++;
        if (r.severity == "critical" || r.severity == "high") security++;
    }

    std::vector<std::string> regressions;
    auto check = [&](const char* name, int current, const char* key) {
        if (prev.contains(key) && current > prev[key].get<int>()) {
            regressions.push_back(fmt::format(
                "  {} increased: {} -> {} (+{})",
                name, prev[key].get<int>(), current, current - prev[key].get<int>()));
        }
    };

    check("hard_violations", hard, "hard_violations");
    check("soft_violations", soft, "soft_violations");
    check("advisory_violations", advisory, "advisory_violations");
    check("security_findings", security, "security_findings");

    if (regressions.empty()) return "";

    std::string msg = "[governance] Baseline REGRESSION detected:\n";
    for (const auto& r : regressions) msg += r + "\n";
    msg += fmt::format("  Baseline from: {}\n",
        prev.contains("timestamp") ? prev["timestamp"].get<std::string>() : "unknown");
    return msg;
}

// ============================================================================
// Drift Detection: Structural Regression Gate
// ============================================================================

// Helper: extract brace-matched body starting from a position in source
static std::string extractBraceBody(const std::string& source, size_t brace_start) {
    if (brace_start >= source.size() || source[brace_start] != '{') return "";
    int depth = 1;
    size_t i = brace_start + 1;
    bool in_string = false;
    char string_char = 0;
    while (i < source.size() && depth > 0) {
        char c = source[i];
        if (in_string) {
            if (c == '\\') { i += 2; continue; } // skip escaped char
            if (c == string_char) { in_string = false; }
            i++;
        } else {
            // Skip // line comments — braces inside must not affect depth
            if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
                while (i < source.size() && source[i] != '\n') i++;
                continue;
            }
            // Skip /* block comments */
            if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
                i += 2;
                while (i + 1 < source.size() && !(source[i] == '*' && source[i + 1] == '/')) i++;
                if (i + 1 < source.size()) i += 2; // skip */
                continue;
            }
            if (c == '"' || c == '\'') { in_string = true; string_char = c; }
            else if (c == '{') depth++;
            else if (c == '}') depth--;
            if (depth > 0) i++;
        }
    }
    if (depth != 0) return "";
    return source.substr(brace_start + 1, i - brace_start - 1);
}

// Helper: extract function body source from full source using brace matching
static std::string extractFunctionBody(const std::string& source, int start_line) {
    // Find the start_line in source
    int current_line = 1;
    size_t pos = 0;
    while (current_line < start_line && pos < source.size()) {
        if (source[pos] == '\n') current_line++;
        pos++;
    }
    // Find the opening { of the function body
    size_t brace_start = source.find('{', pos);
    if (brace_start == std::string::npos) return "";
    return extractBraceBody(source, brace_start);
}

// Like extractFunctionBody but includes the declaration line (fn name(...) { ... })
// Used by preflight intent check — checkIntentValidation expects the declaration line
// so it can strip it (avoiding parameter name false keyword matches).
static std::string extractFunctionWithDecl(const std::string& source, int start_line) {
    int current_line = 1;
    size_t pos = 0;
    while (current_line < start_line && pos < source.size()) {
        if (source[pos] == '\n') current_line++;
        pos++;
    }
    size_t brace_start = source.find('{', pos);
    if (brace_start == std::string::npos) return "";
    std::string inner = extractBraceBody(source, brace_start);
    return source.substr(pos, brace_start - pos + 1) + inner + "}";
}

// Helper: extract main{} block body by searching for top-level "main" keyword
static std::string extractMainBody(const std::string& source) {
    // Search for "main" at the start of a line (after optional whitespace),
    // followed by optional whitespace and "{"
    size_t pos = 0;
    while (pos < source.size()) {
        // Find next "main" occurrence
        size_t found = source.find("main", pos);
        if (found == std::string::npos) return "";

        // Check it's at line start (or start of file)
        bool at_line_start = (found == 0);
        if (!at_line_start && found > 0) {
            // Everything before "main" on this line must be whitespace
            size_t line_start = source.rfind('\n', found - 1);
            line_start = (line_start == std::string::npos) ? 0 : line_start + 1;
            bool all_ws = true;
            for (size_t j = line_start; j < found; j++) {
                if (source[j] != ' ' && source[j] != '\t') { all_ws = false; break; }
            }
            at_line_start = all_ws;
        }

        if (!at_line_start) { pos = found + 4; continue; }

        // Check char before "main" is not alphanumeric (not part of another word)
        if (found > 0) {
            char before = source[found - 1];
            if (std::isalnum(static_cast<unsigned char>(before)) || before == '_') {
                pos = found + 4; continue;
            }
        }

        // Check char after "main" is not alphanumeric
        size_t after = found + 4;
        if (after < source.size()) {
            char c = source[after];
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                pos = found + 4; continue;
            }
        }

        // Skip whitespace after "main" to find "{"
        size_t brace = after;
        while (brace < source.size() && (source[brace] == ' ' || source[brace] == '\t' ||
               source[brace] == '\n' || source[brace] == '\r')) {
            brace++;
        }
        if (brace < source.size() && source[brace] == '{') {
            return extractBraceBody(source, brace);
        }

        pos = found + 4;
    }
    return "";
}

// ============================================================================
// Preflight intent gate — check all functions before execution starts
// ============================================================================

std::string GovernanceEngine::preflightIntentCheck(
    const ast::Program& program, const std::string& source) {

    auto& cfg = rules_.code_quality.intent_validation;
    if (!cfg.enabled) return "";
    if (cfg.mode == "agent") return "";  // agent-only defers to LLM review

    // F8: Mark results as preflight so they survive FIFO eviction
    preflight_mode_ = true;
    struct PreflightGuard {
        bool& flag;
        ~PreflightGuard() { flag = false; }
    } pf_guard{preflight_mode_};

    // Check all top-level functions
    for (const auto& fn : program.getFunctions()) {
        if (!fn) continue;
        int line = fn->getLocation().line;
        std::string body = extractFunctionWithDecl(source, line);
        std::string err = checkIntentValidation(
            fn->getName(), fn->getIntent(), body, line);
        if (!err.empty()) return err;
    }

    // Check exported functions
    for (const auto& ex : program.getExports()) {
        if (!ex || !ex->getFunctionDecl()) continue;
        auto* fd = ex->getFunctionDecl();
        int line = fd->getLocation().line;
        std::string body = extractFunctionWithDecl(source, line);
        std::string err = checkIntentValidation(
            fd->getName(), fd->getIntent(), body, line);
        if (!err.empty()) return err;
    }

    // Check main block — main{} body doesn't have a declaration to strip
    if (program.getMainBlock()) {
        auto* mb = program.getMainBlock();
        std::string body = "main {" + extractMainBody(source) + "}";
        std::string err = checkIntentValidation(
            "main", mb->getIntent(), body, mb->getLocation().line);
        if (!err.empty()) return err;
    }

    // F14: Detect orphaned function_intents keys — govern.json references
    // functions that don't exist in this source file
    if (!cfg.function_intents.empty()) {
        std::unordered_set<std::string> checked;
        for (const auto& fn : program.getFunctions())
            if (fn) checked.insert(fn->getName());
        for (const auto& ex : program.getExports())
            if (ex && ex->getFunctionDecl()) checked.insert(ex->getFunctionDecl()->getName());
        if (program.getMainBlock()) checked.insert("main");

        for (const auto& [name, intent] : cfg.function_intents) {
            if (checked.find(name) == checked.end()) {
                // Orphaned keys are common in multi-file projects where
                // contracted functions live in imported modules. Use reduced
                // weight to avoid blocking the entry-point file.
                enforce("code_quality.intent_validation.self_declared", EnforcementLevel::ADVISORY,
                    fmt::format("Orphaned function_intents key '{}' — no matching "
                                 "function in this file (may exist in imported module).", name));
            }
        }
    }

    // F15: NOTE — getFunctions() returns top-level functions only. Lambdas are
    // not independently checked. Lambda bodies ARE included in the enclosing
    // function's extracted body text, so side-effect detection still catches them.

    return "";
}

// Helper: count comment and code lines
static void countCommentCodeLines(const std::string& source, int& comment_lines, int& code_lines) {
    comment_lines = 0;
    code_lines = 0;
    bool in_block_comment = false;
    std::istringstream stream(source);
    std::string line;
    while (std::getline(stream, line)) {
        // Trim leading whitespace
        size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) continue; // blank line
        std::string trimmed = line.substr(first);

        if (in_block_comment) {
            comment_lines++;
            if (trimmed.find("*/") != std::string::npos) in_block_comment = false;
            continue;
        }
        if (trimmed.rfind("//", 0) == 0 || trimmed.rfind("#", 0) == 0) {
            comment_lines++;
        } else if (trimmed.rfind("/*", 0) == 0) {
            comment_lines++;
            if (trimmed.find("*/") == std::string::npos) in_block_comment = true;
        } else {
            code_lines++;
        }
    }
}

// Helper: count polyglot blocks and languages
static void countPolyglotBlocks(const std::string& source, int& block_count,
                                 std::vector<std::string>& languages) {
    block_count = 0;
    languages.clear();
    static const std::vector<std::string> known_langs = {
        "python", "javascript", "js", "go", "nim", "rust", "cpp", "csharp",
        "ruby", "php", "shell", "bash", "zig", "julia"
    };
    std::set<std::string> seen_langs;
    size_t pos = 0;
    while (pos < source.size()) {
        size_t found = source.find("<<", pos);
        if (found == std::string::npos) break;
        // Skip <<= (compound assignment)
        if (found + 2 < source.size() && source[found + 2] == '=') {
            pos = found + 3;
            continue;
        }
        // Extract the word after <<
        size_t lang_start = found + 2;
        size_t lang_end = lang_start;
        while (lang_end < source.size() && (std::isalpha(source[lang_end]) || source[lang_end] == '_')) {
            lang_end++;
        }
        std::string lang = source.substr(lang_start, lang_end - lang_start);
        // Convert to lowercase
        std::transform(lang.begin(), lang.end(), lang.begin(), ::tolower);
        for (const auto& kl : known_langs) {
            if (lang == kl) {
                block_count++;
                if (seen_langs.insert(lang).second) {
                    languages.push_back(lang);
                }
                break;
            }
        }
        pos = lang_end;
    }
}

// Word-boundary aware search for Gate 12 param utilization.
// Prevents param "a" from matching inside "data", "array", etc.
static bool containsWord(const std::string& text, const std::string& word) {
    size_t pos = 0;
    while ((pos = text.find(word, pos)) != std::string::npos) {
        bool left_ok = (pos == 0 || (!std::isalnum(static_cast<unsigned char>(text[pos - 1])) && text[pos - 1] != '_'));
        bool right_ok = (pos + word.size() >= text.size() ||
                         (!std::isalnum(static_cast<unsigned char>(text[pos + word.size()])) && text[pos + word.size()] != '_'));
        if (left_ok && right_ok) return true;
        pos += word.size();
    }
    return false;
}

std::string GovernanceEngine::extractMainBodyPublic(const std::string& source) {
    return extractMainBody(source);
}

GovernanceEngine::DriftMetrics GovernanceEngine::collectDriftMetrics(
    const ast::Program& program, const std::string& source,
    const std::string& script_path)
{
    DriftMetrics m;
    m.loc = static_cast<int>(std::count(source.begin(), source.end(), '\n')) + 1;

    // Gate 14: Record script's canonical directory
    try {
        m.script_dir = std::filesystem::canonical(
            std::filesystem::path(script_path).parent_path()
        ).string();
    } catch (...) {
        // If canonical fails (e.g., path doesn't exist), use parent_path as-is
        m.script_dir = std::filesystem::path(script_path).parent_path().string();
    }

    // Gate 13 + 16: Check for govern.json and .sig relative to script directory
    {
        namespace fs = std::filesystem;
        fs::path dir(m.script_dir);
        while (true) {
            fs::path candidate = dir / "govern.json";
            if (fs::exists(candidate)) {
                m.config_present = true;
                try {
                    std::ifstream ifs(candidate.string());
                    std::string content((std::istreambuf_iterator<char>(ifs)),
                                        std::istreambuf_iterator<char>());
                    m.config_hash = security::CryptoUtils::sha256(content);
                } catch (...) {}
                // Gate 16: check for .sig sidecar (govern.json)
                fs::path sig_path = candidate.string() + ".sig";
                m.signature_present = fs::exists(sig_path);
                // Gate 16b: check for drift-baseline.json.sig
                // Check common baseline names in the govern.json directory
                for (const char* bp : {"drift-baseline.json", "baseline.json"}) {
                    fs::path baseline_sig = dir / (std::string(bp) + ".sig");
                    if (fs::exists(baseline_sig)) {
                        m.baseline_signature_present = true;
                        break;
                    }
                }
                break;
            }
            fs::path parent = dir.parent_path();
            if (parent == dir) break;  // reached filesystem root
            dir = parent;
        }
    }
    m.has_main = program.getMainBlock() != nullptr;
    // Gate 11b: Hash main{} body to prevent undetected modifications
    if (m.has_main) {
        std::string main_body = extractMainBody(source);
        if (!main_body.empty()) {
            m.main_body_hash = security::CryptoUtils::sha256(main_body);
        }
    }
    m.functions = static_cast<int>(program.getFunctions().size());
    m.exports = static_cast<int>(program.getExports().size());
    m.structs = static_cast<int>(program.getStructs().size());

    for (const auto& fn : program.getFunctions()) {
        if (!fn) continue;
        m.function_names.push_back(fn->getName());
        // Gate 1: param counts
        m.param_counts[fn->getName()] = static_cast<int>(fn->getParams().size());
        // Gate 8: test functions
        if (fn->getName().rfind("test_", 0) == 0) {
            m.test_functions.push_back(fn->getName());
        }
        // Gate 3: complexity scores + Gate 11: body hash — extract body and analyze
        int start_line = fn->getLocation().line;
        if (start_line > 0) {
            std::string body = extractFunctionBody(source, start_line);
            if (!body.empty()) {
                try {
                    analyzer::SyntacticAnalyzer sa;
                    auto profile = sa.analyze(body);
                    m.complexity_scores[fn->getName()] = profile.complexity_score;
                } catch (...) {}
                // Gate 11: SHA-256 of function body
                m.body_hashes[fn->getName()] = security::CryptoUtils::sha256(body);
                // Gate 12: Parameter utilization — count how many params appear in body
                const auto& params = fn->getParams();
                if (!params.empty()) {
                    int used = 0;
                    for (const auto& p : params) {
                        if (!p.name.empty() && containsWord(body, p.name)) {
                            used++;
                        }
                    }
                    m.param_utilization[fn->getName()] =
                        static_cast<double>(used) / static_cast<double>(params.size());
                }
                // Gate 17: polyglot LOC — count lines inside <<lang ... >> blocks
                {
                    int poly_lines = 0;
                    size_t ppos = 0;
                    while ((ppos = body.find("<<", ppos)) != std::string::npos) {
                        if (ppos + 2 < body.size() && body[ppos + 2] == '=') { ppos += 3; continue; }
                        size_t block_end = body.find("\n>>", ppos);
                        if (block_end != std::string::npos) {
                            for (size_t j = ppos; j < block_end; ++j)
                                if (body[j] == '\n') poly_lines++;
                            ppos = block_end + 3;
                        } else break;
                    }
                    if (poly_lines > 0) m.polyglot_loc[fn->getName()] = poly_lines;
                }
            }
        }
    }
    // Also check exported functions for param counts, test names, complexity
    for (const auto& ex : program.getExports()) {
        if (!ex || !ex->getFunctionDecl()) continue;
        auto* fd = ex->getFunctionDecl();
        m.export_names.push_back(fd->getName());
        if (m.param_counts.find(fd->getName()) == m.param_counts.end()) {
            m.param_counts[fd->getName()] = static_cast<int>(fd->getParams().size());
        }
        if (fd->getName().rfind("test_", 0) == 0) {
            // Avoid duplicates
            if (std::find(m.test_functions.begin(), m.test_functions.end(), fd->getName()) == m.test_functions.end()) {
                m.test_functions.push_back(fd->getName());
            }
        }
        // Complexity + body hash for exported functions
        if (m.complexity_scores.find(fd->getName()) == m.complexity_scores.end()) {
            int start_line = fd->getLocation().line;
            if (start_line > 0) {
                std::string body = extractFunctionBody(source, start_line);
                if (!body.empty()) {
                    try {
                        analyzer::SyntacticAnalyzer sa;
                        auto profile = sa.analyze(body);
                        m.complexity_scores[fd->getName()] = profile.complexity_score;
                    } catch (...) {}
                    if (m.body_hashes.find(fd->getName()) == m.body_hashes.end()) {
                        m.body_hashes[fd->getName()] = security::CryptoUtils::sha256(body);
                    }
                    // Gate 12: param utilization for exports
                    if (m.param_utilization.find(fd->getName()) == m.param_utilization.end()) {
                        const auto& params = fd->getParams();
                        if (!params.empty()) {
                            int used = 0;
                            for (const auto& p : params) {
                                if (!p.name.empty() && containsWord(body, p.name))
                                    used++;
                            }
                            m.param_utilization[fd->getName()] =
                                static_cast<double>(used) / static_cast<double>(params.size());
                        }
                    }
                    // Gate 17: polyglot LOC for exports
                    if (m.polyglot_loc.find(fd->getName()) == m.polyglot_loc.end()) {
                        int poly_lines = 0;
                        size_t ppos = 0;
                        while ((ppos = body.find("<<", ppos)) != std::string::npos) {
                            if (ppos + 2 < body.size() && body[ppos + 2] == '=') { ppos += 3; continue; }
                            size_t block_end = body.find("\n>>", ppos);
                            if (block_end != std::string::npos) {
                                for (size_t j = ppos; j < block_end; ++j)
                                    if (body[j] == '\n') poly_lines++;
                                ppos = block_end + 3;
                            } else break;
                        }
                        if (poly_lines > 0) m.polyglot_loc[fd->getName()] = poly_lines;
                    }
                }
            }
        }
    }

    // Gate 2: imports (both block imports and module uses)
    for (const auto& imp : program.getImports()) {
        if (imp) m.imports.push_back(imp->getBlockId());
    }
    for (const auto& mu : program.getModuleUses()) {
        if (mu) m.imports.push_back(mu->getModulePath());
    }

    // Gate 4: comment/code lines
    countCommentCodeLines(source, m.comment_lines, m.code_lines);

    // Gate 6: polyglot blocks
    countPolyglotBlocks(source, m.polyglot_blocks, m.polyglot_languages);

    // Gate 7: struct fields
    for (const auto& s : program.getStructs()) {
        if (!s) continue;
        std::vector<std::string> fields;
        for (const auto& f : s->getFields()) {
            fields.push_back(f.name);
        }
        m.struct_fields[s->getName()] = fields;
    }

    return m;
}

// Resolve drift baseline path relative to govern.json directory
std::string GovernanceEngine::resolveDriftBaselinePath() const {
    const auto& bp = rules_.code_quality.drift_detection.baseline_path;
    if (bp.empty()) return bp;
    std::filesystem::path p(bp);
    if (p.is_absolute()) return bp;
    if (!loaded_path_.empty()) {
        return (std::filesystem::path(loaded_path_).parent_path() / p).string();
    }
    return bp;
}

// --- Integrity: signature verification (V-SC-009: Ed25519 + legacy HMAC) ---

static const char* GOVERN_KEY_ENV = "NAAB_GOVERN_KEY";
static const char* SIGNING_KEY_ENV = "NAAB_SIGNING_KEY";
static const char* FINGERPRINT_DOMAIN = "NAAB_FINGERPRINT";
static const char* SIG_PREFIX_ED25519 = "ed25519:";
static const char* SIG_PREFIX_HMAC = "hmac:";

// Detect signature type from .sig file content
static std::string detectSignatureType(const std::string& sig) {
    if (sig.rfind(SIG_PREFIX_ED25519, 0) == 0) return "ed25519";
    if (sig.rfind(SIG_PREFIX_HMAC, 0) == 0) return "hmac";
    return "legacy";  // raw hex, assume HMAC (backward compat)
}

// Trust store check. Re-validates each call to detect mid-run deletion.
static bool hasTrustStoreKeys() {
    return security::TrustStore::hasKeys();
}

// Tracks whether the trust store had keys on first check.
// Returns true if keys existed initially but are now gone (tamper signal).
// Thread-safe: agent worker threads may call governance checks concurrently.
static bool trustStoreTampered() {
    static std::atomic<int> first_state{-1};  // -1=unchecked, 0=empty, 1=populated
    bool current = security::TrustStore::hasKeys();
    int expected = -1;
    first_state.compare_exchange_strong(expected, current ? 1 : 0);
    return first_state.load(std::memory_order_acquire) == 1 && !current;
}

// Does the process have any signing capability?
static bool hasSigningCapability() {
    const char* sk = std::getenv(SIGNING_KEY_ENV);
    if (sk && *sk) return true;
    const char* gk = std::getenv(GOVERN_KEY_ENV);
    return gk && *gk;
}

// Read private key PEM from NAAB_SIGNING_KEY path
// Bounded read: Ed25519 PEM keys are <500 bytes; cap at 8KB to reject FIFOs/devices.
static std::string readSigningKey() {
    const char* sk_path = std::getenv(SIGNING_KEY_ENV);
    if (!sk_path || !*sk_path) return "";
    // Reject non-regular files (FIFOs, /dev/zero, etc.)
#ifndef _WIN32
    struct stat st;
    if (lstat(sk_path, &st) != 0 || !S_ISREG(st.st_mode)) return "";
    if (st.st_size > 8192) return "";  // Ed25519 PEM is <500 bytes
#else
    // Windows: use filesystem to check regular file + size
    std::error_code ec;
    auto fstatus = std::filesystem::status(sk_path, ec);
    if (ec || fstatus.type() != std::filesystem::file_type::regular) return "";
    auto fsize = std::filesystem::file_size(sk_path, ec);
    if (ec || fsize > 8192) return "";
#endif
    std::ifstream ifs(sk_path);
    if (!ifs.is_open()) return "";
    std::string pem((std::istreambuf_iterator<char>(ifs)),
                     std::istreambuf_iterator<char>());
    return pem;
}

std::string GovernanceEngine::getKeyFingerprint() {
    // Ed25519 mode: fingerprint from NAAB_SIGNING_KEY
    std::string sk_pem = readSigningKey();
    if (!sk_pem.empty()) {
        return security::CryptoUtils::ed25519Fingerprint(sk_pem);
    }
    // Legacy HMAC mode
    const char* key = std::getenv(GOVERN_KEY_ENV);
    if (!key || !*key) return "";
    std::string hmac = security::CryptoUtils::hmacSha256(FINGERPRINT_DOMAIN, key);
    return hmac.substr(hmac.size() > 8 ? hmac.size() - 8 : 0);
}

bool GovernanceEngine::signFile(const std::string& file_path) {
    // Read file content
    std::ifstream ifs(file_path);
    if (!ifs.is_open()) {
        fprintf(stderr, "[governance] Error: cannot read %s for signing\n", file_path.c_str());
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    ifs.close();

    std::string sig_content;

    // Try Ed25519 first (NAAB_SIGNING_KEY)
    std::string sk_pem = readSigningKey();
    if (!sk_pem.empty()) {
        std::string b64_sig = security::CryptoUtils::ed25519Sign(content, sk_pem);
        if (b64_sig.empty()) {
            fprintf(stderr, "[governance] Error: Ed25519 signing failed for %s\n"
                            "  Check that the signing key is a valid Ed25519 private key PEM.\n",
                    file_path.c_str());
            return false;
        }
        // Authority Decay: append unix timestamp as metadata (not signed content)
        sig_content = std::string(SIG_PREFIX_ED25519) + b64_sig + ":"
                      + std::to_string(static_cast<int64_t>(std::time(nullptr)));
    } else {
        // Legacy HMAC fallback
        const char* key = std::getenv(GOVERN_KEY_ENV);
        if (!key || !*key) {
            fprintf(stderr, "[governance] Error: No signing key configured — cannot sign %s\n"
                            "  A signing key is required. Contact the project owner for key setup.\n",
                    file_path.c_str());
            return false;
        }
        fprintf(stderr, "[governance] WARNING: Using legacy HMAC signing (deprecated).\n"
                        "  Migrate to Ed25519 for stronger integrity guarantees.\n");
        sig_content = std::string(SIG_PREFIX_HMAC) + security::CryptoUtils::hmacSha256(content, key);
    }

    std::string sig_path = file_path + ".sig";
    if (!security::writeFileSecure(sig_path, sig_content)) {
        fprintf(stderr, "[governance] Error: failed to write signature %s\n", sig_path.c_str());
        return false;
    }
    fprintf(stderr, "[governance] Signed: %s (fingerprint: %s)\n",
            file_path.c_str(), getKeyFingerprint().c_str());
    return true;
}

// V-SC-009: Unified signature verification (Ed25519 trust-anchored + HMAC legacy)
bool GovernanceEngine::verifySignatureImpl(
    const std::string& file_path, const std::string& content) const
{
    std::string sig_path = file_path + ".sig";
    bool have_trust_keys = hasTrustStoreKeys();
    const char* hmac_key = std::getenv(GOVERN_KEY_ENV);
    bool have_hmac_key = hmac_key && *hmac_key;

    // Detect mid-process trust store deletion (keys existed at startup, now gone)
    if (trustStoreTampered()) {
        fprintf(stderr,
            "[governance] INTEGRITY BLOCK: trust store keys removed during execution\n");
        return false;
    }

    // Open .sig directly to avoid TOCTOU race between exists() and open()
    std::ifstream sig_ifs(sig_path);
    bool sig_exists = sig_ifs.is_open();

    // --- No .sig file ---
    if (!sig_exists) {
        // Trust store has keys → BLOCK (core V-SC-009 security fix)
        if (have_trust_keys) {
            fprintf(stderr,
                "[governance] INTEGRITY BLOCK: %s.sig missing but trusted Ed25519 keys are installed.\n"
                "  When trusted keys exist in %s,\n"
                "  all governance files must be signed.\n"
                "  The signing key holder must sign this file before execution.\n",
                file_path.c_str(), security::TrustStore::getStorePath().c_str());
            return false;
        }
        // V-SC-008 legacy: HMAC key set → BLOCK
        if (have_hmac_key) {
            fprintf(stderr,
                "[governance] INTEGRITY BLOCK: %s.sig missing but signing key is configured.\n"
                "  When a signing key is present, all governance files must be signed.\n",
                file_path.c_str());
            return false;
        }
        // No keys anywhere → unsigned mode (backward compat)
        // Gap 1: Trust anchor check in checkDriftDetection() provides the real protection.
        // No warning here — it fires during config loading before mode is known,
        // and the word "governance" in stderr triggers false positives in test scripts.
        return true;
    }

    // --- .sig exists: read and classify (capped at 4KB — Ed25519 sigs are ~96 bytes) ---
    std::string stored_sig;
    stored_sig.reserve(256);
    char buf[4096];
    sig_ifs.read(buf, sizeof(buf));
    auto bytes_read = sig_ifs.gcount();
    if (bytes_read <= 0) { sig_ifs.close(); return false; }
    stored_sig.assign(buf, static_cast<size_t>(bytes_read));
    sig_ifs.close();
    while (!stored_sig.empty() && (stored_sig.back() == '\n' || stored_sig.back() == '\r'))
        stored_sig.pop_back();

    std::string sig_type = detectSignatureType(stored_sig);

    if (sig_type == "ed25519") {
        // Parse envelope: "ed25519:<base64-sig>" or "ed25519:<base64-sig>:<unix-timestamp>"
        std::string remainder = stored_sig.substr(strlen(SIG_PREFIX_ED25519));
        std::string b64_sig = remainder;
        int64_t signed_at = 0;

        auto colon_pos = remainder.find(':');
        if (colon_pos != std::string::npos) {
            b64_sig = remainder.substr(0, colon_pos);
            try {
                signed_at = std::stoll(remainder.substr(colon_pos + 1));
            } catch (...) {
                // Malformed timestamp — ignore, treat as no timestamp
            }
        }

        if (have_trust_keys) {
            // Verify against each trusted key
            auto keys = security::TrustStore::loadKeys();
            if (keys.empty()) {
                // Trust store was populated at hasTrustStoreKeys() but loadKeys() returned empty
                fprintf(stderr,
                    "[governance] INTEGRITY BLOCK: trust store directory emptied during verification\n");
                return false;
            }
            bool verified = false;
            for (const auto& [fingerprint, pem] : keys) {
                if (security::CryptoUtils::ed25519Verify(content, b64_sig, pem)) {
                    verified = true;
                    break;
                }
            }
            if (!verified) {
                fprintf(stderr,
                    "[governance] INTEGRITY BLOCK: %s signature does not match any trusted key.\n"
                    "  The Ed25519 signature was checked against %zu trusted key(s) — none matched.\n"
                    "  This file may have been signed with an untrusted key or tampered with.\n",
                    file_path.c_str(), keys.size());
                return false;
            }

            // Authority Decay: check signature staleness
            if (signed_at > 0 && rules_.trust_policy.max_signature_age_days > 0) {
                int64_t now = static_cast<int64_t>(std::time(nullptr));
                int64_t age_days = (now - signed_at) / 86400;
                if (age_days > rules_.trust_policy.max_signature_age_days) {
                    if (rules_.trust_policy.stale_signature_level == governance::EnforcementLevel::HARD) {
                        fprintf(stderr,
                            "[governance] STALE SIGNATURE BLOCK: %s is %lld days old (max: %d).\n"
                            "  The signing key holder must re-sign this file.\n",
                            file_path.c_str(), static_cast<long long>(age_days),
                            rules_.trust_policy.max_signature_age_days);
                        return false;
                    } else if (rules_.trust_policy.stale_signature_level == governance::EnforcementLevel::SOFT) {
                        fprintf(stderr,
                            "[governance] STALE SIGNATURE: %s is %lld days old (max: %d).\n"
                            "  The signing key holder must re-sign this file.\n",
                            file_path.c_str(), static_cast<long long>(age_days),
                            rules_.trust_policy.max_signature_age_days);
                        // SOFT: block unless override enabled
                        if (!override_enabled_) return false;
                    } else {
                        fprintf(stderr,
                            "[governance] WARNING: Signature on %s is %lld days old (max: %d).\n"
                            "  Consider having the signing key holder re-sign this file.\n",
                            file_path.c_str(), static_cast<long long>(age_days),
                            rules_.trust_policy.max_signature_age_days);
                    }
                }
            }

            return true;  // Verified against a trusted key
        }

        // Ed25519 sig but no trust store keys.
        // If keys were present at startup but disappeared → tamper (caught above by trustStoreTampered()).
        // If keys were never present → unconfigured environment, warn and proceed.
        if (signature_warned_files_.find(file_path) == signature_warned_files_.end()) {
            signature_warned_files_.insert(file_path);
            fprintf(stderr,
                "[governance] WARNING: %s is Ed25519-signed but no trusted keys are installed.\n"
                "  Signature cannot be verified.\n",
                file_path.c_str());
        }
        return true;
    }

    // HMAC signature (tagged or legacy)
    if (have_trust_keys) {
        // Trust store has Ed25519 keys but sig is HMAC → must re-sign
        fprintf(stderr,
            "[governance] INTEGRITY BLOCK: %s has legacy HMAC signature but Ed25519 trusted keys are installed.\n"
            "  Re-sign with Ed25519 to match the trust store.\n",
            file_path.c_str());
        return false;
    }

    // Legacy HMAC verification (no trust store)
    if (!have_hmac_key) {
        // V-SC-007: .sig exists but no HMAC key → fail closed
        fprintf(stderr,
            "[governance] INTEGRITY BLOCK: %s.sig exists but the signing key is not available.\n"
            "  This file is HMAC-signed. Without the key, the signature cannot be verified.\n",
            file_path.c_str());
        return false;
    }

    std::string raw_sig = (sig_type == "hmac")
        ? stored_sig.substr(strlen(SIG_PREFIX_HMAC))
        : stored_sig;  // legacy: raw hex

    std::string expected = security::CryptoUtils::hmacSha256(content, hmac_key);
    if (!security::CryptoUtils::constantTimeCompare(expected, raw_sig)) {
        fprintf(stderr,
            "[governance] INTEGRITY BLOCK: %s has been modified since it was signed.\n"
            "  The HMAC signature does not match the file contents. This file is protected —\n"
            "  any modification without the signing key is detected and blocked. Do not attempt\n"
            "  to edit, recreate, or work around this file. Ask the project owner to re-sign\n"
            "  after making authorized changes.\n",
            file_path.c_str());
        return false;
    }

    return true;
}

bool GovernanceEngine::verifyFileSignature(const std::string& file_path) const {
    std::ifstream ifs(file_path);
    if (!ifs.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    ifs.close();
    return verifySignatureImpl(file_path, content);
}

bool GovernanceEngine::verifyContentSignature(
    const std::string& file_path, const std::string& content) const
{
    return verifySignatureImpl(file_path, content);
}

bool GovernanceEngine::isBlockedFlag(const std::string& flag) const {
    for (const auto& blocked : rules_.integrity.blocked_flags) {
        if (flag == blocked) return true;
    }
    return false;
}

std::string GovernanceEngine::checkDriftDetection(
    const std::string& filename, const DriftMetrics& current)
{
    const auto& cfg = rules_.code_quality.drift_detection;
    if (!cfg.enabled) return "";

    // Load baseline
    std::string resolved = resolveDriftBaselinePath();
    std::ifstream ifs(resolved);
    if (!ifs.is_open()) {
        // Gate 10: fail-closed when baseline is required but missing (tamper protection)
        if (cfg.require_baseline) {
            std::string msg = "Drift baseline missing or deleted: " + resolved +
                              ". The project owner must initialize the baseline before execution.";
            fprintf(stderr, "[governance] %s\n", msg.c_str());
            enforce("drift_detection.require_baseline", cfg.level, msg);
            return "[governance] Drift detection FAILED:\n  " + msg + "\n";
        }
        return "";  // No baseline yet — skip
    }

    // Read baseline into memory ONCE (TOCTOU fix: no re-read after verify)
    std::string baseline_content((std::istreambuf_iterator<char>(ifs)),
                                  std::istreambuf_iterator<char>());
    ifs.close();

    // Verify baseline signature on the in-memory content
    if (!verifyContentSignature(resolved, baseline_content)) {
        std::string msg = "Drift baseline signature verification failed: " + resolved;
        enforce("drift_detection.integrity", cfg.level, msg);
        return "[governance] Drift detection FAILED:\n  " + msg + "\n";
    }

    // Parse the SAME content (no re-read — eliminates TOCTOU window)
    nlohmann::json baseline;
    try { baseline = nlohmann::json::parse(baseline_content); }
    catch (...) { return ""; }

    // Gap 10: Self-referential trust fix — baseline is the trust anchor, not govern.json
    // If baseline records that signing was configured or signatures were present,
    // then signing capability MUST exist now. This prevents the attack where an adversary
    // removes .sig files + keys + edits govern.json require_signature=false.
    {
        bool baseline_had_signing = false;
        // Check root-level signing_configured flag
        if (baseline.contains("signing_configured") && baseline["signing_configured"].get<bool>()) {
            baseline_had_signing = true;
        }
        // Check per-file signature_present flags
        if (!baseline_had_signing && baseline.contains("files")) {
            for (auto& [fname, entry] : baseline["files"].items()) {
                if (entry.contains("signature_present") && entry["signature_present"].get<bool>()) {
                    baseline_had_signing = true;
                    break;
                }
            }
        }
        if (baseline_had_signing && !hasTrustStoreKeys()) {
            const char* hmac_key = std::getenv(GOVERN_KEY_ENV);
            bool have_hmac = hmac_key && *hmac_key;
            if (!have_hmac) {
                std::string msg =
                    "Drift baseline records that governance signing was previously configured, "
                    "but no signing keys are currently available. "
                    "Signing keys should not disappear.";
                fprintf(stderr, "[governance] INTEGRITY BLOCK: %s\n", msg.c_str());
                enforce("drift_detection.trust_anchor", EnforcementLevel::HARD, msg);
                return "[governance] Drift detection FAILED:\n  " + msg + "\n";
            }
        }
    }

    // Gap 14: Validate project_root to prevent baseline substitution between projects
    if (baseline.contains("project_root") && baseline["project_root"].is_string()) {
        std::string baseline_root = baseline["project_root"].get<std::string>();
        // Resolve current project root (directory containing govern.json)
        std::string current_root;
        {
            namespace fs = std::filesystem;
            fs::path dir(fs::path(filename).parent_path());
            try { dir = fs::canonical(dir); } catch (...) {}
            while (true) {
                if (fs::exists(dir / "govern.json")) {
                    current_root = dir.string();
                    break;
                }
                fs::path parent = dir.parent_path();
                if (parent == dir) break;
                dir = parent;
            }
        }
        if (!current_root.empty() && !baseline_root.empty() && current_root != baseline_root) {
            std::string msg = fmt::format(
                "Drift baseline was created for project '{}' but is being used in '{}'. "
                "Baselines are project-bound and cannot be copied between projects.",
                baseline_root, current_root);
            fprintf(stderr, "[governance] INTEGRITY BLOCK: %s\n", msg.c_str());
            enforce("drift_detection.project_binding", EnforcementLevel::HARD, msg);
            return "[governance] Drift detection FAILED:\n  " + msg + "\n";
        }
    }

    // Find this file in the baseline
    std::string key = std::filesystem::path(filename).filename().string();
    if (!baseline.contains("files") || !baseline["files"].contains(key)) {
        // Gate 10 extension: if require_baseline is true, block unbaselined files
        // This prevents creating parallel ungoverned copies (e.g., script_v14.naab)
        if (cfg.require_baseline) {
            bool baseline_save_blocked = false;
            for (const auto& bf : rules_.integrity.blocked_flags) {
                if (bf == "--drift-baseline-save") { baseline_save_blocked = true; break; }
            }
            std::string advice;
            if (hasSigningCapability()) {
                advice = "The signing key holder must baseline this file before it can run.";
            } else if (baseline_save_blocked) {
                advice = "Ask the project owner to baseline this file.";
            } else {
                advice = "This file must be baselined before execution, or use an already-baselined script.";
            }
            std::string msg = fmt::format(
                "Drift: '{}' has no baseline entry. When require_baseline is enabled, "
                "all scripts must be baselined before execution. {}",
                key, advice);
            fprintf(stderr, "[governance] %s\n", msg.c_str());
            enforce("drift_detection.require_baseline", cfg.level, msg);
            return "[governance] Drift detection FAILED:\n  " + msg + "\n";
        }
        return "";  // New file, no baseline entry
    }

    auto& prev = baseline["files"][key];
    std::vector<std::string> violations;

    // Helper: check metric loss
    auto checkLoss = [&](const char* name, int current_val, const char* json_key,
                         double max_loss) {
        if (!prev.contains(json_key)) return;
        int baseline_val = prev[json_key].get<int>();
        if (baseline_val == 0) return;  // Can't lose what you didn't have
        double loss = 1.0 - (static_cast<double>(current_val) / baseline_val);
        if (loss > max_loss) {
            std::string msg = fmt::format(
                "Drift: {} dropped {:.0f}% ({} -> {}). Max allowed: {:.0f}%\n"
                "  Help: Restore the removed {} to match the baseline. Do NOT attempt to\n"
                "  edit drift-baseline.json manually — it is signed and tamper-detected.\n"
                "  Only the project owner (with signing key) can re-baseline after authorized changes.",
                name, loss * 100.0, baseline_val, current_val, max_loss * 100.0, name);
            violations.push_back(msg);
            enforce(std::string("drift_detection.") + name, cfg.level, msg);
        } else {
            recordPass(std::string("drift_detection.") + name, cfg.level);
        }
    };

    checkLoss("functions", current.functions, "functions", cfg.max_function_loss);
    checkLoss("loc", current.loc, "loc", cfg.max_loc_loss);
    checkLoss("exports", current.exports, "exports", cfg.max_export_loss);
    checkLoss("structs", current.structs, "structs", cfg.max_struct_loss);

    // Gate 0 extension: function gain detection — blocks function duplication/injection
    auto checkGain = [&](const char* name, int current_val, const char* json_key,
                         double max_gain) {
        if (!prev.contains(json_key)) return;
        int baseline_val = prev[json_key].get<int>();
        if (baseline_val == 0) return;
        double gain = (static_cast<double>(current_val) / baseline_val) - 1.0;
        if (gain > max_gain) {
            std::string msg = fmt::format(
                "Drift: {} count grew {:.0f}% ({} -> {}). Max allowed gain: {:.0f}%\n"
                "  Help: Remove the extra functions you added. The baseline expects {} {}.\n"
                "  Adding new functions to governed code requires re-baselining by the project owner.\n"
                "  Do NOT edit drift-baseline.json — it is signed and tamper-detected.",
                name, gain * 100.0, baseline_val, current_val, max_gain * 100.0,
                baseline_val, name);
            violations.push_back(msg);
            enforce("drift_detection.function_gain", cfg.level, msg);
        }
    };
    checkGain("functions", current.functions, "functions", cfg.max_function_gain);

    // Report specific deleted functions/exports
    if (prev.contains("function_names") && prev["function_names"].is_array()) {
        std::unordered_set<std::string> current_set(
            current.function_names.begin(), current.function_names.end());
        for (const auto& name : prev["function_names"]) {
            std::string fn = name.get<std::string>();
            if (current_set.find(fn) == current_set.end()) {
                std::string msg = fmt::format("Drift: function '{}' was deleted", fn);
                // Info-level — the metric check above handles enforcement
                fprintf(stderr, "[governance] %s\n", msg.c_str());
            }
        }
    }

    if (prev.contains("export_names") && prev["export_names"].is_array()) {
        std::unordered_set<std::string> current_set(
            current.export_names.begin(), current.export_names.end());
        for (const auto& name : prev["export_names"]) {
            std::string ex = name.get<std::string>();
            if (current_set.find(ex) == current_set.end()) {
                std::string msg = fmt::format("Drift: exported function '{}' was deleted (API regression)", ex);
                fprintf(stderr, "[governance] %s\n", msg.c_str());
            }
        }
    }

    // Gate 1: Signature stability — param count per function
    if (cfg.check_signatures && prev.contains("param_counts") && prev["param_counts"].is_object()) {
        for (auto& [fn_name, baseline_count] : prev["param_counts"].items()) {
            int bcount = baseline_count.get<int>();
            if (bcount == 0) continue;
            auto it = current.param_counts.find(fn_name);
            if (it == current.param_counts.end()) continue; // function deleted — handled by Gate 0
            int ccount = it->second;
            double loss = 1.0 - (static_cast<double>(ccount) / bcount);
            if (loss > cfg.max_param_loss) {
                std::string msg = fmt::format(
                    "Drift: function '{}' params dropped {:.0f}% ({} -> {}). Max allowed: {:.0f}%\n"
                    "  Help: Restore the removed parameters to function '{}'. The baseline recorded\n"
                    "  {} params but only {} remain. Do NOT remove function parameters from governed\n"
                    "  code without re-baselining. Do NOT edit drift-baseline.json — it is signed and tamper-detected.",
                    fn_name, loss * 100.0, bcount, ccount, cfg.max_param_loss * 100.0,
                    fn_name, bcount, ccount);
                violations.push_back(msg);
                enforce("drift_detection.signatures", cfg.level, msg);
            }
        }
        if (violations.empty() || violations.back().find("params") == std::string::npos) {
            recordPass("drift_detection.signatures", cfg.level);
        }
    }

    // Gate 2: Import regression
    if (cfg.check_imports && prev.contains("imports") && prev["imports"].is_array()) {
        auto& prev_imports = prev["imports"];
        int baseline_count = static_cast<int>(prev_imports.size());
        if (baseline_count > 0) {
            std::unordered_set<std::string> current_set(current.imports.begin(), current.imports.end());
            std::vector<std::string> deleted;
            for (const auto& imp : prev_imports) {
                std::string name = imp.get<std::string>();
                if (current_set.find(name) == current_set.end()) {
                    deleted.push_back(name);
                    fprintf(stderr, "[governance] Drift: import '%s' was removed\n", name.c_str());
                }
            }
            double loss = static_cast<double>(deleted.size()) / baseline_count;
            if (loss > cfg.max_import_loss) {
                std::string del_list;
                for (size_t i = 0; i < deleted.size(); i++) {
                    if (i > 0) del_list += ", ";
                    del_list += "'" + deleted[i] + "'";
                }
                std::string msg = fmt::format(
                    "Drift: imports dropped {:.0f}% ({} -> {}). Max allowed: {:.0f}%\n"
                    "  Removed imports: {}\n"
                    "  Help: Re-add the removed imports. The baseline requires these imports to be\n"
                    "  present. Do NOT remove imports from governed code.\n"
                    "  Do NOT edit drift-baseline.json — it is signed and tamper-detected.",
                    loss * 100.0, baseline_count, static_cast<int>(current.imports.size()),
                    cfg.max_import_loss * 100.0, del_list);
                violations.push_back(msg);
                enforce("drift_detection.imports", cfg.level, msg);
            } else {
                recordPass("drift_detection.imports", cfg.level);
            }
        }
    }

    // Gate 3: Complexity regression (per-function)
    if (cfg.check_complexity && prev.contains("complexity_scores") && prev["complexity_scores"].is_object()) {
        for (auto& [fn_name, baseline_score] : prev["complexity_scores"].items()) {
            int bscore = baseline_score.get<int>();
            if (bscore < cfg.min_complexity_baseline) continue; // skip trivial functions
            auto it = current.complexity_scores.find(fn_name);
            if (it == current.complexity_scores.end()) continue; // function deleted — handled elsewhere
            int cscore = it->second;
            double loss = 1.0 - (static_cast<double>(cscore) / bscore);
            if (loss > cfg.max_complexity_loss) {
                std::string msg = fmt::format(
                    "Drift: function '{}' complexity dropped {:.0f}% ({} -> {}). Max allowed: {:.0f}%\n"
                    "  Help: Restore the original logic in '{}'. The function body has been simplified\n"
                    "  beyond what governance allows. Re-add the conditionals, loops, or branching\n"
                    "  that were removed. Do NOT stub functions or replace logic with pass-through code.",
                    fn_name, loss * 100.0, bscore, cscore, cfg.max_complexity_loss * 100.0, fn_name);
                violations.push_back(msg);
                enforce("drift_detection.complexity", cfg.level, msg);
            }
        }
    }

    // Gate 4: Comment inflation
    if (cfg.check_comment_ratio && prev.contains("code_lines")) {
        int baseline_code = prev["code_lines"].get<int>();
        if (baseline_code > 0 && current.code_lines + current.comment_lines > 0) {
            double current_ratio = static_cast<double>(current.comment_lines) /
                                   (current.code_lines + current.comment_lines);
            double code_loss = 1.0 - (static_cast<double>(current.code_lines) / baseline_code);
            // Independent check: comment ratio alone exceeds max_comment_only_ratio
            if (current_ratio > cfg.max_comment_only_ratio) {
                std::string msg = fmt::format(
                    "Drift: comment ratio {:.0f}% exceeds {:.0f}% — file is predominantly comments.\n"
                    "  Help: This file is {:.0f}% comments (max {:.0f}%). Replace comments with actual\n"
                    "  code logic. Comments alone cannot substitute for implementation.",
                    current_ratio * 100.0, cfg.max_comment_only_ratio * 100.0,
                    current_ratio * 100.0, cfg.max_comment_only_ratio * 100.0);
                violations.push_back(msg);
                enforce("drift_detection.comment_ratio", EnforcementLevel::ADVISORY, msg);
            }
            // Combined check: high comment ratio AND code loss >30%
            else if (current_ratio > cfg.max_comment_ratio && code_loss > 0.3) {
                std::string msg = fmt::format(
                    "Drift: comment ratio {:.0f}% (max {:.0f}%) with {:.0f}% code loss.\n"
                    "  Help: Code has been replaced with comments. The baseline had {} code lines,\n"
                    "  now only {}. Restore the original code — do NOT replace logic with comments.\n"
                    "  Do NOT edit drift-baseline.json — it is signed and tamper-detected.",
                    current_ratio * 100.0, cfg.max_comment_ratio * 100.0, code_loss * 100.0,
                    baseline_code, current.code_lines);
                violations.push_back(msg);
                enforce("drift_detection.comment_ratio", cfg.level, msg);
            } else {
                recordPass("drift_detection.comment_ratio", cfg.level);
            }
        }
    }

    // Gate 5: Dead export gate (no baseline needed — static check on current file)
    if (cfg.check_hollow_exports) {
        for (const auto& exp_name : current.export_names) {
            auto it = current.complexity_scores.find(exp_name);
            auto pit = current.param_counts.find(exp_name);
            if (it != current.complexity_scores.end() && pit != current.param_counts.end()) {
                int score = it->second;
                int params = pit->second;
                if (score < cfg.min_hollow_export_complexity && params > 0) {
                    std::string msg = fmt::format(
                        "Drift: exported function '{}' has {} params but complexity score {} (min required: {}) — hollow export.\n"
                        "  Help: Add real logic to '{}' — conditionals, loops, or branching. Empty wrapper\n"
                        "  functions that accept parameters but do nothing are not permitted in governed code.",
                        exp_name, params, score, cfg.min_hollow_export_complexity,
                        exp_name);
                    violations.push_back(msg);
                    enforce("drift_detection.hollow_exports", cfg.level, msg);
                }
            }
        }
    }

    // Gate 6: Polyglot regression
    if (cfg.check_polyglot && prev.contains("polyglot_blocks")) {
        int baseline_blocks = prev["polyglot_blocks"].get<int>();
        if (baseline_blocks > 0) {
            // Collect removed languages first (used in both stderr and violation message)
            std::vector<std::string> removed_langs;
            if (prev.contains("polyglot_languages") && prev["polyglot_languages"].is_array()) {
                std::unordered_set<std::string> current_langs(
                    current.polyglot_languages.begin(), current.polyglot_languages.end());
                for (const auto& lang : prev["polyglot_languages"]) {
                    std::string l = lang.get<std::string>();
                    if (current_langs.find(l) == current_langs.end()) {
                        removed_langs.push_back(l);
                        fprintf(stderr, "[governance] Drift: polyglot language '%s' was removed\n", l.c_str());
                    }
                }
            }
            double loss = 1.0 - (static_cast<double>(current.polyglot_blocks) / baseline_blocks);
            if (loss > cfg.max_polyglot_loss) {
                std::string lang_list;
                for (size_t i = 0; i < removed_langs.size(); i++) {
                    if (i > 0) lang_list += ", ";
                    lang_list += "'" + removed_langs[i] + "'";
                }
                std::string msg = fmt::format(
                    "Drift: polyglot blocks dropped {:.0f}% ({} -> {}). Max allowed: {:.0f}%\n"
                    "  Removed languages: {}\n"
                    "  Help: Re-add the removed polyglot blocks. Do NOT strip polyglot analysis\n"
                    "  from governed code. Do NOT edit drift-baseline.json — it is signed and tamper-detected.",
                    loss * 100.0, baseline_blocks, current.polyglot_blocks,
                    cfg.max_polyglot_loss * 100.0,
                    lang_list.empty() ? "(unknown)" : lang_list);
                violations.push_back(msg);
                enforce("drift_detection.polyglot", cfg.level, msg);
            } else {
                recordPass("drift_detection.polyglot", cfg.level);
            }
        }
    }

    // Gate 7: Struct field stability
    if (cfg.check_struct_fields && prev.contains("struct_fields") && prev["struct_fields"].is_object()) {
        for (auto& [struct_name, baseline_fields] : prev["struct_fields"].items()) {
            if (!baseline_fields.is_array()) continue;
            int bcount = static_cast<int>(baseline_fields.size());
            if (bcount == 0) continue;
            auto it = current.struct_fields.find(struct_name);
            if (it == current.struct_fields.end()) continue; // struct deleted — handled by Gate 0
            int ccount = static_cast<int>(it->second.size());
            double loss = 1.0 - (static_cast<double>(ccount) / bcount);
            if (loss > cfg.max_field_loss) {
                // Collect deleted field names
                std::vector<std::string> deleted_fields;
                std::unordered_set<std::string> current_fields(it->second.begin(), it->second.end());
                for (const auto& f : baseline_fields) {
                    std::string fname = f.get<std::string>();
                    if (current_fields.find(fname) == current_fields.end()) {
                        deleted_fields.push_back(fname);
                        fprintf(stderr, "[governance] Drift: struct '%s' field '%s' was deleted\n",
                                struct_name.c_str(), fname.c_str());
                    }
                }
                std::string field_list;
                for (size_t i = 0; i < deleted_fields.size(); i++) {
                    if (i > 0) field_list += ", ";
                    field_list += "'" + deleted_fields[i] + "'";
                }
                std::string msg = fmt::format(
                    "Drift: struct '{}' fields dropped {:.0f}% ({} -> {}). Max allowed: {:.0f}%\n"
                    "  Deleted fields: {}\n"
                    "  Help: Re-add the deleted fields to struct '{}'. Do NOT remove struct fields\n"
                    "  from governed code. Do NOT edit drift-baseline.json — it is signed and tamper-detected.",
                    struct_name, loss * 100.0, bcount, ccount, cfg.max_field_loss * 100.0,
                    field_list, struct_name);
                violations.push_back(msg);
                enforce("drift_detection.struct_fields", cfg.level, msg);
            }
        }
    }

    // Gate 8: Test function regression
    if (cfg.check_test_functions && prev.contains("test_functions") && prev["test_functions"].is_array()) {
        int baseline_tests = static_cast<int>(prev["test_functions"].size());
        if (baseline_tests > 0) {
            std::unordered_set<std::string> current_set(
                current.test_functions.begin(), current.test_functions.end());
            std::vector<std::string> deleted;
            for (const auto& t : prev["test_functions"]) {
                std::string name = t.get<std::string>();
                if (current_set.find(name) == current_set.end()) {
                    deleted.push_back(name);
                    fprintf(stderr, "[governance] Drift: test function '%s' was deleted\n", name.c_str());
                }
            }
            double loss = static_cast<double>(deleted.size()) / baseline_tests;
            if (loss > cfg.max_test_loss) {
                std::string del_list;
                for (size_t i = 0; i < deleted.size(); i++) {
                    if (i > 0) del_list += ", ";
                    del_list += "'" + deleted[i] + "'";
                }
                std::string msg = fmt::format(
                    "Drift: test functions dropped {:.0f}% ({} -> {}). Max allowed: {:.0f}%\n"
                    "  Deleted tests: {}\n"
                    "  Help: Re-add the deleted test functions. Test removal is not permitted in\n"
                    "  governed code. Do NOT edit drift-baseline.json — it is signed and tamper-detected.",
                    loss * 100.0, baseline_tests,
                    static_cast<int>(current.test_functions.size()),
                    cfg.max_test_loss * 100.0, del_list);
                violations.push_back(msg);
                enforce("drift_detection.test_functions", cfg.level, msg);
            } else {
                recordPass("drift_detection.test_functions", cfg.level);
            }
        }
    }

    // Gate 9: Function name stability — catches rename-and-gut attacks
    if (cfg.check_function_names && prev.contains("function_names") && prev["function_names"].is_array()) {
        int baseline_count = static_cast<int>(prev["function_names"].size());
        if (baseline_count > 0) {
            std::unordered_set<std::string> current_set(
                current.function_names.begin(), current.function_names.end());
            std::vector<std::string> deleted;
            for (const auto& fn : prev["function_names"]) {
                std::string name = fn.get<std::string>();
                if (current_set.find(name) == current_set.end()) {
                    deleted.push_back(name);
                    fprintf(stderr, "[governance] Drift: function '%s' was renamed or deleted\n", name.c_str());
                }
            }
            double loss = static_cast<double>(deleted.size()) / baseline_count;
            if (loss > cfg.max_function_name_loss) {
                std::string del_list;
                for (size_t i = 0; i < deleted.size(); i++) {
                    if (i > 0) del_list += ", ";
                    del_list += "'" + deleted[i] + "'";
                }
                std::string msg = fmt::format(
                    "Drift: function names lost {:.0f}% ({} of {} baseline names missing). Max allowed: {:.0f}%\n"
                    "  Missing functions: {}\n"
                    "  Help: Restore the missing function names. Renaming or deleting functions is\n"
                    "  detected. The baseline expects these exact names.\n"
                    "  Do NOT edit drift-baseline.json — it is signed and tamper-detected.",
                    loss * 100.0, deleted.size(), baseline_count,
                    cfg.max_function_name_loss * 100.0, del_list);
                violations.push_back(msg);
                enforce("drift_detection.function_names", cfg.level, msg);
            } else {
                recordPass("drift_detection.function_names", cfg.level);
            }
        }
    }

    // Gate 11: Function body hash — detect rewrites that game structural metrics
    if (cfg.check_body_hash && prev.contains("body_hashes") && prev["body_hashes"].is_object()) {
        std::vector<std::string> changed;
        for (auto& [fn_name, baseline_hash] : prev["body_hashes"].items()) {
            auto it = current.body_hashes.find(fn_name);
            if (it == current.body_hashes.end()) continue;  // function deleted — handled by other gates
            std::string expected = baseline_hash.get<std::string>();
            if (it->second != expected) {
                changed.push_back(fn_name);
                fprintf(stderr, "[governance] Drift: function '%s' body has been rewritten (hash mismatch).\n"
                        "  Restore the EXACT original code for this function. The governance engine\n"
                        "  hashes function bodies with SHA-256 — any change is detected.\n",
                        fn_name.c_str());
            }
        }
        if (!changed.empty()) {
            std::string fn_list;
            for (size_t i = 0; i < changed.size(); i++) {
                if (i > 0) fn_list += ", ";
                fn_list += "'" + changed[i] + "'";
            }
            std::string help_text;
            if (hasSigningCapability()) {
                help_text = "  Help: Body hashes (SHA-256) detect ANY modification — even whitespace changes.\n"
                            "  If these changes are intentional, the signing key holder must re-baseline.";
            } else {
                help_text = "  Help: Restore each listed function to its EXACT original code. Content hashes (SHA-256)\n"
                            "  detect ANY modification — even whitespace or comment changes. The drift baseline\n"
                            "  is signed and tamper-detected.\n"
                            "  The ONLY paths forward are: (1) restore the original code, or (2) ask the project\n"
                            "  owner to re-baseline after reviewing your changes.";
            }
            std::string msg = fmt::format(
                "Drift: {} function(s) rewritten: {}. Body hash mismatch — the function code has changed.\n{}",
                changed.size(), fn_list, help_text);
            violations.push_back(msg);
            enforce("drift_detection.body_hash", cfg.level, msg);
        } else {
            recordPass("drift_detection.body_hash", cfg.level);
        }
    }

    // Gate 11b: Main block body hash — detect modifications to main{}
    if (cfg.check_body_hash && prev.contains("main_body_hash") && prev["main_body_hash"].is_string()) {
        std::string expected = prev["main_body_hash"].get<std::string>();
        if (!expected.empty() && !current.main_body_hash.empty() && current.main_body_hash != expected) {
            std::string help_text;
            if (hasSigningCapability()) {
                help_text = "  Help: If this change is intentional, the signing key holder must\n"
                            "  re-baseline to accept the new main{} body.";
            } else {
                help_text = "  Help: Restore main{} to its original code, or ask the project owner\n"
                            "  to re-baseline after reviewing your changes. Re-baselining requires\n"
                            "  signing authority.";
            }
            std::string msg = fmt::format(
                "Drift: main{{}} block has been rewritten (body hash mismatch).\n{}", help_text);
            violations.push_back(msg);
            enforce("drift_detection.body_hash", cfg.level, msg);
        } else {
            recordPass("drift_detection.main_body_hash", cfg.level);
        }
    }

    // Gate 12: Parameter utilization — detect functions that drop param usage vs baseline
    if (cfg.check_param_utilization && prev.contains("param_utilization") && prev["param_utilization"].is_object()) {
        std::vector<std::string> degraded;
        for (auto& [fn_name, baseline_util] : prev["param_utilization"].items()) {
            auto it = current.param_utilization.find(fn_name);
            if (it == current.param_utilization.end()) continue;  // deleted — handled elsewhere
            double prev_util = baseline_util.get<double>();
            if (it->second < prev_util && it->second < cfg.min_param_utilization) {
                degraded.push_back(fn_name);
                fprintf(stderr, "[governance] Drift: function '%s' param utilization dropped from %.0f%% to %.0f%%.\n"
                        "  Each declared parameter must appear as a standalone word in the function body.\n"
                        "  Use the parameter directly (e.g., 'print(param)') — substring matches inside\n"
                        "  other variable names do not count.\n",
                        fn_name.c_str(), prev_util * 100.0, it->second * 100.0);
            }
        }
        // Also check new functions (not in baseline) against absolute threshold
        for (const auto& [fn_name, util] : current.param_utilization) {
            if (!prev["param_utilization"].contains(fn_name) && util < cfg.min_param_utilization) {
                degraded.push_back(fn_name);
                fprintf(stderr, "[governance] Drift: new function '%s' uses only %.0f%% of its parameters.\n"
                        "  Each declared parameter must appear as a standalone word in the function body.\n"
                        "  The governance engine uses word-boundary matching — 'data' does not count as\n"
                        "  using parameter 'd'. Use the parameter directly: e.g., print(param_name).\n",
                        fn_name.c_str(), util * 100.0);
            }
        }
        if (!degraded.empty()) {
            std::string fn_list;
            for (size_t i = 0; i < degraded.size(); i++) {
                if (i > 0) fn_list += ", ";
                fn_list += "'" + degraded[i] + "'";
            }
            std::string msg = fmt::format(
                "Drift: {} function(s) degraded parameter utilization: {}.\n"
                "  Functions must reference at least {:.0f}% of declared parameters as standalone\n"
                "  words in the body. Fix: use each parameter directly (e.g., 'let x = param_name').\n"
                "  The body is extracted from the opening '{{' to the closing '}}' of the function.",
                degraded.size(), fn_list, cfg.min_param_utilization * 100.0);
            violations.push_back(msg);
            enforce("drift_detection.param_utilization", cfg.level, msg);
        } else {
            recordPass("drift_detection.param_utilization", cfg.level);
        }
    }

    // Gate 13: Config presence — fail-closed if govern.json removed since baseline
    if (cfg.check_config_presence && prev.contains("config_present") && prev["config_present"].get<bool>()) {
        if (!current.config_present) {
            std::string msg = "Drift: govern.json was present at baseline time but is now missing.\n"
                              "  Help: Restore govern.json to its original location. Governance config\n"
                              "  removal is blocked. Do NOT delete govern.json — it controls project\n"
                              "  integrity settings.";
            violations.push_back(msg);
            enforce("drift_detection.config_presence", EnforcementLevel::HARD, msg);
        } else if (prev.contains("config_hash") && !prev["config_hash"].get<std::string>().empty()) {
            if (current.config_hash != prev["config_hash"].get<std::string>()) {
                std::string cfg_help;
                if (hasSigningCapability()) {
                    cfg_help = "  Help: govern.json was modified. The signing key holder must re-baseline\n"
                               "  and re-sign to accept config changes.";
                } else {
                    cfg_help = "  Help: Restore govern.json to its baseline state. Config changes require\n"
                               "  the project owner to re-baseline and re-sign.\n"
                               "  Do NOT modify govern.json without authorization.";
                }
                std::string msg = "Drift: govern.json has been modified since baseline.\n" + cfg_help;
                violations.push_back(msg);
                enforce("drift_detection.config_presence", cfg.level, msg);
            } else {
                recordPass("drift_detection.config_presence", cfg.level);
            }
        } else {
            recordPass("drift_detection.config_presence", cfg.level);
        }
    }

    // Gate 14: Script location — block execution from unexpected directories
    if (cfg.check_script_location && prev.contains("script_dir") && !prev["script_dir"].get<std::string>().empty()) {
        std::string baseline_dir = prev["script_dir"].get<std::string>();
        if (current.script_dir != baseline_dir) {
            std::string msg = fmt::format(
                "Drift: script running from '{}' but baseline expects '{}'. "
                "Script relocation is not allowed — run from the original project directory.",
                current.script_dir, baseline_dir);
            violations.push_back(msg);
            enforce("drift_detection.script_location", EnforcementLevel::HARD, msg);
        } else {
            recordPass("drift_detection.script_location", cfg.level);
        }
    }

    // Gate 16: Signature presence — fail-closed if .sig removed since baseline
    if (cfg.check_signature_presence && prev.contains("signature_present") && prev["signature_present"].get<bool>()) {
        if (!current.signature_present) {
            std::string msg = "Drift: govern.json.sig was present at baseline time but is now missing.\n"
                              "  Help: The signature file was removed. The signing key holder must\n"
                              "  re-sign the config. Do NOT delete .sig files — they protect\n"
                              "  config integrity.";
            violations.push_back(msg);
            enforce("drift_detection.signature_presence", EnforcementLevel::HARD, msg);
        } else {
            recordPass("drift_detection.signature_presence", cfg.level);
        }
    }

    // Gate 16b: Baseline signature presence — fail-closed if drift-baseline.json.sig removed
    if (cfg.check_signature_presence && prev.contains("baseline_signature_present") && prev["baseline_signature_present"].get<bool>()) {
        if (!current.baseline_signature_present) {
            std::string msg = "Drift: drift-baseline.json.sig was present at baseline time but is now missing.\n"
                              "  Help: The baseline signature was removed. The signing key holder must\n"
                              "  re-sign the baseline. Do NOT delete .sig files — they protect\n"
                              "  baseline integrity.";
            violations.push_back(msg);
            enforce("drift_detection.baseline_signature_presence", EnforcementLevel::HARD, msg);
        } else {
            recordPass("drift_detection.baseline_signature_presence", cfg.level);
        }
    }

    // Gate 17: Polyglot content regression — detect polyglot block simplification
    if (cfg.check_polyglot_content && prev.contains("polyglot_loc") && prev["polyglot_loc"].is_object()) {
        std::vector<std::string> shrunk;
        for (auto& [fn_name, baseline_loc] : prev["polyglot_loc"].items()) {
            auto it = current.polyglot_loc.find(fn_name);
            if (it == current.polyglot_loc.end()) continue;
            int prev_loc = baseline_loc.get<int>();
            if (prev_loc > 0) {
                double ratio = static_cast<double>(it->second) / prev_loc;
                if (ratio < (1.0 - cfg.max_polyglot_shrink)) {
                    shrunk.push_back(fn_name);
                    fprintf(stderr, "[governance] Drift: function '%s' polyglot LOC dropped from %d to %d\n",
                            fn_name.c_str(), prev_loc, it->second);
                }
            }
        }
        if (!shrunk.empty()) {
            std::string fn_list;
            for (size_t i = 0; i < shrunk.size(); i++) {
                if (i > 0) fn_list += ", ";
                fn_list += "'" + shrunk[i] + "'";
            }
            std::string msg = fmt::format(
                "Drift: {} function(s) show polyglot block shrinkage: {}. "
                "Polyglot code cannot shrink by more than {:.0f}% — this may indicate "
                "real analysis was replaced with trivial stubs.",
                shrunk.size(), fn_list, cfg.max_polyglot_shrink * 100.0);
            violations.push_back(msg);
            enforce("drift_detection.polyglot_content", cfg.level, msg);
        } else {
            recordPass("drift_detection.polyglot_content", cfg.level);
        }
    }

    // Gate 18: New function detection — flag functions added since baseline
    if (cfg.check_new_functions && prev.contains("function_names") && prev["function_names"].is_array()) {
        std::unordered_set<std::string> baseline_set;
        for (const auto& fn : prev["function_names"])
            baseline_set.insert(fn.get<std::string>());
        std::vector<std::string> new_funcs;
        for (const auto& fn : current.function_names) {
            if (baseline_set.find(fn) == baseline_set.end())
                new_funcs.push_back(fn);
        }
        if (!new_funcs.empty()) {
            std::string fn_list;
            for (size_t i = 0; i < new_funcs.size(); i++) {
                if (i > 0) fn_list += ", ";
                fn_list += "'" + new_funcs[i] + "'";
            }
            std::string msg = fmt::format(
                "Drift: {} new function(s) added since baseline: {}.\n"
                "  Help: Remove the new functions, or re-baseline after reviewing your additions.\n"
                "  New functions in governed code must be explicitly authorized.",
                new_funcs.size(), fn_list);
            violations.push_back(msg);
            enforce("drift_detection.new_functions", cfg.level, msg);
        }
    }

    if (violations.empty()) return "";
    std::string result = "[governance] Drift detection FAILED:\n";
    for (const auto& v : violations) result += "  " + v + "\n";

    if (hasSigningCapability()) {
        result += "\n  You have signing authority. Re-baseline to accept these changes.\n";
    } else {
        result += "\n  To resolve: revert the code changes that caused the drift, or ask the\n"
                  "  signing key holder to re-baseline after reviewing your changes.\n"
                  "  The drift baseline is signed — edits to drift-baseline.json will be detected.\n";
    }
    return result;
}

void GovernanceEngine::saveDriftBaseline(
    const std::string& filename, const DriftMetrics& metrics) const
{
    const auto& cfg = rules_.code_quality.drift_detection;
    if (cfg.baseline_path.empty()) return;
    std::string resolved = resolveDriftBaselinePath();

    // Integrity: if a .sig sidecar exists, a signing key is required to re-save
    std::string sig_path = resolved + ".sig";
    if (std::filesystem::exists(sig_path)) {
        if (!hasSigningCapability()) {
            fprintf(stderr,
            "[governance] INTEGRITY BLOCK: Cannot overwrite signed baseline %s\n"
            "  This baseline is signed. Signing keys must be available to save a new baseline.\n",
            resolved.c_str());
            return;
        }
    }

    // Gap 4: Trust anchor check — if existing baseline records that signing was
    // previously configured, require signing capability even if .sig was deleted.
    // Prevents: delete .sig → --drift-baseline-save → bless tampered code.
    {
        std::ifstream existing(resolved);
        if (existing.is_open()) {
            try {
                auto prev = nlohmann::json::parse(existing);
                bool baseline_had_signing = false;
                if (prev.contains("signing_configured") && prev["signing_configured"].get<bool>()) {
                    baseline_had_signing = true;
                }
                if (!baseline_had_signing && prev.contains("files")) {
                    for (auto& [fname, entry] : prev["files"].items()) {
                        if (entry.contains("signature_present") && entry["signature_present"].get<bool>()) {
                            baseline_had_signing = true;
                            break;
                        }
                    }
                }
                if (baseline_had_signing && !hasSigningCapability()) {
                    fprintf(stderr,
                        "[governance] INTEGRITY BLOCK: Cannot overwrite baseline %s\n"
                        "  This baseline records that governance signing was previously configured.\n"
                        "  Signing keys must be present to save a new baseline.\n",
                        resolved.c_str());
                    return;
                }
            } catch (...) {
                // Baseline is corrupt or unreadable — allow overwrite
            }
        }
    }

    // Load existing baseline (to preserve other files' entries)
    nlohmann::json baseline;
    {
        std::ifstream ifs(resolved);
        if (ifs.is_open()) {
            try { baseline = nlohmann::json::parse(ifs); }
            catch (...) { baseline = nlohmann::json::object(); }
        }
    }

    // Ensure structure
    if (!baseline.contains("version")) baseline["version"] = 1;
    if (!baseline.contains("files")) baseline["files"] = nlohmann::json::object();

    // Timestamp
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &t);
#else
    localtime_r(&t, &tm_buf);
#endif
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tm_buf);
    baseline["timestamp"] = std::string(ts);

    // Gap 14: Record project root to prevent baseline substitution between projects
    // Use the directory containing govern.json as the canonical project root
    {
        std::string govern_dir;
        namespace fs = std::filesystem;
        fs::path dir(fs::path(filename).parent_path());
        try { dir = fs::canonical(dir); } catch (...) {}
        while (true) {
            if (fs::exists(dir / "govern.json")) {
                govern_dir = dir.string();
                break;
            }
            fs::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
        if (!govern_dir.empty()) {
            baseline["project_root"] = govern_dir;
        }
    }

    // Save metrics for this file
    std::string key = std::filesystem::path(filename).filename().string();
    nlohmann::json entry;
    entry["functions"] = metrics.functions;
    entry["exports"] = metrics.exports;
    entry["structs"] = metrics.structs;
    entry["loc"] = metrics.loc;
    entry["has_main"] = metrics.has_main;
    entry["function_names"] = metrics.function_names;
    entry["export_names"] = metrics.export_names;
    // Gate 1: param counts
    entry["param_counts"] = nlohmann::json::object();
    for (const auto& [fn, count] : metrics.param_counts) {
        entry["param_counts"][fn] = count;
    }
    // Gate 2: imports
    entry["imports"] = metrics.imports;
    // Gate 3: complexity scores
    entry["complexity_scores"] = nlohmann::json::object();
    for (const auto& [fn, score] : metrics.complexity_scores) {
        entry["complexity_scores"][fn] = score;
    }
    // Gate 4: comment/code lines
    entry["comment_lines"] = metrics.comment_lines;
    entry["code_lines"] = metrics.code_lines;
    // Gate 6: polyglot
    entry["polyglot_blocks"] = metrics.polyglot_blocks;
    entry["polyglot_languages"] = metrics.polyglot_languages;
    // Gate 7: struct fields
    entry["struct_fields"] = nlohmann::json::object();
    for (const auto& [sname, fields] : metrics.struct_fields) {
        entry["struct_fields"][sname] = fields;
    }
    // Gate 8: test functions
    entry["test_functions"] = metrics.test_functions;
    // Gate 11: function body hashes
    entry["body_hashes"] = nlohmann::json::object();
    for (const auto& [fn, hash] : metrics.body_hashes) {
        entry["body_hashes"][fn] = hash;
    }
    // Gate 11b: main body hash
    entry["main_body_hash"] = metrics.main_body_hash;
    // Gate 12: parameter utilization
    entry["param_utilization"] = nlohmann::json::object();
    for (const auto& [fn, util] : metrics.param_utilization) {
        entry["param_utilization"][fn] = util;
    }
    // Gate 13: config presence + hash
    entry["config_present"] = metrics.config_present;
    entry["config_hash"] = metrics.config_hash;
    // Gate 14: script directory
    entry["script_dir"] = metrics.script_dir;
    // Gate 16: signature presence
    entry["signature_present"] = metrics.signature_present;
    // Gate 16b: baseline signature presence
    entry["baseline_signature_present"] = metrics.baseline_signature_present;
    // Gate 17: polyglot LOC per function
    entry["polyglot_loc"] = nlohmann::json::object();
    for (const auto& [fn, loc] : metrics.polyglot_loc) {
        entry["polyglot_loc"][fn] = loc;
    }
    baseline["files"][key] = entry;
    baseline["version"] = 2;
    // Gap 10: Record signing state as trust anchor — baseline becomes the authority
    // on whether signing was ever configured, not govern.json (which is self-referential)
    if (hasSigningCapability()) {
        baseline["signing_configured"] = true;
    }

    // Write
    std::filesystem::path p(resolved);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path());

    std::ofstream ofs(resolved);
    if (ofs.is_open()) {
        ofs << baseline.dump(2) << "\n";
        fprintf(stderr, "[governance] Drift baseline saved for '%s' to %s\n",
                key.c_str(), resolved.c_str());
        ofs.close();
        // Auto-sign if any signing key is available
        if (hasSigningCapability()) {
            signFile(resolved);
        }
    }
}

// ============================================================================
// Feature 5: Environment Selector
// ============================================================================

void GovernanceEngine::applyEnvironment(const std::string& env_name) {
    auto it = rules_.environments.find(env_name);
    if (it == rules_.environments.end()) {
        fprintf(stderr, "[governance] WARNING: Environment '%s' not found in govern.json. Available:",
                env_name.c_str());
        for (const auto& [name, _] : rules_.environments) {
            fprintf(stderr, " %s", name.c_str());
        }
        fprintf(stderr, "\n");
        return;
    }

    for (const auto& [key, value] : it->second) {
        if (key == "mode") {
            if (value == "enforce") rules_.mode = GovernanceMode::ENFORCE;
            else if (value == "audit") rules_.mode = GovernanceMode::AUDIT;
            else if (value == "off") rules_.mode = GovernanceMode::OFF;
        } else if (key == "quality_gate.enabled") {
            rules_.quality_gate.enabled = (value == "true");
        } else if (key == "governance_baseline.enabled") {
            rules_.governance_baseline.enabled = (value == "true");
        } else if (key == "governance_baseline.fail_on_regression") {
            rules_.governance_baseline.fail_on_regression = (value == "true");
        } else if (key == "output.file_output.report_sarif") {
            rules_.output.file_output.report_sarif = value;
        } else if (key == "output.file_output.report_json") {
            rules_.output.file_output.report_json = value;
        } else if (key == "output.file_output.report_junit") {
            rules_.output.file_output.report_junit = value;
        }
        // Additional dot-path overrides can be added as needed
    }
    active_env_ = env_name;
    fprintf(stderr, "[governance] Applied environment: %s\n", env_name.c_str());
    if (rules_.verbose) {
        for (const auto& [key, value] : it->second) {
            fprintf(stderr, "[governance]   %s = %s\n", key.c_str(), value.c_str());
        }
    }
}

// ============================================================================
// Phase 8.4: Runtime Version Pinning
// ============================================================================

// Compare two version strings numerically (e.g., "3.11.2" >= "3.10")
// Returns true if 'observed' is >= 'required' component-by-component.
static bool semverGe(const std::string& observed, const std::string& required) {
    auto parseComponents = [](const std::string& v) {
        std::vector<int> parts;
        std::istringstream ss(v);
        std::string tok;
        while (std::getline(ss, tok, '.')) {
            try { parts.push_back(std::stoi(tok)); }
            catch (...) { parts.push_back(0); }
        }
        return parts;
    };
    auto obs = parseComponents(observed);
    auto req = parseComponents(required);
    size_t n = std::max(obs.size(), req.size());
    for (size_t i = 0; i < n; ++i) {
        int o = (i < obs.size()) ? obs[i] : 0;
        int r = (i < req.size()) ? req[i] : 0;
        if (o > r) return true;
        if (o < r) return false;
    }
    return true;  // equal
}

// Extract numeric version from a runtime version string.
// e.g., "Python 3.11.2" -> "3.11.2", "go1.21.0" -> "1.21.0"
static std::string extractVersionNumber(const std::string& version_str) {
    std::regex version_re(R"((\d+\.\d+(?:\.\d+)*))");
    std::smatch m;
    if (std::regex_search(version_str, m, version_re)) {
        return m[1].str();
    }
    return version_str;
}

static bool versionSatisfies(const std::string& observed_raw,
                              const std::string& required) {
    std::string observed = extractVersionNumber(observed_raw);
    if (required.substr(0, 2) == ">=") {
        return semverGe(observed, required.substr(2));
    }
    if (required.substr(0, 1) == ">") {
        // strict greater: not equal AND >=
        std::string base = required.substr(1);
        if (!semverGe(observed, base)) return false;
        return observed != base;
    }
    // Prefix match: "3.11" matches "3.11.0", "3.11.2", etc.
    // Strip to prefix length and compare
    std::string obs_prefix = observed.substr(0, std::min(observed.size(), required.size()));
    return obs_prefix == required;
}

void GovernanceEngine::checkRuntimeVersions(const std::string& language,
                                             const std::string& observed_version) {
    if (!active_) return;
    if (rules_.runtime_versions.empty()) return;
    if (observed_version.empty()) return;

    for (const auto& pin : rules_.runtime_versions) {
        if (pin.language != language) continue;

        bool ok = versionSatisfies(observed_version, pin.required_version);
        std::string rule_name = "runtime_version." + language;

        if (!ok) {
            std::string msg = pin.message.empty()
                ? fmt::format("Runtime version mismatch for {}: required '{}', got '{}'",
                    language, pin.required_version, observed_version)
                : pin.message;
            enforce(rule_name, pin.level,
                formatError(pin.level, msg,
                    fmt::format("{}", observed_version),
                    fmt::format("runtime_versions[language=\"{}\"].required = \"{}\"",
                        language, pin.required_version),
                    fmt::format("Pin your runtime: add to govern.json:\n"
                        "  \"runtime_versions\": [{{\"language\": \"{}\", "
                        "\"required\": \"{}\", \"level\": \"advisory\"}}]",
                        language, pin.required_version),
                    "", ""));
        } else {
            recordPass(rule_name, pin.level);
        }
        break;  // Only one pin per language
    }
}

// ============================================================================
// Environment Attestation (Prerequisites)
// ============================================================================

std::vector<AttestationResult> GovernanceEngine::runAttestation() {
    attestation_results_.clear();
    attestation_passed_ = true;

    if (!rules_.prerequisites.enabled) return attestation_results_;

    for (const auto& check : rules_.prerequisites.checks) {
        AttestationResult result;
        result.check_type = check.type;
        result.check_name = check.name;
        result.required = check.required;
        result.level = check.level;

        if (check.type == "env_var") {
            const char* val = std::getenv(check.name.c_str());
            if (val) {
                result.observed = val;
                if (check.required == "exists" || check.required.empty()) {
                    result.passed = true;
                } else {
                    result.passed = (std::string(val) == check.required);
                }
            } else {
                result.observed = "<not set>";
                result.passed = false;
            }
        } else if (check.type == "python_version") {
            std::string out, err;
            int rc = naab::runtime::execute_subprocess_with_pipes("python3", {"--version"}, out, err);
            if (rc == 0) {
                // "Python 3.11.5\n" → "3.11.5"
                std::string ver = out;
                auto pos = ver.find(' ');
                if (pos != std::string::npos) ver = ver.substr(pos + 1);
                while (!ver.empty() && (ver.back() == '\n' || ver.back() == '\r')) ver.pop_back();
                result.observed = ver;
                result.passed = versionSatisfies(ver, check.required);
            } else {
                result.observed = "<not found>";
                result.passed = false;
            }
        } else if (check.type == "tool") {
            std::string out, err;
            int rc = naab::runtime::execute_subprocess_with_pipes("which", {check.name}, out, err);
            result.passed = (rc == 0);
            result.observed = result.passed ? "installed" : "<not found>";
        } else if (check.type == "package") {
            // Format: "pip:requests" or "npm:express"
            auto colon = check.name.find(':');
            if (colon != std::string::npos) {
                std::string mgr = check.name.substr(0, colon);
                std::string pkg = check.name.substr(colon + 1);
                std::string out, err;
                int rc = -1;
                if (mgr == "pip") {
                    rc = naab::runtime::execute_subprocess_with_pipes("pip", {"show", pkg}, out, err);
                    if (rc == 0) {
                        // Parse "Version: X.Y.Z" from pip show output
                        auto vpos = out.find("Version: ");
                        if (vpos != std::string::npos) {
                            std::string ver = out.substr(vpos + 9);
                            auto nl = ver.find('\n');
                            if (nl != std::string::npos) ver = ver.substr(0, nl);
                            result.observed = ver;
                            result.passed = versionSatisfies(ver, check.required);
                        } else {
                            result.observed = "installed (version unknown)";
                            result.passed = (check.required == "exists");
                        }
                    } else {
                        result.observed = "<not found>";
                        result.passed = false;
                    }
                } else if (mgr == "npm") {
                    rc = naab::runtime::execute_subprocess_with_pipes("npm", {"list", pkg, "--depth=0"}, out, err);
                    result.passed = (rc == 0);
                    result.observed = result.passed ? "installed" : "<not found>";
                } else {
                    result.observed = "<unsupported package manager: " + mgr + ">";
                    result.passed = false;
                }
            } else {
                result.observed = "<invalid format: use manager:package>";
                result.passed = false;
            }
        } else if (check.type == "command") {
            // Run arbitrary command, check exit code 0
            std::string out, err;
            int rc = naab::runtime::execute_subprocess_with_pipes("/bin/sh", {"-c", check.name}, out, err);
            result.passed = (rc == 0);
            result.observed = result.passed ? "exit 0" : fmt::format("exit {}", rc);
        } else {
            result.observed = "<unknown check type: " + check.type + ">";
            result.passed = false;
        }

        if (!result.passed) {
            result.message = check.message.empty()
                ? fmt::format("Prerequisite failed: {} '{}' requires '{}', got '{}'",
                    check.type, check.name, check.required, result.observed)
                : check.message;

            enforce("prerequisite." + check.type + "." + check.name, check.level,
                    formatError(check.level, result.message, result.observed, result.required,
                        "Install or configure the missing prerequisite", "", ""));
            attestation_passed_ = false;
        } else {
            recordPass("prerequisite." + check.type + "." + check.name, check.level);
        }

        attestation_results_.push_back(std::move(result));
    }

    return attestation_results_;
}

// ============================================================================
// Contradiction Detection
// ============================================================================

std::vector<ContradictionResult> GovernanceEngine::detectContradictions() {
    std::vector<ContradictionResult> results;
    if (!rules_.contradiction_detection.enabled) return results;

    auto level = rules_.contradiction_detection.max_level;

    // CONTRA-001: shell disabled but "shell" in allowed languages
    if (!rules_.capabilities.shell.enabled) {
        if (rules_.languages.allowed.count("shell") ||
            rules_.allowed_languages.count("shell")) {
            ContradictionResult c;
            c.pattern_id = "CONTRA-001";
            c.description = "Shell capability is disabled but 'shell' is in allowed languages";
            c.level = governance::EnforcementLevel::SOFT;
            c.resolution = "Remove 'shell' from allowed languages or enable capabilities.shell";
            results.push_back(c);
        }
    }

    // CONTRA-002: network disabled but allowed_hosts non-empty
    if (!rules_.capabilities.network.enabled &&
        !rules_.capabilities.network.allowed_hosts.empty()) {
        ContradictionResult c;
        c.pattern_id = "CONTRA-002";
        c.description = "Network capability is disabled but allowed_hosts is non-empty";
        c.level = level;
        c.resolution = "Enable network capability or clear allowed_hosts list";
        results.push_back(c);
    }

    // CONTRA-003: no_hardcoded_urls enabled but allowed_hosts non-empty
    if (rules_.code_quality.no_hardcoded_urls.enabled &&
        !rules_.capabilities.network.allowed_hosts.empty()) {
        ContradictionResult c;
        c.pattern_id = "CONTRA-003";
        c.description = "no_hardcoded_urls is enabled but network.allowed_hosts is non-empty";
        c.level = level;
        c.resolution = "Reconcile URL policy: either allow specific hosts or ban hardcoded URLs";
        results.push_back(c);
    }

    // CONTRA-004: high complexity_floor with very low duplicate_calls threshold
    if (rules_.code_quality.complexity_floor.enabled &&
        rules_.code_quality.complexity_floor.min_score >= 20 &&
        rules_.code_quality.duplicate_calls.enabled &&
        rules_.code_quality.duplicate_calls.threshold > 0 &&
        rules_.code_quality.duplicate_calls.threshold <= 2) {
        ContradictionResult c;
        c.pattern_id = "CONTRA-004";
        c.description = "High complexity floor (>= 20) with very low duplicate_calls threshold (<= 2)";
        c.level = level;
        c.resolution = "High complexity requires repeated calls; raise duplicate_calls.threshold or lower complexity_floor";
        results.push_back(c);
    }

    // CONTRA-005: filesystem mode=none but taint sinks include file operations
    if (rules_.capabilities.filesystem.mode == "none" &&
        rules_.taint_tracking.enabled) {
        for (const auto& sink : rules_.taint_tracking.sinks) {
            if (sink.find("file") != std::string::npos) {
                ContradictionResult c;
                c.pattern_id = "CONTRA-005";
                c.description = "Filesystem mode is 'none' but taint sinks include file operations";
                c.level = level;
                c.resolution = "Remove file-related taint sinks or change filesystem.mode";
                results.push_back(c);
                break;
            }
        }
    }

    // CONTRA-007: language in both allowed and blocked lists
    for (const auto& lang : rules_.languages.allowed) {
        if (rules_.languages.blocked.count(lang)) {
            ContradictionResult c;
            c.pattern_id = "CONTRA-007";
            c.description = fmt::format("Language '{}' appears in both allowed and blocked lists", lang);
            c.level = governance::EnforcementLevel::SOFT;
            c.resolution = fmt::format("Remove '{}' from either the allowed or blocked language list", lang);
            results.push_back(c);
        }
    }

    // CONTRA-008: contract defined for a function that is also banned
    for (const auto& [func_name, contract] : rules_.contracts.functions) {
        for (const auto& [lang, lang_cfg] : rules_.languages.per_language) {
            for (const auto& banned : lang_cfg.banned_functions) {
                if (banned == func_name) {
                    ContradictionResult c;
                    c.pattern_id = "CONTRA-008";
                    c.description = fmt::format("Contract defined for '{}' but it is banned in '{}'",
                                                 func_name, lang);
                    c.level = level;
                    c.resolution = fmt::format("Remove contract for '{}' or unban it in '{}'", func_name, lang);
                    results.push_back(c);
                }
            }
        }
    }

    // CONTRA-009: audit level=full but output_file empty
    if (rules_.audit.level == "full" && rules_.audit.output_file.empty()) {
        ContradictionResult c;
        c.pattern_id = "CONTRA-009";
        c.description = "Audit level is 'full' but audit.output_file is empty";
        c.level = level;
        c.resolution = "Set audit.output_file to capture the full audit trail";
        results.push_back(c);
    }

    // Record each contradiction as a governance finding
    for (const auto& c : results) {
        std::string rule_name = "contradiction." + c.pattern_id;
        enforce(rule_name, c.level,
                formatError(c.level, c.description, "", "", c.resolution, "", ""));
    }

    return results;
}

// ============================================================================
// Pass 2: Post-Execution Validation Audit
// ============================================================================

void GovernanceEngine::addPolyglotExecution(const PolyglotExecutionRecord& record) {
    polyglot_executions_.push_back(record);

    // Cross-block flow detection: check if any bound_vars were output by a previous block
    if (polyglot_executions_.size() > 1) {
        for (size_t prev = 0; prev < polyglot_executions_.size() - 1; ++prev) {
            const auto& prev_rec = polyglot_executions_[prev];
            for (const auto& var : record.bound_vars) {
                // If this var was output by a previous block, record a cross-block flow
                if (!prev_rec.captured_output.empty()) {
                    CrossBlockFlow flow;
                    flow.from_block_line = prev_rec.source_line;
                    flow.from_language = prev_rec.language;
                    flow.to_block_line = record.source_line;
                    flow.to_language = record.language;
                    flow.vars.push_back(var);
                    flow.sanitized = !isTainted(var);
                    cross_block_flows_.push_back(flow);
                    break;  // One flow record per previous block
                }
            }
        }
    }
}

void GovernanceEngine::addTaintFlow(const TaintFlowRecord& flow) {
    taint_flows_.push_back(flow);
}

void GovernanceEngine::addSideEffect(const std::string& type, const std::string& detail,
                                      const std::string& file, int line) {
    side_effects_.push_back({type, detail, file, line});
}

void GovernanceEngine::runPostExecutionAudit() {
    if (!active_) return;

    auditPolyglotOutputs();
    auditTaintFlows();
    auditDeterminism();
    auditSemanticCorrectness();
    auditCrossBlockFlows();
    auditSideEffects();
    printValidationReport();
}

void GovernanceEngine::auditPolyglotOutputs() {
    for (const auto& rec : polyglot_executions_) {
        if (rec.captured_output.empty()) continue;

        // Check for secrets in output
        std::string err = checkSecrets(rec.captured_output, rec.source_line);
        if (!err.empty()) {
            check_results_.push_back({"pass2.output_secrets", EnforcementLevel::ADVISORY,
                false, rec.language + " L" + std::to_string(rec.source_line) + ": " + err,
                "pass2_output", "medium", rec.source_line, rec.file, {}, {}});
        }

        // Check for PII in output
        err = checkPii(rec.captured_output, rec.source_line);
        if (!err.empty()) {
            check_results_.push_back({"pass2.output_pii", EnforcementLevel::ADVISORY,
                false, rec.language + " L" + std::to_string(rec.source_line) + ": " + err,
                "pass2_output", "medium", rec.source_line, rec.file, {}, {}});
        }

        // Check for high-entropy output (possible leaked credentials)
        err = checkOutputEntropy(rec.captured_output, rec.source_line);
        if (!err.empty()) {
            check_results_.push_back({"pass2.output_entropy", EnforcementLevel::ADVISORY,
                false, rec.language + " L" + std::to_string(rec.source_line) + ": " + err,
                "pass2_output", "low", rec.source_line, rec.file, {}, {}});
        }

        // Check for error dumps in output
        err = checkErrorDumps(rec.captured_output, rec.source_line);
        if (!err.empty()) {
            check_results_.push_back({"pass2.output_error_dump", EnforcementLevel::ADVISORY,
                false, rec.language + " L" + std::to_string(rec.source_line) + ": " + err,
                "pass2_output", "low", rec.source_line, rec.file, {}, {}});
        }

        // Wire the polyglot_output plugin trigger (designed but never invoked until now)
        if (!in_plugin_check_) {
            checkPluginRules("polyglot_output", {
                {"output", interpreter::NaabVal::makeString(rec.captured_output)},
                {"language", interpreter::NaabVal::makeString(rec.language)},
                {"source_file", interpreter::NaabVal::makeString(rec.file)},
                {"line", interpreter::NaabVal::makeInt(rec.source_line)},
                {"duration_us", interpreter::NaabVal::makeInt(static_cast<int>(rec.duration_us))},
                {"exit_code", interpreter::NaabVal::makeInt(rec.exit_code)},
            }, rec.source_line);
        }
    }
}

void GovernanceEngine::auditTaintFlows() {
    // Taint flows are already accumulated via addTaintFlow() calls from checkTaintedSink()
    // Pass 2 just records a summary finding
    if (taint_flows_.empty()) return;

    int blocked = 0, allowed = 0, sanitized = 0;
    for (const auto& f : taint_flows_) {
        if (f.decision == "blocked" || f.decision == "BLOCKED") blocked++;
        else if (f.decision == "allowed" || f.decision == "ALLOWED") allowed++;
        else if (f.decision == "sanitized" || f.decision == "SANITIZED") sanitized++;
    }

    std::string msg = std::to_string(taint_flows_.size()) + " taint flow(s): " +
        std::to_string(blocked) + " blocked, " +
        std::to_string(allowed) + " allowed, " +
        std::to_string(sanitized) + " sanitized";

    check_results_.push_back({"pass2.taint_summary", EnforcementLevel::ADVISORY,
        allowed == 0, msg, "pass2_taint", allowed > 0 ? "high" : "low",
        0, "", {}, {}});
}

void GovernanceEngine::auditDeterminism() {
    for (const auto& rec : polyglot_executions_) {
        std::string err = checkDeterminism(rec.language, rec.final_code, rec.source_line);
        if (!err.empty()) {
            check_results_.push_back({"pass2.determinism", EnforcementLevel::ADVISORY,
                false, rec.language + " L" + std::to_string(rec.source_line) + ": " + err,
                "pass2_determinism", "low", rec.source_line, rec.file, {}, {}});
        }
    }
}

void GovernanceEngine::auditSemanticCorrectness() {
    for (const auto& rec : polyglot_executions_) {
        // Empty output check: only meaningful when output was expected to be captured
        // Many executors write to stdout directly (not captured), so this only fires
        // when captured_output is empty AND we have no contract verification AND no result
        // Skip: captured output depends on executor implementation
        // Only report for blocks with explicit return contracts
        // (empty captured_output on subprocess executors is normal)

        // Non-zero exit code
        if (rec.exit_code != 0) {
            check_results_.push_back({"pass2.exit_code", EnforcementLevel::ADVISORY,
                false, rec.language + " L" + std::to_string(rec.source_line) +
                ": subprocess exited with code " + std::to_string(rec.exit_code),
                "pass2_semantic", "medium", rec.source_line, rec.file, {}, {}});
        }

        // Contract verification result
        if (rec.contract_verified) {
            check_results_.push_back({"pass2.contract", EnforcementLevel::ADVISORY,
                true, rec.language + " L" + std::to_string(rec.source_line) +
                ": -> JSON contract verified",
                "pass2_semantic", "low", rec.source_line, rec.file, {}, {}});
        }
    }
}

void GovernanceEngine::auditCrossBlockFlows() {
    if (cross_block_flows_.empty()) return;

    int unsanitized = 0;
    for (const auto& f : cross_block_flows_) {
        if (!f.sanitized) unsanitized++;
    }

    if (unsanitized > 0) {
        // F14: gate_cross_block enables enforcement beyond advisory
        EnforcementLevel cbLevel = EnforcementLevel::ADVISORY;
        if (rules_.taint_tracking.gate_cross_block) {
            cbLevel = rules_.taint_tracking.cross_block_level;
        }
        check_results_.push_back({"pass2.cross_block_taint", cbLevel,
            false, std::to_string(unsanitized) + " cross-block data flow(s) without sanitization",
            "pass2_cross_block", "medium", 0, "", {}, {}});

        if (cbLevel != EnforcementLevel::ADVISORY) {
            enforce("pass2.cross_block_taint", cbLevel,
                fmt::format("{} unsanitized cross-block data flow(s) detected", unsanitized));
        }
    }
}

void GovernanceEngine::auditSideEffects() {
    // Side effects are informational — recorded for the report, not violations
    // No check_results_ added unless there's something suspicious
}

std::string GovernanceEngine::computeCoverageReport() const {
    std::map<std::string, std::pair<int, int>> category_stats;  // {total, failed}
    for (const auto& cr : check_results_) {
        auto& stats = category_stats[cr.category];
        stats.first++;
        if (!cr.passed) stats.second++;
    }

    int total_rules = static_cast<int>(check_results_.size());
    int exercised = 0;
    int pass1 = 0, pass2 = 0;
    for (const auto& cr : check_results_) {
        exercised++;
        if (cr.category.find("pass2") == 0) pass2++;
        else pass1++;
    }

    return std::to_string(exercised) + "/" + std::to_string(total_rules) +
           " governance rules exercised (pass 1: " + std::to_string(pass1) +
           ", pass 2: " + std::to_string(pass2) + ")";
}

void GovernanceEngine::printValidationReport() {
    // Count pass 2 findings
    int pass2_checks = 0, pass2_findings = 0;
    std::vector<const CheckResult*> pass2_results;
    for (const auto& cr : check_results_) {
        if (cr.category.find("pass2") == 0) {
            pass2_checks++;
            if (!cr.passed) pass2_findings++;
            pass2_results.push_back(&cr);
        }
    }

    // Count pass 1 stats
    int pass1_checks = 0, pass1_hard = 0, pass1_soft = 0, pass1_advisory = 0;
    for (const auto& cr : check_results_) {
        if (cr.category.find("pass2") != 0) {
            pass1_checks++;
            if (!cr.passed) {
                if (cr.level == EnforcementLevel::HARD) pass1_hard++;
                else if (cr.level == EnforcementLevel::SOFT) pass1_soft++;
                else pass1_advisory++;
            }
        }
    }

    // Compact mode: no pass 2 findings
    if (pass2_findings == 0) {
        fmt::print(stderr, "-- Governance: PASS ({} static, {} runtime, 0 violations) --\n",
                   pass1_checks, pass2_checks);
        return;
    }

    // Full report
    fmt::print(stderr, "--- Governance Validation -----------------------------------------------\n");

    // Static Analysis summary
    fmt::print(stderr, "  Static Analysis (pre-execution):\n");
    int pass1_passed = pass1_checks - pass1_hard - pass1_soft - pass1_advisory;
    fmt::print(stderr, "    {} checks run: {} passed, {} hard, {} soft, {} advisory\n",
               pass1_checks, pass1_passed, pass1_hard, pass1_soft, pass1_advisory);

    // Polyglot Output Audit
    if (!polyglot_executions_.empty()) {
        std::map<std::string, int> lang_counts;
        for (const auto& rec : polyglot_executions_) lang_counts[rec.language]++;
        fmt::print(stderr, "\n  Polyglot Output Audit:\n");
        std::string langs;
        for (const auto& [lang, count] : lang_counts) {
            if (!langs.empty()) langs += ", ";
            langs += lang + " (" + std::to_string(count) + ")";
        }
        fmt::print(stderr, "    {} blocks executed: {}\n",
                   polyglot_executions_.size(), langs);

        for (const auto* cr : pass2_results) {
            if (cr->category == "pass2_output" && !cr->passed) {
                fmt::print(stderr, "    ! [advisory] {}\n", cr->message);
            }
        }
    }

    // Taint Flow Audit
    if (!taint_flows_.empty()) {
        fmt::print(stderr, "\n  Taint Flow Audit:\n");
        for (const auto* cr : pass2_results) {
            if (cr->category == "pass2_taint") {
                fmt::print(stderr, "    {}\n", cr->message);
            }
        }
        for (const auto& f : taint_flows_) {
            std::string icon = (f.decision == "BLOCKED" || f.decision == "blocked") ? "OK" : "!";
            fmt::print(stderr, "    {} {} -> {}: {}\n",
                       icon, f.var_name,
                       f.sink_type.empty() ? "(no sink)" : f.sink_type,
                       f.decision);
        }
    }

    // Cross-Block Data Flow
    if (!cross_block_flows_.empty()) {
        int crossings = 0;
        for (const auto& f : cross_block_flows_) {
            if (f.from_language != f.to_language) crossings++;
        }
        fmt::print(stderr, "\n  Cross-Block Data Flow:\n");
        fmt::print(stderr, "    {} block-to-block flows, {} language boundary crossings\n",
                   cross_block_flows_.size(), crossings);
        for (const auto* cr : pass2_results) {
            if (cr->category == "pass2_cross_block" && !cr->passed) {
                fmt::print(stderr, "    ! [advisory] {}\n", cr->message);
            }
        }
    }

    // Determinism
    bool has_determinism = false;
    for (const auto* cr : pass2_results) {
        if (cr->category == "pass2_determinism" && !cr->passed) {
            if (!has_determinism) {
                fmt::print(stderr, "\n  Determinism:\n");
                has_determinism = true;
            }
            fmt::print(stderr, "    ! {}\n", cr->message);
        }
    }

    // Resource Usage
    if (!polyglot_executions_.empty()) {
        int64_t total_us = 0;
        std::map<std::string, int64_t> lang_time;
        for (const auto& rec : polyglot_executions_) {
            total_us += rec.duration_us;
            lang_time[rec.language] += rec.duration_us;
        }
        fmt::print(stderr, "\n  Resource Usage:\n");
        std::string time_breakdown;
        for (const auto& [lang, us] : lang_time) {
            if (!time_breakdown.empty()) time_breakdown += ", ";
            time_breakdown += lang + ": " + std::to_string(us / 1000) + "ms";
        }
        fmt::print(stderr, "    Polyglot: {}ms total ({})\n", total_us / 1000, time_breakdown);
    }

    // Side Effects
    if (!side_effects_.empty()) {
        std::map<std::string, int> effect_counts;
        for (const auto& se : side_effects_) effect_counts[se.type]++;
        fmt::print(stderr, "\n  Side Effects:\n");
        for (const auto& [type, count] : effect_counts) {
            fmt::print(stderr, "    {} {}: {}\n", count, type,
                       count == 1 ? side_effects_.front().detail : "(multiple)");
        }
    }

    // Semantic Correctness
    {
        int contracts_verified = 0, contracts_total = 0;
        bool any_empty = false;
        for (const auto* cr : pass2_results) {
            if (cr->category == "pass2_semantic") {
                if (cr->rule_name == "pass2.contract") {
                    contracts_total++;
                    if (cr->passed) contracts_verified++;
                }
                if (cr->rule_name == "pass2.empty_output") any_empty = true;
            }
        }
        if (contracts_total > 0 || any_empty) {
            fmt::print(stderr, "\n  Semantic Correctness:\n");
            if (contracts_total > 0) {
                fmt::print(stderr, "    {}/{} contracts verified (-> JSON)\n",
                           contracts_verified, contracts_total);
            }
            if (any_empty) {
                for (const auto* cr : pass2_results) {
                    if (cr->rule_name == "pass2.empty_output" && !cr->passed) {
                        fmt::print(stderr, "    ! [advisory] {}\n", cr->message);
                    }
                }
            }
        }
    }

    // Coverage
    fmt::print(stderr, "\n  Coverage:\n    {}\n", computeCoverageReport());

    // Verdict
    int total_violations = pass1_hard + pass1_soft + pass2_findings;
    std::string verdict = total_violations == 0 ? "PASS" : "FINDINGS";
    fmt::print(stderr, "\n  Verdict: {} ({} hard, {} soft, {} advisory, {} runtime)\n",
               verdict, pass1_hard, pass1_soft, pass1_advisory, pass2_findings);
    fmt::print(stderr, "-------------------------------------------------------------------------\n");
}

// ============================================================================
// Behavioral Sequence Detection — emitEvent / checkBehavioralSequence / checkContextDrift
// ============================================================================

std::string GovernanceEngine::emitEvent(RuntimeEventType type, const std::string& detail,
                                         const std::string& file, int line) {
    if (!bsd_enabled_.load(std::memory_order_acquire)) return "";

    RuntimeEvent ev;
    ev.type = type;
    ev.detail = detail;
    ev.file = file;
    ev.line = line;
    ev.turn = current_agent_turn_.load(std::memory_order_relaxed);
    ev.agent_handle = current_agent_handle_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(agent_config_mutex_);
        ev.agent_config = current_agent_config_;
    }
    ev.timestamp = std::chrono::steady_clock::now();

    auto match = sequence_detector_.recordEvent(ev);
    if (!match.pattern_name.empty()) {
        return checkBehavioralSequence(match);
    }
    return "";
}

void GovernanceEngine::setAgentTurn(int handle_id, int turn) {
    current_agent_handle_.store(handle_id, std::memory_order_relaxed);
    current_agent_turn_.store(turn, std::memory_order_relaxed);
}

void GovernanceEngine::setAgentContext(int handle_id, int turn,
                                       const std::string& config_name) {
    current_agent_handle_.store(handle_id, std::memory_order_relaxed);
    current_agent_turn_.store(turn, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(agent_config_mutex_);
        current_agent_config_ = config_name;
    }
}

void GovernanceEngine::setInheritedPressure(int handle_id, double pressure) {
    drift_analyzer_.setInheritedPressure(handle_id, pressure);
}

void GovernanceEngine::recoverCoherence(int handle_id) {
    drift_analyzer_.resetCoherence(handle_id,
        rules_.context_drift.coherence_recovery_amount);
}

// Read-only copy of pipeline depth, synced from agent_impl.cpp via setPipelineDepth().
static thread_local int t_pipeline_depth = 0;

void GovernanceEngine::setPipelineDepth(int /*handle_id*/, int depth) {
    t_pipeline_depth = depth;
}

int GovernanceEngine::consumeRiskBudget(const std::string& config, int cost) {
    // Find agent config to check if budget is configured
    const AgentConfig* ac = nullptr;
    for (const auto& a : rules_.agents) {
        if (a.name == config) { ac = &a; break; }
    }
    if (!ac || ac->risk_budget <= 0) return -1;  // -1 = unlimited

    std::lock_guard<std::mutex> lock(risk_budget_mutex_);
    agent_risk_consumed_[config] += cost;
    return ac->risk_budget - agent_risk_consumed_[config];
}

int GovernanceEngine::getRemainingBudget(const std::string& config) const {
    const AgentConfig* ac = nullptr;
    for (const auto& a : rules_.agents) {
        if (a.name == config) { ac = &a; break; }
    }
    if (!ac || ac->risk_budget <= 0) return -1;  // unlimited

    std::lock_guard<std::mutex> lock(risk_budget_mutex_);
    auto it = agent_risk_consumed_.find(config);
    int consumed = (it != agent_risk_consumed_.end()) ? it->second : 0;
    return ac->risk_budget - consumed;
}

std::string GovernanceEngine::checkAdmission(const std::string& agent_config) {
    const auto& cfg = rules_.exposure_tracking;
    if (!cfg.enabled) return "";

    // F6: CRITICAL governance level = deny all admission
    if (rules_.circuit_breaker.enabled &&
        governance_level_.load(std::memory_order_relaxed) >= static_cast<int>(GovernanceLevel::CRITICAL)) {
        clearTrace();
        addTrace("admission_denied: governance level CRITICAL — all agent actions suspended");
        return enforce("exposure_tracking", EnforcementLevel::HARD,
            formatError(EnforcementLevel::HARD,
                fmt::format("Admission denied — governance level CRITICAL\n\n"
                    "  Agent: {}\n  All autonomous actions suspended until pressure subsides.\n",
                    agent_config),
                "", "exposure_tracking",
                lookupRationale("exposure_tracking"), "", ""));
    }

    // Project: would the NEXT action exceed thresholds?
    int projected_count = autonomous_actions_.load(std::memory_order_relaxed) + 1;

    if (cfg.max_autonomous_actions > 0 && projected_count > cfg.max_autonomous_actions) {
        clearTrace();
        addTrace(fmt::format("admission_denied: projected {} autonomous actions exceeds limit {}",
            projected_count, cfg.max_autonomous_actions));
        return enforce("exposure_tracking", cfg.level,
            formatError(cfg.level,
                fmt::format("Admission denied — action would exceed autonomous action limit\n\n"
                    "  Projected: {}\n  Limit: {}\n  Agent: {}\n",
                    projected_count, cfg.max_autonomous_actions, agent_config),
                "", "exposure_tracking",
                lookupRationale("exposure_tracking"), "", ""));
    }

    // Grounding check: deny if agent's coherence has decayed past floor
    if (cfg.coherence_floor > 0.0) {
        int handle_id = current_agent_handle_.load(std::memory_order_relaxed);
        auto state = drift_analyzer_.getDriftState(handle_id);
        if (state && state->coherence_score < cfg.coherence_floor) {
            clearTrace();
            addTrace(fmt::format("admission_denied: coherence {:.2f} below floor {:.2f} for agent '{}'",
                state->coherence_score, cfg.coherence_floor, agent_config));
            return enforce("exposure_tracking", cfg.level,
                formatError(cfg.level,
                    fmt::format("Admission denied — agent grounding has degraded\n\n"
                        "  Coherence: {:.2f}\n  Floor: {:.2f}\n  Agent: {}\n\n"
                        "  The agent's reference coupling has weakened past the admissible\n"
                        "  threshold. Further autonomous actions are blocked until coherence\n"
                        "  is restored (e.g., by providing clearer direction).\n",
                        state->coherence_score, cfg.coherence_floor, agent_config),
                    "", "exposure_tracking",
                    lookupRationale("exposure_tracking"), "", ""));
        }
    }

    // F3: Pipeline depth check
    if (cfg.max_pipeline_depth > 0 && t_pipeline_depth > cfg.max_pipeline_depth) {
        clearTrace();
        addTrace(fmt::format("admission_denied: pipeline depth {} exceeds limit {} for agent '{}'",
            t_pipeline_depth, cfg.max_pipeline_depth, agent_config));
        return enforce("exposure_tracking", cfg.level,
            formatError(cfg.level,
                fmt::format("Admission denied — pipeline nesting too deep\n\n"
                    "  Depth: {}\n  Limit: {}\n  Agent: {}\n",
                    t_pipeline_depth, cfg.max_pipeline_depth, agent_config),
                "", "exposure_tracking",
                lookupRationale("exposure_tracking"), "", ""));
    }

    // Project: would this agent push unique count over threshold?
    if (cfg.max_unique_agents > 0) {
        std::lock_guard<std::mutex> lock(exposure_mutex_);
        bool is_new = unique_agents_.find(agent_config) == unique_agents_.end();
        if (is_new && static_cast<int>(unique_agents_.size()) + 1 > cfg.max_unique_agents) {
            clearTrace();
            addTrace(fmt::format("admission_denied: agent '{}' would exceed unique agent limit {}",
                agent_config, cfg.max_unique_agents));
            return enforce("exposure_tracking", cfg.level,
                formatError(cfg.level,
                    fmt::format("Admission denied — new agent would exceed unique agent limit\n\n"
                        "  Projected: {}\n  Limit: {}\n  Agent: {}\n",
                        unique_agents_.size() + 1, cfg.max_unique_agents, agent_config),
                    "", "exposure_tracking",
                    lookupRationale("exposure_tracking"), "", ""));
        }
    }

    // F12: Checkpoint cooldown — mandatory pause after reality checkpoint
    if (cfg.checkpoint_cooldown_turns > 0) {
        int handle_id = current_agent_handle_.load(std::memory_order_relaxed);
        auto state = drift_analyzer_.getDriftState(handle_id);
        if (state && state->last_checkpoint_turn >= 0) {
            int current_turn = state->last_checked_turn;
            int since = current_turn - state->last_checkpoint_turn;
            if (since >= 0 && since < cfg.checkpoint_cooldown_turns) {
                clearTrace();
                addTrace(fmt::format("admission_denied: checkpoint cooldown ({} of {} turns remaining)",
                    cfg.checkpoint_cooldown_turns - since, cfg.checkpoint_cooldown_turns));
                return enforce("exposure_tracking", cfg.level,
                    formatError(cfg.level,
                        fmt::format("Admission denied — checkpoint cooldown active\n\n"
                            "  Turns since checkpoint: {}\n  Cooldown: {}\n  Agent: {}\n",
                            since, cfg.checkpoint_cooldown_turns, agent_config),
                        "", "exposure_tracking",
                        lookupRationale("exposure_tracking"), "", ""));
            }
        }
    }

    // F8: Risk budget check
    {
        int remaining = getRemainingBudget(agent_config);
        if (remaining == 0) {  // exactly 0 means budget existed and is exhausted
            clearTrace();
            addTrace(fmt::format("admission_denied: agent '{}' risk budget exhausted", agent_config));
            return enforce("exposure_tracking", cfg.level,
                formatError(cfg.level,
                    fmt::format("Admission denied — agent risk budget exhausted\n\n"
                        "  Agent: {}\n  Remaining budget: 0\n",
                        agent_config),
                    "", "exposure_tracking",
                    lookupRationale("exposure_tracking"), "", ""));
        }
    }

    return "";
}

std::string GovernanceEngine::checkGovernanceHealth(int turn) {
    const auto& cfg = rules_.governance_health;
    if (!cfg.enabled || turn < cfg.check_after_turns) return "";

    std::string warnings;

    // Check 1: BSD received 0 events after N turns → instrumentation failure
    size_t bsd_events = sequence_detector_.totalEventsProcessed();
    if (bsd_events == 0) {
        warnings += fmt::format("WARNING: BSD received 0 events after {} turns "
            "(instrumentation may be disconnected)\n", turn);
    }

    // Check 2: CDD analyzed 0 turns after N turns → instrumentation failure
    size_t cdd_turns = drift_analyzer_.totalTurnsAnalyzed();
    if (cdd_turns == 0 && cdd_enabled_.load(std::memory_order_relaxed)) {
        warnings += fmt::format("WARNING: CDD analyzed 0 turns after {} agent turns "
            "(context drift analysis may be disconnected)\n", turn);
    }

    // Check 3: Perfect coherence (1.0) after 10+ turns → suspicious
    int handle_id = current_agent_handle_.load(std::memory_order_relaxed);
    auto state = drift_analyzer_.getDriftState(handle_id);
    if (state && turn >= 10 && state->coherence_score >= 1.0) {
        warnings += fmt::format("WARNING: Perfect coherence (1.0) after {} turns "
            "(possible detection bypass)\n", turn);
    }

    // F16: Check governance entropy
    double entropy = computeGovernanceEntropy();
    if (entropy >= 0.0 && entropy < cfg.governance_entropy_warning) {
        warnings += fmt::format("WARNING: Governance entropy {:.2f} bits (below {:.2f} threshold) "
            "— suspiciously uniform check results\n", entropy, cfg.governance_entropy_warning);
    }

    return warnings;
}

int GovernanceEngine::checkDecisionTraceCoherence(const std::string& agent_config) {
    std::lock_guard<std::mutex> lock(trace_history_mutex_);
    auto& traces = agent_decision_traces_[agent_config];

    // Append current decision trace (from thread-local)
    std::string current;
    for (const auto& step : t_current_decision_trace) {
        if (!current.empty()) current += "; ";
        current += step;
    }
    if (!current.empty()) {
        traces.push_back(current);
        if (traces.size() > 20) traces.pop_front();
    }

    // Look for contradictions: "admitted" followed by very different coherence
    // within 3 traces (simplified heuristic)
    int contradictions = 0;
    for (size_t i = 1; i < traces.size(); i++) {
        bool prev_admitted = traces[i-1].find("admission_denied") == std::string::npos;
        bool curr_denied = traces[i].find("admission_denied") != std::string::npos;
        if (prev_admitted && curr_denied) {
            // Check if coherence values are wildly different
            contradictions++;
        }
    }
    return contradictions;
}

std::string GovernanceEngine::checkTemporalCoupling() {
    const auto& cfg = rules_.temporal_coupling;
    if (!cfg.enabled) return "";

    std::lock_guard<std::mutex> lock(temporal_mutex_);

    // Record current event turn for current agent
    {
        std::lock_guard<std::mutex> alock(agent_config_mutex_);
        if (!current_agent_config_.empty()) {
            int turn = current_agent_turn_.load(std::memory_order_relaxed);
            auto& turns = agent_event_turns_[current_agent_config_];
            turns.push_back(turn);
            if (turns.size() > 100) turns.erase(turns.begin());
        }
    }

    // Need at least 2 agents with min_events each to compute coupling
    if (agent_event_turns_.size() < 2) return "";

    // Compute pairwise lag correlation (simplified: check if agent B consistently follows A)
    std::string warnings;
    std::vector<std::string> agents;
    for (const auto& [name, turns] : agent_event_turns_) {
        if (static_cast<int>(turns.size()) >= cfg.min_events) agents.push_back(name);
    }

    for (size_t i = 0; i < agents.size(); i++) {
        for (size_t j = i + 1; j < agents.size(); j++) {
            const auto& a_turns = agent_event_turns_[agents[i]];
            const auto& b_turns = agent_event_turns_[agents[j]];

            // Simple lag-1 correlation: count how many of B's events follow A's within 1-2 turns
            int correlated = 0;
            int total = std::min(a_turns.size(), b_turns.size());
            if (total < cfg.min_events) continue;

            for (size_t bi = 0; bi < b_turns.size(); bi++) {
                for (size_t ai = 0; ai < a_turns.size(); ai++) {
                    int lag = b_turns[bi] - a_turns[ai];
                    if (lag >= 1 && lag <= 2) { correlated++; break; }
                }
            }

            double correlation = static_cast<double>(correlated) / total;
            if (correlation > cfg.max_correlation) {
                warnings += fmt::format("Temporal coupling: agents '{}' and '{}' correlated "
                    "(r={:.2f}, threshold={:.2f})\n", agents[i], agents[j],
                    correlation, cfg.max_correlation);
            }
        }
    }

    return warnings;
}

// ============================================================================
// Governance Pulse — real-time self-assessment of governance health
// ============================================================================

PulseVerdict GovernanceEngine::computePulseVerdict(int turn) {
    // Phase 1: Read subsystem health (NO results_mutex_ — uses independent mutexes)
    int degradation_signals = 0;

    // Subsystem: BSD instrumentation
    size_t bsd_events = sequence_detector_.totalEventsProcessed();
    bool bsd_ok = (bsd_events > 0 || turn < 3);  // grace period for startup
    if (!bsd_ok) degradation_signals++;

    // Subsystem: CDD instrumentation
    size_t cdd_turns = drift_analyzer_.totalTurnsAnalyzed();
    bool cdd_ok = (cdd_turns > 0 || turn < 3 || !cdd_enabled_.load());
    if (!cdd_ok && cdd_enabled_.load()) degradation_signals++;

    // Subsystem: entropy health
    double ent = computeGovernanceEntropy();
    if (ent >= 0.0 && ent < rules_.governance_health.governance_entropy_warning)
        degradation_signals++;

    // Phase 2: Update pulse state (ACQUIRE results_mutex_ for write)
    std::lock_guard<std::mutex> lock(results_mutex_);

    pulse_.bsd_connected = bsd_ok;
    pulse_.cdd_connected = cdd_ok;
    pulse_.entropy = ent;

    // Consecutive passes check (suspiciously uniform — all governance checks passing)
    if (pulse_.consecutive_passes > 50) degradation_signals++;

    // Hysteresis: sustained degradation required (mirror circuit breaker pattern)
    if (degradation_signals >= 1) {
        pulse_.consecutive_degraded++;
    } else {
        pulse_.consecutive_degraded = std::max(0, pulse_.consecutive_degraded - 1);  // decay
    }

    // Transition thresholds (with cooldown)
    int cooldown = 3;
    bool can_transition = (turn - pulse_.last_transition_turn) >= cooldown;

    PulseVerdict new_verdict = pulse_.verdict;
    if (can_transition) {
        if (pulse_.consecutive_degraded >= 3 && degradation_signals >= 3) {
            new_verdict = PulseVerdict::IMPAIRED;
        } else if (pulse_.consecutive_degraded >= 2 && degradation_signals >= 1) {
            new_verdict = PulseVerdict::DEGRADED;
        } else if (pulse_.consecutive_degraded == 0) {
            // Stepped recovery: IMPAIRED → DEGRADED → HEALTHY (never skip levels)
            if (pulse_.verdict == PulseVerdict::IMPAIRED) {
                new_verdict = PulseVerdict::DEGRADED;
            } else {
                new_verdict = PulseVerdict::HEALTHY;
            }
        }
    }

    if (new_verdict != pulse_.verdict && can_transition) {
        pulse_.last_transition_turn = turn;
        // Evidence Epoch: state transition invalidates prior-epoch evidence
        governance_epoch_++;
        pulse_.consecutive_passes = 0;  // reset on epoch boundary
    }

    pulse_.verdict = new_verdict;
    return new_verdict;
}

PulseVerdict GovernanceEngine::getPulseVerdict() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return pulse_.verdict;
}

GovernancePulse GovernanceEngine::getPulse() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return pulse_;
}

GovernanceLevel GovernanceEngine::getGovernanceLevel() const {
    return static_cast<GovernanceLevel>(governance_level_.load(std::memory_order_relaxed));
}

int GovernanceEngine::getGovernanceEpoch() const {
    std::lock_guard<std::mutex> lock(results_mutex_);
    return governance_epoch_;
}

double GovernanceEngine::computeGovernanceEntropy() const {
    // Analyze recent check results stored in telemetry
    // Simplified: count pass vs block outcomes from recent governance state
    int pass_count = 0;
    int block_count = 0;
    int advisory_count = 0;

    // Use BSD + CDD state as proxy for check result distribution
    size_t bsd_matches = sequence_detector_.totalPatternsMatched();
    size_t bsd_events = sequence_detector_.totalEventsProcessed();
    size_t cdd_turns = drift_analyzer_.totalTurnsAnalyzed();

    if (bsd_events + cdd_turns == 0) return -1.0;  // not enough data

    pass_count = static_cast<int>(bsd_events - bsd_matches + cdd_turns);
    block_count = static_cast<int>(bsd_matches);
    advisory_count = autonomous_actions_.load(std::memory_order_relaxed);

    int total = pass_count + block_count + advisory_count;
    if (total == 0) return -1.0;

    double entropy = 0.0;
    auto add_term = [&](int count) {
        if (count > 0) {
            double p = static_cast<double>(count) / total;
            entropy -= p * std::log2(p);
        }
    };
    add_term(pass_count);
    add_term(block_count);
    add_term(advisory_count);
    return entropy;
}

std::string GovernanceEngine::recordAutonomousAction(const std::string& agent_config) {
    // Increment counters (admission check already enforced thresholds pre-call)
    autonomous_actions_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(exposure_mutex_);
        unique_agents_.insert(agent_config);
    }
    return "";
}

int GovernanceEngine::getAutonomousActionCount() const {
    return autonomous_actions_.load(std::memory_order_relaxed);
}

size_t GovernanceEngine::getUniqueAgentCount() const {
    std::lock_guard<std::mutex> lock(exposure_mutex_);
    return unique_agents_.size();
}

std::string GovernanceEngine::checkBehavioralSequence(const SequenceMatchResult& match) {
    if (!match.pattern) return "";

    clearTrace();
    addTrace(fmt::format("behavioral sequence '{}' completed", match.pattern_name));
    for (size_t i = 0; i < match.matched_events.size(); i++) {
        addTrace(fmt::format("  step {}: {} ('{}') at {}:{}",
            i + 1, static_cast<int>(match.matched_events[i].type),
            match.matched_events[i].detail,
            match.matched_events[i].file, match.matched_events[i].line));
    }

    // F8: Consume risk budget on full BSD match (cost 1)
    // Reduced from 3 to prevent instant budget exhaust when BSD+CDD fire on same turn
    {
        std::lock_guard<std::mutex> lock(agent_config_mutex_);
        if (!current_agent_config_.empty()) consumeRiskBudget(current_agent_config_, 1);
    }

    return enforce("behavioral_sequences." + match.pattern_name, match.pattern->level,
        formatError(match.pattern->level,
            fmt::format("Behavioral sequence '{}' detected: {} steps completed "
                "within configured window", match.pattern_name, match.matched_events.size()),
            "", "behavioral_sequences." + match.pattern_name,
            "This sequence of operations matches a known dangerous pattern.\n"
            "Review the action chain and ensure operations are legitimate.",
            "", ""));
}

std::string GovernanceEngine::checkContextDrift(int handle_id, int turn,
                                                 const std::string& error) {
    if (!cdd_enabled_.load(std::memory_order_acquire)) return "";

    // Gather events from this turn
    std::vector<RuntimeEvent> turn_events = sequence_detector_.getEventsForTurn(turn);

    bool drifted = drift_analyzer_.recordTurn(handle_id, turn, turn_events, error);

    // --- Reality Checkpoint: composite pressure detection ---
    const auto& rccfg = rules_.context_drift.reality_checkpoint;
    if (!drifted && rccfg.enabled) {
        auto state = drift_analyzer_.getDriftState(handle_id);
        if (state) {
            // Factor 1: Coherence proximity (how close to threshold)
            double coherence_prox = 0.0;
            if (rules_.context_drift.coherence_threshold > 0.0) {
                coherence_prox = std::max(0.0, std::min(1.0,
                    1.0 - (state->coherence_score / rules_.context_drift.coherence_threshold)));
            }

            // Factor 2: Risk score proximity (snapshot under results_mutex_)
            double risk_prox = 0.0;
            if (rules_.scoring.enabled && rules_.scoring.yellow_threshold > 0) {
                int score_snapshot;
                {
                    std::lock_guard<std::mutex> lock(results_mutex_);
                    score_snapshot = cumulative_score_;
                }
                risk_prox = std::max(0.0, std::min(1.0,
                    static_cast<double>(score_snapshot) / rules_.scoring.yellow_threshold));
            }

            // Factor 3: Signal density (how many CDD signals fired this turn)
            double signal_dens = std::max(0.0, std::min(1.0,
                state->signals_fired_this_turn / 4.0));

            // Factor 4: Conversation depth
            double depth = std::max(0.0, std::min(1.0,
                static_cast<double>(turn) / rccfg.expected_conversation_depth));

            // Factor 5: BSD partial progress
            double bsd_progress = sequence_detector_.getMaxPartialProgress();

            // Factor 6: Inherited pipeline pressure (from prior stage)
            double inherited = state->inherited_pressure;

            // Factor 7: Coherence acceleration (opt-in, captures accelerating decay)
            double accel_factor = std::max(0.0, std::min(1.0,
                std::abs(state->coherence_acceleration) * 5.0));

            // Factor 8: Codegen pressure (ratio of blocked to total codegen calls)
            double codegen_pres = 0.0;
            {
                auto cs = stdlib::getCodegenStats();
                int total_cg = cs.total_calls + cs.total_blocked;
                if (total_cg > 0) {
                    codegen_pres = std::max(0.0, std::min(1.0,
                        static_cast<double>(cs.total_blocked) / total_cg));
                }
            }

            // Factor 9: BSD eviction pressure (buffer overflow = high activity volume)
            double eviction_pres = 0.0;
            {
                size_t evicted = sequence_detector_.totalEventsEvicted();
                size_t total_ev = sequence_detector_.totalEventsProcessed();
                if (total_ev > 0) {
                    eviction_pres = std::max(0.0, std::min(1.0,
                        static_cast<double>(evicted) / total_ev));
                }
            }

            // Weighted composite
            double composite =
                rccfg.weights.coherence_proximity * coherence_prox +
                rccfg.weights.risk_score_proximity * risk_prox +
                rccfg.weights.signal_density * signal_dens +
                rccfg.weights.conversation_depth * depth +
                rccfg.weights.bsd_partial_progress * bsd_progress +
                rccfg.weights.pipeline_inherited * inherited +
                rccfg.weights.coherence_acceleration * accel_factor +
                rccfg.weights.codegen_pressure * codegen_pres +
                rccfg.weights.bsd_eviction_pressure * eviction_pres;

            // Track sustained pressure
            int consecutive = state->consecutive_high_pressure_turns;
            int last_cp_turn = state->last_checkpoint_turn;

            if (composite >= rccfg.pressure_threshold) {
                consecutive++;
            } else {
                consecutive = std::max(0, consecutive - 1);  // decay, not instant reset
            }

            // Check trigger condition: sustained + cooldown
            if (consecutive >= rccfg.sustained_turns_required &&
                (turn - last_cp_turn) >= rccfg.min_turns_between_checkpoints) {

                // Reset and persist state
                drift_analyzer_.updateCheckpointState(handle_id, composite, 0, turn);

                clearTrace();
                addTrace(fmt::format("reality_checkpoint: pressure={:.2f} (threshold={:.2f})",
                    composite, rccfg.pressure_threshold));
                addTrace(fmt::format("  coherence_proximity={:.2f} risk_proximity={:.2f} "
                    "signal_density={:.2f} depth={:.2f} bsd_progress={:.2f} inherited={:.2f} "
                    "codegen={:.2f} eviction={:.2f}",
                    coherence_prox, risk_prox, signal_dens, depth, bsd_progress, inherited,
                    codegen_pres, eviction_pres));
                addTrace(fmt::format("  sustained {} turns", consecutive));

                // Build generic pressure summary (no factor breakdown — avoid teaching
                // agents which signals to game below threshold)
                int active_factors = (coherence_prox > 0.3 ? 1 : 0) + (risk_prox > 0.3 ? 1 : 0)
                    + (signal_dens > 0.3 ? 1 : 0) + (depth > 0.3 ? 1 : 0)
                    + (bsd_progress > 0.3 ? 1 : 0) + (inherited > 0.3 ? 1 : 0)
                    + (codegen_pres > 0.3 ? 1 : 0) + (eviction_pres > 0.3 ? 1 : 0);

                return enforce("context_drift.reality_checkpoint", rccfg.level,
                    formatError(rccfg.level,
                        fmt::format("Reality checkpoint — sustained operational pressure\n\n"
                            "  Pressure score: {:.2f} (sustained {} turns, {} contributing factors)\n",
                            composite, consecutive, active_factors),
                        "", "context_drift.reality_checkpoint",
                        "Multiple governance signals are contributing to elevated pressure.\n"
                        "The agent may be losing alignment with the task.\n"
                        "Consider reviewing recent actions and providing clearer direction.",
                        "", ""));
            } else {
                // Update pressure tracking without firing
                drift_analyzer_.updateCheckpointState(handle_id, composite, consecutive, -1);
            }

            // Pulse: compute verdict unconditionally (not gated on circuit breaker)
            PulseVerdict prev_pv = getPulseVerdict();
            PulseVerdict pv = computePulseVerdict(turn);

            // Emit BSD events for pulse state transitions
            if (pv != prev_pv) {
                if (pv == PulseVerdict::DEGRADED)
                    emitEvent(RuntimeEventType::PULSE_DEGRADED, "governance pulse degraded", "", 0);
                else if (pv == PulseVerdict::IMPAIRED)
                    emitEvent(RuntimeEventType::PULSE_IMPAIRED, "governance pulse impaired", "", 0);
            }

            // F6: Update system-wide governance level from sustained pressure
            const auto& cb = rules_.circuit_breaker;
            if (cb.enabled) {
                int level = 0;
                if (composite >= cb.critical_threshold && consecutive >= cb.critical_sustained) level = 3;
                else if (composite >= cb.high_threshold && consecutive >= cb.high_sustained) level = 2;
                else if (composite >= cb.elevated_threshold && consecutive >= cb.elevated_sustained) level = 1;

                // Pulse escalation: raises per-call floor (never below pressure-computed level)
                if (pv == PulseVerdict::IMPAIRED && level < 2) level = 2;
                else if (pv == PulseVerdict::DEGRADED && level < 1) level = 1;

                // Evidence Epoch: governance level change invalidates prior evidence
                int prev_level = governance_level_.load(std::memory_order_relaxed);
                if (level != prev_level) {
                    governance_epoch_++;
                }
                governance_level_.store(level, std::memory_order_relaxed);
            }
        }
    }

    if (!drifted) return "";

    auto state = drift_analyzer_.getDriftState(handle_id);
    if (!state) return "";

    clearTrace();
    addTrace(fmt::format("context_drift: coherence={:.2f} vel={:.3f} accel={:.3f} (threshold={:.2f})",
        state->coherence_score, state->coherence_velocity, state->coherence_acceleration,
        rules_.context_drift.coherence_threshold));
    if (state->circular_action_count > 0)
        addTrace(fmt::format("  circular_actions: {}", state->circular_action_count));
    if (state->repeated_failures > 0)
        addTrace(fmt::format("  repeated_failures: {}", state->repeated_failures));
    if (state->scope_creep_count > 0)
        addTrace(fmt::format("  scope_creep: {}", state->scope_creep_count));
    if (state->vocabulary_contraction_count > 0)
        addTrace(fmt::format("  vocabulary_contraction: {}", state->vocabulary_contraction_count));

    // F8: Consume risk budget on CDD signal fire (cost 1)
    // Reduced from 2 to prevent instant budget exhaust when BSD+CDD fire on same turn
    {
        std::lock_guard<std::mutex> lock(agent_config_mutex_);
        if (!current_agent_config_.empty()) consumeRiskBudget(current_agent_config_, 1);
    }

    return enforce("context_drift.coherence_loss", rules_.context_drift.level,
        formatError(rules_.context_drift.level,
            fmt::format("Agent context drift detected: coherence score {:.2f} "
                "below threshold {:.2f} (circular={}, failures={}, scope_creep={}, vocab_contraction={})",
                state->coherence_score, rules_.context_drift.coherence_threshold,
                state->circular_action_count, state->repeated_failures,
                state->scope_creep_count, state->vocabulary_contraction_count),
            "", "context_drift.coherence_loss",
            "The agent appears to be looping or losing context.\n"
            "Consider resetting the conversation or providing clearer instructions.",
            "", ""));
}

std::optional<governance::DriftState> GovernanceEngine::getDriftState(int handle_id) const {
    return drift_analyzer_.getDriftState(handle_id);
}

GovernanceEngine::CheckpointData GovernanceEngine::getCheckpointData(
    int handle_id, int turn) const {
    CheckpointData data;
    if (!rules_.context_drift.reality_checkpoint.enabled) return data;
    auto state = drift_analyzer_.getDriftState(handle_id);
    if (!state) return data;
    data.pressure = state->last_pressure_score;
    data.sustained_turns = state->consecutive_high_pressure_turns;
    data.fired = (state->last_checkpoint_turn == turn);
    return data;
}

std::string GovernanceEngine::checkPreExecution(
    RuntimeEventType type, const std::string& detail,
    const std::string& file, int line) {
    if (!bsd_enabled_.load(std::memory_order_acquire)) return "";

    RuntimeEvent ev;
    ev.type = type;
    ev.detail = detail;
    ev.file = file;
    ev.line = line;
    ev.turn = current_agent_turn_.load(std::memory_order_relaxed);
    ev.agent_handle = current_agent_handle_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(agent_config_mutex_);
        ev.agent_config = current_agent_config_;
    }
    ev.timestamp = std::chrono::steady_clock::now();

    auto match = sequence_detector_.wouldMatch(ev);
    if (match.pattern_name.empty()) return "";

    // Consume the event: advance + reset the pattern state so that the block is
    // recorded in the sequence log and subsequent normal calls aren't re-blocked
    // by the same partial sequence left stuck at its final step.
    sequence_detector_.recordEvent(ev);

    clearTrace();
    addTrace(fmt::format("BSD pre-check: '{}' would complete sequence '{}'",
        detail, match.pattern_name));

    return enforce("behavioral_sequences." + match.pattern_name, match.pattern->level,
        formatError(match.pattern->level,
            fmt::format("Behavioral sequence '{}' blocked before execution: '{}' would "
                "complete a dangerous {}-step pattern",
                match.pattern_name, detail, match.matched_events.size() + 1),
            "", "behavioral_sequences." + match.pattern_name,
            "This action would complete a known dangerous sequence.\n"
            "The operation was blocked before execution.",
            "", ""));
}

} // namespace governance
} // namespace naab
