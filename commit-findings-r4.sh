#!/bin/bash
set -e

cd "$(dirname "$0")"

# Rebuild to ensure binary is current
echo "Building naab-lang..."
cmake --build build --target naab-lang -j4

git add \
    include/naab/resource_limits.h \
    src/runtime/resource_limits.cpp \
    src/vm/vm.cpp \
    src/cli/main.cpp \
    src/interpreter/call_dispatch.cpp \
    src/interpreter/interpreter.cpp \
    src/interpreter/modules.cpp \
    src/stdlib/file_impl.cpp \
    src/vm/compiler.cpp \
    docs/SECURITY_GUIDE.md \
    run-all-tests.sh \
    tests/cli/test_timeout_ab.sh \
    tests/security/test_governance_funcref_c.sh \
    tests/security/test_polyglot_exception_taint_d.sh \
    tests/security/test_stdlib_shadow_e.sh \
    tests/security/test_sandbox_symlink_f.sh \
    tests/vm/test_use_statement_error.sh

git commit -m "$(cat <<'EOF'
security: remediate Round 4 findings A–H (timeout, taint, symlink, VM compiler)

Finding A — non-enforceable execution timeouts:
- Add isTimeoutTriggered() check to VM_NEXT() macro (vm.cpp) so the bytecode
  VM polls the timeout flag on every instruction dispatch
- Add ScopedTimeout before VM and tree-walker execution in main.cpp so the
  --timeout CLI flag actually arms SIGALRM for NAAb script execution
  (previously it only set max_cpu_seconds for polyglot subprocesses)

Finding B — global signal interference / DoS:
- Change ResourceLimiter::timeout_triggered_ from static to thread_local
  (resource_limits.h + resource_limits.cpp) so concurrent REST API requests
  cannot contaminate each other's timeout state via a shared flag
- Add isTimeoutTriggered() public accessor used by Finding A's VM_NEXT check

Finding C — governance bypass via function references (tree-walker):
- Add generic taint sink + dangerous_calls check in the __stdlib_call__: path
  of call_dispatch.cpp, covering all stdlib modules that lacked explicit checks
  (previously only http/env.set_var/file had per-module blocks)
- Add env.delete_var governance check (was entirely absent)
- Add post-call taint source marking: stdlib refs that are taint sources now
  set setLastReturnTainted(true) so the result is tracked by callers

Finding D — taint leakage via exception payloads:
- Fix clearTaint → markTainted for the polyglot catch variable in
  interpreter.cpp; exception messages (e.g. "Invalid API Key: sk-abc123")
  are now correctly marked as tainted so http.post/file.write sinks block them

Finding E — stdlib shadowing via import statement:
- In modules.cpp, check stdlib_->hasModule() for bare names (no path
  separator, no .naab extension) BEFORE module_resolver_->resolve(), so a
  local time.naab cannot shadow the trusted stdlib time module

Finding F — symlink TOCTOU in file operations:
- Add readFileNoFollow() and writeFileNoFollow() helpers in file_impl.cpp
  using O_RDONLY|O_NOFOLLOW and O_WRONLY|O_NOFOLLOW respectively
- Use these helpers for read/write/append when a sandbox is active, closing
  the race window between checkFileSandbox() and the actual open() syscall
- Guarded with #ifndef _WIN32 (O_NOFOLLOW not available on Windows)

Finding G — UseStatement unimplemented in VM compiler:
- Replace the silent no-op stub at compiler.cpp:940 with a compile-time
  throw that produces a clear error message directing users to --tree-walk
  for block-loading syntax (use BLOCK-xyz)

Finding H — block metadata tampering (interim mitigation):
- Add Section 10 to docs/SECURITY_GUIDE.md documenting the blocks.db
  integrity risk, the chmod 600 interim mitigation, and the planned
  HMAC-SHA256 full fix (deferred — requires key management infrastructure)

Tests: 376 pass, 40 error-behavior, 16 missing-executor, 0 unexpected (432 total).
5 examples using use BLOCK-... recategorized from unexpected-fail to missing-executor
(they require tree-walk + block executors not installed on this platform).

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

git push
