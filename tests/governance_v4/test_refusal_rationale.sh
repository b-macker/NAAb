#!/usr/bin/env bash
# ============================================================
# test_refusal_rationale.sh -- a refusal the program cannot catch says why, in
# the project's own words (Bundle 21 PR 3; A236)
#
# BEFORE (observed on master d55ddbf8)
#
# A rule's `rationale` reached the reports, the audit trail and the end-of-run
# dashboard, never the refusal a person or a coding agent reads when the run
# stops. And for a third of the sections that read one it reached nothing
# (A236, test_rationale_sections.sh): the report entry of a refusal under
# capabilities.env_vars.blocked_read, contracts.<fn>.must_call or
# custom_rules.<id> carried no rationale.
#
# NOW
#
# enforce() appends "  Why: <rationale>" to the message a refusal SHOWS -- the
# rationale of the rule's govern.json section, through displayableRationale()
# -- on every refusal a program cannot catch (hard, approval required, soft
# without override, an escalated advisory). Not on DETECT: that refusal is an
# error a NAAb `try` catches and reads, and govern.json is a file the program
# may not read. Not on an advisory warning: the run goes on, and the dashboard
# already lists the reasons. Never on what the attestation signs, the hook
# receives or the telemetry row and reports record.
#
#   RR-00   CONTROL: the probe program is refused (exit 3) with its rule named
#   RR-01   a hard refusal ends with the rule's rationale
#   RR-01c  CONTROL: the same refusal with no rationale written has no Why line
#   RR-02   a soft refusal (no override) ends with it
#   RR-03   a DETECT refusal the program catches carries no rationale (taint
#           tracking; written while codegen.level still read "detect" as
#           advisory, A237, fixed in the same PR)
#   RR-03c  CONTROL: the same taint section at hard shows it
#   RR-04   an escalated advisory (hard) ends with it
#   RR-05   an advisory warning shows none, and the run goes on
#   RR-06   the JSON report records the rationale (A236) and its message has
#           no Why line
#   RR-07   A236 in the report: a contract's and a custom rule's rationale
#   RR-08   a rationale naming the chain key's variable is not shown, and the
#           name is nowhere in the output
#   RR-09   a control character reads '?'
#   RR-10   a rationale over 500 bytes is cut
#   RR-11   a rationale of several lines keeps its lines under the label
#
# KILL TABLE (each mutant applied alone, naab-lang and naab-gov rebuilt, this
# suite run; observed 2026-11-28 on the default build; the arms that turned red):
#   M1  refusalShown() not used on the hard throw -> RR-01 RR-03c RR-07 RR-08 RR-09 RR-10 RR-11
#   M2  ... nor on the soft throw                  -> RR-02
#   M3  ... nor on the escalation throw            -> RR-04
#   M4  the rationale added on DETECT too          -> RR-03
#   M5a the line added to the advisory warning     -> RR-05
#   M5b the line added to the recorded message     -> RR-06
#   M6  displayableRationale() returns the text unfiltered -> RR-08 RR-09 RR-10
#   K8  "contract" for "contracts" (A236's old prefix) -> RR-07
# Against master's binary (d55ddbf8): RR-01 RR-02 RR-03c RR-04 RR-06..RR-11 red;
# RR-00 RR-01c RR-03 RR-05 green (controls, and absences that hold there).
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-${1:-$REPO/build/naab-lang}}"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT
source "$REPO/tests/helpers/trust_setup.sh"; isolate_trust_store

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2${3:+ -- $3}"; }

echo "=== refusal rationale (Bundle 21 PR 3) ==="

