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
# The budget is the run's memory limit -- runtime.memory_limit, 512 MB when
# govern.json sets none -- not the level's own figure (main.cpp writes it over
# elevated's 1024). Node 24 maps its ~512 MB JIT code range WRITABLE (node 22
# maps it with no access), and RLIMIT_DATA counts every private writable
# mapping, used or not, so node 24 needs a data budget of about 560-600 MB
# just to start and does not start under the 512 MB default (A145, observed
# with node 24.22.2; CI's runners moved from node 22 to 24 on 2026-11-09 and
# M-01 went red). The owner's decision (2026-11-10): keep the 512 MB default;
# a project that runs node 24 sets runtime.memory_limit. So M-01 runs node
# under a configured 1024 MB budget, and M-05/M-06 measure the default.
#
# M-01 is the fix: node starts under a configured budget its code range fits.
#      M-01b is its control: that budget really reaches the child (a budget
#      that vanished would let M-01 pass too).
# M-02 and M-03 are why the fix is not "drop the limit": real private
#      allocation over the budget, and a shared anonymous mapping past the
#      ceiling, must both still fail. M-04 is their control -- an allocation
#      well under the budget succeeds, so M-02/M-03 cannot pass because the
#      child failed for some other reason.
# M-05 pins the default: with no runtime.memory_limit the child gets 512 MB of
#      data and a 2 GB ceiling (the owner's decision; raising it loosens).
# M-06 checks the default against the documented limit: node < 24 starts
#      under it, node >= 24 is refused with V8's CodeRange message, and any
#      other outcome fails (a new node that fits, or a different failure).
# A failing arm prints the child's exit code and the first line of its stderr
# that says anything: V8's banner opens with blank lines, so CI's first
# report of A145 showed only "STDERR:".
#
# Kill table (each mutant rebuilt or edited, suite re-run with node 22.22.0
# and node 24.22.2; observed 2026-11-10):
#   M-01's config without runtime.memory_limit (the pre-fix shape) -> M-01, M-01b red (node 24)
#   default run budget raised to 1024 MB (main.cpp)    -> M-05 red; M-06 too on node 24
#   runtime.memory_limit not applied (run_limits.h)    -> M-01b red; M-01 too on node 24
#   RLIMIT_DATA not set in the child (subprocess_helpers.cpp) -> M-01b, M-05 red; M-06 too
#     on node 24. M-02 stays green there: the 2 GB address-space ceiling still
#     refuses its 3 GB allocation, so M-02 alone cannot tell the two limits
#     apart -- M-01b and M-05 read the data limit itself.
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

# Two configs: the default budget, and a configured one node 24 fits (A145).
mkdir -p "$WORK/default" "$WORK/budget"
echo '{ "version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" } }' > "$WORK/default/govern.json"
echo '{ "version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" }, "runtime": { "memory_limit": 1024 } }' > "$WORK/budget/govern.json"
MB=$((1024*1024))

# child CONFIG ID CMD ARGS_NAAB_ARRAY -> OUT (the child's exit code, stdout
# and stderr, via process.run, under CONFIG's govern.json)
child() {
    cat > "$WORK/$1/$2.naab" <<EOF
use process
main {
    let r = process.run("$3", $4)
    print("EXIT:" + string(r["exit_code"]))
    print("STDOUT:" + r["stdout"])
    print("STDERR:" + r["stderr"])
}
EOF
    OUT="$(cd "$WORK/$1" && timeout 60 "$NAAB" "$WORK/$1/$2.naab" 2>&1)"
}
# detail: the child's exit code and the first line of its stderr that has a
# letter in it (V8's fatal banner starts with blank lines and a bare "#");
# without an EXIT line the program itself failed, so its first line instead
detail() {
    local code first
    code=$(grep -m1 '^EXIT:' <<<"$OUT" | cut -d: -f2)
    if [ -n "$code" ]; then
        first=$(sed -n '/^STDERR:/,$p' <<<"$OUT" | sed '1s/^STDERR://' | grep -v '^-- Governance' | grep -m1 '[A-Za-z]')
    else
        first=$(grep -v '^\[governance\]' <<<"$OUT" | grep -m1 '[A-Za-z]')
    fi
    echo "exit=${code:-none}: ${first:-<no stderr>}"
}
# limits CONFIG ID -> DATA, AS (the soft limits a process.run child reads)
limits() {
    child "$1" "$2" cat '["/proc/self/limits"]'
    DATA=$(grep -m1 '^Max data size' <<<"$OUT" | awk '{print $4}')
    AS=$(grep -m1 '^Max address space' <<<"$OUT" | awk '{print $4}')
}

