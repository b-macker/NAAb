#!/usr/bin/env bash
# Commit R15 fixes: V-LSP-005 (--lint-only), V-LSP-004 (STDIN+-- separator),
#                   V-RCE-002 (expanded denylist), V-RCE-003 (case-insensitive),
#                   V-API-003 (constantTimeCompare length oracle).
# Builds naab-lang + naab-lsp, runs the security test, then commits.

set -euo pipefail
cd "$(dirname "$0")"

echo "=== Building naab-lang ==="
cmake --build build --target naab-lang -j4

echo ""
echo "=== Building naab-lsp (optional) ==="
cmake --build build --target naab-lsp -j4 2>/dev/null || echo "(naab-lsp skipped)"

echo ""
echo "=== Making test script executable ==="
chmod +x tests/security/test_r15_fixes.sh

echo ""
echo "=== Running R15 security tests ==="
bash tests/security/test_r15_fixes.sh

echo ""
echo "=== Committing ==="
git add \
    src/cli/main.cpp \
    src/stdlib/env_impl.cpp \
    src/runtime/crypto_utils.cpp \
    tools/naab-lsp/document_manager.cpp \
    tests/security/test_r15_fixes.sh \
    commit-findings-r15.sh

git commit -m "$(cat <<'EOF'
fix(security): R15 — V-LSP-005, V-LSP-004, V-RCE-002, V-RCE-003, V-API-003

V-LSP-005 (Critical): LSP executes any .naab file opened in editor
- main.cpp: add --lint-only flag (global_lint_only bool in pre-scan + run loop)
- Lint-only gate inserted before bytecode_vm.execute() and interpreter.execute()
- Parses + compiles + pre-flight governance analysis; exits without running user code
- Pre-scan loop handles --lint-only and "--" (end-of-flags sentinel)
- Run flag loop handles --lint-only and "--" (positional-arg collector)

V-LSP-004 (Critical): LSP stdin hijacking & argument injection
- document_manager.cpp: open /dev/null + dup2 to STDIN_FILENO in child process
  before execvp; child can no longer steal LSP JSON-RPC messages from editor
- argv updated to { naab_path, "--lint-only", "--", file_path, nullptr }
  "--" prevents any file_path value (e.g. file://--repl → "--repl") from being
  parsed as a CLI flag by naab-lang

V-RCE-002 (High): Incomplete environment denylist
- env_impl.cpp: add PATH, BASH_ENV, ENV, COMPILER_PATH, GCC_EXEC_PREFIX,
  PYTHONHOME, PYTHONUSERBASE to NAAB_DANGEROUS_ENV_VARS

V-RCE-003 (High): Case-sensitive environment checks bypass on Windows
- env_impl.cpp: add toUpperCase() helper; replace isBlockedEnvVar() and
  isDangerousEnvVar() with case-insensitive versions (toUpperCase(name) before
  unordered_set::count()); all set entries are already uppercase — no set changes needed

V-API-003 (Medium): API key length disclosure via timing
- crypto_utils.cpp: remove early return on a.length() != b.length()
- Always iterate b.length() bytes; record length mismatch branchlessly via
  result |= (a.length() != len); when i >= a.length() compare b[i]^b[i] = 0

New test: tests/security/test_r15_fixes.sh
  T-LSP5-1: --lint-only exits without executing side-effect script
  T-LSP4-1: STDIN_FILENO redirect present in document_manager.cpp
  T-LSP4-2: "--" separator present in document_manager.cpp argv
  T-RCE2-1/2: PATH and BASH_ENV blocked by env.set_var()
  T-RCE3-1/2: lowercase ld_preload and mixed-case PythonPath blocked
  T-API3-1/2: length oracle comment present; early-return removed

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

echo ""
echo "Done. R15 fixes committed."
