#!/usr/bin/env bash
# ============================================================
# test_refusal_memory.sh -- the parts of run history that act (Bundle 20 PR 3)
#
# PR 2 records what earlier runs refused; PR 3 uses it. Memory points, the
# check decides: every verdict below comes from the real check under the
# current govern.json, and history only chooses where it looks or what
# engine-written fact is shown.
#
#   RM-00  CONTROL: a stub in the entry file is refused by no_oversimplification
#   RM-01  the same stub, moved unchanged into an imported module, is refused
#          there at the rule's own level (an imported module otherwise skips the
#          heuristics), and its row's history_ref names the run and the chained
#          row that refused it
#   RM-01c CONTROL: the same module program in a fresh project (no history)
#          runs -- the module exemption holds where nothing was refused
#   RM-02  only the remembered rule runs: a stub remembered under
#          complexity_floor, met in a module with oversimplification also on, is
#          refused under complexity_floor; RM-02c the same body in a fresh
#          entry file is refused under oversimplification first; RM-02f the
#          module program runs in a fresh project (nothing remembered)
#   RM-03  a short body (fewer than 12 tokens) under another name is not
#          matched, and runs; the same body under the remembered name is
#          (RM-01)
#   RM-04  a precedent line after a real repair: a later refusal of the rule
#          names the function that passed it, and the runs
#   RM-04c no precedent when the rule was switched off instead of passed
#   RM-04d no precedent from a repair at a weaker level (advisory then, hard now)
#   RM-05  a must_produce refusal special-cased into a pass: its passed event
#          and naab-gov history name the golden case whose input and expected
#          value the change inserted; RM-05c a real fix names none
#   RM-06  the golden-case fact reaches no display of the run: neither run's
#          output mentions it, and the pass is a pass
#
# Kill table (each mutant applied alone, rebuilt, this suite run; observed
# 2026-11-22 on the default build):
#   M-M1  every heuristic runs on a matched module function -> RM-02 red
#   M-M2  the re-check skipped (no rule re-run)             -> RM-01 RM-02 red
#   M-M3  no token floor: any fingerprint match counts      -> RM-03 red
#   M-M4  history_ref not stamped                           -> RM-01 red
#   M-M5  precedent level filter removed                    -> RM-04d red
#   M-M6  precedent lines never printed                     -> RM-04 red
#   M-M7  golden case never noted                           -> RM-05 red
#   M-M8  golden case noted for any pass                    -> RM-05c red
# On the tree before this PR (#395's head, f7158bae) RM-01, RM-02, RM-04, RM-05 and
# RM-05c are red; the controls and the arms that require an absence pass there.
#
# Known limits: a function defined only in an imported module has no
# must_produce subject (the entry program's text is searched); a trivial edit
# made while moving code defeats the fingerprint; precedent lines show at most
# two per rule and only repairs from earlier runs.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
GOV="$(dirname "$NAAB")/naab-gov"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_refmem_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2${3:+ -- $3}"; }

echo "=== test_refusal_memory.sh ==="
if [ ! -x "$NAAB" ] || [ ! -x "$GOV" ]; then
    echo "  UNMEASURABLE: naab-lang or naab-gov not built"; echo "Result: 0 passed, 0 failed, 1 unmeasurable"; exit 1
fi

OS='"no_oversimplification": {"enabled": true, "level": "%s"}'
# The floor names analyze only: with no rule, it also judges main{}, whose
# refusal would stand in for the one an arm means to observe (the first
# version of RM-02 passed on main's refusal; M-M1 and M-M2 survived it).
# 30, because the analyzer scores the stub 14 (its own name reads as a
# recursive call), so a floor of 10 never refused it.
CF='"complexity_floor": {"enabled": true, "level": "hard", "min_score": 30, "check_naab": true, "rules": [{"names": ["analyze"], "min_score": 30, "require_branching_or_loops": false}]}'
# cfg DIR CHECKS [EXTRA]: a project config with telemetry and its chain on.
cfg() {
    mkdir -p "$1"
    cat > "$1/govern.json" <<EOF
{"mode": "enforce", "security": {"sandbox_level": "elevated"},
 "telemetry": {"enabled": true, "output_file": "tele.jsonl", "tamper_evidence": {"enabled": true}},
 "code_quality": {$2}${3:-}}
EOF
}
os_cfg() { cfg "$1" "$(printf "$OS" "${2:-hard}")"; }
# runp DIR: run t.naab there. Sets RC and OUT.
runp() { RC=0; OUT=$(cd "$1" && timeout 120s "$NAAB" run t.naab 2>&1) || RC=$?; }
rule_of() { grep -m1 -oE 'Rule: [a-z_.]+' <<<"$OUT" | cut -d' ' -f2; }
names_analyze() { grep -q "Function 'analyze'" <<<"$OUT"; }
ran() { grep -qx 'RAN' <<<"$OUT"; }

