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
    bool active = false;  // false = the subprocess lists above are not in use
    // capabilities.env_vars.blocked_read, in enforce mode only. Withheld from
    // every child the PROGRAM starts, whatever `mode` says and even when
    // `active` is false: a variable env.get() may not read must not be
    // readable through `<<shell>>` or process.run("printenv") instead.
    // Matched case-insensitively, as checkEnvVarRead() matches it.
    std::vector<std::string> read_blocked_vars;
};

// Who the child is started for. A HOOK is a command the operator configured
// in govern.json, not program code, so blocked_read does not apply to it (a
// hook may need the token a script must not read); the subprocess lists do.
enum class EnvScrubScope { Program, Hook };

// The policy for a child started from THIS thread, read live from the
// governance engine bound to the thread (GovernanceEngine::getCurrent()), so a
// mid-run reload, --env selection or role edit is seen by the next spawn. A
// thread with no engine gets the default (NAAb secrets only). It used to be a
// separate thread_local copy set once by loadFromFile(), so a VM async fn's
// children were never scrubbed and a reload never reached the policy.
EnvScrubPolicy currentEnvScrubPolicy(EnvScrubScope scope = EnvScrubScope::Program);

// Check if an env var key should be scrubbed under `policy`. NAAb's own
// secrets (NAAB_GOVERN_KEY, NAAB_LOCK_KEY, NAAB_SIGNING_KEY) always are.
bool shouldScrubEnvVar(const EnvScrubPolicy& policy, const std::string& key);

// True when `key` is withheld because of blocked_read (not the other lists).
bool isReadBlockedEnvVar(const EnvScrubPolicy& policy, const std::string& key);

// Print, once per process per variable, that a blocked_read variable was
// withheld from a child -- its NAME only, never its value. Without it, a CLI
// tool that loses a credential (aws, gh) fails with its own unrelated error.
void noteWithheldEnvVar(const std::string& key);

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

// The complete environment ("KEY=value" strings) for a child, built in the
// PARENT before fork(): the inherited environment minus what `policy`
// scrubs, with the containment layers that edit the environment applied --
// L1 PATH restriction, L6 LD_LIBRARY_PATH, L5 proxy stripping -- and then
// `overrides`, which win over inherited keys. The child must be started with
// exactly this environment (execve). The containment edits used to be
// setenv()/unsetenv() calls in the child, and whenever a scrub policy was
// active the child was started with a prebuilt envp that ignored them, so
// turning env hardening ON switched proxy stripping OFF. Every withheld
// blocked_read variable is reported through noteWithheldEnvVar().
std::vector<std::string> buildChildEnvironment(
    const EnvScrubPolicy& policy,
    const SubprocessContainment* containment,
    const std::map<std::string, std::string>* overrides = nullptr);

// Where execvp() would look for `command` given the child's environment `env`:
// `command` itself when it contains a '/', else each PATH entry joined with it
// (an empty entry means the current directory; no PATH means "/bin:/usr/bin").
// The child execve()s each in turn, as execvp() does. Starting a bare name
// through execve() alone does no PATH search at all -- that is how setting a
// scrub key made php, go and process.run("printenv") exit 127.
std::vector<std::string> execCandidates(const std::string& command,
                                        const std::vector<std::string>& env);

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

