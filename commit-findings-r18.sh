#!/usr/bin/env bash
# commit-findings-r18.sh — Build, test, and commit R18 security fixes
# Fixes: V-RCE-006 (C++/Rust scanner bypass), V-SC-005 (api lock-check),
#        V-RT-011 (JSON depth overflow), V-RCE-007 (BLOCK_LIB TOCTOU),
#        V-CONC-002 (bolo singleton race)

set -euo pipefail
cd "$(dirname "$0")"

echo "=== R18: Building naab-lang ==="
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1 || cmake .. > /dev/null 2>&1
make naab-lang -j4
cd ..

echo ""
echo "=== R18: Running security fix tests ==="
chmod +x tests/security/test_r18_fixes.sh
bash tests/security/test_r18_fixes.sh

echo ""
echo "=== R18: Running full test suite ==="
bash run-all-tests.sh 2>&1 | tail -5

echo ""
echo "=== R18: Committing ==="
git add \
    src/runtime/cpp_executor_adapter.cpp \
    src/runtime/rust_executor.cpp \
    src/cli/main.cpp \
    src/runtime/json_result_parser.cpp \
    src/runtime/cpp_executor.cpp \
    src/stdlib/bolo_impl.cpp \
    tests/security/test_r18_fixes.sh \
    commit-findings-r18.sh

git commit -m "security(R18): fix V-RCE-006/007, V-SC-005, V-RT-011, V-CONC-002

V-RCE-006: harden C++ source scanner — strip line continuations before
  regex scan; add angle-bracket absolute #include check; harden Rust
  scanner to reject include_str!/include_bytes! with raw-string paths
  (r#\"/etc/...\"#)

V-SC-005: add lock_check gate to api command handler — verifySignature
  + checkDrift before server.start(); mirrors the run command gate added
  in V-SC-004/R14

V-RT-011: add checkJsonDepth() guard in JsonResultParser::parse() before
  nlohmann::json::parse(); rejects maliciously nested JSON (>128 levels)
  that would cause stack overflow in the parent process

V-RCE-007: use mkdtemp for CppExecutor::compileBlock source write;
  atomic fs::rename to install .so to persistent cache; eliminates
  predictable path TOCTOU / symlink pre-creation attack

V-CONC-002: wrap bolo_impl ensureEngine() in std::mutex lock_guard;
  prevents two threads from each creating a separate GovernanceEngine
  on concurrent first-call

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"

echo ""
echo "=== R18 COMPLETE ==="
