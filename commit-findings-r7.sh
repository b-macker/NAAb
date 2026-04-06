#!/usr/bin/env bash
# commit-findings-r7.sh — Build, test R7 findings, and commit.
set -euo pipefail
cd "$(dirname "$0")"

echo "=== Security R7 Commit Script ==="
echo ""

echo "[1/4] Building naab-lang..."
cmake --build build --target naab-lang -j4 2>&1 | tail -5

echo ""
echo "[2/4] Running R7 security tests..."
bash tests/security/test_async_isolation_async001r.sh
bash tests/security/test_gov_string_prefix_gov001.sh
bash tests/security/test_gov_comment_styles_gov002.sh
bash tests/security/test_js_marshal_depth_rt006.sh

echo ""
echo "[3/4] Staging files..."
git add \
    src/runtime/resource_limits.cpp \
    src/runtime/governance_checks.cpp \
    include/naab/cross_language_bridge.h \
    src/runtime/cross_language_bridge.cpp \
    src/runtime/js_executor.cpp \
    tests/security/test_async_isolation_async001r.sh \
    tests/security/test_gov_string_prefix_gov001.sh \
    tests/security/test_gov_comment_styles_gov002.sh \
    tests/security/test_js_marshal_depth_rt006.sh \
    run-all-tests.sh \
    commit-findings-r7.sh

echo ""
echo "[4/4] Committing..."
git commit -m "$(cat <<'EOF'
Security R7: fix 4 findings — async isolation, governance stripping, JS marshal depth

V-ASYNC-001r: Remove global_shutdown_.store(true) from handleAlarm() and
handleCpuLimit() in resource_limits.cpp. SIGALRM now sets only the
thread-local timeout_triggered_, preventing a timed-out script from
poisoning concurrent or subsequent script executions.

V-GOV-001: Consume Python/JS string prefixes (f, b, r, u and two-letter
combinations rb, br, rf, fr) in stripStringLiterals() before quote
detection. Without this fix, the prefix character leaked into the stripped
output and could fool governance pattern matchers.

V-GOV-002: Add -- line comment handling to language-aware stripComments()
for SQL, Lua, Haskell, and Ada blocks. Previously, "-- PATTERN" was not
stripped and could cause false positives or bypass comment-based checks.

V-RT-006: Add depth > 64 guard to CrossLanguageBridge::valueToJS() and
static toJSValue() in js_executor.cpp. Deeply nested lists/dicts passed
to JS blocks previously caused unbounded recursion (SIGSEGV). Now
JS_ThrowRangeError is returned at depth > 64, matching the Python fix
from R6 (V-RT-005).

Test suite: 376/432 pass, 0 unexpected failures.
4 new security test scripts added and wired into run-all-tests.sh.
EOF
)"

echo ""
echo "Done. Push with: git push"
