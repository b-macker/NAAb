#!/bin/bash
set -e

cd "$(dirname "$0")"

git add \
    include/naab/governance.h \
    include/naab/interpreter.h \
    src/api/rest_api.cpp \
    src/cli/main.cpp \
    src/interpreter/governance_taint.cpp \
    src/interpreter/interpreter.cpp \
    src/interpreter/polyglot.cpp \
    src/runtime/governance_checks.cpp \
    src/runtime/governance_engine.cpp \
    src/stdlib/file_impl.cpp \
    src/stdlib/process_impl.cpp \
    tests/chaos/resource_stress.naab \
    "tests/chapter verification/chaos_tests/resource_stress.naab" \
    tests/stdlib/test_stdlib_file.naab \
    tests/stdlib/test_stdlib_io.naab \
    tests/stdlib/test_stdlib_log.naab \
    tests/test_stdlib_file_new.naab

git commit -m "$(cat <<'EOF'
security: remediate 7 governance engine vulnerabilities (findings A–G)

Finding A — fail-open governance baseline: discoverAndLoad() now records
last_error_ to distinguish absent vs corrupt config; main.cpp and
interpreter.cpp warn when no govern.json is found; --require-governance
flag added to abort execution (exit 4) when governance is mandatory.

Finding B — process.kill() sandbox bypass: added ScopedSandbox guard
matching the existing process.run() check; requires SYS_EXEC capability.

Finding C — REST API missing governance context: setSourceCode() now
called before execute() in the /api/v1/execute handler so discoverAndLoad
is triggered for API-submitted code.

Finding D — memory exhaustion governance bypass: explicit std::bad_alloc
catch added to searchPatterns() and checkPii() with a fail-safe error
message instead of an opaque crash.

Finding E — TOCTOU in file operations: resolveCanonical() helper resolves
symlinks between checkFileSandbox() and the actual OS call; guarded by
ScopedSandbox::getCurrent() so it is a no-op when no sandbox is active
(preserves backward compatibility on systems where /tmp is a symlink).

Finding F — polyglot return taint gap: checkRhsTainted() now
auto-propagates taint from bound input variables to the polyglot return
value when taint_tracking is enabled, preventing silent laundering.

Finding G — FFI output size overflow: size of polyglot executeWithReturn()
result is checked against limits.data.output_size (global) and
LanguageConfig::max_output_size (per-language) before being stored.

Tests: replace hardcoded /tmp paths with env.get("HOME") in 5 test files
to fix failures on systems where /tmp does not exist (e.g. Termux).
Rebuild naab-gov to pick up version string 0.8.1 already in gov_main.cpp.
Result: 386 pass, 40 error-behavior, 6 missing-executor, 0 unexpected.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

git push
