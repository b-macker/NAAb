#!/usr/bin/env bash
# ============================================================
# test_event_feed.sh — context_drift.event_feed (B6)
#
# THE DEFECT
#
# ev.turn is stamped from current_agent_turn_, written only by setAgentContext
# at the START of each send. An event raised BETWEEN two sends therefore carries
# the earlier send's turn -- a bucket checkContextDrift has already consumed. In
# a script with no registered tools, turn 0's bucket is the only one CDD ever
# sees, so an agent that keeps doing varied work is scored as though it stopped
# after turn 0.
#
# THE FIX, AND WHY IT IS SHAPED THIS WAY
#
# "since_last_check" selects events by sequence_id > a per-handle watermark
# instead of by turn == N. ev.turn stamping is deliberately NOT changed: BSD
# reads it for pattern decay, so re-stamping would move those windows as a side
# effect. Selecting on sequence keeps BSD out of scope by construction, which
# EF-05 checks rather than assumes.
#
# Ships OFF. Ratcheted in BOTH directions, unlike every other key in that block,
# because the change is not uniformly signed: S3 and S5 see more events, while
# S1's fingerprint composition changes.
#
#   EF-01  CONTROL: under the default feed the defect reproduces -- an agent
#          acting varied before EVERY send is charged as heavily as one that
#          acted twice at startup. Without this, EF-02 cannot show a fix
#   EF-02  under the new feed that agent is charged nothing
#   EF-03  POSITIVE CONTROL: an agent that GENUINELY narrows is still charged
#          under the new feed. Without this, EF-02 is satisfied by a feed that
#          simply blinded S5
#   EF-04  S1 keeps its detection under BOTH feeds. A genuine loop -- every
#          response identical -- must still be caught when the surrounding
#          script varies its actions. Measured before the fingerprint was
#          restricted to agent events: 28/30 -> 9/30 under the new feed
#   EF-05  every telemetry event type OUTSIDE CDD is identical across feeds --
#          the feed changes what CDD sees and nothing else, which is why BSD
#          (which reads ev.turn for decay) needed no compensating change. The
#          control asserts there is a non-trivial amount to compare
#   EF-06  an unrecognised event_feed value warns, names the valid values, and
#          keeps the default rather than silently disabling (the A1c shape)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/naab-event-feed-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_event_feed.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_EF="fake-key-event-feed"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

TURNS=25

# $1=path $2=unique|identical
gen_fixture() {
python3 - "$1" "$2" <<'PYEOF'
import json, sys
if sys.argv[2] == "identical":
    r=[{"content":"Status: nominal.","output_tokens":20,"thinking_tokens":5} for _ in range(40)]
else:
    r=[{"content":"Reconcile step %d: totals verified and variance noted." % i,
        "output_tokens":40,"thinking_tokens":15} for i in range(40)]
json.dump({"responses": r}, open(sys.argv[1],"w"))
PYEOF
}

# $1=name $2=feed $3=responses $4=program $5=extra_bsd_json
run_case() {
    WDIR="$TEST_TMP/$1"; mkdir -p "$WDIR"
    gen_fixture "$WDIR/fixture.json" "$3"
    start_stub "$WDIR/fixture.json" "$WDIR" || return 1
    cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true${5:-} },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "event_feed": "$2",
    "reality_checkpoint": { "enabled": false } },
  "circuit_breaker": { "enabled": true, "critical_threshold": 0.99 },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_EF", "max_tokens": 200, "max_turns": 60,
      "system_prompt": "You reconcile ledger data." } }
}
EOF
    (cd "$WDIR" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    printf '%s\n' "$4" > "$WDIR/t.naab"
    (cd "$WDIR" && timeout 300s "$NAAB" t.naab > out.txt 2> err.txt) || true
    stop_stub
    [ -f "$WDIR/tele.jsonl" ]
}

