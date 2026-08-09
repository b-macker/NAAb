#!/usr/bin/env bash
# ============================================================
# gatelib.sh — one definition of a test gate, and the machinery that makes a
# gate's failure case provable.
#
# WHY THIS EXISTS
#
# 146 shell suites in this repo each declare their own pass/fail/skip. That
# duplication is survivable. What is not survivable is that a gate can be
# written which cannot fail, and nothing notices — because a passing gate and
# an unfailable gate produce identical output. Found repeatedly:
#
#   * seven assertions in tests/security + tests/governance_v4 where every
#     reachable branch passed (three were concealing real defects);
#   * examples/living-script_v2 L7-03 and L10-01, whose grep -E patterns ended
#     in an empty alternative and so matched every non-empty line;
#   * a JS timeout suite that had never once run a JS timeout.
#
# None were caught by review. Every one was caught by running the degraded
# case. So the degraded case stops being a thing someone remembers to do:
# every gate declares itself in a registry, ships a fixture on which it MUST
# fail, and `gate_selftest.sh` enforces both — keylessly, in CI.
#
# THE CONTRACT
#
# A gate is a shell function `gate_<id>` reading exactly two globals:
#     G_OUT   path to a file holding captured run stdout
#     G_TELE  path to telemetry.jsonl
# and calling pass/fail/skip/gk_fail exactly once. Nothing in a gate may know
# whether those bytes came from a live run or a fixture. That is the entire
# refactor that makes a gate independently invocable.
#
# USAGE
#     source "$SCRIPT_DIR/../../tests/helpers/gatelib.sh"
#     gate_init "my-suite"
#     gate_def L1-01 INIT "agent created"
#     gate_L1_01() { grep -q 'CREATED' "$G_OUT" && pass L1-01 "agent created" \
#                                              || fail L1-01 "no agent"; }
#     G_OUT=$WORKDIR/stdout.txt G_TELE=$WORKDIR/telemetry.jsonl
#     gate_run L1-01
#     gate_summary_json "$RESULTS/summary.json" keyed
#     gate_exit
# ============================================================

# --- counters -------------------------------------------------------------
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0
FAILURES=""
# Machine-readable failure IDs. summary.json used to record only a COUNT, and
# the identities lived solely in results_<ts>.txt — which one keyed run did not
# commit, leaving "fail: 1" in the permanent record with nothing naming it.
FAILED_IDS=""
EMITTED_IDS=""

GATE_SUITE=""
GATE_QUIET="${GATE_QUIET:-}"      # self-test sets this to suppress per-gate echo

# Registry: parallel indexed arrays rather than an associative array, so this
# works on bash 3.2 (macOS) as well as 4+.
GATE_IDS=(); GATE_PHASES=(); GATE_DESCS=()

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

gate_init() {
    GATE_SUITE="${1:-suite}"
    gate_reset_counters
    GATE_IDS=(); GATE_PHASES=(); GATE_DESCS=()
}

gate_reset_counters() {
    PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0
    FAILURES=""; FAILED_IDS=""; EMITTED_IDS=""
}

# --- verdicts -------------------------------------------------------------
_gate_note_emitted() { EMITTED_IDS="${EMITTED_IDS}${EMITTED_IDS:+ }$1"; }

pass() {
    local id="$1" desc="${2:-}"
    PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); _gate_note_emitted "$id"
    [ -n "$GATE_QUIET" ] || echo -e "  ${GREEN}PASS${NC} [$id] $desc"
}

fail() {
    local id="$1" desc="${2:-}" detail="${3:-}"
    FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); _gate_note_emitted "$id"
    FAILURES="${FAILURES}\n  [$id] $desc${detail:+ -- $detail}"
    FAILED_IDS="${FAILED_IDS}${FAILED_IDS:+,}\"$id\""
    [ -n "$GATE_QUIET" ] || { echo -e "  ${RED}FAIL${NC} [$id] $desc"
        [ -n "$detail" ] && echo -e "       ${RED}-> $detail${NC}"; }
    return 0
}

skip() {
    local id="$1" desc="${2:-}" reason="${3:-}"
    SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1)); _gate_note_emitted "$id"
    [ -n "$GATE_QUIET" ] || echo -e "  ${YELLOW}SKIP${NC} [$id] $desc${reason:+ -- $reason}"
}

# --- governance-kill awareness -------------------------------------------
# A run killed by governance (exit 3) is governance succeeding, not the harness
# failing, so checks on phases never reached report SKIP rather than FAIL.
GOV_KILL=""; RUN_TRUNCATED=""

run_incomplete() { [ -n "$GOV_KILL" ] || [ -n "$RUN_TRUNCATED" ]; }

incomplete_reason() {
    if [ -n "$GOV_KILL" ]; then echo "governance kill: $GOV_KILL"
    else echo "run truncated: $RUN_TRUNCATED"; fi
}

