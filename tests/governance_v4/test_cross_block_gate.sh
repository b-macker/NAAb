#!/usr/bin/env bash
# test_cross_block_gate.sh -- the post-run cross-block audit, made blocking by
# taint_tracking.gate_cross_block (A127, A121(b))
#
# After a run, `pass2.cross_block_taint` judges every block that receives bound
# variables after an earlier block produced output: a flow is unsanitized when
# tainted data is bound into it, and with gate_cross_block it is enforced at
# cross_block_level. Two defects met here.
#   A127  the flow was judged by the FIRST bound variable alone (56f25210 wrote
#         it when the audit was advisory only; 3e3013dd made it blocking), so a
#         block bound [clean, tainted] passed a gate that refused [tainted, clean].
#   A121(b) the audit read each variable's NAME in the engine's run-wide taint
#         set, which the VM wrote only from the bound-variable check -- the mark
#         PR 3 removes. The VM now hands the audit each bound value's own taint
#         (PolyglotExecutionRecord::bound_tainted).
# No suite exercised a real cross-block flow before this one: test_depth_runtime
# T7 and T17 pass whether or not the gate works.
#
# The earlier block's output must be captured for a flow to exist; the Ruby
# executor captures what a block writes to stderr. A block's value is tainted
# whatever its inputs (V-GOV-006), so `a` below is tainted data.
#
#   XB-N    CONTROL: the interpreter runs a plain program (a dead one FAILS here,
#           rather than reading as "no Ruby")
#   XB-V    viability: a Ruby block runs here (else every arm is UNMEASURABLE)
#   XB-00   CONTROL: [a] -- the tainted output alone -- is refused at the end of
#           the run (exit 3, after RAN), as on master
#   XB-01   [b, a] -- a clean variable first -- is refused too (A127)
#   XB-02   [a, b] is refused (as on master)
#   XB-03c  CONTROL: [b] -- clean only -- passes (exit 0): the gate does not
#           refuse every flow
#
# Kill table (each mutant rebuilt, suite re-run; observed 2026-11-08):
#   judge the first bound variable only (the pre-A127 loop)  -> XB-01 red
#   the audit reads the variable's name, not the value         -> XB-00, XB-01, XB-02 red
#   every flow unsanitized                                     -> XB-03c red
# Red first (observed, master 977e7a40): XB-01 fails (exit 0); XB-00, XB-02 and
# XB-03c pass.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_xblock_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then echo "UNMEASURABLE: naab-lang not built"; exit 1; fi

CFG='{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "languages": {"allowed": ["ruby"]},
 "taint_tracking": {"enabled": true, "level": "hard", "sources": ["env.get"], "sinks": ["print"],
  "sanitizers": ["sanitize_"], "gate_cross_block": true, "cross_block_level": "hard"}}'

# run ID BOUND -> OUT, RC. `a` is the first block's (tainted) output, `b` a constant.
run() {
    local d="$W/$1"; mkdir -p "$d"
    printf '%s\n' "$CFG" > "$d/govern.json"
    cat > "$d/p.naab" <<EOF
main {
    let a = <<ruby
\$stderr.puts "first"
"v"
>>
    let b = "clean"
    let r = <<ruby[$2]
\$stderr.puts "second"
1
>>
    print("RAN")
}
EOF
    RC=0; OUT=$(cd "$d" && timeout 60s "$NAAB" run p.naab 2>&1) || RC=$?
}
has() { grep -q -F -- "$1" <<<"$OUT"; }
refused() { [ "$RC" -eq 3 ] && has "RAN" && has "unsanitized cross-block data flow"; }

echo "=== the cross-block gate (A127, A121(b)) ==="

# XB-N: the interpreter runs a program at all. Without it a dead interpreter
# would read as "no Ruby here" below, every arm UNMEASURABLE.
d="$W/n"; mkdir -p "$d"
printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}}\n' > "$d/govern.json"
printf 'main {\n    print("XB" + "-ALIVE")\n}\n' > "$d/p.naab"
NOUT=$(cd "$d" && timeout 60s "$NAAB" run p.naab 2>&1); NRC=$?
if [ "$NRC" -eq 0 ] && grep -q -F "XB-ALIVE" <<<"$NOUT"; then
    pass XB-N "CONTROL: the interpreter runs a plain program"
else
    fail XB-N "the interpreter did not run a plain program (rc=$NRC)" "$NOUT"
    echo ""; echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$UNM"; exit 1
fi

d="$W/v"; mkdir -p "$d"
printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}, "languages": {"allowed": ["ruby"]}}\n' > "$d/govern.json"
printf 'main {\n    let a = <<ruby\n"VIA" + "BLE"\n>>\n    print("V=" + string(a))\n}\n' > "$d/p.naab"
VOUT=$(cd "$d" && timeout 60s "$NAAB" run p.naab 2>&1); VRC=$?
if [ "$VRC" -ne 0 ] || ! grep -q -F "V=VIABLE" <<<"$VOUT"; then
    unm XB-V "no usable Ruby executor here (rc=$VRC): $(grep -m1 -iE 'error|not found|executor' <<<"$VOUT")"
    for a in XB-00 XB-01 XB-02 XB-03c; do unm "$a" "needs a Ruby executor"; done
    echo ""; echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$UNM"; exit 0
fi
pass XB-V "a Ruby block runs here"

run xb00 "a"
if refused; then pass XB-00 "CONTROL: the tainted output bound alone is refused at the end of the run"
else fail XB-00 "expected exit 3 after RAN with the cross-block refusal (rc=$RC)" "$OUT"; fi

run xb01 "b, a"
if refused; then pass XB-01 "[b, a]: a clean variable first does not hide the tainted one (A127)"
else fail XB-01 "[b, a] was not refused (rc=$RC)" "$OUT"; fi

run xb02 "a, b"
if refused; then pass XB-02 "[a, b] is refused"
else fail XB-02 "[a, b] was not refused (rc=$RC)" "$OUT"; fi

run xb03 "b"
if [ "$RC" -eq 0 ] && has "RAN" && ! has "unsanitized cross-block"; then
    pass XB-03c "CONTROL: a clean variable alone passes"
else fail XB-03c "the clean flow was refused or the run failed (rc=$RC)" "$OUT"; fi

echo ""
echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$UNM"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
