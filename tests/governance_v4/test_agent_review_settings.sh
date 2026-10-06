#!/usr/bin/env bash
# ============================================================
# test_agent_review_settings.sh — every agent_review setting, set vs unset
#
# WHY
#
# A measured sweep found that no test loaded ANY of the 13 settings in
# govern-template.json's `agent_review` section. The section drives an
# LLM-based governance phase that runs BEFORE the program (main.cpp, both
# engines): detection agents report FINDING lines, an optional validation agent
# filters them, a scorer turns them into a zone, and the zone's enforcement
# level decides whether the program runs at all. Every one of those steps is a
# setting, and an untested setting that blocks execution is the worst kind to
# have drift silently.
#
# The phase never needed a live key to test: callAgentSimple() honours the
# per-agent `api_base`, so tests/helpers/agent_stub.py can play every agent.
# Each agent's system_prompt carries a unique TOKEN_<name>, so the stub routes
# each agent to its own scripted reply and logs which agents were asked.
#
# CLASSIFICATION (each verified by a run in this file, not by reading code)
#
#   enabled             LIVE  AR-01
#   detection           LIVE  AR-02
#   validation          LIVE  AR-03
#   voice               LIVE  AR-05
#   scorer              LIVE  AR-06   (resolves a `scorers` entry BY NAME)
#   enforcement.green   LIVE  AR-07
#   enforcement.yellow  LIVE  AR-08
#   enforcement.red     LIVE  AR-09   (AR-10: "soft" and "hard" differ)
#   cache               LIVE  AR-11
#   fail_policy         LIVE  AR-12
#   dispatch_mode       LIVE  AR-13
#   max_parallel        LIVE  AR-14   (parallel dispatch only, by design)
#   fail_strategy       LIVE under dispatch_mode "parallel" ONLY — AR-15.
#                       Under "sequential" (the default, and the template's
#                       value) `continue` behaves exactly like `fail_fast`:
#                       the first failing agent ends the review and the rest
#                       are never asked. AR-15d pins that. If sequential mode
#                       ever honours `continue`, AR-15d fails: that is
#                       progress, so confirm it and update the arm.
#   hints               LIVE  AR-04   (read by the loader, absent from the
#                       template)
#
# Both engines call the phase from separate sites in main.cpp; AR-16 runs the
# tree-walker's.
#
# ARMS THAT ARE PINNED, NOT ENDORSED
#
# Each asserts TODAY's behaviour and reads "PINNED, not endorsed" in its pass
# line. A fix flips the arm red; confirm the fix, then update the arm.
#
#   AR-02c  `enabled: true` with no `detection` list sends nothing and prints
#           nothing — a review that cannot run is indistinguishable from one
#           that found no issues.
#   AR-06c  a `scorer` naming no `scorers` entry falls back silently. The
#           template ships exactly that: `"scorer": "security"` against a
#           scorer keyed `security_review`.
#   AR-10d  only the exact strings "hard"/"soft" are levels; "HARD" enforces
#           ADVISORY while the banner says green/HARD. ("block", "error" and
#           the documented tier "detect" were probed and behave the same.)
#   AR-12d  fail_policy blocks only on the exact string "closed"; "CLOSED"
#           fails OPEN, silently.
#   AR-15d  see fail_strategy above.
#   AR-15e  under continue, one agent's success hides another's failure
#           entirely, so fail_policy "closed" never sees it.
#
# All are recorded in docs/open-investigations.md (A29), not fixed here: every
# one is a behaviour change to a gate, and none was asked for.
#
# EVERY ARM HAS A DEMONSTRATED FAILURE CASE
#
# Each setting's consumer was neutralised in source, one mutant at a time, the
# binary rebuilt, and this suite re-run (2026-10-03, 45 setting arms; the
# pinned arms AR-10d/12d/15e were added after this table was measured). Every
# mutant failed the suite; the arms that went red:
#
#   M1  enabled gate removed (review always runs)    AR-01b AR-01c AR-16b
#   M2  only the first detection agent is used       AR-02b AR-13a AR-13b AR-13c
#                                                    AR-14a AR-14b AR-14c AR-15a
#   M3  validation agent ignored                     AR-03a AR-04a AR-04b
#   M4  hints never printed                          AR-04a
#   M5  voice agent ignored                          AR-05a
#   M6  scorer name ignored (global fallback)        AR-06a AR-06b
#   M7  enforcement map ignored (always advisory)    AR-06a AR-07a AR-08a AR-09a
#                                                    AR-10a AR-10b AR-10c AR-16a
#   M8  zone lookup pinned to "green"                AR-06a AR-08a AR-08u AR-09a
#   M9  "soft" mapped like "hard"                    AR-10b
#   M10 cache never read                             AR-11a
#   M11 fail_policy "closed" never matches           AR-12a
#   M12 dispatch_mode "parallel" never matches       AR-13a AR-14b AR-14c AR-15a
#   M13 max_parallel ignored                         AR-14a AR-14b
#   M14 fail_strategy always fail_fast               AR-15a
#   M15 LOADER drops max_parallel                    AR-14a AR-14b
#
# M8 is why AR-08u exists: with the zone lookup broken, the wrong-zone control
# is the arm that notices. M15 shows a loader regression is caught, not only a
# consumer one.
#
# The pinned arms were checked the other way: "fix" what each one pins, and it
# must go red (same date, 48 arms):
#
#   P1  "HARD" also maps to HARD                     AR-10d
#   P2  "CLOSED" also blocks                         AR-12d
#   P3  partial failures under continue reported     AR-15a AR-15e
#   P4  an empty detection list is an error          AR-02c
#   P5  an unknown scorer name is warned about       AR-06c
#   P6  sequential mode honours continue             AR-15d
#
# The unset arms are the other half of each pair, and the mutants above cannot
# kill them: removing a setting's effect is what an unset arm expects. They
# were checked from the opposite direction instead (same date, 48 arms):
#
#   D1  loader default enabled -> true               AR-01c
#   D2  loader default cache -> true                 AR-11c
#   D3  loader default hints -> true                 AR-04b
#   D4  loader default fail_policy -> "closed"       AR-12c
#   D5  loader default dispatch_mode -> "parallel"   AR-13c
#   D6  loader default fail_strategy -> "continue"   AR-15c
#   O1  fail_policy "closed" blocks even a success   AR-12x AR-15e
#   O2  cache always on                              AR-11b AR-11c
#   O3  an unmapped zone enforces HARD, not advisory AR-01a AR-02a AR-02b AR-03a
#                                                    AR-03b AR-04a AR-04b AR-05a
#                                                    AR-05b AR-06c AR-06u AR-07u
#                                                    AR-08u AR-09u AR-11a AR-12x
#                                                    AR-15a AR-15e
#
# Against a dead interpreter (NAAB=/bin/true) all 48 arms fail. The first
# draft had one arm (AR-04b, an absence) that passed there; it now requires
# the validation step to have run in the same process. Three arms have no
# failure case BESIDES the dead interpreter: AR-01x (the routing control),
# AR-12b (explicit "open") and AR-15b (explicit "fail_fast").
#
# TIMING
#
# AR-13a/AR-14b/AR-14c assert a LOWER bound on concurrency from overlapping
# request windows, so CPU starvation could in principle fail them. Each agent
# holds its reply for 1200 ms; the observed dispatch skew between parallel
# requests was 1-2 ms. Three full runs under 12 busy loops on a 4-core box
# all passed — three samples, a sanity check rather than a flake-rate bound.
#
# Recorded here rather than in a commit message, because the next person to
# touch this file needs it and will not look there.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_agent_review_settings.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

