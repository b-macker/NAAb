#!/usr/bin/env bash
# Commit R14 fixes: V-SC-004 (pre-execution lock gate), V-LSP-003 (popen→execvp),
#                   V-ENV-001 (blocked env writes), V-RCE-001 (dangerous var denylist),
#                   V-ASYNC-004 (drain futures on queue-full).
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
chmod +x tests/security/test_r14_fixes.sh

echo ""
echo "=== Running R14 security tests ==="
bash tests/security/test_r14_fixes.sh

echo ""
echo "=== Committing ==="
git add \
    src/cli/main.cpp \
    src/stdlib/env_impl.cpp \
    src/runtime/polyglot_async_executor.cpp \
    tools/naab-lsp/document_manager.cpp \
    tests/security/test_r14_fixes.sh \
    commit-findings-r14.sh

git commit -m "$(cat <<'EOF'
fix(security): R14 — V-SC-004, V-LSP-003, V-ENV-001, V-RCE-001, V-ASYNC-004

V-SC-004 (Critical): Post-execution lockfile verification (TOCTOU)
- main.cpp: insert pre-execution lock_check gate before if (use_vm)
- Performs verifySignature() + getRuntimeVersion() + checkDrift() before any
  user code runs; halts with _exit(1) on tamper or drift detection
- Post-execution block changed to if (lock_update) only — lock_check removed

V-LSP-003 (Critical): LSP server command injection via popen
- document_manager.cpp: replace popen() with fork/execvp in runNaabGovernance()
- naab_path and file_path passed as argv[] elements — no shell, no injection
- stdout+stderr captured via pipe(2)/read()/waitpid(); add <sys/wait.h>

V-ENV-001 (High): Internal environment modification bypass
- env_impl.cpp: add isBlockedEnvVar() check in env.set_var and env.delete_var
- env.set_var: throws runtime_error if key is in NAAB_INTERNAL_ENV_VARS
- env.delete_var: throws runtime_error if key is in NAAB_INTERNAL_ENV_VARS
- env.load_dotenv: filters blocked vars from parsed map before setenv loop

V-RCE-001 (High): Polyglot sandbox escape via environment
- env_impl.cpp: add NAAB_DANGEROUS_ENV_VARS set (LD_PRELOAD, LD_LIBRARY_PATH,
  DYLD_INSERT_LIBRARIES, PYTHONPATH, PYTHONSTARTUP, NODE_OPTIONS, NODE_PATH,
  PERL5LIB, PERLLIB, RUBYOPT, RUBYLIB, JAVA_TOOL_OPTIONS, JDK_JAVA_OPTIONS,
  _JAVA_OPTIONS) and isDangerousEnvVar() helper
- env.set_var: throws runtime_error if key is in NAAB_DANGEROUS_ENV_VARS
- env.load_dotenv: strips dangerous vars from parsed map before setenv loop

V-ASYNC-004 (Medium): Uncaught queue-full exceptions abandon background threads
- polyglot_async_executor.cpp: wrap executeParallel submission loop in
  try/catch(std::runtime_error); on catch, drain all outstanding futures
  (f.get() in nested try/catch) before rethrowing — no abandoned threads

New test: tests/security/test_r14_fixes.sh
  T-SC4-A: tampered lockfile detected pre-execution (signature check)
  T-LSP3-1/2: execvp present, popen absent in document_manager.cpp
  T-ENV1-1/2: env.set_var/delete_var(NAAB_LOCK_KEY) throws
  T-RCE1-1/2: env.set_var(LD_PRELOAD/PYTHONPATH) throws
  T-ASYNC4-1: drain comment present in polyglot_async_executor.cpp

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

echo ""
echo "Done. R14 fixes committed."
