#!/usr/bin/env bash
# test_child_memory_limit.sh -- the child memory budget must bound real memory
# without refusing runtimes that merely RESERVE address space.
#
# Children of process.run / polyglot subprocesses used to get RLIMIT_AS at the
# memory budget. Address space counts reservations that are never touched, and
# node 22 needs 512-768 MB of it just to start, so under `sandbox_level:
# elevated` process.run("node", ...) died with "Fatal process out of memory:
# Failed to reserve virtual memory" before running a line (found while
# building a real project, repo-sentinel). The budget now applies to
# RLIMIT_DATA, with an address-space ceiling (4x, at least 2 GB) kept as a
# backstop for shared anonymous memory, which RLIMIT_DATA does not count.
#
# M-01 is the fix. M-02 and M-03 are why the fix is not "drop the limit":
#      real private allocation over the budget, and a shared anonymous mapping
#      past the ceiling, must both still fail. M-04 is their control -- an
#      allocation well under the budget succeeds, so M-02/M-03 cannot pass
#      because the child failed for some other reason.
# Linux only: the rlimit semantics are Linux's, and Windows children are
# contained by a job object instead.

set -uo pipefail
PASS=0
FAIL=0
SKIP=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="$REPO/build/naab-lang"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/naab_cmem.XXXXXX")"
[ -n "$WORK" ] && [ -d "$WORK" ] || { echo "FATAL: could not create work dir" >&2; exit 1; }
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'rm -rf "$WORK"; teardown_isolated_trust' EXIT

if [ ! -x "$NAAB" ]; then
    echo "FAIL: naab-lang not built at $NAAB (UNMEASURABLE, not a pass)"
    exit 1
fi
if [ "$(uname -s)" != "Linux" ]; then
    echo "SKIP: rlimit containment is Linux-specific (UNMEASURABLE here)"
    exit 0
fi

pass() { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
fail() { echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "         $3"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] $2 (UNMEASURABLE)"; SKIP=$((SKIP+1)); }

echo '{ "version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" } }' > "$WORK/govern.json"

# child ID CMD ARGS_NAAB_ARRAY -> OUT (the child's stdout+stderr, via process.run)
child() {
    cat > "$WORK/$1.naab" <<EOF
use process
main {
    let r = process.run("$2", $3)
    print("STDOUT:" + r["stdout"])
    print("STDERR:" + r["stderr"])
}
EOF
    OUT="$(cd "$WORK" && timeout 60 "$NAAB" "$WORK/$1.naab" 2>&1)"
}

echo "=== M: child memory containment ==="
if command -v node >/dev/null 2>&1; then
    child m01 node '["-e", "console.log(\"NODE_OK\")"]'
    case "$OUT" in
        *STDOUT:NODE_OK*) pass M-01 "node starts under the elevated memory budget" ;;
        *) fail M-01 "node could not start under the budget" "$(grep -m1 -i 'memory\|STDERR' <<<"$OUT")" ;;
    esac
else
    skip M-01 "node not installed"
fi

if command -v python3 >/dev/null 2>&1; then
    child m04 python3 '["-c", "a = bytearray(64 * 1024 * 1024); print(\"SMALL_OK\")"]'
    if [[ "$OUT" != *STDOUT:SMALL_OK* ]]; then
        skip M-02 "python3 cannot run as a child here (control M-04 failed)"
        skip M-03 "python3 cannot run as a child here (control M-04 failed)"
        fail M-04 "a 64 MB allocation failed -- the budget is too tight or python3 is broken" "$(head -3 <<<"$OUT")"
    else
        pass M-04 "control: a 64 MB allocation succeeds"
        child m02 python3 '["-c", "a = bytearray(3 * 1024 * 1024 * 1024); a[-1] = 1; print(\"BIG_OK\")"]'
        if [[ "$OUT" == *STDOUT:BIG_OK* ]]; then
            fail M-02 "a 3 GB private allocation succeeded -- the budget no longer bounds memory"
        else
            pass M-02 "a 3 GB private allocation is refused"
        fi
        child m03 python3 '["-c", "import mmap; m = mmap.mmap(-1, 5 * 1024 * 1024 * 1024); print(\"SHARED_OK\")"]'
        if [[ "$OUT" == *STDOUT:SHARED_OK* ]]; then
            fail M-03 "a 5 GB shared anonymous mapping succeeded -- no address-space backstop"
        else
            pass M-03 "a 5 GB shared anonymous mapping is refused by the ceiling"
        fi
    fi
else
    skip M-02 "python3 not installed"; skip M-03 "python3 not installed"; skip M-04 "python3 not installed"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP unmeasurable"
[ "$FAIL" -eq 0 ]
