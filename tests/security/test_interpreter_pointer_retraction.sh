#!/usr/bin/env bash
# ============================================================
# test_interpreter_pointer_retraction.sh -- F40
#
# Interpreter's constructor publishes three pointers to itself:
#   g_current_interpreter          (thread_local, interpreter.cpp:639, ALWAYS set)
#   DebugModule::setInterpreter    (file-static, set only if the debug module
#                                   is already loaded)
#   GovernanceEngine::setCurrent   (thread_local, set if governance_ exists)
#
# The destructor got two of them wrong in OPPOSITE directions. g_current_interpreter
# was never retracted, so it outlived its object; the other two were retracted
# UNCONDITIONALLY, so `p = make_unique<Interpreter>()` (cli/repl.cpp:104, the
# REPL's .clear -- new object constructed BEFORE the old is destroyed) let the
# departing destructor null pointers the live replacement had just published.
# Both are fixed by one rule: only retract a pointer that still refers to ME.
#
# WHAT THIS FILE DOES AND DOES NOT ESTABLISH -- read before trusting it.
#
# It does NOT discriminate the guarded fix from the unconditional one. That was
# measured, not assumed: an earlier version of this file passed 3/3 against a
# deliberately naive build. Every reader of g_current_interpreter
# (governance_checks.cpp:3738 execution contracts; governance_reports.cpp:2956
# and :3006 plugin rules) requires governance plugins or behavioural contracts
# configured, and the two paths that destroy an interpreter mid-thread -- the
# REPL's .clear and a pooled REST worker -- do not combine with those readers in
# anything a shell test can drive. The fix rests on a source trace plus a
# standalone ordering probe (ctor(new) runs before dtor(old); an unconditional
# clear then nulls a live pointer), both recorded in the PR. This file is a
# guard against gross breakage, not a proof of the fix, and should not be cited
# as one.
#
# It also does not cover the use-after-free itself. Proving a UAF needs ASAN
# plus a pooled worker serving /execute then /check with plugins configured; an
# exit-code assertion would be claiming absence of something it cannot observe.
#
# THE FIRST VERSION OF THIS FILE WAS A FALSE PASS, which is why the pre-check
# below exists. It invoked `naab-lang repl` -- not a valid subcommand. The REPL
# never started, and the assertions matched the USAGE TEXT: `grep -q "2"` hit
# the version string and `grep -qiE "int"` hit the words "interpreter" and
# "interactive". Two green assertions, subject never executed.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PASS=0; FAIL=0
ok()  { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); }

[ -x "$NAAB" ] || { echo "  FAIL [IP-00] naab binary missing at $NAAB"; exit 1; }

source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
W="$(mktemp -d)"
cleanup() { teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
cd "$W"
echo '{ "governance": { "version": "1.0", "mode": "audit" } }' > govern.json

# --- usability pre-check: did the REPL actually start? ---
# The REPL is `naab-lang` with NO subcommand. Anything else prints usage text,
# and usage text satisfies loose greps -- see the header.
REPL_OUT="$(printf '.exit\n' | timeout 60 "$NAAB" 2>&1)"
if ! echo "$REPL_OUT" | grep -q "NAAb REPL"; then
    echo "  FAIL [IP-01] REPL did not start (UNMEASURABLE, not a pass)"
    echo "$REPL_OUT" | head -3 | sed 's/^/       /'
    echo ""
    echo "interpreter pointer retraction: 0 passed, 1 failed"
    exit 1
fi
ok "IP-01" "REPL starts (pre-check: assertions below are about the REPL, not usage text)"

echo "=== Group A: the REPL survives .clear and keeps evaluating ==="
OUT_A="$(printf 'let a = 41\n.clear\nlet b = 1 + 1\nb\n.exit\n' | timeout 60 "$NAAB" 2>&1)"; rc=$?
if [ "$rc" -ge 128 ]; then
    bad "A-01" "REPL died with signal (exit $rc) across .clear"
elif ! echo "$OUT_A" | grep -q "Environment cleared."; then
    bad "A-01" ".clear did not run -- the scenario under test never happened"
    echo "$OUT_A" | head -5 | sed 's/^/       /'
elif echo "$OUT_A" | grep -qE '(^|[^0-9])2([^0-9]|$)' && echo "$OUT_A" | grep -q "Bye!"; then
    ok "A-01" "REPL ran .clear, evaluated after it, and exited cleanly"
else
    bad "A-01" "REPL did not evaluate after .clear (exit $rc)"
    echo "$OUT_A" | head -8 | sed 's/^/       /'
fi

echo "=== Group B: ordinary single-interpreter runs unaffected by the guard ==="
cat > plain.naab <<'EOF'
main { print("PLAIN_OK") }
EOF
if timeout 60 "$NAAB" plain.naab 2>/dev/null | grep -q "PLAIN_OK"; then
    ok "B-01" "single-interpreter run unaffected"
else
    bad "B-01" "the guard changed the common path"
fi

echo ""
echo "interpreter pointer retraction: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