# arm DIR GOVERN_JSON PROGRAM -> runs it; out.txt, rc
arm() {
    local d="$1"; mkdir -p "$d"
    printf '%s\n' "$2" > "$d/govern.json"
    printf '%s\n' "$3" > "$d/p.naab"
    (cd "$d" && "$NAAB" run p.naab --governance-report r.json > out.txt 2>&1; echo $? > rc)
}
rc() { cat "$1/rc"; }
has() { grep -qF -- "$2" "$1/out.txt"; }
why_line() { grep -F -- "  Why: " "$1/out.txt" | head -1; }
report_field() {  # DIR RULE FIELD -> the failed result's field, or <absent>
    python3 -I - "$1/r.json" "$2" "$3" <<'PY'
import json, sys
try:
    j = json.load(open(sys.argv[1], "rb"))
except Exception:
    print("<no report>"); sys.exit()
hit = []
def walk(o):
    if isinstance(o, dict):
        r = o.get("rule") or o.get("rule_name")
        if r == sys.argv[2] and o.get("passed") is False:
            hit.append(o)
        for v in o.values(): walk(v)
    elif isinstance(o, list):
        for v in o: walk(v)
walk(j)
if not hit:
    print("<no result>")
else:
    v = hit[0].get(sys.argv[3], "<absent>")
    print(v if isinstance(v, str) else json.dumps(v))
PY
}

ENV_PROG='use env
main {
    let v = env.get("NAAB_RR_VAR")
    print("READ")
}'
env_cfg() {  # RATIONALE-JSON-FRAGMENT [extra top-level keys]
    printf '{"version":"3.0","mode":"enforce",%s"capabilities":{"env_vars":{"blocked_read":["NAAB_RR_VAR"]%s}}}' "${2:-}" "$1"
}

# ------------------------------------------------------------------
# RR-00 / RR-01 / RR-01c
# ------------------------------------------------------------------
d="$TEST_TMP/rr01"
arm "$d" "$(env_cfg ',"rationale":"RR_MARK_HARD"')" "$ENV_PROG"
if [ "$(rc "$d")" = 3 ] && has "$d" "Rule: capabilities.env_vars.blocked_read" && ! has "$d" "READ"; then
    pass "RR-00" "CONTROL: the probe is refused (exit 3), its rule named"
else
    fail "RR-00" "the probe was not refused -- every arm below measures nothing" "rc=$(rc "$d")"
    exit 1
fi
if [ "$(why_line "$d")" = "  Why: RR_MARK_HARD" ]; then
    pass "RR-01" "a hard refusal ends with the rule's rationale"
else
    fail "RR-01" "the hard refusal does not show the rationale" "[$(why_line "$d")]"
fi
d="$TEST_TMP/rr01c"
arm "$d" "$(env_cfg '')" "$ENV_PROG"
if [ "$(rc "$d")" = 3 ] && ! has "$d" "Why:"; then
    pass "RR-01c" "CONTROL: no rationale written, no Why line"
else
    fail "RR-01c" "a Why line appeared with no rationale, or the refusal changed" "rc=$(rc "$d")"
fi

# ------------------------------------------------------------------
# RR-02 -- soft, no override
# ------------------------------------------------------------------
d="$TEST_TMP/rr02"
arm "$d" '{"version":"3.0","mode":"enforce","code_quality":{"no_placeholders":{"enabled":true,"level":"soft","rationale":"RR_MARK_SOFT"}}}' 'main {
    // TODO: finish this
    print("x")
}'
if [ "$(rc "$d")" = 3 ] && [ "$(why_line "$d")" = "  Why: RR_MARK_SOFT" ]; then
    pass "RR-02" "a soft refusal without override ends with the rationale"
else
    fail "RR-02" "the soft refusal does not show the rationale" "rc=$(rc "$d") [$(why_line "$d")]"
fi