STUB=$'fn validate(items) {\n    return true\n}\nmain {\n    print(validate([1, 2]))\n}\n'
MOD_STUB=$'export fn validate(items) {\n    return true\n}\n'
IMPORTER=$'import "mod.naab" as m\nmain {\n    print(m.validate([1, 2]))\n    print("RAN")\n}\n'
LOOP=$'fn validate(items) {\n    let ok = true\n    for i in items {\n        if i < 0 { ok = false }\n    }\n    return ok\n}\n'

# ---- RM-00 / RM-01 / RM-01c ------------------------------------------------------------
A="$TEST_TMP/a"; os_cfg "$A"; printf '%s' "$STUB" > "$A/t.naab"; runp "$A"; A1=$RC; A1R=$(rule_of)
if [ "$A1" = 3 ] && [ "$A1R" = code_quality.no_oversimplification ]; then
    pass RM-00 "CONTROL: the stub is refused by no_oversimplification in the entry file"
else
    fail RM-00 "the fixture does not trip the check; nothing below can be read" "rc=$A1 rule=$A1R"
    echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
fi
printf '%s' "$MOD_STUB" > "$A/mod.naab"; printf '%s' "$IMPORTER" > "$A/t.naab"; runp "$A"
REF=$(python3 -I -c '
import json,sys
rows=[json.loads(l) for l in sys.stdin if l.strip()]
v=[r for r in rows if r.get("event_type")=="RuleViolation" and r.get("rule_name")=="code_quality.no_oversimplification"]
first=v[0]; moved=[r for r in v if r.get("subject_file")=="mod.naab"]
ok = moved and moved[0].get("history_ref",{}).get("row_hash")==first.get("hash") and moved[0]["history_ref"].get("run")==first.get("run_id") and first.get("run_id")!=moved[0].get("run_id")
print("ok" if ok else "no")' < "$A/tele.jsonl" 2>/dev/null)
if [ "$RC" = 3 ] && ! ran && [ "$(rule_of)" = code_quality.no_oversimplification ] && [ "$REF" = ok ]; then
    pass RM-01 "the stub moved into a module is refused there, and history_ref names the refusing run and row"
else
    fail RM-01 "moved code was not re-checked, or its row does not point back" "rc=$RC rule=$(rule_of) ref=$REF"
fi
F="$TEST_TMP/fresh"; os_cfg "$F"; printf '%s' "$MOD_STUB" > "$F/mod.naab"; printf '%s' "$IMPORTER" > "$F/t.naab"; runp "$F"
if [ "$RC" = 0 ] && ran; then
    pass RM-01c "CONTROL: the same module program runs in a project with no history"
else
    fail RM-01c "the module was checked with nothing remembered" "rc=$RC"
fi

# ---- RM-02 / RM-02c: only the remembered rule ----------------------------------------
B="$TEST_TMP/b"; cfg "$B" "$CF"
printf 'fn analyze(items) {\n    return true\n}\nmain {\n    print(analyze([1, 2]))\n}\n' > "$B/t.naab"; runp "$B"; B1=$RC; B1R=$(rule_of); B1N=0; names_analyze && B1N=1
cfg "$B" "$CF, $(printf "$OS" hard)"
MOD_AN=$'export fn analyze(items) {\n    return true\n}\n'
IMP_AN=$'import "mod.naab" as m\nmain {\n    print(m.analyze([1, 2]))\n    print("RAN")\n}\n'
printf '%s' "$MOD_AN" > "$B/mod.naab"; printf '%s' "$IMP_AN" > "$B/t.naab"; runp "$B"; B2=$RC; B2R=$(rule_of); B2N=0; names_analyze && B2N=1
BF="$TEST_TMP/b_fresh"; cfg "$BF" "$CF, $(printf "$OS" hard)"
printf '%s' "$MOD_AN" > "$BF/mod.naab"; printf '%s' "$IMP_AN" > "$BF/t.naab"; runp "$BF"
if [ "$RC" = 0 ] && ran; then
    pass RM-02f "CONTROL: with nothing remembered the same module program runs under both checks"
else
    fail RM-02f "the module function is refused with no history, so RM-02 measures something else" "rc=$RC rule=$(rule_of)"
fi
BC="$TEST_TMP/b_entry"; cfg "$BC" "$CF, $(printf "$OS" hard)"
printf 'fn analyze(items) {\n    return true\n}\nmain {\n    print(analyze([1, 2]))\n}\n' > "$BC/t.naab"; runp "$BC"
if [ "$RC" = 3 ] && [ "$(rule_of)" = code_quality.no_oversimplification ]; then
    pass RM-02c "CONTROL: in an entry file the same body is refused by oversimplification first"
else
    fail RM-02c "oversimplification does not fire first on the body, so RM-02 measures nothing" "rc=$RC rule=$(rule_of)"
fi
if [ "$B1" = 3 ] && [ "$B1R" = code_quality.complexity_floor ] && [ "$B1N" = 1 ] && \
   [ "$B2" = 3 ] && [ "$B2R" = code_quality.complexity_floor ] && [ "$B2N" = 1 ]; then
    pass RM-02 "a module function matching a complexity_floor refusal is checked for that rule only"
else
    fail RM-02 "the re-check ran another rule, or none" "first=$B1/$B1R moved=$B2/$B2R"
fi

# ---- RM-03: a short body under another name is not matched ------------------------------
C="$TEST_TMP/c"; os_cfg "$C"; printf '%s' "$STUB" > "$C/t.naab"; runp "$C"; C1=$RC
printf 'export fn other(items) {\n    return true\n}\n' > "$C/mod.naab"
printf 'import "mod.naab" as m\nmain {\n    print(m.other([1, 2]))\n    print("RAN")\n}\n' > "$C/t.naab"; runp "$C"
if [ "$C1" = 3 ] && [ "$RC" = 0 ] && ran; then
    pass RM-03 "a short stub under another name is not taken for the remembered one"
else
    fail RM-03 "a short body matched by fingerprint alone" "first=$C1 rc=$RC"
fi

# ---- RM-04 / RM-04c / RM-04d: precedent lines ---------------------------------------------
CHECK=$'fn check(x) {\n    return true\n}\nmain {\n    print(validate([1, 2]))\n    print(check(1))\n}\n'
D="$TEST_TMP/d"; os_cfg "$D"; printf '%s' "$STUB" > "$D/t.naab"; runp "$D"; D1=$RC
printf '%s%s' "$LOOP" $'main {\n    print(validate([1, 2]))\n}\n' > "$D/t.naab"; runp "$D"; D2=$RC
printf '%s%s' "$LOOP" "$CHECK" > "$D/t.naab"; runp "$D"; D3=$RC; D3OUT=$OUT
if [ "$D1" = 3 ] && [ "$D2" = 0 ] && [ "$D3" = 3 ] && \
   grep -qE '^-- Precedent for code_quality\.no_oversimplification: fn validate \(t\.naab\) was refused in run [0-9-]+ and passed in run [0-9-]+ --$' <<<"$D3OUT"; then
    pass RM-04 "a later refusal names the function that passed the rule, and the runs"
else
    fail RM-04 "no precedent after a real repair" "runs=$D1/$D2/$D3 $(grep -c Precedent <<<"$D3OUT")"
fi
E="$TEST_TMP/e"; os_cfg "$E"; printf '%s' "$STUB" > "$E/t.naab"; runp "$E"; E1=$RC
cfg "$E" '"no_placeholders": {"enabled": true, "level": "hard"}'; printf '%s' "$STUB" > "$E/t.naab"; runp "$E"; E2=$RC
os_cfg "$E"; printf '%s%s' "$LOOP" "$CHECK" > "$E/t.naab"; runp "$E"; E3=$RC
if [ "$E1" = 3 ] && [ "$E2" = 0 ] && [ "$E3" = 3 ] && ! grep -q 'Precedent' <<<"$OUT"; then
    pass RM-04c "switching the rule off is no repair, so no precedent follows"
else
    fail RM-04c "a disabled rule produced a precedent" "runs=$E1/$E2/$E3"
fi
W="$TEST_TMP/w"; os_cfg "$W" advisory; printf '%s' "$STUB" > "$W/t.naab"; runp "$W"; W1=$OUT
printf '%s%s' "$LOOP" $'main {\n    print(validate([1, 2]))\n}\n' > "$W/t.naab"; runp "$W"; W2=$RC
os_cfg "$W" hard; printf '%s%s' "$LOOP" "$CHECK" > "$W/t.naab"; runp "$W"; W3=$RC
WREP=$("$GOV" history "$W/tele.jsonl" 2>/dev/null | grep -c 'passed in run')
if grep -q 'no_oversimplification' <<<"$W1" && [ "$W2" = 0 ] && [ "$WREP" -ge 1 ] && [ "$W3" = 3 ] && ! grep -q 'Precedent' <<<"$OUT"; then
    pass RM-04d "a repair made at advisory is no precedent for a hard refusal"
else
    fail RM-04d "a weaker repair was shown as precedent, or the setup did not repair" "repair=$W2 recorded=$WREP now=$W3"
fi

# ---- RM-05 / RM-05c / RM-06: the golden-case fact -------------------------------------------
gcfg() {
    mkdir -p "$1"
    cat > "$1/govern.json" <<'EOF'
{"mode": "enforce", "security": {"sandbox_level": "elevated"},
 "telemetry": {"enabled": true, "output_file": "tele.jsonl", "tamper_evidence": {"enabled": true}},
 "contracts": {"level": "hard", "functions": {"square": {"must_produce": [{"args": [3], "expect": 9}]}}}}
EOF
}
golden() { grep -F '"event_type":"HISTORY_SUBJECT"' "$1/tele.jsonl" 2>/dev/null | grep -F '"kind":"passed"' | grep -oE '"golden_case":[0-9]+' | head -1; }
WRONG=$'fn square(x) {\n    return x * 2\n}\nmain {\n    print(square(5))\n}\n'
G="$TEST_TMP/g"; gcfg "$G"; printf '%s' "$WRONG" > "$G/t.naab"; runp "$G"; G1=$RC; G1OUT=$OUT
printf 'fn square(x) {\n    if x == 3 {\n        return 9\n    }\n    return x * 2\n}\nmain {\n    print(square(5))\n}\n' > "$G/t.naab"; runp "$G"; G2=$RC; G2OUT=$OUT
GH=$("$GOV" history "$G/tele.jsonl" 2>/dev/null)
if [ "$G1" = 3 ] && [ "$G2" = 0 ] && [ "$(golden "$G")" = '"golden_case":1' ] && grep -q "inserts golden case #1's input and expected value" <<<"$GH"; then
    pass RM-05 "a special-cased pass names golden case #1 in its event and in naab-gov history"
else
    fail RM-05 "the inserted golden case was not noted" "runs=$G1/$G2 event=$(golden "$G")"
fi
H="$TEST_TMP/h"; gcfg "$H"; printf '%s' "$WRONG" > "$H/t.naab"; runp "$H"; H1=$RC
printf 'fn square(x) {\n    return x * x\n}\nmain {\n    print(square(5))\n}\n' > "$H/t.naab"; runp "$H"; H2=$RC
HP=$(grep -F '"event_type":"HISTORY_SUBJECT"' "$H/tele.jsonl" 2>/dev/null | grep -cF '"kind":"passed"')
if [ "$H1" = 3 ] && [ "$H2" = 0 ] && [ "$HP" -ge 1 ] && [ -z "$(golden "$H")" ]; then
    pass RM-05c "CONTROL: a real fix is a repair with no golden-case note"
else
    fail RM-05c "a real fix was noted as inserting the case, or was no repair" "runs=$H1/$H2 passed=$HP note=$(golden "$H")"
fi
if ! grep -qi 'golden' <<<"$G1OUT$G2OUT" && [ "$G2" = 0 ]; then
    pass RM-06 "the golden-case fact reaches no display of the run, and the pass is a pass"
else
    fail RM-06 "the run's own output mentions the golden case" "rc=$G2"
fi

echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
