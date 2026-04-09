#!/bin/bash
set -e

cd "$(dirname "$0")"

echo "Building naab-lang..."
cmake --build build --target naab-lang -j4

git add \
    include/naab/resource_limits.h \
    src/runtime/resource_limits.cpp

git commit -m "$(cat <<'EOF'
fix: Windows VM timeout — timer thread sets global_shutdown_ in place of alarm()

alarm() is POSIX-only; on Windows setExecutionTimeout() was a no-op so the
VM_NEXT() isTimeoutTriggered() check never fired and infinite loops ran until
an unrelated error (e.g. integer overflow at INT_MAX) after ~125s.

Fix: on Windows, setExecutionTimeout() spawns a detached timer thread that
polls win_timer_cancel_ every 50ms and sets global_shutdown_ at the deadline.
isTimeoutTriggered() already checks global_shutdown_ (added in V-ASYNC-001)
so the VM dispatch loop picks up the timeout on the next instruction.

clearTimeout() sets win_timer_cancel_ = true before resetting global_shutdown_
to minimize the race window where the timer fires after a clean completion.

No POSIX code paths changed. Fixes test_timeout_ab.sh A1 on Windows CI.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

git push
