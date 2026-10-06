#pragma once

#include <string>
#include <vector>
#include <map>
#if defined(_MSC_VER)
#  include <process.h>
#  ifndef _PID_T_DEFINED
#    define _PID_T_DEFINED
     typedef int pid_t;
#  endif
#else
#  include <unistd.h>     // For pid_t
#endif

namespace naab {
namespace runtime {

// --- Environment Scrubbing Policy (V-SC-006-ext) ---
// Controls which environment variables are passed to polyglot subprocesses.
// Default: scrub only NAAb-internal secrets (NAAB_GOVERN_KEY, etc.)
// When governance configures capabilities.env, this extends to block
// credential patterns or restrict to an explicit allowlist.
enum class EnvScrubMode { BLOCKLIST, ALLOWLIST };

struct EnvScrubPolicy {
    EnvScrubMode mode = EnvScrubMode::BLOCKLIST;
    // BLOCKLIST mode: these exact var names are scrubbed (in addition to NAAb internals)
    std::vector<std::string> blocked_vars;
    // BLOCKLIST mode: vars matching any prefix are scrubbed (e.g., "AWS_", "OPENAI_")
    std::vector<std::string> blocked_prefixes;
    // ALLOWLIST mode: only these vars are passed through (plus PATH, HOME, LANG, TERM, TMPDIR)
    std::vector<std::string> allowed_vars;
    bool active = false;  // false = use default 3-var scrub only
};

// Set/get the env scrub policy (thread-local, set by governance engine)
void setEnvScrubPolicy(const EnvScrubPolicy& policy);
const EnvScrubPolicy& getEnvScrubPolicy();

// Check if an env var key should be scrubbed based on the active policy.
// Used by both execute_subprocess_with_pipes() and PersistentProcessExecutor.
bool shouldScrubEnvVar(const std::string& key);

// --- OS-Level Subprocess Containment ---
// Applied post-fork/pre-exec (POSIX) or via per-child Job Object (Windows).
// Enforces 5 layers: PATH restriction, fork prevention, resource limits,
// privilege lock, and network env stripping. Built from current ScopedSandbox
// config via fromCurrentSandbox().
struct SubprocessContainment {
    bool restrict_path = false;       // L1: strip PATH to interpreter dir only
    std::string interpreter_dir;      // directory of the language interpreter

    bool block_fork = false;          // L2: RLIMIT_NPROC=0 / ACTIVE_PROCESS=1
    size_t max_fsize_bytes = 0;       // L3: RLIMIT_FSIZE (0 = no limit)
    size_t max_nofile = 0;            // L3: RLIMIT_NOFILE (0 = no limit)
    size_t max_memory_bytes = 0;      // L3/L7: Memory limit (RLIMIT_DATA + RLIMIT_AS ceiling / Job memory)
    size_t max_cpu_ms = 0;            // L3/L8: CPU time limit (RLIMIT_CPU / Job CPU time)

    bool no_new_privs = false;        // L4: prctl(PR_SET_NO_NEW_PRIVS)
    bool strip_network_env = false;   // L5: remove proxy env vars

    // Factory: build from current ScopedSandbox + command path
    static SubprocessContainment fromCurrentSandbox(const std::string& command_path);
};

// The version line of a runtime BINARY, for runtime_versions pins: runs
// `binary args...` (e.g. "node --version") and returns the first non-empty
// output line, or "" when it cannot be read. Pass the same binary the
// executor runs, so the answer is about the runtime that runs the block.
// Honours the current sandbox: where process execution is not permitted it
// runs nothing and returns "" (the pin then reports "cannot be determined";
// the block could not run there anyway). Successful answers are cached for
// the process.
//
// It only STARTS a process inside a ScopedRuntimeVersionProbe; elsewhere it
// returns the cached answer or "". getRuntimeVersion() is also read by the
// per-block execution audit records, and a version probe on every block of
// every unpinned runtime would be a process launch in the hot path. Pins and
// the lockfile commands open the scope; everything else sees what they found.
std::string probeRuntimeVersion(const std::string& binary,
                                const std::vector<std::string>& args);

class ScopedRuntimeVersionProbe {
public:
    ScopedRuntimeVersionProbe();
    ~ScopedRuntimeVersionProbe();
    ScopedRuntimeVersionProbe(const ScopedRuntimeVersionProbe&) = delete;
    ScopedRuntimeVersionProbe& operator=(const ScopedRuntimeVersionProbe&) = delete;
    static bool active();
};

// Helper to execute a subprocess and capture its stdout/stderr separately
// Returns exit code, fills stdout_str and stderr_str
int execute_subprocess_with_pipes(
    const std::string& command_path,
    const std::vector<std::string>& args,
    std::string& stdout_str,
    std::string& stderr_str,
    const std::map<std::string, std::string>* env = nullptr,
    const SubprocessContainment* containment = nullptr
);

} // namespace runtime
} // namespace naab