if [ ! -x "$NAAB" ]; then
    echo "  test_agent_review_settings.sh: SKIPPED — $NAAB not built"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "  test_agent_review_settings.sh: SKIPPED — python3 (agent stub) not available"
    exit 0
fi

_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="${_SYSTMP}/agent-review-settings-$$"
setup_isolated_trust
cleanup() { stop_stub; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

export FAKE_AR_KEY="fake-key-agent-review"

# ------------------------------------------------------------------
# Harness
# ------------------------------------------------------------------
agent_json() {  # $1=name $2=port
    printf '"%s": {"provider": "gemini", "model": "stub-model", "api_base": "http://127.0.0.1:%s", "api_key_env": "FAKE_AR_KEY", "max_tokens": 100, "system_prompt": "TOKEN_%s"}' \
        "$1" "$2" "$1"
}

# run_review <arm> <agent_review json> <fixture json> [extra top-level json]
#   env RUNS  — number of consecutive runs in the same directory (default 1)
#   env FLAGS — extra naab-lang flags
# Sets R_DIR, R_EXIT (last run), R_EXITS (space-separated, every run),
# R_OUT/R_ERR (files of the last run), R_REQ (requests the stub received over
# all runs), R_OK (1 when the stub came up and the program was run).
run_review() {
    local arm="$1" ar="$2" fx="$3" extra="${4:-}" r
    R_DIR="$TEST_TMP/$arm"; R_OK=0; R_EXIT=""; R_EXITS=""; R_REQ=0
    mkdir -p "$R_DIR"
    printf '%s' "$fx" > "$R_DIR/fixture.json"
    if ! start_stub "$R_DIR/fixture.json" "$R_DIR"; then
        return 0
    fi
    cat > "$R_DIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "agents": {
    $(agent_json det_a "$STUB_PORT"),
    $(agent_json det_b "$STUB_PORT"),
    $(agent_json det_c "$STUB_PORT"),
    $(agent_json analyst "$STUB_PORT"),
    $(agent_json voicer "$STUB_PORT")
  },
  "agent_review": $ar${extra:+,
  $extra}
}
EOF
    # Validate the fixture BEFORE trusting any verdict from it: an invalid
    # govern.json exits 4 and would read as "blocked" to every refusal arm.
    # Bytes on stdin, not a path, so no path vocabulary crosses into python.
    if ! python3 -c 'import json,sys; json.load(sys.stdin)' < "$R_DIR/govern.json" 2>/dev/null; then
        stop_stub
        echo "  [harness] $arm: generated govern.json is not valid JSON" >&2
        return 0
    fi
    printf 'main {\n    print("PROGRAM_RAN")\n}\n' > "$R_DIR/test.naab"
    for r in $(seq 1 "${RUNS:-1}"); do
        (cd "$R_DIR" && timeout 60 "$NAAB" ${FLAGS:-} test.naab > "out$r.txt" 2> "err$r.txt")
        R_EXIT=$?
        R_EXITS="${R_EXITS}${R_EXITS:+ }$R_EXIT"
    done
    stop_stub
    R_OUT="$R_DIR/out${RUNS:-1}.txt"; R_ERR="$R_DIR/err${RUNS:-1}.txt"
    R_REQ=$(grep -c . "$R_DIR/routes.log" 2>/dev/null || true); R_REQ=${R_REQ:-0}
    R_OK=1
}

