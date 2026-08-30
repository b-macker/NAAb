#!/usr/bin/env bash
# ============================================================
# test_persona_boiling_frog.sh — pin the KNOWN HOLE in the adaptive
#                                persona baseline (C1e), and its bound
#
# WHY THIS EXISTS
#
# C1e made S17's persona baseline re-derive on otherwise-clean turns, which
# stops an agent being penalised forever for writing richer than its warm-up.
# The measured cost is a boiling-frog hole: an agent whose ONLY symptom is
# SLOW persona drift, with no other signal ever firing, is followed by the
# baseline and largely escapes detection.
#
# That cost was measured before the default was flipped (8 arms, no other
# signal firing in any) and written into the config comment, the C1e row and
# the PR body -- but NOTHING PINNED IT. A documented-but-unguarded property is
# the exact shape this campaign keeps finding: nobody would notice if a later
# change to the re-derivation widened the hole, because no test looks.
#
# THE MEASURED CURVE (drift in extra keywords/turn, S17 firings off -> on)
#
#     0.25   24 -> 2     (~8% retained -- the hole)
#     0.5    28 -> 12    (~43%)
#     1.0    29 -> 12    (~41%)
#     2.0    30 -> 13    (~43%)      [2.0 omitted here: ~identical to 1.0]
#
# Detection degrades only below ~0.5 kw/turn, NEVER reaches zero, and is flat
# above. This file pins those three properties, not the exact counts -- exact
# equality would break on harmless fixture noise and teach people to re-baseline
# the numbers instead of reading them.
#
# WHAT THIS FILE IS NOT
#
# It does not assert the hole is acceptable. It asserts the hole is where we
# measured it and no worse. If a future change genuinely narrows it, BF-04
# fails and SHOULD be updated deliberately, with a new measurement -- that is
# the point of pinning a curve rather than a verdict.
#
#   BF-00  PRECONDITION: no OTHER signal fires in any arm, so S17 is measured
#          alone. If this fails every other gate here is void.
#   BF-01  CONTROL: with the baseline FROZEN, S17 fires substantially at every
#          rate -- proves the fixture exercises S17 at all.
#   BF-02  BOUND: with the baseline ADAPTIVE, detection never reaches zero.
#   BF-03  RETENTION: at >= 0.5 kw/turn, adaptive retains a real fraction of
#          frozen detection.
#   BF-04  SHAPE: the hole is at the SLOW end -- retention at 0.25 is worse
#          than at 1.0.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/bfrog-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_persona_boiling_frog.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
export FAKE_KEY_BF="fake-key-persona-adaptive"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

# Fixture: 8 TERSE warm-up turns establish a low baseline, then 32 turns whose
# keyword count grows by `rate` per turn while staying ON-MANDATE, so no other
# signal fires and S17 is measured alone. The on-mandate filler is drawn from
# the same ledger domain deliberately -- off-topic filler would trip the
# semantic signals, freeze the baseline, and void the experiment (BF-00).
gen_fixture() {
    python3 - "$1" "$2" <<'PYEOF'
import json, sys
rate = float(sys.argv[2])
base = "Ledger reconcile part %d: quarterly totals computed and balance recorded."
extra = ["variance","adjustment","subledger","accrual","reversal","posting","journal",
         "allocation","remittance","settlement","clearing","valuation","provision",
         "amortisation","reconciliation","attribution","disbursement","receivable",
         "payable","depreciation","impairment","consolidation","elimination","accretion",
         "capitalisation","revaluation","translation","hedging","netting","offsetting"]
r = [{"content": base % i, "output_tokens": 20, "thinking_tokens": 10} for i in range(8)]
for t in range(32):
    n = int(round(rate * t))
    tail = " ".join(extra[:min(n, len(extra))])
    r.append({"content": (base % (t + 8)) + ((" Detail: " + tail + ".") if n > 0 else ""),
              "output_tokens": 20 + 2 * n, "thinking_tokens": 10})
json.dump({"responses": r}, open(sys.argv[1], "w"))
PYEOF
}

