#!/usr/bin/env bash
# ============================================================
# test_contradiction_coverage.sh — every contradiction pattern still fires
#
# WHY THIS EXISTS
#
# CONTRA-007 shipped DEAD: it iterated a set that its own loader empties at parse
# time, so the conflict it looks for could not exist by the time it ran. It was
# configured, believed working, and structurally incapable of firing — and it was
# found by accident, from a different direction, not by review.
#
# All eleven patterns were then probed BY HAND once (2026-08-09) and confirmed
# live. Nothing pinned them afterwards, so the next loader change that normalises
# an input away re-creates CONTRA-007 silently. Five had no test of any kind:
# CONTRA-001, 003, 004, 005 and 008. This suite covers those five.
#
# (002, 007, 009, 010, 011 and 012 are already covered elsewhere —
# test_config_contradictions.sh, test_inert_limits.sh,
# test_restrictions_enabled_key.sh, test_governance_validity.sh. This file
# deliberately does not duplicate them.)
#
# EVERY CHECK GETS A NEGATIVE FIXTURE
#
# "CONTRA-004 appears in the output" is satisfied by an engine that prints every
# pattern id unconditionally, and equally by one that prints them on any config
# error at all. So each pattern is run twice: once against a config that meets
# its condition, and once against a config that differs ONLY in the field that
# condition tests. The negative half is what makes the positive half evidence.
#
# GATE IDS ARE CCOV-, NOT CC-
#
# test_config_contradictions.sh already uses CC-01..CC-04 for different gates,
# and both suites print into the same run log twenty lines apart. A bare
# `grep CC-01` over that log returns two unrelated assertions, which is precisely
# the ambiguity that has misdirected forensic passes in this campaign before.
#
# CONTRA-001 IS THE ODD ONE
#
# It is hardcoded SOFT rather than taking contradiction_detection.max_level, so
# it ABORTS THE LOAD instead of warning. Its assertion therefore reads the load
# error, not the advisory stream, and it doubles as the regression for the
# `Rule:` field being empty on that path — the one contradiction that stops a run
# was also the only one that would not name itself.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/contracov-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