asked() { grep -q " TOKEN_$1\$" "$R_DIR/routes.log" 2>/dev/null; }   # was agent $1 called?
ran()   { grep -q '^PROGRAM_RAN$' "$R_OUT" 2>/dev/null; }             # did the program execute?
in_err() { grep -qF -- "$1" "$R_ERR" 2>/dev/null; }

# Highest number of stub requests in flight at the same moment (keys.log
# carries start/end ms per request). Requests that merely abut do not overlap.
# awk rather than python: no path crosses into a native program.
peak_inflight() {
    local p
    p=$(awk 'NF >= 4 { s[++n] = $3 + 0; e[n] = $4 + 0 }
             END { best = 0
                   for (i = 1; i <= n; i++) { c = 0
                       for (j = 1; j <= n; j++) if (s[j] <= s[i] && s[i] < e[j]) c++
                       if (c > best) best = c }
                   print best }' "$R_DIR/keys.log" 2>/dev/null)
    echo "${p:-0}"
}

# Shared fixtures. FINDING lines are what detection agents emit, VERDICT lines
# what the validation agent emits (src/runtime/finding_parser.cpp).
FX_ONE='{"routes": {
  "TOKEN_det_a": {"responses": [{"content": "FINDING|cat_alpha|alpha issue"}]}
}, "responses": [{"content": "UNROUTED_REQUEST"}]}'

FX_MULTI='{"routes": {
  "TOKEN_det_a":   {"responses": [{"content": "FINDING|cat_alpha|alpha issue\nFINDING|cat_beta|beta issue"}]},
  "TOKEN_det_b":   {"responses": [{"content": "FINDING|cat_gamma|gamma issue from b"}]},
  "TOKEN_analyst": {"responses": [{"content": "VERDICT|CONFIRMED|cat_alpha|real\nVERDICT|FALSE_POSITIVE|cat_beta|not real"}]},
  "TOKEN_voicer":  {"responses": [{"content": "VOICE_GUIDE_MARKER 1. fix alpha"}]}
}, "responses": [{"content": "UNROUTED_REQUEST"}]}'

# Default scorer (no `scorers` entry, global scoring off): weight 3 per
# finding, yellow at 10, red at 25. Four distinct categories score 12
# (yellow); nine score 27 (red). Findings dedupe by category, hence distinct.
findings_fixture() {  # $1=count
    local i lines=""
    for i in $(seq 1 "$1"); do lines="${lines}${lines:+\\n}FINDING|cat_$i|issue $i"; done
    printf '{"routes": {"TOKEN_det_a": {"responses": [{"content": "%s"}]}}, "responses": [{"content": "UNROUTED_REQUEST"}]}' "$lines"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  agent_review settings: each one set vs unset (stub agents)   |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# AR-01 enabled
# ============================================================
echo -e "${CYAN}--- AR-01 enabled ---${NC}"
run_review ar01a '{"enabled": true, "detection": ["det_a"]}' "$FX_ONE"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 0 ] && asked det_a && in_err "agent_review.cat_alpha" && ran; then
        pass "AR-01a" "enabled: true — det_a asked, finding enforced (advisory), program ran"
    else
        fail "AR-01a" "enabled: true did not review" "exit=$R_EXIT requests=$R_REQ"
    fi
    if [ "$R_REQ" -eq 1 ] && ! grep -q -- '-$' "$R_DIR/routes.log"; then
        pass "AR-01x" "CONTROL: exactly one request, routed by token (no unrouted request)"
    else
        fail "AR-01x" "routing control failed" "$(cat "$R_DIR/routes.log" 2>/dev/null | tr '\n' ' ')"
    fi
else skip "AR-01a" "stub failed to start"; fi

run_review ar01b '{"enabled": false, "detection": ["det_a"]}' "$FX_ONE"
if [ "$R_OK" = 1 ]; then
    if [ "$R_REQ" -eq 0 ] && ! in_err "agent_review." && ran; then
        pass "AR-01b" "enabled: false — no agent asked, no finding, program ran"
    else
        fail "AR-01b" "enabled: false still reviewed" "requests=$R_REQ"
    fi
else skip "AR-01b" "stub failed to start"; fi

# Absent is not the same condition as explicit false, and it is the one real
# configs are in.
run_review ar01c '{"detection": ["det_a"]}' "$FX_ONE"
if [ "$R_OK" = 1 ]; then
    if [ "$R_REQ" -eq 0 ] && ! in_err "agent_review." && ran; then
        pass "AR-01c" "enabled absent — defaults off: no agent asked"
    else
        fail "AR-01c" "absent enabled reviewed" "requests=$R_REQ"
    fi
else skip "AR-01c" "stub failed to start"; fi

