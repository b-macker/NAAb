#!/usr/bin/env bash
# ============================================================
# test_taint_engine_parity.sh — VM vs tree-walker taint agreement
#
# Taint tracking is implemented TWICE: the tree-walker walks the AST
# (governance_taint.cpp) while the VM carries taint on taint_stack_. CLAUDE.md
# says changes "must be made in both paths", and nothing checked that.
#
# tests/differential/ cannot: diff_runner.py runs both engines with
# --no-governance, so it compares language semantics with taint switched off.
# This is the governance-on counterpart.
#
# Two real defects were found by running this matrix, and they were cancelling
# each other:
#   1. VarDeclStmt left lastReturnWasTainted set, so the declaration AFTER a
#      taint source was marked tainted whatever it contained — a false positive
#      on clean data (clean_after_source below).
#   2. expressionContainsTaint() did not handle TryCatchExpr, so a value
#      produced by `try {} catch {}` lost its taint — a false negative
#      (try_expr below), invisible while (1) was tainting it anyway.
#
# The pass condition is AGREEMENT plus a per-engine expectation, not agreement
# alone: two engines that both miss a taint agree perfectly.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/taint-parity-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"

cat > "$TEST_TMP/govern.json" << 'EOF'
{
  "version": "5.0",
  "mode": "monitor",
  "security": { "sandbox_level": "elevated" },
  "taint_tracking": {
    "enabled": true,
    "level": "advisory",
    "sources": ["env.get", "io.read_line", "file.read", "polyglot_output"],
    "sinks": ["shell_exec", "python_exec", "file.write", "file.append"],
    "sanitizers": ["validate_", "sanitize_", "escape_"]
  }
}
EOF
(cd "$TEST_TMP" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|   Taint tracking: VM vs tree-walker must agree               |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# Violations are reported in TWO formats: checkTaintedSink() names the variable
# ("taint_tracking.sink_violation"), while checkExpressionTaintedSink()'s
# expression path emits "Taint tracking violation: ...". Matching only the first
# made a working engine look broken and produced a bogus finding — the detector
# has to cover both.
count_violations() {  # $1=file $2=extra flags
    (cd "$TEST_TMP" && timeout 30s "$NAAB" ${2:-} "$1.naab" 2>&1) \
        | grep -ciE "sink_violation|Taint tracking violation" || true
}

# scenario|expectation  (tainted = must flag, clean = must not)
SCENARIOS="
direct|tainted
concat|tainted
interp|tainted
list_elem|tainted
dict_val|tainted
if_expr|tainted
try_expr|tainted
match_arm|tainted
null_coalesce|tainted
clean_only|clean
clean_after_source|clean
sanitized|clean
"

w() { cat > "$TEST_TMP/$1.naab"; }
w direct        <<'EOF'
use env
use file
main { let t = env.get("HOME") let x = t file.write("o.txt", x) }
EOF
w concat        <<'EOF'
use env
use file
main { let t = env.get("HOME") let x = "p" + t file.write("o.txt", x) }
EOF
w interp        <<'EOF'
use env
use file
main { let t = env.get("HOME") let x = "v=${t}" file.write("o.txt", x) }
EOF
w list_elem     <<'EOF'
use env
use file
main { let t = env.get("HOME") let l = [t, "c"] file.write("o.txt", l[0]) }
EOF
w dict_val      <<'EOF'
use env
use file
main { let t = env.get("HOME") let d = {k: t} file.write("o.txt", d.get("k")) }
EOF
w if_expr       <<'EOF'
use env
use file
main { let t = env.get("HOME") let x = if true { t } else { "c" } file.write("o.txt", x) }
EOF
# The TryCatchExpr gap: value-producing try, taint must flow through both arms.
w try_expr      <<'EOF'
use env
use file
main { let t = env.get("HOME") let x = try { t } catch (e) { "" } file.write("o.txt", x) }
EOF
w match_arm     <<'EOF'
use env
use file
main { let t = env.get("HOME") let x = match 1 { 1 => t, _ => "c" } file.write("o.txt", x) }
EOF
w null_coalesce <<'EOF'
use env
use file
main { let t = env.get("HOME") let x = t ?? "d" file.write("o.txt", x) }
EOF
w clean_only    <<'EOF'
use file
main { let c = "clean" let x = c file.write("o.txt", x) }
EOF
# The stale-flag leak: the declaration immediately AFTER a taint source must not
# inherit taint from it. Adjacency matters — with more statements in between the
# leak lands on a variable that never reaches a sink and hides.
w clean_after_source <<'EOF'
use env
use file
main { let t = env.get("HOME") let c = "clean" let x = c file.write("o.txt", x) }
EOF
w sanitized     <<'EOF'
use env
use file
fn sanitize_it(s) { return "safe:" + s.length() }
main { let t = env.get("HOME") let x = sanitize_it(t) file.write("o.txt", x) }
EOF

TAINTED_SEEN=0
for entry in $SCENARIOS; do
    [ -z "$entry" ] && continue
    name="${entry%%|*}"; expect="${entry##*|}"
    vm=$(count_violations "$name" "")
    tw=$(count_violations "$name" "--tree-walk")

    if [ "$vm" != "$tw" ]; then
        fail "PARITY-$name" "engines disagree (VM=$vm tree-walk=$tw)" \
             "taint is implemented twice; a divergence means one path is wrong"
        continue
    fi
    # Agreement alone is not enough — two engines that both miss it agree.
    if [ "$expect" = "tainted" ]; then
        if [ "$vm" -gt 0 ]; then
            TAINTED_SEEN=$((TAINTED_SEEN + 1))
            pass "PARITY-$name" "both engines flag the taint ($vm)"
        else
            fail "PARITY-$name" "both engines MISSED the taint" \
                 "agreement on a false negative — taint reached a sink unflagged"
        fi
    else
        if [ "$vm" -eq 0 ]; then
            pass "PARITY-$name" "both engines leave clean data alone"
        else
            fail "PARITY-$name" "both engines flagged CLEAN data ($vm)" \
                 "a false positive makes every taint count untrustworthy"
        fi
    fi
done

# Control: if the harness never produced a single violation, every "clean" case
# above passed for the wrong reason and the file proves nothing.
if [ "$TAINTED_SEEN" -ge 5 ]; then
    pass "PARITY-control" "taint tracking is live in this environment ($TAINTED_SEEN positive cases)"
else
    fail "PARITY-control" "taint never fired — the clean cases pass vacuously" \
         "only $TAINTED_SEEN positive detections; expected >= 5"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}Failures:${NC}$FAILURES"
    exit 1
fi
exit 0
