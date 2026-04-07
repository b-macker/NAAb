#!/usr/bin/env bash
# Commit Round 8 security fixes: V-GOV-004, V-ASYNC-002, V-RT-007
set -e

NAAB_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$NAAB_DIR"

echo "=== Round 8 Security Fix Commit Script ==="
echo ""

# Step 1: Build
echo "--- Building naab-lang ---"
cmake --build build --target naab-lang -j4 2>&1 | tail -5
echo ""

# Step 2: chmod test scripts
chmod +x \
    tests/security/test_async_counter_gov004.sh \
    tests/security/test_queue_cap_async002.sh \
    tests/security/test_alarm_delivery_rt007.sh

# Step 3: Run R8 test scripts
echo "--- Running V-GOV-004 tests ---"
bash tests/security/test_async_counter_gov004.sh
echo ""

echo "--- Running V-ASYNC-002 tests ---"
bash tests/security/test_queue_cap_async002.sh
echo ""

echo "--- Running V-RT-007 tests ---"
bash tests/security/test_alarm_delivery_rt007.sh
echo ""

# Step 4: Stage files
echo "--- Staging files ---"
git add \
    include/naab/governance.h \
    src/runtime/governance_checks.cpp \
    src/interpreter/call_dispatch.cpp \
    include/naab/thread_pool.h \
    src/runtime/thread_pool.cpp \
    include/naab/resource_limits.h \
    src/runtime/resource_limits.cpp \
    tests/security/test_async_counter_gov004.sh \
    tests/security/test_queue_cap_async002.sh \
    tests/security/test_alarm_delivery_rt007.sh \
    run-all-tests.sh \
    commit-findings-r8.sh

echo "--- Staged files ---"
git diff --cached --name-only
echo ""

# Step 5: Commit
git commit -m "$(cat <<'EOF'
security(r8): fix V-GOV-004, V-ASYNC-002, V-RT-007

V-GOV-004 (Critical): Async tasks now inherit parent governance counter
state (polyglot_block_count_, advisory_count_, etc.) via new
GovernanceEngine::saveCounterState()/restoreCounterState() following the
existing saveTaintState()/restoreTaintState() pattern. Previously, each
async task started with zeroed counters, allowing N×limit polyglot blocks.

V-ASYNC-002 (High): ThreadPool now enforces a max_queue_size cap
(default 1000). enqueue() throws a clear "Async queue full: N tasks
pending" error instead of exhausting memory when async tasks are spawned
in a tight loop without awaiting.

V-RT-007 (Medium): Replace alarm(seconds) with a detached timer thread
that uses pthread_kill(executing_tid_, SIGALRM) to deliver SIGALRM to
the exact thread that called setExecutionTimeout(). Eliminates false
negatives (correct thread never timed out) in multi-threaded processes.
posix_timer_cancel_ atomic allows clearTimeout() to cancel early.

V-ASYNC-003, V-GOV-005: Confirmed refuted — no code changes.

3 test scripts added. run-all-tests.sh updated with R8 sections.
EOF
)"

echo ""
echo "=== Round 8 commit complete ==="
git log --oneline -3