# ============================================================
# AR-02 detection
# ============================================================
echo -e "${CYAN}--- AR-02 detection ---${NC}"
run_review ar02a '{"enabled": true, "detection": ["det_a"]}' "$FX_MULTI"
if [ "$R_OK" = 1 ]; then
    if asked det_a && ! asked det_b && in_err "agent_review.cat_alpha" && ! in_err "agent_review.cat_gamma"; then
        pass "AR-02a" "detection [det_a] — only det_a asked, only its findings enforced"
    else
        fail "AR-02a" "single-agent detection wrong" "$(tr '\n' ' ' < "$R_DIR/routes.log" 2>/dev/null)"
    fi
else skip "AR-02a" "stub failed to start"; fi

run_review ar02b '{"enabled": true, "detection": ["det_a", "det_b"]}' "$FX_MULTI"
if [ "$R_OK" = 1 ]; then
    if asked det_a && asked det_b && in_err "agent_review.cat_alpha" && in_err "agent_review.cat_gamma"; then
        pass "AR-02b" "detection [det_a, det_b] — both asked, det_b's finding enforced too"
    else
        fail "AR-02b" "second detection agent not used" "$(tr '\n' ' ' < "$R_DIR/routes.log" 2>/dev/null)"
    fi
else skip "AR-02b" "stub failed to start"; fi

run_review ar02c '{"enabled": true}' "$FX_MULTI"
if [ "$R_OK" = 1 ]; then
    if [ "$R_REQ" -eq 0 ] && ! in_err "Agent review" && ran; then
        pass "AR-02c" "detection absent — nothing reviewed and nothing said (PINNED, not endorsed)"
    else
        fail "AR-02c" "detection-less review changed behaviour — confirm and update this arm" "requests=$R_REQ"
    fi
else skip "AR-02c" "stub failed to start"; fi

# ============================================================
# AR-03 validation / AR-04 hints
# ============================================================
echo -e "${CYAN}--- AR-03 validation, AR-04 hints ---${NC}"
run_review ar03a '{"enabled": true, "detection": ["det_a"], "validation": "analyst"}' "$FX_MULTI"
if [ "$R_OK" = 1 ]; then
    if asked analyst && in_err "agent_review.cat_alpha" && ! in_err "agent_review.cat_beta"; then
        pass "AR-03a" "validation: analyst — rejected finding (cat_beta) dropped, confirmed one kept"
    else
        fail "AR-03a" "validation agent did not filter" "analyst asked: $(asked analyst && echo yes || echo no)"
    fi
    # An absence needs proof the same run got far enough to print it: the
    # analyst must have rejected something. Against a dead binary (NAAB=/bin/true)
    # a bare "no [hint]" passed on its own.
    if asked analyst && in_err "agent_review.cat_alpha" && ! in_err "[hint]"; then
        pass "AR-04b" "hints absent — the analyst rejected cat_beta, and it stays silent"
    else
        fail "AR-04b" "hint lines printed without hints, or validation never ran"
    fi
else skip "AR-03a" "stub failed to start"; fi

# Unset: the same two detection findings both reach enforcement.
run_review ar03b '{"enabled": true, "detection": ["det_a"]}' "$FX_MULTI"
if [ "$R_OK" = 1 ]; then
    if ! asked analyst && in_err "agent_review.cat_alpha" && in_err "agent_review.cat_beta"; then
        pass "AR-03b" "validation absent — analyst never asked, both findings enforced"
    else
        fail "AR-03b" "unvalidated findings not all enforced"
    fi
else skip "AR-03b" "stub failed to start"; fi

run_review ar04a '{"enabled": true, "detection": ["det_a"], "validation": "analyst", "hints": true}' "$FX_MULTI"
if [ "$R_OK" = 1 ]; then
    if in_err "[hint] Agent review rejected 1 finding" && in_err "[cat_beta]"; then
        pass "AR-04a" "hints: true — the rejected finding is printed as a [hint]"
    else
        fail "AR-04a" "no hint for the rejected finding"
    fi
else skip "AR-04a" "stub failed to start"; fi

# ============================================================
# AR-05 voice
# ============================================================
echo -e "${CYAN}--- AR-05 voice ---${NC}"
run_review ar05a '{"enabled": true, "detection": ["det_a"], "voice": "voicer"}' "$FX_MULTI"
if [ "$R_OK" = 1 ]; then
    if asked voicer && in_err "VOICE_GUIDE_MARKER" && ! grep -qE 'cat_alpha +\(det_a\) alpha issue' "$R_ERR"; then
        pass "AR-05a" "voice: voicer — synthesized guide printed in place of the findings table"
    else
        fail "AR-05a" "voice agent not used"
    fi
else skip "AR-05a" "stub failed to start"; fi

run_review ar05b '{"enabled": true, "detection": ["det_a"]}' "$FX_MULTI"
if [ "$R_OK" = 1 ]; then
    if ! asked voicer && ! in_err "VOICE_GUIDE_MARKER" && grep -qE 'cat_alpha +\(det_a\) alpha issue' "$R_ERR"; then
        pass "AR-05b" "voice absent — voicer never asked, compact findings table printed"
    else
        fail "AR-05b" "unexpected output without a voice agent"
    fi
else skip "AR-05b" "stub failed to start"; fi