cleanup() { rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP/trust"

if [ ! -x "$NAAB" ]; then
    skip "CCOV-00" "build/naab-lang not found"
    echo "  Results: 0 passed, 0 failed, 1 skipped"
    exit 0
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Contradiction coverage: can each pattern still fire?         |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# Run one config, echo combined output. An isolated (empty) trust store keeps
# unsigned fixtures from hitting INTEGRITY BLOCK before the check ever runs.
run_cfg() {
    local dir="$1"
    mkdir -p "$dir"
    printf 'main { print("ok") }\n' > "$dir/t.naab"
    (cd "$dir" && NAAB_TRUST_STORE_DIR="$TEST_TMP/trust" \
        timeout 60s "$NAAB" t.naab 2>&1)
}

# Match the FINDING form, never the bare id. The engine echoes the absolute path
# of the config it loaded, so a fixture directory named after the pattern makes a
# bare `grep CONTRA-004` match the path on every run, positive and negative
# alike. That is how the first draft of this suite reported all five negatives
# as firing when the engine was behaving correctly. Directories are named by
# gate, and the grep is anchored to `contradiction.<id>`, which only ever appears
# in a finding line.
fired() { grep -qE "contradiction\.$1(\b|$)" <<< "$2"; }

# $1 gate  $2 pattern id  $3 positive json  $4 negative json
check_pattern() {
    local gate="$1" pid="$2" pos_json="$3" neg_json="$4"
    local pdir="$TEST_TMP/$gate-pos" ndir="$TEST_TMP/$gate-neg"
    mkdir -p "$pdir" "$ndir"
    printf '%s\n' "$pos_json" > "$pdir/govern.json"
    printf '%s\n' "$neg_json" > "$ndir/govern.json"
    local pout nout
    pout=$(run_cfg "$pdir"); nout=$(run_cfg "$ndir")
    if ! fired "$pid" "$pout"; then
        fail "$gate" "$pid did not fire on a config that meets its condition" \
             "the pattern may be structurally unable to fire — this is the CONTRA-007 shape"
    elif fired "$pid" "$nout"; then
        fail "$gate" "$pid also fired on the negative fixture" \
             "it is not keyed to the condition it claims to test, so the positive result proves nothing"
    else
        pass "$gate" "$pid fires on its condition and not otherwise"
    fi
}

# --- CCOV-01: CONTRA-001 — shell disabled, 'shell' still an allowed language ---
# Hardcoded SOFT: aborts the load. Negative flips ONLY the capability.
CC1_POS='{ "version":"5.0","mode":"enforce","capabilities":{"shell":{"enabled":false}},"languages":{"allowed":["python","shell"]} }'
CC1_NEG='{ "version":"5.0","mode":"enforce","capabilities":{"shell":{"enabled":true}},"languages":{"allowed":["python","shell"]} }'
check_pattern "CCOV-01" "CONTRA-001" "$CC1_POS" "$CC1_NEG"

# --- CCOV-02: the SOFT path names the pattern (regression for an empty Rule:) ---
# ADVISORY findings carry the rule through the finding record, but a SOFT
# contradiction aborts the load and the operator sees only the formatted error.
# That path passed "" as the rule, so `Rule:` rendered empty on the ONE pattern
# that stops a run. Assert the id is in the Rule line specifically, not merely
# somewhere in the output — the description alone would satisfy the latter.
RULE_OUT=$(run_cfg "$TEST_TMP/CCOV-01-pos")
if grep -qE '^[[:space:]]*Rule:[[:space:]]*contradiction\.CONTRA-001[[:space:]]*$' <<< "$RULE_OUT"; then
    pass "CCOV-02" "a load-aborting contradiction names its pattern in Rule:"
else
    fail "CCOV-02" "the Rule: line does not name the pattern" \
         "got: $(grep -E '^[[:space:]]*Rule:' <<< "$RULE_OUT" | head -1 | sed 's/^[[:space:]]*//')"
fi

# --- CCOV-03: CONTRA-003 — no_hardcoded_urls with a non-empty allowed_hosts ---
CC3_POS='{ "version":"5.0","mode":"enforce","code_quality":{"no_hardcoded_urls":{"enabled":true}},"capabilities":{"network":{"enabled":true,"allowed_hosts":["api.example.com"]}} }'
CC3_NEG='{ "version":"5.0","mode":"enforce","code_quality":{"no_hardcoded_urls":{"enabled":false}},"capabilities":{"network":{"enabled":true,"allowed_hosts":["api.example.com"]}} }'
check_pattern "CCOV-03" "CONTRA-003" "$CC3_POS" "$CC3_NEG"

# --- CCOV-04: CONTRA-004 — complexity floor >= 20 vs duplicate_calls <= 2 -------
# Negative raises ONLY the duplicate_calls threshold past the boundary (2 -> 5),
# so it also pins the comparison, not just the pair of features being enabled.
CC4_POS='{ "version":"5.0","mode":"enforce","code_quality":{"complexity_floor":{"enabled":true,"min_score":25},"duplicate_calls":{"enabled":true,"threshold":2}} }'
CC4_NEG='{ "version":"5.0","mode":"enforce","code_quality":{"complexity_floor":{"enabled":true,"min_score":25},"duplicate_calls":{"enabled":true,"threshold":5}} }'
check_pattern "CCOV-04" "CONTRA-004" "$CC4_POS" "$CC4_NEG"

# --- CCOV-05: CONTRA-005 — filesystem 'none' with a file-shaped taint sink -----
# Negative keeps filesystem 'none' and taint on, changing only the SINK, so the
# substring match on "file" is what is under test.
CC5_POS='{ "version":"5.0","mode":"enforce","capabilities":{"filesystem":{"mode":"none"}},"taint_tracking":{"enabled":true,"sinks":["file.write"]} }'
CC5_NEG='{ "version":"5.0","mode":"enforce","capabilities":{"filesystem":{"mode":"none"}},"taint_tracking":{"enabled":true,"sinks":["http.post"]} }'
check_pattern "CCOV-05" "CONTRA-005" "$CC5_POS" "$CC5_NEG"

# --- CCOV-06: CONTRA-008 — a contract on a function banned for a language ------
# Negative keeps both the contract and the ban list, and only renames the banned
# function, so the NAME COMPARISON is what is pinned.
CC8_POS='{ "version":"5.0","mode":"enforce","contracts":{"functions":{"compute_total":{"description":"totals","return_type":"int"}}},"languages":{"per_language":{"python":{"banned_functions":["compute_total"]}}} }'
CC8_NEG='{ "version":"5.0","mode":"enforce","contracts":{"functions":{"compute_total":{"description":"totals","return_type":"int"}}},"languages":{"per_language":{"python":{"banned_functions":["something_else"]}}} }'
check_pattern "CCOV-06" "CONTRA-008" "$CC8_POS" "$CC8_NEG"

echo ""
echo -e "${CYAN}==============================================================${NC}"
echo -e "  Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC}"
echo -e "${CYAN}==============================================================${NC}"
[ "$FAIL_COUNT" -eq 0 ] || { echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; }
exit 0