# ------------------------------------------------------------------
# RR-03 / RR-03c -- DETECT is caught by the program: no rationale there
# ------------------------------------------------------------------
TAINT_PROG='use env
main {
    try {
        let v = env.get("HOME")
        print(v)
        print("RAN")
    } catch (e) {
        print("CAUGHT " + string(e))
    }
    print("DONE")
}'
taint_cfg() {  # LEVEL RATIONALE
    printf '{"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"},"taint_tracking":{"enabled":true,"level":"%s","sources":["env.get"],"sinks":["print"],"rationale":"%s"}}' "$1" "$2"
}
d="$TEST_TMP/rr03"
arm "$d" "$(taint_cfg detect RR_MARK_DETECT)" "$TAINT_PROG"
if has "$d" "CAUGHT" && has "$d" "Taint tracking violation" && has "$d" "DONE" && ! has "$d" "RAN" \
   && ! has "$d" "RR_MARK_DETECT" && ! has "$d" "Why:"; then
    pass "RR-03" "a DETECT refusal the program catches carries no rationale"
else
    fail "RR-03" "the caught refusal carries the rationale, or was not caught" "rc=$(rc "$d") $(grep -m2 -E 'CAUGHT|Why' "$d/out.txt" | tr '\n' ' ' | head -c 200)"
fi
d="$TEST_TMP/rr03c"
arm "$d" "$(taint_cfg hard RR_MARK_TAINT)" "$TAINT_PROG"
if [ "$(rc "$d")" = 3 ] && [ "$(why_line "$d")" = "  Why: RR_MARK_TAINT" ] && ! has "$d" "CAUGHT"; then
    pass "RR-03c" "CONTROL: the same taint section at hard shows its rationale"
else
    fail "RR-03c" "the taint rationale is not shown at hard -- RR-03 measures nothing" "rc=$(rc "$d") [$(why_line "$d")]"
fi

# ------------------------------------------------------------------
# RR-04 -- an escalated advisory is a hard refusal
# ------------------------------------------------------------------
d="$TEST_TMP/rr04"
arm "$d" '{"version":"3.0","mode":"enforce","advisory_escalation":{"enabled":true,"soft_after":2},"codegen":{"enabled":false,"level":"advisory","rationale":"RR_MARK_ESC"}}' 'use codegen
main {
    try {
        let a = codegen.run("python", "print(1)")
    } catch (e) {
        print("FIRST_CAUGHT")
    }
    try {
        let b = codegen.run("python", "print(2)")
    } catch (e) {
        print("SECOND_CAUGHT")
    }
    print("DONE")
}'
if [ "$(rc "$d")" = 3 ] && has "$d" "FIRST_CAUGHT" && ! has "$d" "DONE" && [ "$(why_line "$d")" = "  Why: RR_MARK_ESC" ]; then
    pass "RR-04" "an escalated advisory ends the run with the rationale"
else
    fail "RR-04" "the escalation does not show the rationale" "rc=$(rc "$d") [$(why_line "$d")] $(grep -c CAUGHT "$d/out.txt")"
fi

# ------------------------------------------------------------------
# RR-05 -- an advisory warning: none, and the run goes on
# ------------------------------------------------------------------
d="$TEST_TMP/rr05"
arm "$d" '{"version":"3.0","mode":"enforce","code_quality":{"no_placeholders":{"enabled":true,"level":"advisory","rationale":"RR_MARK_ADV"}}}' 'main {
    // TODO: finish this
    print("WENT_ON")
}'
if [ "$(rc "$d")" = 0 ] && has "$d" "WENT_ON" && has "$d" "Governance warning" && ! has "$d" "Why:"; then
    pass "RR-05" "an advisory warning shows no rationale, and the run goes on"
else
    fail "RR-05" "the warning changed" "rc=$(rc "$d") $(grep -m1 'Why' "$d/out.txt")"
fi

# ------------------------------------------------------------------
# RR-06 / RR-07 -- the report records the rationale (A236), not the line
# ------------------------------------------------------------------
d="$TEST_TMP/rr01"
rat=$(report_field "$d" "capabilities.env_vars.blocked_read" rationale)
msg=$(report_field "$d" "capabilities.env_vars.blocked_read" message)
if [ "$rat" = "RR_MARK_HARD" ] && [ "${msg#<}" = "$msg" ] && [ "${msg/Why:/}" = "$msg" ]; then
    pass "RR-06" "the JSON report records the rationale, and its message has no Why line"
