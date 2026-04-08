#!/usr/bin/env bash
# Build, test, and commit R19 security fixes
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$REPO/build"

echo "=== R19 Security Fix — Build + Test + Commit ==="
echo ""

# Step 1: Build
echo "[1/3] Building naab-lang..."
cd "$BUILD"
cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1 || cmake .. > /dev/null 2>&1
make naab-lang -j4
echo "  Build OK"

# Step 2: R19 source checks
echo ""
echo "[2/3] Running R19 security tests..."
chmod +x "$REPO/tests/security/test_r19_fixes.sh"
bash "$REPO/tests/security/test_r19_fixes.sh"

# Step 3: Commit
echo ""
echo "[3/3] Committing R19 fixes..."
cd "$REPO"

git add \
    src/interpreter/interpreter.cpp \
    src/api/rest_api.cpp \
    tools/naab-lsp/document_manager.h \
    tools/naab-lsp/document_manager.cpp \
    src/runtime/block_loader.cpp \
    src/stdlib/bolo_impl.cpp \
    tests/security/test_r19_fixes.sh \
    commit-findings-r19.sh

git commit -m "$(cat <<'EOF'
security(r19): fix V-GOV-012, V-CONC-003, V-CONC-004, V-RT-012, V-DOS-001

V-GOV-012 (Critical) — Container taint loss bypass
  interpreter.cpp: add mutation-site taint propagation in ExprStmt::visit;
  when push/insert/append/set/prepend receives a tainted argument, mark the
  container variable as tainted (name-based propagation, no NaabVal redesign).

V-CONC-003 (High) — REST API global stdout race
  rest_api.cpp: add static stdout_capture_mutex_ serializing the rdbuf
  redirect block; restored inside lock scope to prevent interleaving across
  concurrent httplib threads.

V-CONC-004 (High) — LSP diagnostic data race
  document_manager.h: add mutable diag_mutex_; getDiagnostics() returns
  by value (copy) eliminating dangling-reference race with debounce thread.
  document_manager.cpp: parse()/typeCheck()/runGovernanceChecks() accumulate
  into local vectors and assign/append under lock_guard.

V-RT-012 (Medium) — Fragile block code extraction
  block_loader.cpp: replace raw json_content.find("\"code\": \"") with
  nlohmann::json::parse() + j["code"].get<string>(); handles all JSON
  escape sequences and rejects attacker-controlled metadata misdirection.

V-DOS-001 (Medium) — BOLO unbounded input DoS
  bolo_impl.cpp: enforce MAX_BOLO_SCAN_BYTES (10 MiB) before passing
  code to GovernanceEngine::checkPolyglotBlock().

Tests: tests/security/test_r19_fixes.sh (7 checks: 6 source + 1 runtime)

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

echo ""
echo "=== R19 commit complete ==="
git log --oneline -3
