#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

git add "tests/chapter verification/MONO_EXHAUSTIVE_TEST.naab" \
         "tests/chapter verification/chaos_tests/resource_stress.naab"
git stash drop 2>/dev/null || true

git add \
    src/runtime/cpp_executor_adapter.cpp \
    src/runtime/rust_executor.cpp \
    src/cli/main.cpp \
    src/runtime/json_result_parser.cpp \
    src/runtime/cpp_executor.cpp \
    src/stdlib/bolo_impl.cpp \
    tests/security/test_r18_fixes.sh \
    commit-findings-r18.sh \
    commit-r18.sh

git commit -m "$(cat <<'EOF'
security(R18): fix V-RCE-006/007, V-SC-005, V-RT-011, V-CONC-002

V-RCE-006: harden C++ source scanner — strip line continuations before
  regex scan; add angle-bracket absolute #include check; harden Rust
  scanner to reject include_str!/include_bytes! with raw-string paths

V-SC-005: add lock_check gate to api command handler — verifySignature
  + checkDrift before server.start(); mirrors the run command gate

V-RT-011: add checkJsonDepth() guard in JsonResultParser::parse()
  before nlohmann::json::parse(); rejects maliciously nested JSON

V-RCE-007: use mkdtemp for CppExecutor::compileBlock source write;
  atomic fs::rename to install .so to cache; eliminates TOCTOU attack

V-CONC-002: wrap bolo_impl ensureEngine() in std::mutex lock_guard;
  prevents concurrent first-call race creating two GovernanceEngine instances

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

echo "=== R18 committed ==="
git log --oneline -1
