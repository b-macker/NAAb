#!/usr/bin/env bash
# ============================================================
# test_marshal_recursion_depth.sh -- F42: cyclic polyglot returns must not crash
#
# A self-referential structure returned from a polyglot block recursed without
# limit in the INBOUND marshallers (pyObjectToValue, jsToValue) and killed the
# process with SIGSEGV. The OUTBOUND twin, valueToPyObject, has carried a depth
# limit since V-RT-005 -- only the return direction was unguarded.
#
# WHY A CRASH IS THE POINT, not the rejection. A SIGSEGV renders NO verdict: it
# neither blocks nor allows, it exits on a signal rather than a documented exit
# code, and nothing downstream can tell it apart from a kill. Same reasoning as
# the SECRET_PATTERNS ReDoS (tests/security/test_secret_scan_redos.sh): the
# governance answer was never computed, so "did it pass?" has no answer.
#
# Group A  the crash regression -- cyclic input must not SIGSEGV (exit 139)
# Group B  POSITIVE CONTROL -- ordinary nested structures must STILL marshal.
#          Without this the suite passes for a build that rejects everything,
#          which is the cheap wrong fix for a depth bug.
# Group C  the GIL/leak control -- a second polyglot block must still run after
#          a rejected one. The throw escapes a region holding the Python GIL
#          whose release is a manual call, not a scope guard, so an unguarded
#          throw deadlocks the NEXT block rather than the failing one. A test
#          that only checks the error message cannot see that.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PASS=0; FAIL=0
ok()  { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); }

if [ ! -x "$NAAB" ]; then echo "  FAIL [MR-00] naab binary missing at $NAAB"; exit 1; fi

# HAZARD: this suite writes an UNSIGNED govern.json. With any key installed in
# the real trust store (~/.naab/trusted-keys), an unsigned config is an
# INTEGRITY BLOCK -- exit 3, which --no-governance cannot escape. Group A reads
# "not 139" as PASS, so on a machine with keys installed every probe would exit 3
# and the crash regression would report VERIFIED without a single polyglot block
# running. Isolating the trust store is what keeps that from being a false pass.
# tests/self-audit/test_coverage_visibility.sh CV-02 is the gate that catches a
# suite shipping without this; it caught this one.
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust

W="$(mktemp -d)"
cleanup() { teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
cd "$W"
echo '{ "governance": { "version": "1.0", "mode": "audit" } }' > govern.json

# --- instrument usability: are the executors actually live on this platform? ---
# Without this, "no crash" is indistinguishable from "never ran" -- the probe
# would report the fix verified on a box with no Python at all.
cat > _alive_py.naab <<'EOF'
main {
    let x = <<python
"PY_ALIVE"
>>
    print(x)
}
EOF
cat > _alive_js.naab <<'EOF'
main {
    let x = <<javascript
"JS_ALIVE"
>>
    print(x)
}
EOF
PY_LIVE=0; JS_LIVE=0
"$NAAB" _alive_py.naab 2>/dev/null | grep -q PY_ALIVE && PY_LIVE=1
"$NAAB" _alive_js.naab 2>/dev/null | grep -q JS_ALIVE && JS_LIVE=1

run_exit() { "$NAAB" "$1" >/dev/null 2>&1; echo $?; }

echo "=== Group A: a cyclic polyglot return must not crash the process ==="
cat > cyc_py.naab <<'EOF'
main {
    let x = <<python
a = []
a.append(a)
a
>>
}
EOF
cat > cyc_js.naab <<'EOF'
main {
    let x = <<javascript
let a = [];
a.push(a);
a;
>>
}
EOF
for lang in py js; do
    live_var="$(echo "$lang" | tr '[:lower:]' '[:upper:]')_LIVE"
    if [ "${!live_var}" -ne 1 ]; then
        echo "  SKIP [A-$lang] $lang executor not available on this platform (UNMEASURABLE, not PASS)"
        continue
    fi
    rc="$(run_exit "cyc_$lang.naab")"
    if [ "$rc" -eq 139 ]; then
        bad "A-$lang" "cyclic $lang return SIGSEGV (exit 139) -- crash regression"
    else
        ok "A-$lang" "cyclic $lang return did not crash (exit $rc)"
    fi
done

echo "=== Group B: POSITIVE CONTROL -- ordinary nesting must still marshal ==="
cat > deep_py.naab <<'EOF'
main {
    let x = <<python
{"a": [1, 2, {"b": [3, 4, {"c": "NESTED_OK"}]}]}
>>
    print(x["a"][2]["b"][2]["c"])
}
EOF
if [ "$PY_LIVE" -eq 1 ]; then
    if "$NAAB" deep_py.naab 2>/dev/null | grep -q "NESTED_OK"; then
        ok "B-01" "ordinary nested python structure still marshals"
    else
        bad "B-01" "depth guard is over-broad -- legitimate nesting no longer marshals"
    fi
else
    echo "  SKIP [B-01] python executor unavailable (UNMEASURABLE)"
fi

echo "=== Group C: a rejected block must not wedge the NEXT one (GIL release) ==="
cat > after_py.naab <<'EOF'
main {
    try {
        let bad = <<python
a = []
a.append(a)
a
>>
    } catch (e) {
        print("caught")
    }
    let good = <<python
"SECOND_BLOCK_RAN"
>>
    print(good)
}
EOF
if [ "$PY_LIVE" -eq 1 ]; then
    out="$(timeout 30 "$NAAB" after_py.naab 2>&1)"; rc=$?
    if [ "$rc" -eq 124 ]; then
        bad "C-01" "TIMED OUT -- GIL held after the rejected block deadlocked the next one"
    elif echo "$out" | grep -q "SECOND_BLOCK_RAN"; then
        ok "C-01" "a second python block runs after a rejected cyclic one"
    else
        bad "C-01" "second python block did not run (exit $rc)"
    fi
else
    echo "  SKIP [C-01] python executor unavailable (UNMEASURABLE)"
fi

echo ""
echo "marshal recursion depth: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