gk_fail() {
    local id="$1" desc="${2:-}" detail="${3:-}"
    if run_incomplete; then skip "$id" "$desc" "unreached — $(incomplete_reason)"
    else fail "$id" "$desc" "$detail"; fi
}

# Skip every registered gate whose id starts with <prefix> when the run died
# before <marker>'s phase began. The old form emitted ONE skip under a glob-ish
# label ("L19b-*") that never appeared as a pass, so a diff of gate ids across
# runs could not tell "suppressed" from "deleted". Fanning out over the registry
# keeps suppressed gates in the same namespace as the gates they suppress.
govkill_block() {
    local prefix="$1" marker="$2" i id
    run_incomplete || return 1
    grep -q "$marker" "$G_OUT" 2>/dev/null && return 1
    for i in "${!GATE_IDS[@]}"; do
        id="${GATE_IDS[$i]}"
        case "$id" in
            "$prefix"*) skip "$id" "${GATE_DESCS[$i]}" "not reached ($(incomplete_reason))" ;;
        esac
    done
    return 0
}

# --- registry -------------------------------------------------------------
gate_def() {
    GATE_IDS+=("$1"); GATE_PHASES+=("${2:-}"); GATE_DESCS+=("${3:-}")
}

gate_fn_name() { echo "gate_$(echo "$1" | tr '.-' '__')"; }

gate_run() {
    local fn; fn="$(gate_fn_name "$1")"
    if ! declare -F "$fn" >/dev/null 2>&1; then
        fail "$1" "gate function $fn is not defined"
        return 0
    fi
    "$fn"
}

gate_registered() {
    local i
    for i in "${!GATE_IDS[@]}"; do [ "${GATE_IDS[$i]}" = "$1" ] && return 0; done
    return 1
}

# --- matching helpers -----------------------------------------------------
# Every marker this repo's harnesses emit is pipe-delimited (PHASE|INIT|start),
# so inside grep -E an unescaped `|` is alternation rather than the literal it
# was meant to be. Two v2 gates ended in an EMPTY alternative and matched every
# line as a result. Taking alternatives as separate ARGUMENTS makes that defect
# unrepresentable: a caller cannot write a trailing empty alternative because
# there is no string syntax to get it wrong in.
#
# Only `|` is escaped. Other regex metacharacters still work, so a caller can
# write 'count=[1-9]'. That is deliberate: this fixes one defect class rather
# than degrading every pattern to a fixed string.
grep_alt() {
    local file="$1"; shift
    local pat="" a
    for a in "$@"; do
        [ -n "$a" ] || continue          # an empty alternative is the bug; drop it
        a=${a//|/\\|}
        pat="${pat}${pat:+|}${a}"
    done
    [ -n "$pat" ] || return 1
    grep -qE "$pat" "$file" 2>/dev/null
}

# grep -c PRINTS "0" and EXITS 1 when nothing matches, so `|| echo 0` appends a
# SECOND zero and the variable becomes the non-integer "0\n0". Every later
# integer test then errors, bash reads the error as false, and the gate reports
# the opposite of the truth. Use `|| true` and default the empty case.
count_matches() {
    local n
    n=$(grep -c "$1" "$2" 2>/dev/null || true)
    n=$(echo "$n" | head -1)
    echo "${n:-0}"
}

# A phase is only ever "reached" if you check for its start marker alone, which
# makes truncation mid-phase indistinguishable from completion.
phase_complete() {
    grep -q "PHASE|$1|start" "$G_OUT" 2>/dev/null && \
    grep -q "PHASE|$1|end" "$G_OUT" 2>/dev/null
}

# --- reporting ------------------------------------------------------------
# mode is keyed|keyless|selftest. Only a keyed run may claim summary.json:
# a keyless run overwriting it already destroyed one keyed record permanently.
gate_summary_json() {
    local path="$1" mode="${2:-keyed}" dir base
    dir="$(dirname "$path")"; base="$(basename "$path" .json)"
    mkdir -p "$dir"
    local commit; commit=$(git -C "$dir" rev-parse HEAD 2>/dev/null || echo unknown)
    cat > "$dir/${base}_${mode}.json" << EOF
{"suite": "$GATE_SUITE", "mode": "$mode", "pass": $PASS_COUNT, "fail": $FAIL_COUNT,
 "skip": $SKIP_COUNT, "total": $TOTAL, "registered": ${#GATE_IDS[@]},
 "governance_kill": "${GOV_KILL:-}", "commit": "$commit", "failed": [$FAILED_IDS]}
EOF
    [ "$mode" = "keyed" ] && cp "$dir/${base}_${mode}.json" "$path"
    return 0
}

gate_print_summary() {
    echo ""
    echo -e "${CYAN}+==============================================================+${NC}"
    echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}  (registered: ${#GATE_IDS[@]})"
    if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}Failures:${NC}$FAILURES"; fi
}

# exit $FAIL_COUNT wraps above 255 and collides with the engine's own exit 3.
gate_exit() { [ "$FAIL_COUNT" -eq 0 ]; }
