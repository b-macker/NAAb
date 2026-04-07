#!/usr/bin/env bash
# Commit R13 fixes: V-SC-003 (fail-closed lockfile), V-API-002 (timing attack),
#                   V-SC-002 (env key leakage), V-LSP-002 (workspace symbol cap).
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
chmod +x tests/security/test_r13_fixes.sh

echo ""
echo "=== Running R13 security tests ==="
bash tests/security/test_r13_fixes.sh

echo ""
echo "=== Committing ==="
git add \
    src/runtime/lockfile.cpp \
    src/api/rest_api.cpp \
    src/stdlib/env_impl.cpp \
    tools/naab-lsp/lsp_server.cpp \
    tests/security/test_r13_fixes.sh \
    commit-findings-r13.sh

git commit -m "$(cat <<'EOF'
fix(security): R13 — V-SC-003, V-API-002, V-SC-002, V-LSP-002

V-SC-003 (High): Fail-open lockfile verification bypass
- lockfile.cpp verifySignature(): when NAAB_LOCK_KEY is absent, probe for .sig sidecar;
  if .sig exists, fail closed (return false) — signing was previously enabled, no key
  means we cannot verify; if no .sig either, signing not enabled, warn and proceed

V-API-002 (High): Timing attack on REST API key comparison
- rest_api.cpp: add #include crypto_utils.h
- Replace both == comparisons (Authorization + X-API-Key) with
  CryptoUtils::constantTimeCompare() to prevent character-by-character timing attacks

V-SC-002 (High): NAAB_LOCK_KEY exfiltration via env stdlib
- env_impl.cpp: add NAAB_INTERNAL_ENV_VARS static set ({"NAAB_LOCK_KEY"})
- Block in env.get, env.has, env.get_int, env.get_float, env.get_bool: return
  default/null/false silently without revealing the variable exists
- Block in env.get_all: erase all internal vars from map before returning

V-LSP-002 (Medium): Unbounded workspace symbol search
- lsp_server.cpp handleWorkspaceSymbol(): MAX_WORKSPACE_SYMBOLS = 10000 cap;
  breaks early across both document and symbol loops; logs truncation to stderr

V-CLI-001 REFUTED: parseProgram() breaks after first main{} block; tokens after
the closing } are permanently orphaned — semicolon injection cannot run code.

New test: tests/security/test_r13_fixes.sh (T-SC3×2, T-API2×1, T-SC2×2, T-LSP2×1)

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

echo ""
echo "Done. R13 fixes committed."