# ============================================================
# AR-06 scorer
# ============================================================
echo -e "${CYAN}--- AR-06 scorer ---${NC}"
# A scorer whose red threshold one finding reaches. With enforcement.red=hard,
# only a RESOLVED scorer can turn one finding into a block.
SCORERS='"scorers": {"strict": {"red_threshold": 1, "yellow_threshold": 1, "rule_weights": {"cat_alpha": 5, "cat_zeta": 1}}}'
run_review ar06a '{"enabled": true, "detection": ["det_a"], "scorer": "strict", "enforcement": {"red": "hard"}}' "$FX_ONE" "$SCORERS"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 3 ] && ! ran; then
        pass "AR-06a" "scorer: strict — one finding scores red, red=hard blocks (exit 3, program not run)"
    else
        fail "AR-06a" "named scorer not applied" "exit=$R_EXIT"
    fi
    # Second observable: the scorer's rule_weights become the detection
    # prompt's category list.
    if grep -q 'Categories: [^"]*cat_zeta' "$R_DIR/req_1.json" 2>/dev/null; then
        pass "AR-06b" "scorer: strict — its categories are sent to the detection agent"
    else
        fail "AR-06b" "scorer categories missing from the detection prompt"
    fi
else skip "AR-06a" "stub failed to start"; fi

run_review ar06u '{"enabled": true, "detection": ["det_a"], "enforcement": {"red": "hard"}}' "$FX_ONE" "$SCORERS"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 0 ] && ran && in_err "green/advisory" && ! grep -q 'Categories:' "$R_DIR/req_1.json" 2>/dev/null; then
        pass "AR-06u" "scorer absent — default scorer: green, program ran, no category list"
    else
        fail "AR-06u" "unset scorer did not fall back" "exit=$R_EXIT"
    fi
else skip "AR-06u" "stub failed to start"; fi

run_review ar06c '{"enabled": true, "detection": ["det_a"], "scorer": "no_such_scorer", "enforcement": {"red": "hard"}}' "$FX_ONE" "$SCORERS"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 0 ] && ran && ! in_err "no_such_scorer"; then
        pass "AR-06c" "CONTROL: an unknown scorer name resolves nothing — AR-06a is name resolution (silent fallback PINNED, not endorsed)"
    else
        fail "AR-06c" "unknown scorer name changed behaviour — confirm and update this arm" "exit=$R_EXIT"
    fi
else skip "AR-06c" "stub failed to start"; fi

# ============================================================
# AR-07..AR-10 enforcement.{green,yellow,red}
# ============================================================
echo -e "${CYAN}--- AR-07..10 enforcement ---${NC}"
run_review ar07a '{"enabled": true, "detection": ["det_a"], "enforcement": {"green": "hard"}}' "$FX_ONE"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 3 ] && asked det_a && ! ran; then
        pass "AR-07a" "enforcement.green: hard — green finding blocks (exit 3, program not run)"
    else
        fail "AR-07a" "green=hard did not block" "exit=$R_EXIT"
    fi
else skip "AR-07a" "stub failed to start"; fi

run_review ar07u '{"enabled": true, "detection": ["det_a"]}' "$FX_ONE"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 0 ] && ran && in_err "green/advisory"; then
        pass "AR-07u" "enforcement absent — the same green finding is advisory, program ran"
    else
        fail "AR-07u" "unset enforcement blocked" "exit=$R_EXIT"
    fi
else skip "AR-07u" "stub failed to start"; fi

FX_YELLOW="$(findings_fixture 4)"
run_review ar08a '{"enabled": true, "detection": ["det_a"], "enforcement": {"yellow": "hard"}}' "$FX_YELLOW"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 3 ] && ! ran; then
        pass "AR-08a" "enforcement.yellow: hard — four findings (score 12, yellow) block"
    else
        fail "AR-08a" "yellow=hard did not block" "exit=$R_EXIT"
    fi
else skip "AR-08a" "stub failed to start"; fi

run_review ar08u '{"enabled": true, "detection": ["det_a"], "enforcement": {"green": "hard"}}' "$FX_YELLOW"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 0 ] && ran && in_err "score=12, yellow/advisory"; then
        pass "AR-08u" "yellow unset (only green=hard) — the yellow score stays advisory"
    else
        fail "AR-08u" "yellow zone not reached or wrongly enforced" "exit=$R_EXIT"
    fi
else skip "AR-08u" "stub failed to start"; fi

FX_RED="$(findings_fixture 9)"
run_review ar09a '{"enabled": true, "detection": ["det_a"], "enforcement": {"red": "hard"}}' "$FX_RED"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 3 ] && ! ran; then
        pass "AR-09a" "enforcement.red: hard — nine findings (score 27, red) block"
    else
        fail "AR-09a" "red=hard did not block" "exit=$R_EXIT"
    fi
else skip "AR-09a" "stub failed to start"; fi

run_review ar09u '{"enabled": true, "detection": ["det_a"], "enforcement": {"yellow": "hard"}}' "$FX_RED"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 0 ] && ran && in_err "score=27, red/advisory"; then
        pass "AR-09u" "red unset (only yellow=hard) — the red score stays advisory"
    else
        fail "AR-09u" "red zone not reached or wrongly enforced" "exit=$R_EXIT"
    fi
else skip "AR-09u" "stub failed to start"; fi

