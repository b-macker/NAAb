#!/bin/bash
set -e

cd "$(dirname "$0")"

# Rebuild to ensure binary is current
echo "Building naab-lang..."
cmake --build build --target naab-lang -j4

git add \
    src/vm/vm.cpp \
    src/interpreter/polyglot.cpp \
    include/naab/interpreter.h \
    include/naab/resource_limits.h \
    src/runtime/resource_limits.cpp \
    src/runtime/js_executor.cpp \
    src/runtime/block_loader.cpp \
    include/naab/block_tester.h \
    src/testing/block_tester.cpp \
    run-all-tests.sh \
    tests/security/test_taint_polyglot_vm001.sh \
    tests/security/test_serialize_depth_vm002.sh \
    tests/cli/test_js_timeout_rt002.sh \
    tests/cli/test_async_timeout_async001.sh \
    tests/security/test_sqlite_null_reg001.sh

git commit -m "$(cat <<'EOF'
security: remediate Round 5 findings (taint laundering, overflow, timeout bypasses, SQLite NULL)

V-VM-001 — taint laundering via polyglot bound inputs (vm.cpp):
- OP_POLYGLOT post-execution taint block now iterates bound_taints vector
  and sets output taint if ANY bound input variable was tainted
- Previously taint was only propagated when polyglot_output was a configured
  source in govern.json, so tainted data passed to <<python[x]>> blocks
  silently lost its taint tracking on the returned value

V-VM-002 — serialization stack overflow on deeply-nested structures:
- serializeForLanguage() in vm.cpp now takes int depth=0; throws clear error
  at depth > 64 instead of overflowing the C++ call stack
- serializeValueForLanguage() in polyglot.cpp same treatment; all 15+
  recursive call sites updated to pass depth+1
- Updated declaration in interpreter.h to match

V-RT-002 — JavaScript timeout bypass via QuickJS interruptHandler:
- interruptHandler() in js_executor.cpp previously only checked
  executor->timeout_triggered_ (set by a hardcoded 30s detached thread)
- Now also checks ResourceLimiter::isTimeoutTriggered() so the --timeout N
  CLI flag correctly terminates infinite JS loops

V-ASYNC-001 — async worker threads never see SIGALRM timeout:
- timeout_triggered_ is thread_local; SIGALRM is delivered to the main
  thread only, so ThreadPool workers' flags were never set
- Add static atomic<bool> global_shutdown_ to ResourceLimiter (header +
  translation unit definition in resource_limits.cpp)
- handleAlarm() and handleCpuLimit() now set global_shutdown_ alongside
  the thread_local flag; isTimeoutTriggered() checks both
- setExecutionTimeout() and clearTimeout() reset global_shutdown_ so
  per-request isolation (Finding B's thread_local fix) is preserved

V-REG-001 — SQLite NULL column crash in block_loader.cpp:
- sqlite3_column_text() returns NULL for SQL NULL values or corrupt columns;
  constructing std::string from a NULL pointer is undefined behaviour (SIGSEGV)
- Added safeColumnText() static helper in Impl class; used for all 5 string
  columns (block_id, name, language, file_path, code_hash) in parseRow()

Also: block_tester.h/cpp migrated from shared_ptr<Value> to NaabVal in
checkAssertion() — pre-existing NaN-boxing migration gap that caused
naab-testing target to fail to build (unrelated to R5 findings).

Tests: 432 total, 376 pass, 40 error-behavior, 16 missing-executor, 0 unexpected.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

git push
