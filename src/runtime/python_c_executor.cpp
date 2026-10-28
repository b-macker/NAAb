/**
 * C++ Adapter for Python C API Wrapper - Implementation
 *
 * Uses python_c_gil_acquire/release for safe GIL management.
 * On worker threads, this uses pre-created PyThreadState + PyEval_RestoreThread
 * instead of PyGILState_Ensure (which crashes on Android with bionic CFI).
 */

#include "naab/python_c_executor.h"
#include "naab/interpreter.h"
#include "naab/sandbox.h"
#include "naab/subprocess_helpers.h"  // V-SC-006-ext: env scrub policy
#include "naab/governance.h"         // Import blocking: blocked imports from govern.json
#include <stdexcept>
#include <sstream>
#include <string>
#include <algorithm>
#include <cctype>   // std::toupper (blocked_read is matched case-insensitively)
#include <cstdlib>
#include <optional>  // audit-hook fallback sandbox for Python-spawned threads
#if defined(_WIN32)
#  define NAAB_PY_ENVIRON _environ
#elif defined(__APPLE__)
#  include <crt_externs.h>
#  define NAAB_PY_ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#  define NAAB_PY_ENVIRON environ
#endif

#ifndef _WIN32
#  include <sys/resource.h>  // For setrlimit/getrlimit (RLIMIT_NPROC containment)
#endif
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "naab/host_match.h"   // A35: the sandbox's host matcher, for the socket policy
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

