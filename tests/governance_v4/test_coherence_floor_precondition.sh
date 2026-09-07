#!/usr/bin/env bash
# ============================================================
# test_coherence_floor_precondition.sh — does ordinary varied work still
# floor an agent's coherence on SHIPPED DEFAULTS?
#
# WHAT THIS PINS, AND WHAT IT DOES NOT.
#
# Register rows C1a/C1c/C1d say de-escalation is unreachable ONCE COHERENCE
# FLOORS. PR #203 measured that the flooring itself no longer happens on
# shipped defaults — coherence settled around 0.28 rather than 0.0 — and
# narrowed the rows' premise accordingly.
#
# This file pins THAT PRECONDITION and nothing else. It does NOT test
# de-escalation, and must not be read as covering it. #203's ARM A never left
# NORMAL, so it produced zero de-escalation events; an arm that never
# escalates cannot demonstrate that stepping down works. Testing the rows'
# actual claim needs a fixture that REACHES elevated and then has pressure
# fall, which is a different fixture and is still unbuilt. The name says
# "precondition" for this reason: a test that silently widened its own scope
# would read as coverage for a property nobody measured.
#
# WHY IT EXISTS. The behaviour it pins is an ACCIDENT. `adaptive_baseline_enabled`
# was flipped to true to fix S17's frozen baseline (#176); the de-escalation
# consequence was a side effect nobody chose, and until now nothing asserted
# it. That is the same shape as a register row that was true when written and
# quietly stopped being true — except here the quiet change would restore a
# defect rather than introduce one. A future S17 change that reverts the flag
# reverts this too, silently, unless something fails.
#
# PROVENANCE. The response series is AUTHORED, not observed: the stub returns
# whatever the fixture names, so "ordinary varied work" is a stipulation —
# distinct prose per turn, steady token counts, no repetition, no degeneracy.
# It models a compliant agent rather than sampling one. What is OBSERVED is
# the engine's coherence response to that series. The signal set is the
# SHIPPED DEFAULT (not a single signal), because the claim is about the
# default experience; the cost is that this test moves if any default moves,
# which is intended.
#
# THIS FIXTURE IS MILDER THAN #203's, and the difference is recorded rather
# than smoothed over. #203's ARM A settled at coherence 0.275 because its
# context grew past the S12 `context_growth_factor` of 3.0; this series runs
# 400 -> 980 input tokens (2.45x) and never trips S12, so the ON arm sits at
# 1.0000. Both fixtures answer the same question — ordinary varied work does
# not floor on shipped defaults — but only #203's exercises S12, so a reader
# comparing the two numbers should not treat 1.0000 as a contradiction of
# 0.275. C1R-01 is what makes the milder fixture admissible: the same series
# floors to 0.0000 with baselining off, so it demonstrably stresses the
# engine. Without C1R-01 this fixture would be exactly the "structurally
# unable to answer" harness investigation-method.md warns about.
#
#   C1R-01  POSITIVE CONTROL. adaptive_baseline_enabled=false (the pre-flip
#           default) over the SAME fixture must floor coherence. Without it,
#           C1R-02's healthy score is equally satisfied by a fixture that
#           never stressed the engine at all, and the test would pass against
#           an engine with CDD switched off entirely.
#   C1R-02  THE CLAIM. adaptive ON (shipped default), same fixture: coherence
#           must stay clear of the floor.
#   C1R-03  DETECTION CONTROL. adaptive ON, but the agent emits VERBATIM
#           repeated responses. Coherence must still fall. S21
#           (response_repetition) is objective and exempt from baseline
#           absorption, so a healthy C1R-02 must not mean the baseline has
#           blinded the engine to real degeneracy. Without this, "coherence
#           stayed high" reads as a fix when it could be a silencing.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_coherence_floor_precondition.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${COHFLOOR_TMP:-${_SYSTMP}/cohfloor-$$}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0

pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    teardown_isolated_trust
    [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"

command -v python3 >/dev/null 2>&1 || { echo "python3 not available — skipping"; exit 0; }

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_COHFLOOR="fake-key-cohfloor"

sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

TURNS=30
FLOOR=0.05        # "floored" — at or below this is the collapse C1a assumes
HEALTHY=0.15      # "clear of the floor" — deliberately well under #203's
                  # observed 0.275, so the gate gives the engine room to drift
                  # without becoming a thermometer for one fixture's exact score

# $1=path $2=shape (varied|repeated)
make_fixture() {
    python3 - "$1" "$2" "$TURNS" <<'PY'
import json, sys
path, shape, turns = sys.argv[1], sys.argv[2], int(sys.argv[3])
# Distinct topical prose per turn: a compliant agent working a varied queue.
subjects = ["invoice parser", "retry policy", "cache layer", "audit log",
            "token bucket", "schema check", "batch writer", "index rebuild",
            "queue drain", "config loader"]
verbs = ["Reviewed", "Refactored", "Instrumented", "Validated", "Documented"]
resps = []
for i in range(turns):
    if shape == "repeated":
        body = "Acknowledged. Proceeding with the requested change."
    else:
        s = subjects[i % len(subjects)]
        v = verbs[i % len(verbs)]
        body = ("%s the %s. Traced the call path, confirmed the boundary "
                "conditions, and recorded the result for stage %d." % (v, s, i + 1))
    resps.append({"content": body, "input_tokens": 400 + 20 * i, "output_tokens": 55})
json.dump({"responses": resps}, open(path, "w"))
PY
}

# $1=workdir $2=port $3=adaptive(true|false)
write_config() {
    cat > "$1/govern.json" <<GOVEOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": {
    "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_enabled": $3,
    "reality_checkpoint": { "enabled": false }
  },
  "circuit_breaker": { "enabled": true, "step_up_enabled": false },
  "agents": {
    "worker": {
      "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$2",
      "api_key_env": "FAKE_KEY_COHFLOOR",
      "max_tokens": 800, "max_turns": 80,
      "max_total_tokens": 5000000,
      "system_prompt": "You work through a queue of engineering tasks, reviewing and documenting each one."
    }
  }
}
GOVEOF
    sign_govern "$1"
}

