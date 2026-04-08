#!/usr/bin/env bash
# Commit R16 fixes: V-RCE-004 (mkdtemp), V-RCE-005 (source scanner),
#                   V-LSP-006 (JSON depth LSP), V-GOV-011 (JSON depth project context).
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
chmod +x tests/security/test_r16_fixes.sh

echo ""
echo "=== Running R16 security tests ==="
bash tests/security/test_r16_fixes.sh

echo ""
echo "=== Committing ==="
git add \
    src/runtime/cpp_executor_adapter.cpp \
    src/runtime/rust_executor.cpp \
    src/runtime/go_executor.cpp \
    tools/naab-lsp/lsp_server.cpp \
    src/runtime/project_context.cpp \
    tests/security/test_r16_fixes.sh \
    commit-findings-r16.sh

git commit -m "$(cat <<'EOF'
fix(security): R16 — V-RCE-004, V-RCE-005, V-LSP-006, V-GOV-011

V-RCE-004 (Critical): Predictable temp files in /tmp allow symlink attack
- cpp_executor_adapter.cpp: replace naab_cpp_<tid>_<counter>_* naming with
  mkdtemp("naab_cpp_XXXXXX") across all 4 compilation paths (execute main(),
  execute wrapped, executeWithReturn main(), executeWithReturn expression)
- rust_executor.cpp: replace naab_rust_<tid>_<counter>_* with mkdtemp
  in execute() and executeWithReturn(); add getRustSafeTempDir() Termux fallback
- go_executor.cpp: replace naab_go_<tid>_<counter>_* with mkdtemp
  in execute() and executeWithReturn(); add getGoSafeTempDir() Termux fallback
- All three executors: chmod(dir, 0700) after mkdtemp; replace individual
  remove(src)+remove(bin) with remove_all(compile_dir) for clean teardown
- Add <cstdlib> and <sys/stat.h> includes to all three executors

V-RCE-005 (High): Compilation phase unsandboxed — compile-time file reads bypass governance
- cpp_executor_adapter.cpp: add isCppSourceSafe() regex scanner; rejects
  absolute-path #include (e.g. #include "/etc/shadow") and #pragma GCC plugin
  Called at top of execute(code,mode) and executeWithReturn(code)
- rust_executor.cpp: add isRustSourceSafe() regex scanner; rejects
  include_str!/include_bytes! with absolute paths
  Called at top of execute() and executeWithReturn()
- Add <regex> to cpp_executor_adapter.cpp (rust already had it)

V-LSP-006 (High): LSP server JSON parse with no depth limit → stack overflow DoS
- lsp_server.cpp: add checkJsonDepth(s, max_depth=128) linear scanner that
  counts bracket nesting depth without recursion; returns false if limit exceeded
- Call before json::parse(*message_str) in LSPServer::run(); drop oversized messages
  with WARN log — server remains alive

V-GOV-011 (Medium): Project context loader JSON parse with no depth limit → stack overflow
- project_context.cpp: add checkJsonDepth() (same algorithm as LSP fix)
- parseJsonConfig(): read file to std::string first, depth-check, then parse;
  silently skip maliciously nested configs (consistent with existing malformed-skip)
- Add <iterator> for istreambuf_iterator

New test: tests/security/test_r16_fixes.sh
  T-RCE4-1/2/3: mkdtemp present in all 3 executor source files
  T-RCE5-1: C++ #include "/etc/passwd" rejected at runtime (non-zero, no "bad")
  T-RCE5-2: Rust include_str!("/etc/passwd") rejected at runtime (non-zero, no "bad")
  T-LSP6-1:  checkJsonDepth present in lsp_server.cpp
  T-GOV11-1: checkJsonDepth present in project_context.cpp

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

echo ""
echo "Done. R16 fixes committed."