namespace naab {
namespace runtime {

// NOTE: Don't use 'using namespace interpreter' - causes type conflicts
// Use fully qualified names instead

// --- Sandbox audit policy --------------------------------------------------
// The hook installed in python_c_wrapper.c calls this at the moment of a
// sensitive operation. It asks the sandbox exactly the questions every other
// I/O path in the tree already asks -- file_impl asks canRead/canWrite,
// http_impl asks canConnect, process_impl asks canExecuteCommand -- so a
// polyglot block becomes subject to the same declared policy as file.read
// instead of being governed only by pre-execution source text.
//
// It deliberately mirrors the existing idiom, including its no-sandbox case:
// every call site in the codebase is written `if (sandbox && !sandbox->canX())`,
// i.e. no active sandbox means no policy to enforce. This adds no new
// restriction and no new config key; it only lets policy the operator already
// declared reach the one path that evaded it.
extern "C" int naabPythonAuditPolicy(const char* event, const char* target, int is_write) {
    // The hook is installed process-wide (PySys_AddAuditHook) and fires on every
    // thread that runs Python, but ScopedSandbox and GovernanceEngine are both
    // thread-local. A threading.Thread a <<python>> block spawns runs on a fresh
    // OS thread where neither was installed, so this policy saw sandbox==nullptr
    // / gov==nullptr and failed OPEN -- a spawned thread read blocked_paths files
    // and ran os.system under shell.enabled:false while the block's own thread
    // was correctly refused (measured). Fall back to the RUN-level policy, which
    // outlives any one block and is the same objects the owning thread holds:
    // the sandbox config the run established (SandboxManager default -- exactly
    // what vm.cpp builds every polyglot ScopedSandbox from, so the fallback can
    // only match, never over-restrict), and the process-wide engine. When NO
    // policy was established (an ungoverned run: SandboxManager unconfigured,
    // no engine ever current) both stay absent and behaviour is unchanged.
    auto* sandbox = security::ScopedSandbox::getCurrent();
    std::optional<security::Sandbox> fallback_sandbox;
    if (!sandbox && security::SandboxManager::instance().isConfigured()) {
        fallback_sandbox.emplace(security::SandboxManager::instance().getDefaultConfig());
        sandbox = &*fallback_sandbox;
    }
    const std::string ev = event ? event : "";
    const std::string tgt = target ? target : "";

    if (ev == "open") {
        if (tgt.empty()) return 1;            // bare fd: no path to adjudicate
        if (sandbox && !(is_write ? sandbox->canWrite(tgt) : sandbox->canRead(tgt))) return 0;
        // The project path policy (capabilities.filesystem.allowed_paths /
        // blocked_paths, the auto-protected govern.json/.sig/trusted keys, and
        // the agent-role overlay). The sandbox above is an allowlist with no
        // notion of blocked_paths, so asking only it let a <<python>> block read
        // -- and WRITE -- files NAAb's own file.* is HARD-blocked on (measured:
        // a blocked path read back under Governance: PASS at sandbox standard).
        // This is the same decision file.read/file.write get, from the same
        // function, so the two cannot disagree. pathPolicyDenial() never
        // throws: a C++ exception must not cross CPython's frames, so a denial
        // is returned and surfaces as PermissionError, like a sandbox denial.
        // Interpreter self-loads never get here (runtime-path carve-out in the
        // C hook), so allowed_paths does not break imports.
        auto* gov = governance::GovernanceEngine::getCurrent();
        if (!gov) gov = governance::GovernanceEngine::getProcessCurrent();
        if (gov && gov->isActive() && !gov->pathPolicyDenial(tgt, is_write != 0).empty()) return 0;
        return 1;
    }

    // The filesystem-mutation family that does not go through open(): rename
    // (os.replace and same-filesystem shutil.move audit here too), link,
    // symlink -- which create or overwrite a destination -- and remove
    // (os.unlink audits here), rmdir, truncate -- which destroy or empty an
    // existing path. The open branch above never saw any of them, so a
    // <<python>> block could replace a blocked file with os.rename / os.link /
    // shutil.move, or DELETE govern.json / a guidance source with os.remove,
    // under Governance: PASS -- while NAAb's own file.delete is path-checked.
    // The wrapper forwards each write-intent path here one at a time; judge it
    // exactly as a write open: the sandbox allowlist, then the project path
    // policy (blocked_paths, the auto-protected govern.json/.sig and trusted
    // keys, guidance sources, via pathPolicyDenial). os.mkdir is deliberately
    // NOT forwarded: it fires on makedirs(exist_ok) and __pycache__ creation,
    // and path creation is already covered by rename/link/open.
    if (ev == "os.rename" || ev == "os.link" || ev == "os.symlink" ||
        ev == "os.remove" || ev == "os.rmdir" || ev == "os.truncate") {
        if (tgt.empty()) return 1;            // no path to adjudicate
        if (sandbox && !sandbox->canWrite(tgt)) return 0;
        auto* gov = governance::GovernanceEngine::getCurrent();
        if (!gov) gov = governance::GovernanceEngine::getProcessCurrent();
        if (gov && gov->isActive() && !gov->pathPolicyDenial(tgt, /*write=*/true).empty()) return 0;
        return 1;
    }

    if (!sandbox) return 1;   // no policy active -- same as every other call site

    if (ev == "os.system" || ev == "subprocess.Popen" ||
        ev == "os.posix_spawn" || ev.rfind("os.exec", 0) == 0) {
        // An empty target still represents an exec attempt; judge the
        // capability rather than letting an unnamed command through.
        return sandbox->canExecuteCommand(tgt.empty() ? std::string("python:exec") : tgt) ? 1 : 0;
    }

    if (ev == "socket.connect") {
        return sandbox->getConfig().hasCapability(security::Capability::NET_CONNECT) ? 1 : 0;
    }

    return 1;
}

// --- Network policy for the socket audit events (A35) ----------------------
// The hook answered socket.connect from NET_CONNECT alone, so
// capabilities.network.allowed_hosts / blocked_hosts -- which http.* meets
// through Sandbox::canConnect -- did not reach a <<python>> block, and
// sendto/sendmsg (UDP) were not judged at all: measured, a datagram left with
// network.enabled false through an import the text check cannot see, and a
// TCP connect to an address no allowed_hosts entry admits was made.
//
// The lists hold names; a connect carries an ADDRESS. The owner's decision
// (2026-10-26, "admit allowed lookups"): judge the NAME where the program
// gives one -- at the lookup (getaddrinfo, gethostbyname[_ex]) and at a
// connect given a name -- by the sandbox's own canConnect(). When an allowed
// name is looked up under an allowed_hosts list, resolve it HERE too and admit
// those addresses; a connect to an address no IP entry admits and no allowed
// lookup produced is refused. A resolver that answers the program differently
// from the engine (round-robin DNS) is refused too: fail closed.
namespace {

std::mutex g_admitted_mutex;
// canonical address -> the allowed name whose lookup admitted it
std::unordered_map<std::string, std::string> g_admitted;
constexpr size_t kMaxAdmitted = 4096;   // past this, nothing more is admitted

// One part of an IPv4 address as inet_aton reads it: decimal, 0-prefixed
// octal, or 0x hex. False on anything else.
bool parseInetAtonPart(const std::string& p, unsigned long long& v) {
    if (p.empty()) return false;
    int base = 10;
    size_t i = 0;
    if (p.size() > 1 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; i = 2; if (i == p.size()) return false; }
    else if (p.size() > 1 && p[0] == '0') { base = 8; i = 1; }
    v = 0;
    for (; i < p.size(); ++i) {
        int d;
        char c = p[i];
        if (c >= '0' && c <= '9') d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        if (d >= base) return false;
        v = v * static_cast<unsigned long long>(base) + static_cast<unsigned long long>(d);
        if (v > 0xFFFFFFFFull) return false;
    }
    return true;
}

// The forms an IP literal is judged by: its canonical text, and for an
// IPv4-mapped IPv6 address the IPv4 address too. Empty for a name. CPython
// hands a host inet_pton rejects to getaddrinfo, which reads inet_aton's
// legacy IPv4 forms (2130706434, 0x7f000002 and 127.2 all connect to
// 127.0.0.2, observed), so those are read the same way here; "" is
// INADDR_ANY and "<broadcast>" the broadcast address, as CPython reads them.
std::vector<std::string> ipLiteralForms(const std::string& host) {
    if (host.empty()) return {"0.0.0.0"};
    if (host == "<broadcast>") return {"255.255.255.255"};
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t dot = host.find('.', start);
        parts.push_back(host.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    if (parts.size() <= 4) {
        unsigned long long v[4] = {0, 0, 0, 0};
        bool ok = true;
        for (size_t i = 0; i < parts.size() && ok; ++i) ok = parseInetAtonPart(parts[i], v[i]);
        if (ok) {
            const size_t n = parts.size();
            unsigned long long last_max = n == 1 ? 0xFFFFFFFFull : n == 2 ? 0xFFFFFFull : n == 3 ? 0xFFFFull : 0xFFull;
            for (size_t i = 0; i + 1 < n && ok; ++i) ok = v[i] <= 0xFF;
            if (ok && v[n - 1] <= last_max) {
                unsigned long long a = v[n - 1];
                for (size_t i = 0; i + 1 < n; ++i) a |= v[i] << (8 * (3 - i));
                return {std::to_string((a >> 24) & 0xFF) + "." + std::to_string((a >> 16) & 0xFF) + "." +
                        std::to_string((a >> 8) & 0xFF) + "." + std::to_string(a & 0xFF)};
            }
        }
    }
    in6_addr a6;
    if (inet_pton(AF_INET6, host.c_str(), &a6) == 1) {
        char buf[INET6_ADDRSTRLEN] = {0};
        std::vector<std::string> forms;
        if (inet_ntop(AF_INET6, &a6, buf, sizeof buf)) forms.push_back(buf);
        static const unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        const unsigned char* b = reinterpret_cast<const unsigned char*>(&a6);
        if (std::memcmp(b, kMapped, 12) == 0)
            forms.push_back(std::to_string(b[12]) + "." + std::to_string(b[13]) + "." +
                            std::to_string(b[14]) + "." + std::to_string(b[15]));
        if (!forms.empty()) return forms;
    }
    return {};
}

// Resolve an allowed name the way the program's lookup is about to, and admit
// each address it gives.
void admitResolvedAddresses(const std::string& name) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    addrinfo* res = nullptr;
    if (getaddrinfo(name.c_str(), nullptr, &hints, &res) != 0 || !res) return;
    std::lock_guard<std::mutex> lock(g_admitted_mutex);
    for (addrinfo* r = res; r; r = r->ai_next) {
        char buf[INET6_ADDRSTRLEN] = {0};
        const void* src = nullptr;
        if (r->ai_family == AF_INET) src = &reinterpret_cast<sockaddr_in*>(r->ai_addr)->sin_addr;
        else if (r->ai_family == AF_INET6) src = &reinterpret_cast<sockaddr_in6*>(r->ai_addr)->sin6_addr;
        if (!src || !inet_ntop(r->ai_family, src, buf, sizeof buf)) continue;
        for (const auto& f : ipLiteralForms(buf)) {
            if (g_admitted.size() >= kMaxAdmitted && !g_admitted.count(f)) continue;
            g_admitted[f] = name;
        }
    }
    freeaddrinfo(res);
}

}  // namespace

extern "C" int naabPythonNetPolicy(const char* event, int kind, int address,
                                   const char* host_c, int port) {
    (void)event;
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (!sandbox) return 1;   // no policy active -- as naabPythonAuditPolicy
    const auto& cfg = sandbox->getConfig();
    // The network switch is the one this hook always used, the NET_CONNECT
    // capability; governance's network.enabled false removes it. The CLI's own
    // network default (network_enabled, --allow-network) is not consulted, as
    // before, so a run with no govern.json behaves as it did.
    const bool net_on = cfg.hasCapability(security::Capability::NET_CONNECT);
    const bool allowlist = !cfg.allowed_hosts.empty();
    const std::string host = host_c ? host_c : "";
    const std::vector<std::string> ip_forms =
        address == NAAB_NET_HOST ? ipLiteralForms(host) : std::vector<std::string>{};
    const bool is_ip = !ip_forms.empty();
    auto blocked = [&]() {
        if (naab::net::hostMatchesAny(host, cfg.blocked_hosts)) return true;
        for (const auto& f : ip_forms) if (naab::net::hostMatchesAny(f, cfg.blocked_hosts)) return true;
        return false;
    };
    // An address an IP entry admits, or one an allowed lookup produced and whose
    // name the lists still allow at this port.
    auto admitted_ip = [&]() {
        for (const auto& f : ip_forms) if (sandbox->hostPolicyAllows(f, port)) return true;
        std::lock_guard<std::mutex> lock(g_admitted_mutex);
        for (const auto& f : ip_forms) {
            auto it = g_admitted.find(f);
            if (it != g_admitted.end() && sandbox->hostPolicyAllows(it->second, port)) return true;
        }
        return false;
    };

    if (kind == NAAB_NET_LOOKUP) {
        if (address == NAAB_NET_NONE) return 1;          // getaddrinfo(None, ...): local, no query
        if (address == NAAB_NET_HOST && is_ip) return 1; // a literal: no query, the connect is judged
        if (!net_on) return 0;                           // a name lookup is a network query
        if (address != NAAB_NET_HOST) return allowlist ? 0 : 1;
        if (blocked()) return 0;
        if (!allowlist) return 1;
        if (!naab::net::hostMatchesAny(host, cfg.allowed_hosts)) return 0;
        admitResolvedAddresses(host);
        return 1;
    }
    if (kind == NAAB_NET_REVERSE) {
        if (address == NAAB_NET_NONE) return 1;
        if (!net_on) return 0;
        if (address != NAAB_NET_HOST) return allowlist ? 0 : 1;
        if (blocked()) return 0;
        if (!allowlist) return 1;
        if (naab::net::hostMatchesAny(host, cfg.allowed_hosts)) return 1;
        return is_ip && admitted_ip() ? 1 : 0;
    }
    // A send: connect, sendto, sendmsg.
    if (!net_on) return 0;
    if (address == NAAB_NET_NONE || address == NAAB_NET_NOT_INET) return 1;  // connected socket / AF_UNIX
    if (address == NAAB_NET_BAD_HOST) return allowlist ? 0 : 1;
    if (!is_ip) return sandbox->hostPolicyAllows(host, port) ? 1 : 0;   // a name, as http.* judges it
    if (blocked()) return 0;
    return admitted_ip() ? 1 : 0;
}

/**
 * Execute Python code (statement mode)
 */
void PythonCExecutor::execute(const std::string& code) {
    PythonCResult result = python_c_execute(code.c_str());

    if (!result.success) {
        std::string error_msg = result.error_message ? result.error_message : "Unknown error";
        python_c_free_result(&result);
        throw std::runtime_error("Python execution error: " + error_msg);
    }

    python_c_free_result(&result);
}

/**
 * Helper: trim trailing whitespace from a string
 */
static std::string trimRight(const std::string& s) {
    size_t end = s.find_last_not_of(" \t\r\n");
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

/**
 * Helper: split code into "all lines except last expression" and "last expression"
 * Returns true if code was successfully split, false if it's a single expression
 */
static bool splitStatementsAndLastExpr(const std::string& code,
                                        std::string& statements_out,
                                        std::string& last_expr_out) {
    // Split code into lines
    std::vector<std::string> lines;
    std::istringstream stream(code);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    // Remove trailing empty lines
    while (!lines.empty() && trimRight(lines.back()).empty()) {
        lines.pop_back();
    }

    if (lines.empty()) return false;
    if (lines.size() == 1) return false;  // Single line = single expression

    // Last non-empty line is the expression to return
    last_expr_out = trimRight(lines.back());

    // If the last line has leading whitespace, it's inside a block (for/if/while/etc.)
    // and can't be executed standalone — don't split
    if (!last_expr_out.empty() && (last_expr_out[0] == ' ' || last_expr_out[0] == '\t')) {
        return false;
    }

    // Everything before is statements to exec
    statements_out.clear();
    for (size_t i = 0; i + 1 < lines.size(); i++) {
        statements_out += lines[i] + "\n";
    }

    return true;
}

/**
 * Execute Python expression and return result
 *
 * Handles both single expressions (e.g., "10 + 20") and multi-line code
 * (e.g., "x = 10\ny = 20\nx + y") by:
 * 1. First trying Py_eval_input (single expression mode)
 * 2. If SyntaxError, splitting into statements + last expression:
 *    - Execute all statements with Py_file_input (exec mode)
 *    - Evaluate last line with Py_eval_input (eval mode) to get return value
 *
 * Uses python_c_gil_acquire/release for safe GIL management.
 * On worker threads: uses pre-created PyThreadState + PyEval_RestoreThread
 * On main thread: falls back to PyGILState_Ensure
 */
interpreter::NaabVal PythonCExecutor::executeWithReturn(const std::string& code) {
    // Sandbox gate: Python polyglot blocks require BLOCK_CALL capability
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->getConfig().hasCapability(
            security::Capability::BLOCK_CALL)) {
        sandbox->logViolation("executePython", code.substr(0, 100),
            "BLOCK_CALL capability required");
        throw std::runtime_error(
            "Security: Python execution denied by sandbox\n\n"
            "  Python polyglot blocks require BLOCK_CALL capability.\n"
        );
    }
    // Acquire GIL safely (pre-created state on workers, PyGILState on main)
    int gil_handle = python_c_gil_acquire();

    // Register this thread as running Python so a timeout can interrupt it,
    // including on a worker thread (see python_c_interrupt). leave() needs no
    // GIL, so the guard may unwind after the explicit releases below.
    python_c_running_enter();
    struct RunningGuard { ~RunningGuard() { python_c_running_leave(); } } running_guard;

    // Get __main__ module and globals
    PyObject* main_module = PyImport_AddModule("__main__");
    if (!main_module) {
        python_c_gil_release(gil_handle);
        throw std::runtime_error("Python execution error: Failed to get __main__ module");
    }

    PyObject* globals = PyModule_GetDict(main_module);
    if (!globals) {
        python_c_gil_release(gil_handle);
        throw std::runtime_error("Python execution error: Failed to get globals dict");
    }

    // Redirect sys.stdout to capture print() output
    PyRun_SimpleString(
        "import sys as _naab_sys, io as _naab_io\n"
        "_naab_stdout = _naab_io.StringIO()\n"
        "_naab_old_stdout = _naab_sys.stdout\n"
        "_naab_sys.stdout = _naab_stdout\n"
    );

    // V-SC-006-ext: Scrub env vars from embedded Python's os.environ
    // Always scrub NAAb internal secrets; additionally apply policy-based scrubbing
    bool env_scrub_applied = true;
    {
        const runtime::EnvScrubPolicy policy = runtime::currentEnvScrubPolicy();
        // Names come from govern.json and go into Python source as string
        // literals, so they are escaped rather than pasted in.
        auto pyStr = [](const std::string& v) {
            std::string out = "'";
            for (char ch : v) {
                if (ch == '\\' || ch == '\'') out += '\\';
                if (ch == '\n' || ch == '\r') continue;
                out += ch;
            }
            return out + "'";
        };
        std::ostringstream scrub_code;
        scrub_code << "import os as _naab_os\n"
                   << "_naab_saved_env = {}\n"
                   // Always scrub NAAb internal secrets
                   << "for _k in ['NAAB_GOVERN_KEY','NAAB_LOCK_KEY','NAAB_SIGNING_KEY']:\n"
                   << "    if _k in _naab_os.environ:\n"
                   << "        _naab_saved_env[_k] = _naab_os.environ.pop(_k)\n";

        // blocked_read (enforce mode): the block may not read what env.get()
        // may not, matched case-insensitively as checkEnvVarRead() does.
        if (!policy.read_blocked_vars.empty()) {
            for (char** e = NAAB_PY_ENVIRON; e && *e != nullptr; ++e) {
                std::string entry(*e);
                auto eq = entry.find('=');
                if (eq == std::string::npos) continue;
                std::string key = entry.substr(0, eq);
                if (runtime::isReadBlockedEnvVar(policy, key)) runtime::noteWithheldEnvVar(key);
            }
            scrub_code << "_naab_read_blocked = set([";
            for (const auto& v : policy.read_blocked_vars) {
                std::string upper = v;
                for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                scrub_code << pyStr(upper) << ",";
            }
            scrub_code << "])\n"
                       << "for _k in list(_naab_os.environ.keys()):\n"
                       << "    if _k.upper() in _naab_read_blocked and _k not in _naab_saved_env:\n"
                       << "        _naab_saved_env[_k] = _naab_os.environ.pop(_k)\n"
                       << "del _naab_read_blocked\n";
        }

        if (policy.active) {
            if (policy.mode == runtime::EnvScrubMode::ALLOWLIST) {
                // Allowlist: remove everything except allowed + essential vars
                scrub_code << "_naab_allowed = set([";
                for (const char* v : {"PATH", "HOME", "LANG", "TERM", "TMPDIR",
                        "TMP", "TEMP", "USER", "LOGNAME", "SHELL", "LC_ALL",
                        "LC_CTYPE", "XDG_RUNTIME_DIR", "ANDROID_ROOT",
                        "ANDROID_DATA", "PREFIX", "LD_LIBRARY_PATH"}) {
                    scrub_code << "'" << v << "',";
                }
                for (const auto& v : policy.allowed_vars) {
                    scrub_code << pyStr(v) << ",";
                }
                scrub_code << "])\n"
                           << "for _k in list(_naab_os.environ.keys()):\n"
                           << "    if _k not in _naab_allowed:\n"
                           << "        _naab_saved_env[_k] = _naab_os.environ.pop(_k)\n"
                           << "del _naab_allowed\n";
            } else {
                // Blocklist: remove specific vars and prefixes
                scrub_code << "_naab_blocked = set([";
                for (const auto& v : policy.blocked_vars) {
                    scrub_code << pyStr(v) << ",";
                }
                scrub_code << "])\n"
                           << "_naab_prefixes = [";
                for (const auto& p : policy.blocked_prefixes) {
                    scrub_code << pyStr(p) << ",";
                }
                scrub_code << "]\n"
                           << "for _k in list(_naab_os.environ.keys()):\n"
                           << "    if _k in _naab_blocked or any(_k.startswith(_p) for _p in _naab_prefixes):\n"
                           << "        _naab_saved_env[_k] = _naab_os.environ.pop(_k)\n"
                           << "del _naab_blocked, _naab_prefixes\n";
            }
        }
        scrub_code << "del _naab_os\n";
        PyRun_SimpleString(scrub_code.str().c_str());
    }

    // A32: the scrub pops the withheld values into `_naab_saved_env`, a dict in
    // the SAME namespace the block body runs in, so a block could read the
    // values back out of it under a lease with no capabilities at all. Hold the
    // dict's only reference C-side and remove the name from the namespace while
    // user code runs; reveal() puts it back before the GIL is released.
    //
    // reveal() calls the Python API, so it must run WITH the GIL held -- and
    // every exit below releases the GIL explicitly (python_c_gil_release)
    // before it returns or throws, so a destructor would run too late. reveal()
    // is therefore called explicitly on every exit: captureAndRestoreStdout()
    // (the success returns, before the environ restore reads the dict),
    // restoreStdoutOnly() (every error throw) and the pyObjectToValue catch.
    // The destructor deliberately does NOT touch Python: a path that missed
    // reveal() leaks one reference rather than calling the API without the GIL
    // (undefined behaviour, a crash on worker threads). Error paths re-insert
    // the dict without restoring environ, exactly as they behaved before.
    // gc.get_objects() can still reach the dict object; that deeper
    // introspection route is noted in docs/open-investigations.md A32.
    struct SavedEnvHider {
        PyObject* globals = nullptr;
        PyObject* ref = nullptr;
        void hide(PyObject* g) {
            globals = g;
            PyObject* se = PyDict_GetItemString(g, "_naab_saved_env");  // borrowed
            if (se) {
                Py_INCREF(se);
                ref = se;
                PyDict_DelItemString(g, "_naab_saved_env");
            }
        }
        // GIL must be held.
        void reveal() {
            if (ref) {
                PyDict_SetItemString(globals, "_naab_saved_env", ref);
                Py_DECREF(ref);
                ref = nullptr;
            }
        }
    } saved_env_hider;
    if (env_scrub_applied) saved_env_hider.hide(globals);

    // OS-level containment: set RLIMIT_NPROC soft limit to 0 to prevent Python
    // from forking (subprocess.run, os.system, os.fork all fail with EAGAIN).
    // Only the soft limit is lowered — the hard limit is preserved so the RAII
    // guard can restore the original value.  Lowering the hard limit to 0 is
    // irreversible for non-root processes (setrlimit returns EPERM), which would
    // permanently break any code that needs to create threads (e.g. libcurl's
    // threaded DNS resolver).
#ifndef _WIN32
    struct NprocGuard {
        struct rlimit saved = {0, 0};
        bool active = false;
        ~NprocGuard() { if (active) setrlimit(RLIMIT_NPROC, &saved); }
    } nproc_guard;
    {
        auto containment = SubprocessContainment::fromCurrentSandbox("python3");
        if (containment.block_fork) {
            getrlimit(RLIMIT_NPROC, &nproc_guard.saved);
            struct rlimit zero = {0, nproc_guard.saved.rlim_max};
            setrlimit(RLIMIT_NPROC, &zero);
            nproc_guard.active = true;
        }
    }
#endif

    // Import blocking: override __import__ to enforce govern.json blocked imports
    // Catches __import__("os"), importlib.import_module("os"), and all runtime import paths
    {
        auto* engine = governance::GovernanceEngine::getCurrent();
        if (engine) {
            const auto* lang_cfg = engine->getLanguageConfig("python");
            if (lang_cfg && !lang_cfg->imports.blocked.empty()) {
                std::ostringstream hook;
                hook << "import builtins as _naab_builtins\n"
                     << "if not hasattr(_naab_builtins, '_naab_original_import'):\n"
                     << "    _naab_builtins._naab_original_import = _naab_builtins.__import__\n"
                     << "_naab_original_import = _naab_builtins._naab_original_import\n"
                     << "_naab_blocked_modules = {";
                for (size_t i = 0; i < lang_cfg->imports.blocked.size(); ++i) {
                    if (i > 0) hook << ",";
                    hook << "'" << lang_cfg->imports.blocked[i] << "'";
                }
                hook << "}\n"
                     << "def _naab_safe_import(name, *args, **kwargs):\n"
                     << "    _top = name.split('.')[0]\n"
                     << "    if _top in _naab_blocked_modules:\n"
                     << "        raise ImportError('Import blocked by governance policy: ' + name)\n"
                     << "    return _naab_original_import(name, *args, **kwargs)\n"
                     << "_naab_builtins.__import__ = _naab_safe_import\n"
                     // V-PY-001: Block importlib bypass and hide original import from user scope
                     << "try:\n"
                     << "    import importlib as _naab_importlib\n"
                     << "    _naab_orig_import_module = _naab_importlib.import_module\n"
                     << "    def _naab_safe_import_module(name, package=None):\n"
                     << "        _top = name.split('.')[0]\n"
                     << "        if _top in _naab_blocked_modules:\n"
                     << "            raise ImportError('Import blocked by governance policy: ' + name)\n"
                     << "        return _naab_orig_import_module(name, package)\n"
                     << "    _naab_importlib.import_module = _naab_safe_import_module\n"
                     << "    del _naab_importlib, _naab_orig_import_module\n"
                     << "except Exception:\n"
                     << "    pass\n"
                     << "del _naab_builtins\n";
                PyRun_SimpleString(hook.str().c_str());
            }
        }
    }

    // Helper lambda: restore stdout and get captured output
    auto captureAndRestoreStdout = [&globals, &saved_env_hider, env_scrub_applied]() -> std::string {
        // V-SC-006-ext: Restore scrubbed env vars
        if (env_scrub_applied) {
            // A32: put _naab_saved_env back in the namespace so the restore
            // below can read it; it was hidden from user code during execution.
            saved_env_hider.reveal();
            PyRun_SimpleString(
                "_naab_os = _naab_original_import('os') if '_naab_original_import' in dir() else __import__('os')\n"
                "_naab_os.environ.update(_naab_saved_env)\n"
                "del _naab_saved_env, _naab_os\n"
            );
        }
        PyRun_SimpleString("_naab_sys.stdout = _naab_old_stdout");
        std::string captured;
        PyObject* captured_obj = PyRun_String("_naab_stdout.getvalue()", Py_eval_input, globals, globals);
        if (captured_obj && PyUnicode_Check(captured_obj)) {
            const char* s = PyUnicode_AsUTF8(captured_obj);
            if (s) captured = s;
            // Trim trailing newline
            while (!captured.empty() && (captured.back() == '\n' || captured.back() == '\r')) {
                captured.pop_back();
            }
        }
        Py_XDECREF(captured_obj);
        PyRun_SimpleString("del _naab_stdout, _naab_old_stdout, _naab_io, _naab_sys");
        return captured;
    };

    // Helper lambda: restore stdout on error (no capture needed)
    auto restoreStdoutOnly = [&saved_env_hider]() {
        // A32: every error throw calls this before releasing the GIL, so put
        // the hidden save dict back here, while the GIL is still held.
        saved_env_hider.reveal();
        PyRun_SimpleString(
            "import sys as _naab_sys\n"
            "if hasattr(_naab_sys, '_naab_old_stdout') and _naab_sys._naab_old_stdout is not None:\n"
            "    _naab_sys.stdout = _naab_sys._naab_old_stdout\n"
            "for _n in ['_naab_stdout', '_naab_old_stdout', '_naab_io', '_naab_sys']:\n"
            "    try:\n"
            "        exec(f'del {_n}', globals())\n"
            "    except: pass\n"
        );
    };

    // Step 1: Try evaluating as a single expression (Py_eval_input)
    PyObject* py_result = PyRun_String(code.c_str(), Py_eval_input, globals, globals);

    if (!py_result) {
        // Check if it's a SyntaxError (meaning multi-line code, not a real error)
        PyObject *ptype, *pvalue, *ptraceback;
        PyErr_Fetch(&ptype, &pvalue, &ptraceback);

        bool is_syntax_error = ptype && PyErr_GivenExceptionMatches(ptype, PyExc_SyntaxError);

        Py_XDECREF(ptype);
        Py_XDECREF(pvalue);
        Py_XDECREF(ptraceback);

        if (!is_syntax_error) {
            // Not a SyntaxError - re-raise as runtime error
            // Re-evaluate to get the error message cleanly
            py_result = PyRun_String(code.c_str(), Py_eval_input, globals, globals);
            if (!py_result) {
                PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                std::string error_msg = "Unknown Python error";
                if (ptype && pvalue) {
                    PyObject* type_name = PyObject_GetAttrString(ptype, "__name__");
                    PyObject* str_exc = PyObject_Str(pvalue);
                    if (type_name && str_exc) {
                        error_msg = std::string(PyUnicode_AsUTF8(type_name)) + ": " + PyUnicode_AsUTF8(str_exc);
                    } else if (str_exc) {
                        const char* err = PyUnicode_AsUTF8(str_exc);
                        if (err) error_msg = err;
                    }
                    Py_XDECREF(type_name);
                    Py_XDECREF(str_exc);
                } else if (pvalue) {
                    PyObject* str_exc = PyObject_Str(pvalue);
                    if (str_exc) {
                        const char* err = PyUnicode_AsUTF8(str_exc);
                        if (err) error_msg = err;
                        Py_DECREF(str_exc);
                    }
                }
                Py_XDECREF(ptype);
                Py_XDECREF(pvalue);
                Py_XDECREF(ptraceback);
                restoreStdoutOnly();
                python_c_gil_release(gil_handle);
                throw std::runtime_error("Python execution error: " + error_msg);
            }
        } else {
            // SyntaxError from eval = multi-line code
            // Step 2: Split into statements + last expression
            std::string statements, last_expr;
            if (splitStatementsAndLastExpr(code, statements, last_expr)) {
                // Execute all statements (assignments, imports, etc.)
                PyObject* exec_result = PyRun_String(statements.c_str(), Py_file_input, globals, globals);
                if (!exec_result) {
                    // The naive split may have broken a multi-line expression
                    // (e.g., print(json.dumps({\n...\n})) split at the closing line).
                    // Check if it's a SyntaxError — if so, fall back to executing
                    // the ENTIRE code as Py_file_input instead of split exec+eval.
                    PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                    bool split_syntax_error = ptype && PyErr_GivenExceptionMatches(ptype, PyExc_SyntaxError);
                    Py_XDECREF(ptype);
                    Py_XDECREF(pvalue);
                    Py_XDECREF(ptraceback);

                    if (split_syntax_error) {
                        // Naive split broke the code — execute entire code as file
                        exec_result = PyRun_String(code.c_str(), Py_file_input, globals, globals);
                        if (!exec_result) {
                            PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                            std::string error_msg = "Unknown Python error";
                            if (ptype && pvalue) {
                                PyObject* type_name = PyObject_GetAttrString(ptype, "__name__");
                                PyObject* str_exc = PyObject_Str(pvalue);
                                if (type_name && str_exc) {
                                    error_msg = std::string(PyUnicode_AsUTF8(type_name)) + ": " + PyUnicode_AsUTF8(str_exc);
                                } else if (str_exc) {
                                    const char* err = PyUnicode_AsUTF8(str_exc);
                                    if (err) error_msg = err;
                                }
                                Py_XDECREF(type_name);
                                Py_XDECREF(str_exc);
                            } else if (pvalue) {
                                PyObject* str_exc = PyObject_Str(pvalue);
                                if (str_exc) {
                                    const char* err = PyUnicode_AsUTF8(str_exc);
                                    if (err) error_msg = err;
                                    Py_DECREF(str_exc);
                                }
                            }
                            Py_XDECREF(ptype);
                            Py_XDECREF(pvalue);
                            Py_XDECREF(ptraceback);
                            restoreStdoutOnly();
                            python_c_gil_release(gil_handle);
                            throw std::runtime_error("Python execution error: " + error_msg);
                        }
                        Py_DECREF(exec_result);

                        // Whole-code exec succeeded — return captured stdout
                        std::string captured = captureAndRestoreStdout();
                        python_c_gil_release(gil_handle);
                        if (!captured.empty()) {
                            return interpreter::NaabVal::makeString(captured);
                        }
                        return interpreter::NaabVal::makeNull();
                    }

                    // Not a SyntaxError — real error in the statements
                    // Re-run to get clean error message
                    exec_result = PyRun_String(statements.c_str(), Py_file_input, globals, globals);
                    if (!exec_result) {
                        PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                        std::string error_msg = "Unknown Python error";
                        if (ptype && pvalue) {
                            PyObject* type_name = PyObject_GetAttrString(ptype, "__name__");
                            PyObject* str_exc = PyObject_Str(pvalue);
                            if (type_name && str_exc) {
                                error_msg = std::string(PyUnicode_AsUTF8(type_name)) + ": " + PyUnicode_AsUTF8(str_exc);
                            } else if (str_exc) {
                                const char* err = PyUnicode_AsUTF8(str_exc);
                                if (err) error_msg = err;
                            }
                            Py_XDECREF(type_name);
                            Py_XDECREF(str_exc);
                        } else if (pvalue) {
                            PyObject* str_exc = PyObject_Str(pvalue);
                            if (str_exc) {
                                const char* err = PyUnicode_AsUTF8(str_exc);
                                if (err) error_msg = err;
                                Py_DECREF(str_exc);
                            }
                        }
                        Py_XDECREF(ptype);
                        Py_XDECREF(pvalue);
                        Py_XDECREF(ptraceback);
                        restoreStdoutOnly();
                        python_c_gil_release(gil_handle);
                        throw std::runtime_error("Python execution error: " + error_msg);
                    }
                }
                Py_DECREF(exec_result);

                // Evaluate last expression to get return value
                py_result = PyRun_String(last_expr.c_str(), Py_eval_input, globals, globals);
                if (!py_result) {
                    // Last line might also be a statement (not an expression)
                    // Try executing it and return None
                    PyErr_Clear();
                    exec_result = PyRun_String(last_expr.c_str(), Py_file_input, globals, globals);
                    if (!exec_result) {
                        PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                        std::string error_msg = "Unknown Python error";
                        if (ptype && pvalue) {
                            PyObject* type_name = PyObject_GetAttrString(ptype, "__name__");
                            PyObject* str_exc = PyObject_Str(pvalue);
                            if (type_name && str_exc) {
                                error_msg = std::string(PyUnicode_AsUTF8(type_name)) + ": " + PyUnicode_AsUTF8(str_exc);
                            } else if (str_exc) {
                                const char* err = PyUnicode_AsUTF8(str_exc);
                                if (err) error_msg = err;
                            }
                            Py_XDECREF(type_name);
                            Py_XDECREF(str_exc);
                        } else if (pvalue) {
                            PyObject* str_exc = PyObject_Str(pvalue);
                            if (str_exc) {
                                const char* err = PyUnicode_AsUTF8(str_exc);
                                if (err) error_msg = err;
                                Py_DECREF(str_exc);
                            }
                        }
                        Py_XDECREF(ptype);
                        Py_XDECREF(pvalue);
                        Py_XDECREF(ptraceback);
                        restoreStdoutOnly();
                        python_c_gil_release(gil_handle);
                        throw std::runtime_error("Python execution error: " + error_msg);
                    }
                    Py_DECREF(exec_result);

                    // Return captured stdout as value
                    std::string captured = captureAndRestoreStdout();
                    python_c_gil_release(gil_handle);
                    if (!captured.empty()) {
                        return interpreter::NaabVal::makeString(captured);
                    }
                    return interpreter::NaabVal::makeNull();
                }
            } else {
                // Can't split (last line is indented or single statement)
                // Execute entire code as file input
                PyObject* exec_result = PyRun_String(code.c_str(), Py_file_input, globals, globals);
                if (!exec_result) {
                    PyErr_Fetch(&ptype, &pvalue, &ptraceback);
                    std::string error_msg = "Unknown Python error";
                    if (ptype && pvalue) {
                        PyObject* type_name = PyObject_GetAttrString(ptype, "__name__");
                        PyObject* str_exc = PyObject_Str(pvalue);
                        if (type_name && str_exc) {
                            error_msg = std::string(PyUnicode_AsUTF8(type_name)) + ": " + PyUnicode_AsUTF8(str_exc);
                        } else if (str_exc) {
                            const char* err = PyUnicode_AsUTF8(str_exc);
                            if (err) error_msg = err;
                        }
                        Py_XDECREF(type_name);
                        Py_XDECREF(str_exc);
                    } else if (pvalue) {
                        PyObject* str_exc = PyObject_Str(pvalue);
                        if (str_exc) {
                            const char* err = PyUnicode_AsUTF8(str_exc);
                            if (err) error_msg = err;
                            Py_DECREF(str_exc);
                        }
                    }
                    Py_XDECREF(ptype);
                    Py_XDECREF(pvalue);
                    Py_XDECREF(ptraceback);
                    restoreStdoutOnly();
                    python_c_gil_release(gil_handle);
                    throw std::runtime_error("Python execution error: " + error_msg);
                }
                Py_DECREF(exec_result);

                // Return captured stdout as value
                std::string captured = captureAndRestoreStdout();
                python_c_gil_release(gil_handle);
                if (!captured.empty()) {
                    return interpreter::NaabVal::makeString(captured);
                }
                return interpreter::NaabVal::makeNull();
            }
        }
    }

    // Convert PyObject to NAAb Value (still holding GIL).
    // F42: pyObjectToValue can throw (depth limit, non-string dict key) and the
    // GIL release below is NOT in a scope guard -- every release in this
    // function is a manual call on the success path. An escaping exception
    // therefore left the GIL held and py_result leaked, deadlocking the next
    // Python block instead of reporting the error. The dict-key throw already
    // had this shape; the depth guard makes it reachable from ordinary data, so
    // release and DECREF before rethrowing.
    interpreter::NaabVal value;
    try {
        value = pyObjectToValue(py_result);
    } catch (...) {
        Py_DECREF(py_result);
        saved_env_hider.reveal();  // A32: GIL still held here
        python_c_gil_release(gil_handle);
        throw;
    }

    // Release the Python object
    Py_DECREF(py_result);

    // Check: if result is None/null, return captured stdout as value
    bool is_null = value.isNull();
    std::string captured = captureAndRestoreStdout();
    python_c_gil_release(gil_handle);

    if (is_null && !captured.empty()) {
        return interpreter::NaabVal::makeString(captured);
    }

    return value;
}

/**
 * Convert PyObject* to NAAb Value
 */
interpreter::NaabVal PythonCExecutor::pyObjectToValue(PyObject* obj, int depth) {
    // F42: the INBOUND direction had no depth limit while valueToPyObject (the
    // outbound twin, same file) has carried one since V-RT-005. A Python object
    // that refers to itself -- `a = []; a.append(a); a` -- recursed until the
    // stack was exhausted and the process died with SIGSEGV. A crash is worse
    // than a rejection: it renders NO verdict, exits on a signal rather than a
    // documented exit code, and is reachable from any polyglot block that
    // returns a cyclic structure.
    //
    // Depth, not a visited-set: 64 matches valueToPyObject and V-VM-002
    // (serializeForLanguage), and a cycle is unrepresentable in NaabVal anyway,
    // so there is nothing to preserve by detecting it more precisely -- the
    // structure cannot cross the boundary either way. Depth also catches
    // legitimately-too-deep acyclic nesting, which a cycle detector would not.
    if (depth > 64) {
        throw std::runtime_error(
            "Polyglot marshalling error: nested structure exceeds maximum depth (64)\n\n"
            "  Help:\n"
            "  - A self-referential structure (a list or dict containing itself) has no\n"
            "    finite depth and cannot cross the language boundary.\n"
            "  - Flatten or break the cycle before returning the value.\n\n"
            "  Example:\n"
            "    x Wrong: a = []; a.append(a); a\n"
            "    v Right: a = []; a.append(1); a\n");
    }
    if (!obj || obj == Py_None) {
        return interpreter::NaabVal::makeNull();
    }

    // Bool (check before int, since bool is a subclass of int in Python)
    if (PyBool_Check(obj)) {
        return interpreter::NaabVal::makeBool(obj == Py_True);
    }

    // Int
    if (PyLong_Check(obj)) {
        long long val = PyLong_AsLongLong(obj);
        if (val == -1 && PyErr_Occurred()) {
            PyErr_Clear();
            double dval = PyLong_AsDouble(obj);
            return interpreter::NaabVal::makeDouble(dval);
        }
        if (val >= INT32_MIN && val <= INT32_MAX) {
            return interpreter::NaabVal::makeInt(static_cast<int>(val));
        } else {
            return interpreter::NaabVal::makeDouble(static_cast<double>(val));
        }
    }

    // Float
    if (PyFloat_Check(obj)) {
        return interpreter::NaabVal::makeDouble(PyFloat_AsDouble(obj));
    }

    // String
    if (PyUnicode_Check(obj)) {
        const char* str = PyUnicode_AsUTF8(obj);
        return interpreter::NaabVal::makeString(std::string(str ? str : ""));
    }

    // List
    if (PyList_Check(obj)) {
        std::vector<interpreter::NaabVal> vec;
        Py_ssize_t size = PyList_Size(obj);
        vec.reserve(size);

        for (Py_ssize_t i = 0; i < size; i++) {
            PyObject* item = PyList_GetItem(obj, i);
            vec.push_back(pyObjectToValue(item, depth + 1));
        }

        return interpreter::NaabVal::makeList(std::move(vec));
    }

    // Tuple (convert to list)
    if (PyTuple_Check(obj)) {
        std::vector<interpreter::NaabVal> vec;
        Py_ssize_t size = PyTuple_Size(obj);
        vec.reserve(size);

        for (Py_ssize_t i = 0; i < size; i++) {
            PyObject* item = PyTuple_GetItem(obj, i);
            vec.push_back(pyObjectToValue(item, depth + 1));
        }

        return interpreter::NaabVal::makeList(std::move(vec));
    }

    // Dict
    if (PyDict_Check(obj)) {
        std::unordered_map<std::string, interpreter::NaabVal> map;

        PyObject *key, *value;
        Py_ssize_t pos = 0;

        while (PyDict_Next(obj, &pos, &key, &value)) {
            if (!PyUnicode_Check(key)) {
                throw std::runtime_error("Dictionary keys must be strings");
            }

            const char* key_str = PyUnicode_AsUTF8(key);
            if (!key_str) {
                throw std::runtime_error("Failed to convert dictionary key to string");
            }

            map[key_str] = pyObjectToValue(value, depth + 1);
        }

        return interpreter::NaabVal::makeDict(std::move(map));
    }

    // Unsupported type - wrap in PythonObjectValue
    Py_INCREF(obj);
    return interpreter::NaabVal::makePythonObject(
        std::make_shared<naab::interpreter::PythonObjectValue>(obj));
}

/**
 * Convert NAAb Value to PyObject*
 */
PyObject* PythonCExecutor::valueToPyObject(const interpreter::NaabVal& val, int depth) {
    // V-RT-005: prevent stack overflow on deeply-nested structures.
    // Same depth limit as V-VM-002 (serializeForLanguage): 64 levels maximum.
    if (depth > 64) {
        PyErr_SetString(PyExc_ValueError,
            "NAAb marshalling error: nested structure exceeds maximum depth (64). "
            "Flatten the structure before passing it to a Python block.");
        return nullptr;
    }
    if (val.isNull()) { Py_RETURN_NONE; }
    if (val.isBool()) {
        if (val.asBool()) { Py_RETURN_TRUE; }
        else { Py_RETURN_FALSE; }
    }
    if (val.isInt()) { return PyLong_FromLong(val.asInt()); }
    if (val.isDouble()) { return PyFloat_FromDouble(val.asDouble()); }
    if (val.isString()) { return PyUnicode_FromString(val.asString().c_str()); }
    if (val.isList()) {
        const auto& arr = val.asListConst();
        PyObject* list = PyList_New(static_cast<Py_ssize_t>(arr.size()));
        for (size_t i = 0; i < arr.size(); i++) {
            PyObject* item = valueToPyObject(arr[i], depth + 1);
            if (!item) { Py_DECREF(list); return nullptr; }  // propagate depth error
            PyList_SET_ITEM(list, static_cast<Py_ssize_t>(i), item);
        }
        return list;
    }
    if (val.isDict()) {
        const auto& dict = val.asDictConst();
        PyObject* py_dict = PyDict_New();
        for (const auto& [key, value] : dict) {
            PyObject* py_key = PyUnicode_FromString(key.c_str());
            PyObject* py_val = valueToPyObject(value, depth + 1);
            if (!py_val) { Py_DECREF(py_key); Py_DECREF(py_dict); return nullptr; }
            PyDict_SetItem(py_dict, py_key, py_val);
            Py_DECREF(py_key);
            Py_DECREF(py_val);
        }
        return py_dict;
    }
    Py_RETURN_NONE;
}

/**
 * Call a Python function by name (STUB)
 */
interpreter::NaabVal PythonCExecutor::callFunction(
    const std::string& function_name,
    const std::vector<interpreter::NaabVal>& args
) {
    (void)function_name;
    (void)args;
    throw std::runtime_error(
        "Python C API error: callFunction() is not implemented\n\n"
        "  Help:\n"
        "  - Use executeWithReturn() to run Python code instead\n"
        "  - Or use the subprocess-based Python executor (default)\n");
}

/**
 * Get captured output (STUB - Python uses StringIO capture internally)
 */
std::string PythonCExecutor::getCapturedOutput() {
    return "";
}

} // namespace runtime
} // namespace naab