echo "=== M: child memory containment ==="
if command -v node >/dev/null 2>&1; then
    NODE_V=$(node --version 2>/dev/null)
    NODE_MAJOR=$(sed -n 's/^v\([0-9]*\)\..*/\1/p' <<<"$NODE_V")
    child budget m01 node '["-e", "console.log(\"NODE_OK\")"]'
    case "$OUT" in
        *STDOUT:NODE_OK*) pass M-01 "node $NODE_V starts under a configured 1024 MB budget" ;;
        *) fail M-01 "node $NODE_V could not start under a configured 1024 MB budget" "$(detail)" ;;
    esac

    child default m06 node '["-e", "console.log(\"NODE_OK\")"]'
    if [ -z "$NODE_MAJOR" ]; then
        skip M-06 "node --version gave no major version"
    elif [ "$NODE_MAJOR" -lt 24 ]; then
        case "$OUT" in
            *STDOUT:NODE_OK*) pass M-06 "node $NODE_V starts under the default budget" ;;
            *) fail M-06 "node $NODE_V should start under the default budget" "$(detail)" ;;
        esac
    else
        case "$OUT" in
            *"Failed to reserve virtual memory for CodeRange"*)
                pass M-06 "node $NODE_V is refused under the default budget with V8's CodeRange message (documented, A145)" ;;
            *STDOUT:NODE_OK*)
                fail M-06 "node $NODE_V now starts under the default budget: A145's documented limit is stale, update it" ;;
            *) fail M-06 "node $NODE_V failed under the default budget some other way than A145 documents" "$(detail)" ;;
        esac
    fi
else
    skip M-01 "node not installed"
    skip M-06 "node not installed"
fi

limits budget m01b
if [ "$DATA" = "$((1024*MB))" ] && [ "$AS" = "$((4096*MB))" ]; then
    pass M-01b "control: the configured budget reaches the child (data 1 GB, ceiling 4 GB)"
else
    fail M-01b "the child should get runtime.memory_limit 1024: data $((1024*MB)), ceiling $((4096*MB))" "data=${DATA:-none} as=${AS:-none} $(detail)"
fi
limits default m05
if [ "$DATA" = "$((512*MB))" ] && [ "$AS" = "$((2048*MB))" ]; then
    pass M-05 "the default budget is unchanged: data 512 MB, ceiling 2 GB"
else
    fail M-05 "with no runtime.memory_limit the child should get data $((512*MB)), ceiling $((2048*MB))" "data=${DATA:-none} as=${AS:-none} $(detail)"
fi

if command -v python3 >/dev/null 2>&1; then
    child default m04 python3 '["-c", "a = bytearray(64 * 1024 * 1024); print(\"SMALL_OK\")"]'
    if [[ "$OUT" != *STDOUT:SMALL_OK* ]]; then
        skip M-02 "python3 cannot run as a child here (control M-04 failed)"
        skip M-03 "python3 cannot run as a child here (control M-04 failed)"
        fail M-04 "a 64 MB allocation failed -- the budget is too tight or python3 is broken" "$(detail)"
    else
        pass M-04 "control: a 64 MB allocation succeeds"
        child default m02 python3 '["-c", "a = bytearray(3 * 1024 * 1024 * 1024); a[-1] = 1; print(\"BIG_OK\")"]'
        if [[ "$OUT" == *STDOUT:BIG_OK* ]]; then
            fail M-02 "a 3 GB private allocation succeeded -- the budget no longer bounds memory"
        else
            pass M-02 "a 3 GB private allocation is refused"
        fi
        child default m03 python3 '["-c", "import mmap; m = mmap.mmap(-1, 5 * 1024 * 1024 * 1024); print(\"SHARED_OK\")"]'
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
