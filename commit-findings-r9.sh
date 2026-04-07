#!/usr/bin/env bash
# Commit findings from Round 9 security audit:
#   V-GOV-006 (Critical): Unconditional polyglot return taint
#   V-GOV-007 (High):     Fail-closed default governance
#   V-RT-008  (High):     VM GC cycle detection implemented

set -euo pipefail
cd "$(dirname "$0")"

echo "=== Building naab-lang ==="
cmake --build build --target naab-lang -j4

echo ""
echo "=== Making test scripts executable ==="
chmod +x tests/security/test_polyglot_taint_gov006.sh
chmod +x tests/security/test_require_governance_gov007.sh
chmod +x tests/security/test_vm_gc_rt008.sh

echo ""
echo "=== Running R9 Security Tests ==="
echo ""

echo "--- V-GOV-006: Polyglot Return Taint ---"
bash tests/security/test_polyglot_taint_gov006.sh

echo ""
echo "--- V-GOV-007: Fail-Closed Default Governance ---"
bash tests/security/test_require_governance_gov007.sh

echo ""
echo "--- V-RT-008: VM GC Cycle Detection ---"
bash tests/security/test_vm_gc_rt008.sh

echo ""
echo "=== Committing ==="
git add \
    src/vm/vm.cpp \
    src/interpreter/governance_taint.cpp \
    src/interpreter/cycle_detector.cpp \
    include/naab/vm.h \
    src/cli/main.cpp \
    tests/govern.json \
    tests/security/test_polyglot_taint_gov006.sh \
    tests/security/test_require_governance_gov007.sh \
    tests/security/test_vm_gc_rt008.sh \
    tests/security/test_taint_polyglot_vm001.sh \
    run-all-tests.sh \
    commit-findings-r9.sh

git commit -m "$(cat <<'EOF'
Security R9: V-GOV-006 polyglot taint, V-GOV-007 fail-closed, V-RT-008 VM GC

V-GOV-006 (Critical): All polyglot block outputs are now unconditionally tainted
when governance is active. Previously, only bound-input-tainted or
explicitly-configured-source blocks were tainted, allowing untrusted foreign data
to bypass taint sinks. Fixed in both VM (OP_POLYGLOT handler) and tree-walker
(checkRhsTainted/InlineCodeExpr branch). Updated V-VM-001 T2 to reflect that clean
inputs through polyglot blocks now also produce tainted outputs.

V-GOV-007 (High): global_require_governance defaults to true (fail-closed).
Scripts now exit 4 if no govern.json is found, unless --no-governance is passed
explicitly. Added tests/govern.json (mode: off) as a catch-all for the test suite
so existing tests are unaffected (discoverAndLoad searches upward from script dir).

V-RT-008 (High): gc_collect() builtin now invokes CycleDetector::detectAndCollect()
with VM stack + globals as roots. CycleDetector null root_env guard relaxed so VM
can call it without an Environment. Added periodic GC trigger at OP_JUMP_BACK
(every gc_threshold instructions, default 5000). vm.setGCThreshold() wired from CLI.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

echo ""
echo "Done. R9 findings committed."
