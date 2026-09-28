#!/usr/bin/env bash
# test_timeout_reach.sh -- --timeout must stop work that never returns to NAAb.
#
# The execution timeout is a flag the interpreter POLLS. Work that runs inside
# the process without polling it only noticed the timeout once it returned by
# itself. Two such paths, both measured before this suite with --timeout 3:
#
#   <<python>> busy loop (20s)            ran 20.1s -- embedded CPython had no
#                                          interrupt path (QuickJS has one)
#   http.get(url, {}, 0), SYN-dropping host  still connecting at 20s --
#                                          timeout_ms=0 is libcurl's "never",
#                                          and curl never returns mid-transfer
#
# Group P: Python. Asserted on WALL TIME, not the error text: the old build
#          also ended with a timeout error, 17 seconds late.
#          P-03 catches every Exception in a loop -- the interrupt must re-arm,
#          or one `except Exception: pass` keeps the block alive forever.
#          P-04 is the control that a Python block still COMPLETES under a
#          timeout, so P-01..03 cannot pass by refusing every block.
# Group H: http. Needs an address where a connect attempt hangs; the viability
#          probe checks that first and reports UNMEASURABLE otherwise, since on
#          a network that refuses the connection instantly H-01 would pass for
#          free.
#
# Group N: nesting. One timer per thread, and every executor wrapped its block
#          in its own ScopedTimeout, which re-armed that timer and CLEARED it on
#          exit -- so after a single <<javascript>> expression the script's
#          --timeout was gone (measured: `while true` ran until killed from
#          outside, both engines). codegen.run did the same by hand. The loop
#          AFTER the block is what must be stopped.
#
# Known limit, not asserted: a Python block blocked inside C (time.sleep, a
# socket read) is not interrupted -- CPython runs the interrupt only between
# bytecodes. Python on worker threads (parallel polyglot groups) is not either:
# CPython runs pending calls on its main thread only.

set -uo pipefail
PASS=0
FAIL=0
SKIP=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="$REPO/build/naab-lang"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/naab_tmo.XXXXXX")"
[ -n "$WORK" ] && [ -d "$WORK" ] || { echo "FATAL: could not create work dir" >&2; exit 1; }
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'rm -rf "$WORK"; teardown_isolated_trust' EXIT

if [ ! -x "$NAAB" ]; then
    echo "FAIL: naab-lang not built at $NAAB (UNMEASURABLE, not a pass)"
    exit 1
fi