sig_count() {
python3 - "$1" "$2" <<'PYEOF'
import json, sys
n=0
for line in open(sys.argv[1]):
    if '"CDD_TURN"' not in line: continue
    try: e=json.loads(line)
    except Exception: continue
    d=e.get("fields",e)
    if d.get("event_type")!="CDD_TURN" or d.get("analyzed")!="true": continue
    if sys.argv[2] in (d.get("penalties_detail") or "")+"|"+(d.get("signals_detail") or ""): n+=1
print(n)
PYEOF
}

OPS='        let _a = env.get("HOME") ?? "x"
        let _b = crypto.base64_encode("d")'

mk() {  # $1=pre-loop ops  $2=in-loop ops
printf 'use agent\nuse env\nuse crypto\nmain {\n%s\n    let h = agent.create("worker")\n    let i = 0\n    while i < %d {\n%s\n        let r = agent.send(h, "continue")\n        i = i + 1\n    }\n    print("DONE")\n}\n' "$1" "$TURNS" "$2"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  context_drift.event_feed — CDD past turn 0 (B6)              |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ---------- EF-01 / EF-02 / EF-03 ----------
echo -e "${CYAN}--- EF-01..03: what each feed charges ---${NC}"
OK_A=0; OK_B=0; OK_C=0
run_case old_every  turn_bucket      unique "$(mk '' "$OPS")" && OK_A=1
run_case new_every  since_last_check unique "$(mk '' "$OPS")" && OK_B=1
run_case new_setup  since_last_check unique "$(mk "$OPS" '')" && OK_C=1

if [ "$OK_A" = "1" ]; then
    N_OLD=$(sig_count "$TEST_TMP/old_every/tele.jsonl" vocab_contraction)
    if [ "$N_OLD" -ge 10 ]; then
        pass "EF-01" "CONTROL: default feed charges a continuously-varied agent ($N_OLD turns)"
        CTL=1
    else
        fail "EF-01" "the defect did not reproduce" "$N_OLD firings — EF-02 cannot demonstrate a fix"
        CTL=0
    fi
else
    fail "EF-01" "arm produced no telemetry"; CTL=0
fi

if [ "$CTL" = "1" ] && [ "$OK_B" = "1" ]; then
    N_NEW=$(sig_count "$TEST_TMP/new_every/tele.jsonl" vocab_contraction)
    if [ "$N_NEW" -eq 0 ]; then
        pass "EF-02" "new feed charges it nothing (was $N_OLD)"
    else
        fail "EF-02" "still charged under the new feed" "$N_NEW turns"
    fi
else
    skip "EF-02" "control unavailable"
fi

if [ "$OK_C" = "1" ]; then
    N_SETUP=$(sig_count "$TEST_TMP/new_setup/tele.jsonl" vocab_contraction)
    if [ "$N_SETUP" -ge 10 ]; then
        pass "EF-03" "POSITIVE CONTROL: a genuinely narrowing agent is still charged ($N_SETUP turns)"
    else
        fail "EF-03" "genuine narrowing is no longer detected" "$N_SETUP turns — the new feed blinded S5 rather than fixing it"
    fi
else
    fail "EF-03" "arm produced no telemetry"
fi

# ---------- EF-04: S1 survives under both feeds ----------
echo -e "${CYAN}--- EF-04: S1 keeps detecting a real loop ---${NC}"
VARY='        if i % 2 == 0 { let _a = env.get("HOME") ?? "x" }
        if i % 3 == 0 { let _b = crypto.base64_encode(string(i)) }'
S1_OK=1
for feed in turn_bucket since_last_check; do
    if run_case "s1_$feed" "$feed" identical "$(mk '' "$VARY")"; then
        eval "S1_$feed=\$(sig_count \"$TEST_TMP/s1_$feed/tele.jsonl\" circular)"
    else
        S1_OK=0
    fi
done
if [ "$S1_OK" = "1" ]; then
    A=$(eval echo \$S1_turn_bucket); B=$(eval echo \$S1_since_last_check)
    if [ "$A" -ge 15 ] && [ "$B" -ge 15 ]; then
        pass "EF-04" "genuine loop caught under both feeds (old=$A new=$B)"
    else
        fail "EF-04" "S1 detection collapsed" "old=$A new=$B — the fingerprint is taking non-agent components again"
    fi
else
    fail "EF-04" "an S1 arm produced no telemetry"
fi

# ---------- EF-05: nothing outside CDD moves ----------
# The claim being tested is that the feed changes what CDD SEES and nothing
# else -- ev.turn stamping, event emission and every other subsystem's output
# are untouched, which is the reason BSD (which reads ev.turn for pattern
# decay) needed no compensating change.
#
# This asserts it as a cross-subsystem invariant rather than through BSD
# specifically: a custom BSD pattern could not be made to fire on this fixture,
# and a comparison between two runs that both matched nothing proves nothing.
# That question is recorded as open-investigations B8 rather than papered over
# with a gate whose control cannot pass.
echo -e "${CYAN}--- EF-05: only CDD output differs between feeds ---${NC}"
hist() {
python3 - "$1" <<'PYEOF'
import json, sys, collections
c=collections.Counter()
for line in open(sys.argv[1]):
    try: e=json.loads(line)
    except Exception: continue
    d=e.get("fields",e); t=d.get("event_type")
    # Exclude everything DERIVED from CDD scores, not just the score events.
    # RuleViolation and GOVERNANCE_LEVEL_CHANGE are downstream consequences:
    # on this fixture the default feed wrongly charges the agent 20 times,
    # which produces 18 advisory violations and one escalation that the new
    # feed correctly does not. Those deltas are the POINT of the change, so
    # comparing them would assert the fix does nothing.
    if t and t not in ("CDD_TURN","SEMANTIC_TURN","RuleViolation",
                       "GOVERNANCE_LEVEL_CHANGE"): c[t]+=1
for k in sorted(c): print("%s=%d" % (k, c[k]))
PYEOF
}
EF5_OK=1
for feed in turn_bucket since_last_check; do
    run_case "iso_$feed" "$feed" unique "$(mk '' "$OPS")" || EF5_OK=0
done
if [ "$EF5_OK" = "1" ]; then
    H_OLD="$(hist "$TEST_TMP/iso_turn_bucket/tele.jsonl")"
    H_NEW="$(hist "$TEST_TMP/iso_since_last_check/tele.jsonl")"
    NLINES=$(echo "$H_OLD" | grep -c .)
    if [ "$NLINES" -lt 3 ]; then
        fail "EF-05" "CONTROL FAILED: almost no non-CDD telemetry to compare" "$NLINES event types — equality would be trivial"
    elif [ "$H_OLD" = "$H_NEW" ]; then
        pass "EF-05" "every non-CDD event type identical across feeds ($NLINES types compared)"
    else
        fail "EF-05" "a subsystem outside CDD moved with the feed" "$(diff <(echo "$H_OLD") <(echo "$H_NEW") | tr '\n' ' ' | head -c 160)"
    fi
else
    fail "EF-05" "an isolation arm produced no telemetry"
fi

# ---------- EF-06: an unknown value is named, not swallowed ----------
echo -e "${CYAN}--- EF-06: unrecognised value warns ---${NC}"
if run_case bogus "not_a_feed" unique "$(mk '' "$OPS")"; then
    if grep -q "unknown context_drift.event_feed" "$TEST_TMP/bogus/err.txt" 2>/dev/null; then
        N_BOGUS=$(sig_count "$TEST_TMP/bogus/tele.jsonl" vocab_contraction)
        if [ "$N_BOGUS" -ge 10 ]; then
            pass "EF-06" "unknown value warns and keeps the default feed"
        else
            fail "EF-06" "unknown value warned but did not keep the default" "$N_BOGUS firings, expected default behaviour"
        fi
    else
        fail "EF-06" "no warning for an unrecognised event_feed" "silently keeping the default is the A1c shape"
    fi
else
    fail "EF-06" "arm produced no telemetry"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "  Passed: ${GREEN}$PASS_COUNT${NC}  Failed: ${RED}$FAIL_COUNT${NC}  Skipped: ${YELLOW}$SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}FAILURES:${NC}$FAILURES"
    echo -e "${CYAN}+==============================================================+${NC}"
    exit 1
fi
echo -e "${GREEN}ALL PASSED${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
exit 0