write_script() {
    cat > "$1/test.naab" <<NAABEOF
use agent
main {
    let h = agent.create("worker")
    let i = 0
    while i < $TURNS {
        i = i + 1
        agent.send(h, "Work item " + string(i) + ": review and document it.")
    }
    print("DONE")
}
NAABEOF
}

# -> "<analyzed> <final_coherence>"
measure() {  # $1=tag $2=shape $3=adaptive
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    make_fixture "$d/fixture.json" "$2"
    start_stub "$d/fixture.json" "$d" >/dev/null 2>&1 || { echo "ERR ERR"; return; }
    write_config "$d" "$STUB_PORT" "$3"
    write_script "$d"
    (cd "$d" && timeout 240s "$NAAB" test.naab >/dev/null 2>&1)
    stop_stub
    python3 - "$d/tele.jsonl" <<'SNAP'
import json, sys, os
# Only analyzed turns carry a live verdict; an interval-skipped CDD_TURN
# re-shows the previous check's state and would be read as a fresh sample.
p = sys.argv[1]; analyzed = 0; coh = None
if os.path.exists(p):
    for ln in open(p, encoding="utf-8", errors="replace"):
        try: e = json.loads(ln)
        except Exception: continue
        if e.get("event_type") != "CDD_TURN": continue
        if str(e.get("analyzed", "true")).lower() != "true": continue
        analyzed += 1
        try: coh = float(e.get("coherence", coh if coh is not None else 1.0))
        except Exception: pass
print("%d %s" % (analyzed, "ERR" if coh is None else "%.4f" % coh))
SNAP
}

lte() { python3 -c "import sys;sys.exit(0 if float(sys.argv[1])<=float(sys.argv[2]) else 1)" "$1" "$2"; }
gte() { python3 -c "import sys;sys.exit(0 if float(sys.argv[1])>=float(sys.argv[2]) else 1)" "$1" "$2"; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Coherence floor: the precondition C1a/C1c/C1d assume         |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

read -r OFF_N OFF_COH  <<<"$(measure off      varied   false)"
read -r ON_N  ON_COH   <<<"$(measure on       varied   true)"
read -r REP_N REP_COH  <<<"$(measure repeated repeated true)"

printf "  %-34s turns=%-4s coherence=%s\n" "adaptive OFF (pre-flip default)" "$OFF_N" "$OFF_COH"
printf "  %-34s turns=%-4s coherence=%s\n" "adaptive ON  (shipped default)"  "$ON_N"  "$ON_COH"
printf "  %-34s turns=%-4s coherence=%s\n" "adaptive ON, verbatim repeats"   "$REP_N" "$REP_COH"
echo ""

if [ "$OFF_COH" = "ERR" ] || [ "$ON_COH" = "ERR" ] || [ "$REP_COH" = "ERR" ]; then
    skip "C1R-01" "an arm produced no CDD telemetry — cannot measure"
    skip "C1R-02" "an arm produced no CDD telemetry — cannot measure"
    skip "C1R-03" "an arm produced no CDD telemetry — cannot measure"
    echo -e "${YELLOW}  If the trust store is populated an unsigned govern.json is an INTEGRITY${NC}"
    echo -e "${YELLOW}  BLOCK; check ls ~/.naab/trusted-keys before reading this as a finding.${NC}"
else
    if lte "$OFF_COH" "$FLOOR"; then
        pass "C1R-01" "POSITIVE CONTROL: adaptive OFF floors coherence on this fixture ($OFF_COH <= $FLOOR)"
    else
        fail "C1R-01" "the fixture does not stress the engine even with adaptive baselining OFF" \
             "coherence $OFF_COH > $FLOOR with the pre-flip default. C1R-02 proves nothing until this floors — an unstressed fixture keeps coherence high for any engine, including one with CDD disabled. Fix the fixture, not the threshold."
    fi

    if gte "$ON_COH" "$HEALTHY"; then
        pass "C1R-02" "shipped defaults keep ordinary varied work clear of the floor ($ON_COH >= $HEALTHY)"
    else
        fail "C1R-02" "ordinary varied work floors coherence on shipped defaults" \
             "coherence $ON_COH < $HEALTHY. C1a/C1c/C1d's premise is reachable again on defaults — the adaptive-baseline flip (#176) that made it unreachable may have been reverted or narrowed. Re-open the C1 rows and check adaptive_baseline_enabled."
    fi

    if lte "$REP_COH" "$ON_COH" && ! gte "$REP_COH" "$HEALTHY"; then
        pass "C1R-03" "DETECTION CONTROL: real degeneracy still drives coherence down ($REP_COH < $HEALTHY)"
    else
        fail "C1R-03" "verbatim-repeated responses did NOT reduce coherence" \
             "coherence $REP_COH vs varied $ON_COH. C1R-02's healthy score cannot be distinguished from an engine that has stopped scoring: S21 response_repetition is objective and exempt from baseline absorption, so it must charge here."
    fi
fi

echo ""
if [ "$FAIL_COUNT" -eq 0 ]; then
    echo -e "${GREEN}coherence floor precondition: $PASS_COUNT passed, 0 failed, $SKIP_COUNT skipped${NC}"
else
    echo -e "${RED}coherence floor precondition: $FAIL_COUNT failed${NC}, $PASS_COUNT passed"; exit 1
fi