pass() { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
fail() { echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "         $3"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] $2 (UNMEASURABLE)"; SKIP=$((SKIP+1)); }

# run DIR FILE [flags...] -> OUT, RC, ELAPSED (whole seconds). The outer
# timeout is well past the old failure time, so an unfixed build is reported
# as slow rather than killed silently.
run() {
    local dir="$1" file="$2"; shift 2
    local start=$SECONDS
    OUT="$(cd "$dir" && timeout 40 "$NAAB" "$@" "$file" 2>&1)"
    RC=$?
    ELAPSED=$((SECONDS - start))
}

mkdir -p "$WORK/p" "$WORK/h"
echo '{ "version": "4.0", "mode": "off" }' > "$WORK/p/govern.json"

echo "=== P: embedded Python ==="
cat > "$WORK/p/ctl.naab" <<'EOF'
main {
    let a = <<python
sum(range(1000))
>>
    let b = <<python
len("abc")
>>
    print("CTL:" + string(a) + "," + string(b))
}
EOF
run "$WORK/p" ctl.naab --timeout 5
if [[ "$OUT" != *"CTL:499500,3"* ]]; then
    for id in P-01/vm P-01/tree-walk P-03 P-04; do
        skip "$id" "embedded Python executor not available in this build"
    done
else
    pass P-04 "control: Python blocks still complete under --timeout (${ELAPSED}s)"

    cat > "$WORK/p/busy.naab" <<'EOF'
main {
    <<python
import time
t = time.time()
while time.time() - t < 20:
    pass
>>
    print("AFTER")
}
EOF
    for eng in "" "--tree-walk"; do
        tag="${eng:-vm}"; tag="${tag#--}"
        run "$WORK/p" busy.naab $eng --timeout 3
        if [ "$ELAPSED" -le 8 ] && [ "$RC" -ne 0 ] && [[ "$OUT" != *AFTER* ]]; then
            pass "P-01/$tag" "20s Python loop stopped at the 3s limit (${ELAPSED}s, rc=$RC)"
        else
            fail "P-01/$tag" "Python loop not stopped (took ${ELAPSED}s, rc=$RC)" "$(tail -2 <<<"$OUT")"
        fi
    done

    cat > "$WORK/p/swallow.naab" <<'EOF'
main {
    <<python
import time
t = time.time()
while time.time() - t < 20:
    try:
        while True:
            pass
    except Exception:
        pass
>>
    print("AFTER")
}
EOF
    run "$WORK/p" swallow.naab --timeout 3
    if [ "$ELAPSED" -le 8 ] && [[ "$OUT" != *AFTER* ]]; then
        pass P-03 "a loop that catches every Exception is still stopped (${ELAPSED}s)"
    else
        fail P-03 "the interrupt was swallowed (took ${ELAPSED}s)" "$(tail -2 <<<"$OUT")"
    fi
fi

echo "=== H: http ==="
echo '{ "version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" }, "capabilities": { "network": { "enabled": true } } }' > "$WORK/h/govern.json"
HANG_URL="http://8.8.8.8:81/"   # port 81 is not served; SYNs are dropped
cat > "$WORK/h/probe.naab" <<EOF
use http
main {
    try { http.get("$HANG_URL", {}, 2000) } catch (e) { print("DONE") }
}
EOF
run "$WORK/h" probe.naab
if [ "$ELAPSED" -lt 1 ] || [[ "$OUT" != *DONE* ]]; then
    skip H-01 "no address here where a connect attempt hangs (probe ${ELAPSED}s)"
else
    cat > "$WORK/h/zero.naab" <<EOF
use http
main {
    try {
        http.get("$HANG_URL", {}, 0)
        print("RETURNED")
    } catch (e) {
        print("CAUGHT")
    }
}
EOF
    run "$WORK/h" zero.naab --timeout 3
    if [ "$ELAPSED" -le 8 ]; then
        pass H-01 "http.get with timeout_ms=0 stopped at the 3s limit (${ELAPSED}s)"
    else
        fail H-01 "http request outlived --timeout (took ${ELAPSED}s)" "$(tail -2 <<<"$OUT")"
    fi
fi

echo "=== N: a block's own timeout must not cancel the script's ==="
mkdir -p "$WORK/n"
echo '{ "version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" }, "codegen": { "enabled": true } }' > "$WORK/n/govern.json"
cat > "$WORK/n/js.naab" <<'EOF'
main {
    let v = <<javascript
1 + 1
>>
    print("JS:" + string(v))
    let i = 0
    while true { i = i + 1 }
}
EOF
cat > "$WORK/n/cg.naab" <<'EOF'
use codegen
main {
    let r = codegen.run("javascript", "1 + 1")
    print("CG:" + string(r["exit_code"]))
    let i = 0
    while true { i = i + 1 }
}
EOF
for prog in js:JS:2 cg:CG:0; do
    f="${prog%%:*}"; marker="${prog#*:}"
    for eng in "" "--tree-walk"; do
        tag="${eng:-vm}"; tag="${tag#--}"
        run "$WORK/n" "$f.naab" $eng --timeout 3
        # Slow is a failure whatever the output says: a run killed from outside
        # loses its buffered stdout, so the marker cannot be required first.
        if [ "$ELAPSED" -gt 8 ]; then
            fail "N-$f/$tag" "the $f block cancelled --timeout (took ${ELAPSED}s)"
        elif [[ "$OUT" != *"$marker"* ]]; then
            # The block itself did not run, so the loop after it proves nothing.
            skip "N-$f/$tag" "the $f block did not run here"
        else
            pass "N-$f/$tag" "--timeout still applies after a $f block (${ELAPSED}s)"
        fi
    done
done

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP unmeasurable"
[ "$FAIL" -eq 0 ]