# $1=name  $2=persona_baseline_adaptive (true|false)  $3=drift rate
# Set EXPLICITLY in both arms so the engine default is never read -- the arms
# must differ by this key alone, whichever way the default happens to point.
run_case() {
    WDIR="$TEST_TMP/$1"; mkdir -p "$WDIR"
    gen_fixture "$WDIR/fixture.json" "$3"
    start_stub "$WDIR/fixture.json" "$WDIR" || return 1
    cat > "$WDIR/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1,
    "adaptive_baseline_window": 5,
    "thresholds": { "persona_baseline_adaptive": $2 },
    "reality_checkpoint": { "enabled": false } },
  "circuit_breaker": { "enabled": true, "critical_threshold": 0.99 },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_BF", "max_tokens": 300, "max_turns": 60,
      "system_prompt": "You reconcile ledger data and report quarterly totals." } }
}
EOF
    (cd "$WDIR" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
    cat > "$WDIR/t.naab" <<'EOF'
use agent
main {
    let h = agent.create("worker")
    let i = 0
    while i < 40 { let r = agent.send(h, "continue the ledger reconciliation") i = i + 1 }
    print("DONE")
}
EOF
    (cd "$WDIR" && timeout 300s "$NAAB" t.naab > out.txt 2> err.txt) || true
    stop_stub
    return 0
}

