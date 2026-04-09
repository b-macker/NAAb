#!/usr/bin/env bash
# commit-findings-r6.sh — Build, test, and commit Round 6 security fixes
# Fixes: V-VM-003 (container taint), V-RT-003 (orphan kill), V-RT-004 (block integrity), V-RT-005 (marshal depth)
set -euo pipefail

cd "$(dirname "$0")"

echo "=== Round 6 Security Fixes: Build ==="
cmake --build build --target naab-lang -j4

echo ""
echo "=== Round 6 Security Fixes: New test scripts ==="
chmod +x tests/security/test_container_taint_vm003.sh
chmod +x tests/security/test_orphan_kill_rt003.sh
chmod +x tests/security/test_block_integrity_rt004.sh
chmod +x tests/security/test_marshal_depth_rt005.sh

echo "Running R6 security tests..."
bash tests/security/test_container_taint_vm003.sh build/naab-lang
echo ""
bash tests/security/test_orphan_kill_rt003.sh build/naab-lang
echo ""
bash tests/security/test_block_integrity_rt004.sh build/naab-lang
echo ""
bash tests/security/test_marshal_depth_rt005.sh build/naab-lang

echo ""
echo "=== Committing ==="
git add \
    include/naab/vm.h \
    src/vm/vm.cpp \
    src/runtime/subprocess_helpers.cpp \
    CMakeLists.txt \
    src/runtime/block_registry.cpp \
    include/naab/python_c_executor.h \
    src/runtime/python_c_executor.cpp \
    tests/security/test_container_taint_vm003.sh \
    tests/security/test_orphan_kill_rt003.sh \
    tests/security/test_block_integrity_rt004.sh \
    tests/security/test_marshal_depth_rt005.sh \
    run-all-tests.sh

git commit -m "$(cat <<'EOF'
security: fix V-VM-003, V-RT-003, V-RT-004, V-RT-005 (Round 6)

V-VM-003 (High) — Container taint laundering via OP_SET_INDEX/dict.put:
  Added tainted_containers_ (unordered_set<uint64_t>) to VM keyed by
  rawBits() identity. OP_SET_INDEX and CALL_METHOD (dict.put/list.push)
  insert the container when storing a tainted value. OP_GET_INDEX and
  dict.get check the set so taint survives pop/push cycles.

V-RT-003 (High) — Subprocess orphan on timeout (SA_RESTART swallows SIGALRM):
  Replaced blocking waitpid() with a WNOHANG poll loop that checks
  isTimeoutTriggered() every 50ms. On timeout: kill(pid, SIGKILL) + reap
  + throw, preventing orphan subprocesses when naab is killed by alarm.

V-RT-004 (High) — Block source hash never verified in getBlockSource():
  Added OpenSSL SHA-256 linkage to naab_runtime (CMakeLists.txt).
  computeBlockSHA256() helper + integrity check before caching source.
  Throws "Block integrity check failed: ... has been tampered" with
  expected/actual hashes when code_hash in metadata JSON does not match.

V-RT-005 (Medium) — valueToPyObject() has no recursion depth limit:
  Added depth parameter (default 0) with guard at depth > 64, matching
  the serializeForLanguage fix from R5 (V-VM-002). Both recursive call
  sites (list + dict branches) pass depth+1 and propagate nullptr.

Tests: 4 new scripts in tests/security/ wired into run-all-tests.sh.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

echo ""
echo "=== Pushing ==="
git push

echo ""
echo "Done. Round 6 security fixes committed and pushed."