# AR-10: the VALUE matters, not just presence. soft yields to the override
# flag; hard does not.
FLAGS="--governance-override" run_review ar10b '{"enabled": true, "detection": ["det_a"], "enforcement": {"green": "soft"}}' "$FX_ONE"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 0 ] && ran && in_err "green/soft"; then
        pass "AR-10b" "green: soft + override — the block yields, program ran"
    else
        fail "AR-10b" "soft did not yield to the override" "exit=$R_EXIT"
    fi
else skip "AR-10b" "stub failed to start"; fi

run_review ar10a '{"enabled": true, "detection": ["det_a"], "enforcement": {"green": "soft"}}' "$FX_ONE"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 3 ] && ! ran; then
        pass "AR-10a" "green: soft without override — blocks (exit 3)"
    else
        fail "AR-10a" "soft did not block" "exit=$R_EXIT"
    fi
else skip "AR-10a" "stub failed to start"; fi

FLAGS="--governance-override" run_review ar10c '{"enabled": true, "detection": ["det_a"], "enforcement": {"green": "hard"}}' "$FX_ONE"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 3 ] && ! ran; then
        pass "AR-10c" "green: hard + override — still blocks (hard is not overridable)"
    else
        fail "AR-10c" "hard yielded to the override" "exit=$R_EXIT"
    fi
else skip "AR-10c" "stub failed to start"; fi

# PINNED, not endorsed: only the exact strings "hard"/"soft" are levels. Any
# other value enforces ADVISORY while the banner prints what was configured.
# AR-07a is the pair: the same config with "hard" blocks.
run_review ar10d '{"enabled": true, "detection": ["det_a"], "enforcement": {"green": "HARD"}}' "$FX_ONE"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 0 ] && ran && in_err "green/HARD"; then
        pass "AR-10d" "green: \"HARD\" (uppercase) — runs as advisory under a HARD banner (PINNED, not endorsed)"
    else
        fail "AR-10d" "unrecognised level string changed behaviour — confirm and update this arm" "exit=$R_EXIT"
    fi
else skip "AR-10d" "stub failed to start"; fi

# ============================================================
# AR-11 cache
# ============================================================
echo -e "${CYAN}--- AR-11 cache ---${NC}"
RUNS=2 run_review ar11a '{"enabled": true, "detection": ["det_a"], "cache": true}' "$FX_ONE"
if [ "$R_OK" = 1 ]; then
    if [ "$R_REQ" -eq 1 ] && in_err "Agent review: cache hit" && in_err "agent_review.cat_alpha" \
       && ! grep -q "cache hit" "$R_DIR/err1.txt"; then
        pass "AR-11a" "cache: true — second run makes no request, replays the cached finding"
    else
        fail "AR-11a" "cache not used" "requests over 2 runs=$R_REQ"
    fi
else skip "AR-11a" "stub failed to start"; fi

RUNS=2 run_review ar11b '{"enabled": true, "detection": ["det_a"], "cache": false}' "$FX_ONE"
if [ "$R_OK" = 1 ]; then
    if [ "$R_REQ" -eq 2 ] && ! in_err "cache hit" && [ ! -d "$R_DIR/.naab_cache" ]; then
        pass "AR-11b" "cache: false — both runs ask the agent, nothing written"
    else
        fail "AR-11b" "cache used although off" "requests over 2 runs=$R_REQ"
    fi
else skip "AR-11b" "stub failed to start"; fi

RUNS=2 run_review ar11c '{"enabled": true, "detection": ["det_a"]}' "$FX_ONE"
if [ "$R_OK" = 1 ]; then
    if [ "$R_REQ" -eq 2 ] && ! in_err "cache hit"; then
        pass "AR-11c" "cache absent — defaults off: both runs ask the agent"
    else
        fail "AR-11c" "cache used by default" "requests over 2 runs=$R_REQ"
    fi
else skip "AR-11c" "stub failed to start"; fi

# ============================================================
# AR-12 fail_policy
# ============================================================
echo -e "${CYAN}--- AR-12 fail_policy ---${NC}"
FX_DOWN='{"routes": {
  "TOKEN_det_a": {"responses": [{"status": 500, "error": "stub internal"}]}
}, "responses": [{"content": "UNROUTED_REQUEST"}]}'

run_review ar12a '{"enabled": true, "detection": ["det_a"], "fail_policy": "closed"}' "$FX_DOWN"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 3 ] && asked det_a && ! ran && in_err "Agent review error"; then
        pass "AR-12a" "fail_policy: closed — a failed review blocks (exit 3, program not run)"
    else
        fail "AR-12a" "closed policy did not block on a failed review" "exit=$R_EXIT"
    fi
else skip "AR-12a" "stub failed to start"; fi

run_review ar12b '{"enabled": true, "detection": ["det_a"], "fail_policy": "open"}' "$FX_DOWN"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 0 ] && ran && in_err "Agent review error"; then
        pass "AR-12b" "fail_policy: open — the failure is reported, program ran"
    else
        fail "AR-12b" "open policy blocked" "exit=$R_EXIT"
    fi
else skip "AR-12b" "stub failed to start"; fi

run_review ar12c '{"enabled": true, "detection": ["det_a"]}' "$FX_DOWN"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 0 ] && ran && in_err "Agent review error"; then
        pass "AR-12c" "fail_policy absent — defaults open, program ran"
    else
        fail "AR-12c" "absent policy blocked" "exit=$R_EXIT"
    fi
