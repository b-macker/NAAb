#!/usr/bin/env bash
# ============================================================
# test_gatelib.sh — the gate library proves gates failable, including its own
#
# gatelib.sh exists because a gate that cannot fail is indistinguishable from a
# gate that passes. This suite has to demonstrate that claim rather than assert
# it, so it registers a KNOWN-UNFAILABLE gate (GL-BAD, carrying the exact
# grep -E trailing-empty-alternative defect that made living-script_v2's L7-03
# and L10-01 match every line) and requires the self-test to catch it.
#
# GL-05 is the load-bearing case. Without it this suite would pass just as
# happily against a self-test that rubber-stamped everything.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../helpers/gatelib.sh"
source "$SCRIPT_DIR/../helpers/gate_selftest.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/gatelib-$$"
trap 'rm -rf "$TEST_TMP"' EXIT
FIX="$TEST_TMP/fixtures"
mkdir -p "$TEST_TMP"

# ------------------------------------------------------------------
# Fixtures: a good gate, and a deliberately broken one
# ------------------------------------------------------------------
mk() { mkdir -p "$FIX/$1/$2"; printf '%s\n' "$3" > "$FIX/$1/$2/stdout.txt"; }

mk GL-DEMO pass 'PHASE|INIT|start
CREATED|agent=builder
PHASE|INIT|end'
mk GL-DEMO fail 'PHASE|INIT|start
nothing useful happened here'

# The unfailable gate gets the same fixtures. Its fail/ fixture is non-empty,
# which is exactly the condition its broken pattern matches.
mk GL-BAD pass 'PHASE|INIT|start
CREATED|agent=builder'
mk GL-BAD fail 'PHASE|INIT|start
nothing useful happened here'

# ------------------------------------------------------------------
# Suite under test
# ------------------------------------------------------------------
gate_init "gatelib-demo"
gate_def GL-DEMO INIT "agent creation marker present"
gate_def GL-BAD  INIT "deliberately unfailable (trailing empty alternative)"

gate_GL_DEMO() {
    if grep_alt "$G_OUT" 'CREATED|agent='; then
        pass GL-DEMO "agent creation marker present"
    else
        fail GL-DEMO "no agent creation marker"
    fi
}

# The defect, preserved verbatim: under -E the trailing `|` is an empty
# alternative, so this matches any non-empty line.
gate_GL_BAD() {
    if echo "x" | grep -qE 'CREATED|agent=|'; then
        pass GL-BAD "matched"
    else
        fail GL-BAD "did not match"
    fi
}

# Deliberately EMPTY for the demo run. Pointing it at this file made the
# registry lint fire on GL-01..GL-06 — the assertion ids below, which are not
# registered while the demo suite is loaded — so the self-test exited non-zero
# for a reason unrelated to failability, and GL-03 passed even with
# unfailable-detection disabled. The lint gets its own isolated case (GL-07).
GATE_SOURCES=()

# ------------------------------------------------------------------
# Run the self-test and assert on its verdict
# ------------------------------------------------------------------
SELFTEST_OUT="$TEST_TMP/selftest.txt"
gate_selftest_run "$FIX" > "$SELFTEST_OUT" 2>&1; SELFTEST_RC=$?

# The self-test used gate_* counters; reset before asserting with them.
gate_init "test_gatelib"
gate_def GL-01 SELFTEST "good gate proven failable"
gate_def GL-02 SELFTEST "unfailable gate detected"
gate_def GL-03 SELFTEST "self-test exits non-zero when a gate is unprovable"
gate_def GL-04 SELFTEST "count_matches returns a usable integer on no match"
gate_def GL-05 SELFTEST "grep_alt cannot express a trailing empty alternative"
gate_def GL-06 SELFTEST "phase_complete requires both start and end"
gate_def GL-07 SELFTEST "registry lint catches an unregistered gate id"

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  gatelib: gates prove themselves failable                    |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

grep -q 'PROVEN.*GL-DEMO' "$SELFTEST_OUT" \
    && pass GL-01 "good gate proven failable" \
    || fail GL-01 "self-test did not prove the good gate" "$(head -20 "$SELFTEST_OUT")"

grep -q 'NOT FAILABLE.*GL-BAD' "$SELFTEST_OUT" \
    && pass GL-02 "unfailable gate detected" \
    || fail GL-02 "self-test did NOT catch the unfailable gate" "$(head -20 "$SELFTEST_OUT")"

[ "$SELFTEST_RC" -ne 0 ] \
    && pass GL-03 "self-test exits non-zero when a gate is unprovable" \
    || fail GL-03 "self-test exited 0 despite an unfailable gate"

# The "0\n0" trap: grep -c prints 0 AND exits 1, so `|| echo 0` yields a
# two-line string that every later integer test errors on.
: > "$TEST_TMP/empty.txt"
N=$(count_matches 'nothing-matches-this' "$TEST_TMP/empty.txt")
if [ "$N" = "0" ] && [ "$N" -eq 0 ] 2>/dev/null; then
    pass GL-04 "count_matches returns a usable integer on no match (got '$N')"
else
    fail GL-04 "count_matches returned an unusable value" "got '$N'"
fi

# grep_alt drops empty alternatives, so the defect cannot be expressed.
printf 'totally unrelated line\n' > "$TEST_TMP/unrel.txt"
if grep_alt "$TEST_TMP/unrel.txt" 'CREATED|agent=' '' ; then
    fail GL-05 "grep_alt matched an unrelated line — the empty alternative survived"
else
    pass GL-05 "grep_alt cannot express a trailing empty alternative"
fi

printf 'PHASE|X|start\n' > "$TEST_TMP/half.txt"
G_OUT="$TEST_TMP/half.txt"
if phase_complete X; then
    fail GL-06 "phase_complete accepted a start with no end"
else
    printf 'PHASE|X|start\nPHASE|X|end\n' > "$TEST_TMP/whole.txt"
    G_OUT="$TEST_TMP/whole.txt"
    phase_complete X \
        && pass GL-06 "phase_complete requires both start and end" \
        || fail GL-06 "phase_complete rejected a complete phase"
fi

# GL-07: the lint, isolated. A gate id emitted by a body but never gate_def'd
# is a gate nobody can prove; a declared id no run emits has silently stopped
# existing. Tested on its own file so its verdict cannot be confused with the
# failability verdicts above — which is precisely the mistake this suite made
# on its first run.
LINTSRC="$TEST_TMP/lintsrc.sh"
printf '%s\n' 'gate_GL_LINT() { pass "GL-UNREGISTERED" "never declared"; }' > "$LINTSRC"
(
    source "$SCRIPT_DIR/../helpers/gatelib.sh"
    source "$SCRIPT_DIR/../helpers/gate_selftest.sh"
    gate_init lint-demo
    gate_def GL-LINT LINT "declared"
    mkdir -p "$FIX/GL-LINT/fail"
    printf 'x\n' > "$FIX/GL-LINT/fail/stdout.txt"
    gate_GL_LINT() { fail GL-LINT "always fails"; }
    GATE_SOURCES=("$LINTSRC")
    gate_selftest_run "$FIX" 2>&1
) > "$TEST_TMP/lint.txt" 2>&1
if grep -q 'UNREGISTERED.*GL-UNREGISTERED' "$TEST_TMP/lint.txt"; then
    pass GL-07 "registry lint catches an unregistered gate id"
else
    fail GL-07 "lint missed an unregistered id" "$(head -10 "$TEST_TMP/lint.txt")"
fi

gate_print_summary
gate_exit