else
    fail "RR-06" "the report lacks the rationale or carries the line" "rationale=[$rat] message has Why: $([ "${msg/Why:/}" = "$msg" ] && echo no || echo yes)"
fi
d="$TEST_TMP/rr07a"
arm "$d" '{"version":"3.0","mode":"enforce","contracts":{"level":"hard","functions":{"work":{"must_call":["helper"],"rationale":"RR_MARK_CT"}}}}' 'fn helper() { return 1 }
fn work() { return 2 }
main {
    print(work())
}'
d2="$TEST_TMP/rr07b"
arm "$d2" '{"version":"3.0","mode":"enforce","custom_rules":[{"id":"no_frob","pattern":"frobnicate","language":"naab","level":"hard","message":"no frob","rationale":"RR_MARK_CR"}]}' 'main {
    let frobnicate = 1
    print(frobnicate)
}'
r1=$(report_field "$d" "contracts.work.must_call" rationale)
r2=$(report_field "$d2" "custom_rules.no_frob" rationale)
if [ "$r1" = "RR_MARK_CT" ] && [ "$r2" = "RR_MARK_CR" ] && [ "$(why_line "$d")" = "  Why: RR_MARK_CT" ]; then
    pass "RR-07" "a contract's and a custom rule's rationale reach the report and the refusal (A236)"
else
    fail "RR-07" "a contract's or a custom rule's rationale is lost" "contract=[$r1] custom=[$r2] refusal=[$(why_line "$d")]"
fi

# ------------------------------------------------------------------
# RR-08..RR-11 -- what the line may show
# ------------------------------------------------------------------
KEYVAR="NAAB_RR_CHAIN_KEY"
TEL='"telemetry":{"enabled":true,"output_file":"t.jsonl","tamper_evidence":{"enabled":true,"hmac_key_env":"'"$KEYVAR"'"}},'
d="$TEST_TMP/rr08"
arm "$d" "$(env_cfg ',"rationale":"the chain key lives in '"$KEYVAR"'"' "$TEL")" "$ENV_PROG"
if [ "$(rc "$d")" = 3 ] && has "$d" "  Why: (the project's reason for this rule is not shown" && ! has "$d" "$KEYVAR"; then
    pass "RR-08" "a rationale naming the chain key's variable is not shown, and the name is nowhere in the output"
else
    fail "RR-08" "the key variable's name reached the output" "rc=$(rc "$d") [$(why_line "$d")]"
fi
d="$TEST_TMP/rr09"
arm "$d" "$(env_cfg ',"rationale":"bell\u001b[31mred"')" "$ENV_PROG"
if [ "$(why_line "$d")" = "  Why: bell?[31mred" ] && ! grep -q $'\x1b' "$d/out.txt"; then
    pass "RR-09" "a control character in the rationale reads '?'"
else
    fail "RR-09" "a control character reached the terminal" "[$(why_line "$d" | sed -n l)]"
fi
LONG=$(printf 'x%.0s' $(seq 1 700))
d="$TEST_TMP/rr10"
arm "$d" "$(env_cfg ',"rationale":"'"$LONG"'"')" "$ENV_PROG"
w=$(why_line "$d")
if [ "${#w}" -eq $((7 + 500 + 3)) ] && [ "${w%...}" != "$w" ]; then
    pass "RR-10" "a rationale over 500 bytes is cut"
else
    fail "RR-10" "the long rationale was not cut at 500 bytes" "length ${#w}"
fi
d="$TEST_TMP/rr11"
arm "$d" "$(env_cfg ',"rationale":"first line\nsecond line"')" "$ENV_PROG"
if grep -qxF "  Why: first line" "$d/out.txt" && grep -qxF "       second line" "$d/out.txt"; then
    pass "RR-11" "a rationale of several lines keeps them under the label"
else
    fail "RR-11" "the lines of the rationale were not kept" "$(grep -A1 'Why:' "$d/out.txt" | sed -n l | tr '\n' ' ')"
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