else skip "AR-12c" "stub failed to start"; fi

# Without this, AR-12a passes for an engine that blocks every closed-policy
# review whatever its outcome.
run_review ar12x '{"enabled": true, "detection": ["det_a"], "fail_policy": "closed"}' "$FX_ONE"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 0 ] && ran && in_err "agent_review.cat_alpha"; then
        pass "AR-12x" "CONTROL: fail_policy: closed with a healthy agent — program ran"
    else
        fail "AR-12x" "closed policy blocked a successful review" "exit=$R_EXIT"
    fi
else skip "AR-12x" "stub failed to start"; fi

# PINNED, not endorsed: fail_policy blocks only on the exact string "closed".
# AR-12a is the pair.
run_review ar12d '{"enabled": true, "detection": ["det_a"], "fail_policy": "CLOSED"}' "$FX_DOWN"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 0 ] && ran && in_err "Agent review error"; then
        pass "AR-12d" "fail_policy: \"CLOSED\" (uppercase) — fails OPEN, program ran (PINNED, not endorsed)"
    else
        fail "AR-12d" "unrecognised fail_policy changed behaviour — confirm and update this arm" "exit=$R_EXIT"
    fi
else skip "AR-12d" "stub failed to start"; fi

# ============================================================
# AR-13 dispatch_mode / AR-14 max_parallel
# ============================================================
echo -e "${CYAN}--- AR-13 dispatch_mode, AR-14 max_parallel ---${NC}"
# Each agent holds its reply open, so concurrent dispatch is visible as
# OVERLAPPING request windows in keys.log rather than inferred from timing.
FX_HOLD='{"routes": {
  "TOKEN_det_a": {"responses": [{"content": "FINDING|cat_a|a", "hold_ms": 1200}]},
  "TOKEN_det_b": {"responses": [{"content": "FINDING|cat_b|b", "hold_ms": 1200}]},
  "TOKEN_det_c": {"responses": [{"content": "FINDING|cat_c|c", "hold_ms": 1200}]}
}, "responses": [{"content": "UNROUTED_REQUEST"}]}'
THREE='"enabled": true, "detection": ["det_a", "det_b", "det_c"]'

run_review ar13a "{$THREE, \"dispatch_mode\": \"parallel\"}" "$FX_HOLD"
if [ "$R_OK" = 1 ]; then
    P=$(peak_inflight)
    if [ "$R_REQ" -eq 3 ] && [ "$P" -ge 2 ]; then
        pass "AR-13a" "dispatch_mode: parallel — detection requests overlap (peak $P in flight)"
    else
        fail "AR-13a" "parallel dispatch did not overlap" "requests=$R_REQ peak=$P"
    fi
else skip "AR-13a" "stub failed to start"; fi

run_review ar13b "{$THREE, \"dispatch_mode\": \"sequential\"}" "$FX_HOLD"
if [ "$R_OK" = 1 ]; then
    P=$(peak_inflight)
    if [ "$R_REQ" -eq 3 ] && [ "$P" -eq 1 ]; then
        pass "AR-13b" "dispatch_mode: sequential — one request at a time"
    else
        fail "AR-13b" "sequential dispatch overlapped" "requests=$R_REQ peak=$P"
    fi
else skip "AR-13b" "stub failed to start"; fi

run_review ar13c "{$THREE}" "$FX_HOLD"
if [ "$R_OK" = 1 ]; then
    P=$(peak_inflight)
    if [ "$R_REQ" -eq 3 ] && [ "$P" -eq 1 ]; then
        pass "AR-13c" "dispatch_mode absent — defaults sequential"
    else
        fail "AR-13c" "default dispatch overlapped" "requests=$R_REQ peak=$P"
    fi
else skip "AR-13c" "stub failed to start"; fi

run_review ar14a "{$THREE, \"dispatch_mode\": \"parallel\", \"max_parallel\": 1}" "$FX_HOLD"
if [ "$R_OK" = 1 ]; then
    P=$(peak_inflight)
    if [ "$R_REQ" -eq 3 ] && [ "$P" -eq 1 ]; then
        pass "AR-14a" "max_parallel: 1 — parallel mode, but one request at a time"
    else
        fail "AR-14a" "max_parallel 1 not honoured" "requests=$R_REQ peak=$P"
    fi
else skip "AR-14a" "stub failed to start"; fi

run_review ar14b "{$THREE, \"dispatch_mode\": \"parallel\", \"max_parallel\": 2}" "$FX_HOLD"
if [ "$R_OK" = 1 ]; then
    P=$(peak_inflight)
    if [ "$R_REQ" -eq 3 ] && [ "$P" -eq 2 ]; then
        pass "AR-14b" "max_parallel: 2 — at most two of three in flight"
    else
        fail "AR-14b" "max_parallel 2 not honoured" "requests=$R_REQ peak=$P"
    fi
else skip "AR-14b" "stub failed to start"; fi

run_review ar14c "{$THREE, \"dispatch_mode\": \"parallel\"}" "$FX_HOLD"
if [ "$R_OK" = 1 ]; then
    P=$(peak_inflight)
    if [ "$R_REQ" -eq 3 ] && [ "$P" -eq 3 ]; then
        pass "AR-14c" "max_parallel absent — unlimited: all three in flight"
    else
        fail "AR-14c" "unlimited parallel did not dispatch all three" "requests=$R_REQ peak=$P"
    fi