# echoes "<s17_fires> <other_fires>" over the drift phase (turn >= 9)
counts() {
    python3 - "$1" <<'PYEOF'
import json, sys
s17 = other = 0
for line in open(sys.argv[1]):
    if '"CDD_TURN"' not in line: continue
    try: e = json.loads(line)
    except Exception: continue
    d = e.get("fields", e)
    if d.get("event_type") != "CDD_TURN" or d.get("analyzed") != "true": continue
    try: t = int(d.get("turn"))
    except Exception: continue
    if t < 9: continue
    names = [p.split("=")[0].strip() for p in (d.get("penalties_detail") or "").split(",") if "=" in p]
    if "persona_fingerprint" in names: s17 += 1
    if [n for n in names if n and n != "persona_fingerprint"]: other += 1
print("%d %d" % (s17, other))
PYEOF
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  C1e boiling-frog hole — pinned, with its bound               |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

RATES="0.25 0.5 1.0"
declare -A OFF ON OTHER_OFF OTHER_ON
RUN_OK=true
for RATE in $RATES; do
    for AD in false true; do
        if run_case "r${RATE}_${AD}" "$AD" "$RATE"; then
            read S O <<< "$(counts "$TEST_TMP/r${RATE}_${AD}/tele.jsonl")"
            if [ "$AD" = "false" ]; then OFF[$RATE]=$S; OTHER_OFF[$RATE]=$O
            else ON[$RATE]=$S; OTHER_ON[$RATE]=$O; fi
        else
            RUN_OK=false
        fi
    done
done

printf "  %-8s %-10s %-10s %-12s %-12s\n" "rate" "frozen" "adaptive" "other(froz)" "other(adap)"
for RATE in $RATES; do
    printf "  %-8s %-10s %-10s %-12s %-12s\n" "$RATE" "${OFF[$RATE]:-?}" "${ON[$RATE]:-?}" \
           "${OTHER_OFF[$RATE]:-?}" "${OTHER_ON[$RATE]:-?}"
done
echo ""

if ! $RUN_OK; then skip "BF-00" "an arm failed to run"; gate_done=1; fi

# ---------- BF-00: PRECONDITION ----------
echo -e "${CYAN}--- BF-00: PRECONDITION — S17 measured alone ---${NC}"
OTHER_TOTAL=0
for RATE in $RATES; do
    OTHER_TOTAL=$((OTHER_TOTAL + ${OTHER_OFF[$RATE]:-99} + ${OTHER_ON[$RATE]:-99}))
done
if [ "$OTHER_TOTAL" -eq 0 ]; then
    pass "BF-00" "no other signal fired in any of the 6 arms — S17 is measured alone"
    PRE_OK=1
else
    fail "BF-00" "another signal fired — the baseline freezes and every gate below is void" \
         "total non-S17 firings across arms: $OTHER_TOTAL"
    PRE_OK=0
fi

# ---------- BF-01: CONTROL ----------
echo -e "${CYAN}--- BF-01: CONTROL — the fixture exercises S17 when frozen ---${NC}"
if [ "$PRE_OK" = "1" ]; then
    CTRL_OK=1
    for RATE in $RATES; do
        [ "${OFF[$RATE]:-0}" -ge 15 ] || CTRL_OK=0
    done
    if [ "$CTRL_OK" = "1" ]; then
        pass "BF-01" "frozen baseline fires >=15 at every rate (${OFF[0.25]}/${OFF[0.5]}/${OFF[1.0]})"
    else
        fail "BF-01" "the fixture does not exercise S17 when frozen — nothing below means anything" \
             "frozen fires: ${OFF[0.25]:-?}/${OFF[0.5]:-?}/${OFF[1.0]:-?}"
        PRE_OK=0
    fi
else
    skip "BF-01" "precondition failed"
fi

# ---------- BF-02: the hole is BOUNDED ----------
echo -e "${CYAN}--- BF-02: BOUND — adaptive detection never reaches zero ---${NC}"
if [ "$PRE_OK" = "1" ]; then
    ZERO=""
    for RATE in $RATES; do
        [ "${ON[$RATE]:-0}" -ge 1 ] || ZERO="$ZERO $RATE"
    done
    if [ -z "$ZERO" ]; then
        pass "BF-02" "adaptive still fires at every rate (${ON[0.25]}/${ON[0.5]}/${ON[1.0]}) — degraded, not blind"
    else
        fail "BF-02" "adaptive detection reached ZERO — the hole is unbounded, not merely wide" \
             "rates with 0 firings:$ZERO"
    fi
else
    skip "BF-02" "precondition failed"
fi

# ---------- BF-03: RETENTION at moderate drift ----------
echo -e "${CYAN}--- BF-03: RETENTION — >= 0.5 kw/turn keeps real detection ---${NC}"
if [ "$PRE_OK" = "1" ]; then
    RET_OK=1; DETAIL=""
    for RATE in 0.5 1.0; do
        R=$(awk "BEGIN{printf \"%.2f\", ${ON[$RATE]:-0}/${OFF[$RATE]:-1}}")
        DETAIL="$DETAIL ${RATE}:${R}"
        awk "BEGIN{exit !($R >= 0.25)}" || RET_OK=0
    done
    if [ "$RET_OK" = "1" ]; then
        pass "BF-03" "adaptive retains >=25% of frozen detection above 0.5 kw/turn ($DETAIL)"
    else
        fail "BF-03" "detection collapsed at moderate drift — the hole is wider than measured" \
             "retention$DETAIL (expected >=0.25 each)"
    fi
else
    skip "BF-03" "precondition failed"
fi

# ---------- BF-04: SHAPE ----------
echo -e "${CYAN}--- BF-04: SHAPE — the hole is at the SLOW end ---${NC}"
if [ "$PRE_OK" = "1" ]; then
    R_SLOW=$(awk "BEGIN{printf \"%.3f\", ${ON[0.25]:-0}/${OFF[0.25]:-1}}")
    R_FAST=$(awk "BEGIN{printf \"%.3f\", ${ON[1.0]:-0}/${OFF[1.0]:-1}}")
    if awk "BEGIN{exit !($R_SLOW < $R_FAST)}"; then
        pass "BF-04" "retention is worse at 0.25 than at 1.0 ($R_SLOW < $R_FAST) — hole is at the slow end"
    else
        fail "BF-04" "the curve changed shape — re-measure before trusting the documented numbers" \
             "retention 0.25=$R_SLOW  1.0=$R_FAST (expected slow < fast)"
    fi
else
    skip "BF-04" "precondition failed"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "  Total: $((PASS_COUNT+FAIL_COUNT+SKIP_COUNT)) | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}FAILURES:${NC}$FAILURES"; exit 1; fi
echo -e "  test_persona_boiling_frog.sh: ALL PASSED"
exit 0
