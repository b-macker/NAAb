#!/usr/bin/env bash
# ============================================================
# test_polyglot_line_numbers.sh — a polyglot block must not shift later lines
#
# THE FAILURE THIS CATCHES
#
# The lexer skipped the newline after `<<lang` with BOTH a manual line_++ and
# advance(), which counts newlines itself. Every polyglot block therefore added
# one phantom line, and every line number after it was off by one per block.
# Visible as wrong error locations -- and, worse, invisible in the governance
# checks that look a function up BY LINE (extractFunctionWithDecl for intent
# validation and contracts): they read the wrong function's text. Found adding
# a Python step to examples/agent_harness, where a function after the block was
# refused as a "rubber stamp" with no branches although it had several.
#
#   LN-01  an error after one block reports its real line
#   LN-02  an error after three blocks reports its real line (drift was
#          cumulative)
#   LN-03  intent validation reads the right body for a function after a block
#          (it has branches -> no rubber-stamp refusal)
#   LN-04  CONTROL: the same rubber-stamp check still fires for a function that
#          really has no branches -- LN-03 cannot pass by the check going quiet
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

W="${TMPDIR:-/tmp}/naab-pgline-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }

echo "=== Line numbers after polyglot blocks ==="

# javascript (QuickJS) is in-process and always built, so these do not depend
# on a Python or Node toolchain.
printf '{"version":"5.0","mode":"enforce","languages":{"allowed":["javascript"]}}\n' > "$W/govern.json"

cat > "$W/one.naab" <<'EOF'
fn a() {
  let x = <<javascript
1 + 1
>>
  return x
}

fn b() {
  throw "boom at line 9"
}
main { b() }
EOF
out=$( cd "$W" && "$NAAB" one.naab 2>&1 )
if grep -q 'one.naab:9\]' <<<"$out"; then ok "LN-01" "an error after one block reports line 9"
else bad "LN-01" "wrong line after one block" "$(grep -m1 'at b()' <<<"$out")"; fi

cat > "$W/three.naab" <<'EOF'
fn a() {
  let x = <<javascript
1
>>
  let y = <<javascript
2
>>
  let z = <<javascript
3
>>
  return x + y + z
}

fn b() {
  throw "boom at line 15"
}
main { b() }
EOF
out=$( cd "$W" && "$NAAB" three.naab 2>&1 )
if grep -q 'three.naab:15\]' <<<"$out"; then ok "LN-02" "an error after three blocks reports line 15"
else bad "LN-02" "wrong line after three blocks" "$(grep -m1 'at b()' <<<"$out")"; fi

# LN-03/04: intent validation looks the function up by line.
mk_intent() {  # $1 = body of check_value
    cat > "$W/intent.naab" <<EOF
fn render(x) {
  let s = <<javascript[x]
String(x)
>>
  return s
}

fn check_value(value) {
$1
}
main { print(render(check_value(3))) }
EOF
    printf '{"version":"5.0","mode":"enforce","languages":{"allowed":["javascript"]},"polyglot":{"variable_binding":{"require_explicit":"hard"}},"code_quality":{"intent_validation":{"level":"hard","missing_level":"advisory","project_intent":"check values","function_intents":{"check_value":"check value"}}}}\n' > "$W/govern.json"
}
mk_intent '  if value < 0 {
    throw "negative value rejected"
  }
  return value'
out=$( cd "$W" && "$NAAB" intent.naab 2>&1 ); rc=$?
if [ $rc -eq 0 ] && ! grep -q 'Rubber-stamp' <<<"$out"; then ok "LN-03" "a function after a block is validated against its own body"
else bad "LN-03" "the function after the block was misread (rc=$rc)" "$(grep -m1 -E 'Rubber|Intent' <<<"$out")"; fi

mk_intent '  return value'
out=$( cd "$W" && "$NAAB" intent.naab 2>&1 ); rc=$?
if [ $rc -ne 0 ] && grep -q "Rubber-stamp detected in 'check_value'" <<<"$out"; then
    ok "LN-04" "CONTROL: a branchless check_value is still refused"
else bad "LN-04" "the rubber-stamp check did not fire on a branchless body (rc=$rc)"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