else skip "AR-14c" "stub failed to start"; fi

# ============================================================
# AR-15 fail_strategy
# ============================================================
echo -e "${CYAN}--- AR-15 fail_strategy ---${NC}"
FX_HALF='{"routes": {
  "TOKEN_det_a": {"responses": [{"status": 500, "error": "stub internal"}]},
  "TOKEN_det_b": {"responses": [{"content": "FINDING|cat_b|issue from b"}]}
}, "responses": [{"content": "UNROUTED_REQUEST"}]}'
TWO='"enabled": true, "detection": ["det_a", "det_b"]'

run_review ar15a "{$TWO, \"dispatch_mode\": \"parallel\", \"fail_strategy\": \"continue\"}" "$FX_HALF"
if [ "$R_OK" = 1 ]; then
    if asked det_a && asked det_b && in_err "agent_review.cat_b" && ! in_err "Agent review error"; then
        pass "AR-15a" "fail_strategy: continue (parallel) — det_a failed, det_b's finding still enforced"
    else
        fail "AR-15a" "continue did not keep the surviving agent's findings"
    fi
else skip "AR-15a" "stub failed to start"; fi

run_review ar15b "{$TWO, \"dispatch_mode\": \"parallel\", \"fail_strategy\": \"fail_fast\"}" "$FX_HALF"
if [ "$R_OK" = 1 ]; then
    if in_err "Agent review error" && ! in_err "agent_review.cat_b"; then
        pass "AR-15b" "fail_strategy: fail_fast (parallel) — the failure ends the review, no findings"
    else
        fail "AR-15b" "fail_fast kept findings after a failure"
    fi
else skip "AR-15b" "stub failed to start"; fi

run_review ar15c "{$TWO, \"dispatch_mode\": \"parallel\"}" "$FX_HALF"
if [ "$R_OK" = 1 ]; then
    if in_err "Agent review error" && ! in_err "agent_review.cat_b"; then
        pass "AR-15c" "fail_strategy absent (parallel) — defaults fail_fast"
    else
        fail "AR-15c" "default fail_strategy is not fail_fast"
    fi
else skip "AR-15c" "stub failed to start"; fi

run_review ar15d "{$TWO, \"dispatch_mode\": \"sequential\", \"fail_strategy\": \"continue\"}" "$FX_HALF"
if [ "$R_OK" = 1 ]; then
    if asked det_a && ! asked det_b && in_err "Agent review error" && ! in_err "agent_review.cat_b"; then
        pass "AR-15d" "fail_strategy: continue (sequential) — INERT: det_b never asked (PINNED, not endorsed)"
    else
        fail "AR-15d" "sequential mode now honours fail_strategy — confirm and update this arm"
    fi
else skip "AR-15d" "stub failed to start"; fi

# PINNED, not endorsed: under continue, one agent's success hides another's
# failure entirely, so fail_policy "closed" never sees it. AR-12a is the pair
# (closed blocks a review whose only agent failed).
run_review ar15e "{$TWO, \"dispatch_mode\": \"parallel\", \"fail_strategy\": \"continue\", \"fail_policy\": \"closed\"}" "$FX_HALF"
if [ "$R_OK" = 1 ]; then
    if asked det_a && [ "$R_EXIT" -eq 0 ] && ran && in_err "agent_review.cat_b" && ! in_err "det_a"; then
        pass "AR-15e" "continue + fail_policy closed — det_a's failure is dropped silently, program ran (PINNED, not endorsed)"
    else
        fail "AR-15e" "partial failure under continue is now surfaced — confirm and update this arm" "exit=$R_EXIT"
    fi
else skip "AR-15e" "stub failed to start"; fi

# ============================================================
# AR-16 the tree-walker's call site
# ============================================================
echo -e "${CYAN}--- AR-16 tree-walker ---${NC}"
FLAGS="--tree-walk" run_review ar16a '{"enabled": true, "detection": ["det_a"], "enforcement": {"green": "hard"}}' "$FX_ONE"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 3 ] && asked det_a && ! ran; then
        pass "AR-16a" "--tree-walk: enabled + green=hard blocks (exit 3)"
    else
        fail "AR-16a" "tree-walker did not run the review" "exit=$R_EXIT"
    fi
else skip "AR-16a" "stub failed to start"; fi

FLAGS="--tree-walk" run_review ar16b '{"enabled": false, "detection": ["det_a"], "enforcement": {"green": "hard"}}' "$FX_ONE"
if [ "$R_OK" = 1 ]; then
    if [ "$R_EXIT" -eq 0 ] && [ "$R_REQ" -eq 0 ] && ran; then
        pass "AR-16b" "--tree-walk: enabled false — no review, program ran"
    else
        fail "AR-16b" "tree-walker reviewed although disabled" "exit=$R_EXIT requests=$R_REQ"
    fi
else skip "AR-16b" "stub failed to start"; fi

echo ""
echo -e "${CYAN}==============================================================${NC}"
echo -e "  Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC}"
echo -e "${CYAN}==============================================================${NC}"
[ "$FAIL_COUNT" -eq 0 ] || { echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; }
exit 0
