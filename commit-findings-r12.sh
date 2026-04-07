#!/usr/bin/env bash
# Commit R12 fixes: V-SC-001 (lockfile HMAC), V-LSP-001 (rename DoS bounds), V-GOV-010 (ReDoS).
# Builds naab-lang + naab-gov (+ naab-lsp if available), runs the security test, then commits.

set -euo pipefail
cd "$(dirname "$0")"

echo "=== Building naab-lang and naab-gov ==="
cmake --build build --target naab-lang naab-gov -j4

echo ""
echo "=== Building naab-lsp (optional) ==="
cmake --build build --target naab-lsp -j4 2>/dev/null || echo "(naab-lsp skipped)"

echo ""
echo "=== Making test script executable ==="
chmod +x tests/security/test_r12_fixes.sh

echo ""
echo "=== Running R12 security tests ==="
bash tests/security/test_r12_fixes.sh

echo ""
echo "=== Committing ==="
git add \
    include/naab/crypto_utils.h \
    src/runtime/crypto_utils.cpp \
    include/naab/lockfile.h \
    src/runtime/lockfile.cpp \
    src/cli/main.cpp \
    tools/naab-lsp/lsp_server.cpp \
    src/runtime/governance_config.cpp \
    tests/security/test_r12_fixes.sh \
    commit-findings-r12.sh

git commit -m "$(cat <<'EOF'
fix(security): R12 — V-SC-001, V-LSP-001, V-GOV-010

V-SC-001 (High): naab.lock supply chain tampering
- CryptoUtils::hmacSha256() implemented with OpenSSL HMAC API
- CryptoUtils::constantTimeCompare() promoted to public for direct HMAC comparison
- Lockfile::save(): writes naab.lock.sig HMAC-SHA256 sidecar when NAAB_LOCK_KEY is set
- Lockfile::verifySignature(): reads sidecar, compares HMAC; warns-and-proceeds when
  NAAB_LOCK_KEY is absent; exits 1 on mismatch when key is set
- main.cpp: --lock-check path calls verifySignature() before checkDrift()

V-LSP-001 (High): LSP server rename denial of service
- lsp_server.cpp handleRename(): MAX_RENAME_FILE_BYTES = 1 MiB cap on document size
- handleRename(): MAX_RENAME_EDITS = 10000 cap on collected edits
- Both limits call sendError(-32603) and return early — no unbounded scan possible

V-GOV-010 (Medium): ReDoS via adversarial govern.json custom rule patterns
- governance_config.cpp: SafeRegex::analyzePattern() called before std::regex() compilation
- Patterns longer than 1000 chars are rejected with a warning
- Patterns with !is_safe (nested quantifiers etc.) are skipped with a warning
- Safe patterns continue to compile and enforce normally

New test: tests/security/test_r12_fixes.sh (T-SC × 4, T-LSP × 1, T-GOV × 3)

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

echo ""
echo "Done. R12 fixes committed."
