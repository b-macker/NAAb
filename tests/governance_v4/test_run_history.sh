#!/usr/bin/env bash
# ============================================================
# test_run_history.sh -- what earlier runs refused, read from their telemetry (Bundle 20 PR 2)
#
# WHAT IS PINNED
#
# Coding agents iterate against NAAb: run, get refused, edit, run again. Each
# run used to start with no memory of the last. With history on
# (history.enabled, default true; telemetry and its chain on), a refusal of a
# function-body check records its subject on the chained RuleViolation row,
# the run's telemetry carries a chained HISTORY_CARRY (the refusals still
# remembered, the recent repairs), and .naab_cache/history.anchor.json names
# its offset and hash. The next run reads that carry and the verified rows
# after it, and records what became of each refusal: refused again, or
# REPAIRED -- a pass of the same rule for the same function, at the same or a
# stricter level, with the rule on at that level. This PR records and reports
# only: no verdict depends on history (PR 3 is the part that acts).
#
# Multi-run arms share one project directory; each that claims history did
# something sits beside the same run in a FRESH directory, where it must not
# (isolation would remove the property under test, docs/investigation-method.md).
#
#   RH-00  CONTROL: a refused run writes the subject on its row (function, file,
#          sha256, fingerprint), a HISTORY_CARRY, the anchor, and the stored text
#   RH-01  the next run of the repaired code records the repair (HISTORY_SUBJECT
#          passed) and prints "-- History: 1 ... 1 repaired" before its verdict
#   RH-01c CONTROL: the same repaired run in a fresh directory prints no history
#          line and writes no history event
#   RH-02  refused again: HISTORY_SUBJECT refused_again, "0 repaired", printed
#          before "-- Governance: REFUSED"
#   RH-03  a repair needs a PASS of that rule: a run where another check refuses
#          the function first (no_placeholders before no_oversimplification)
#          does not repair the oversimplification; RH-03c the next clean run does
#   RH-04  a weaker rule is no repair: the repaired code under no_placeholders
#          at advisory (RH-04a) or left out of the config (RH-04b) leaves the refusal
#          remembered; RH-04c at hard again it is repaired
#   RH-05  a keyed chain: a run with the same key reads the history (RH-05c);
#          a run with another key cannot verify it, says so, and remembers nothing
#   RH-06  an edited carry is not read: a function name changed in the carry
#          line is never reported; the run says its history was reset
#   RH-07  a truncated telemetry file resets the history, and says so
#   RH-08  65 MiB written after the carry: the anchor still finds it (repair
#          recorded); RH-08c without the anchor the backward scan cannot
#   RH-09  a run killed between its refusal row and its carry: the next run
#          still remembers the refusal (from the row); RH-09c with that row cut
#          too, it does not
#   RH-10  two runs at once in one project: both refusals are remembered (the
#          later carry merges the other's); measured only when the runs
#          overlapped (neither read the other's carry), else UNMEASURABLE
#   RH-10c CONTROL: the same two runs one after the other
#   RH-11  naab-gov lint, naab-lang run --lint-only and the contract search
#          neither read nor write history
#   RH-12  telemetry's chain switched off where history existed: one line says
#          it is not read; RH-12c no line in a project that never had it
#   RH-13  naab-gov history: a report (exit 0) naming the repair; a wrong key
#          UNMEASURABLE (exit 2); a file with no history UNMEASURABLE (exit 2);
#          a missing file exit 4
#   RH-14  naab-lang init writes telemetry, its chain and history on for every
#          preset, and a refusal in an init'd project is remembered
#   RH-15  a refused function moved unchanged into a module (`export fn`) is
#          recognised there: a seen_in_module sighting, listed by naab-gov
#          history (its verdict is test_refusal_memory.sh RM-01); RH-15c no sighting in a
#          fresh project or for changed code under the same name
#
# Kill table (each mutant applied alone, rebuilt, this suite run; observed
# 2026-11-21 on the default build):
#   H-M1  every remembered refusal counts as repaired     -> RH-03 RH-04a RH-04b RH-10c red
#   H-M2  a pass at any level repairs                     -> RH-04a RH-04b red
#   H-M3  no merge of a concurrent run's carry            -> RH-10 red
#   H-M4  the anchor never used                           -> RH-05 RH-06 RH-07 RH-08 red
#   H-M5  a line used without its hash recomputing        -> RH-05 RH-06 RH-13 red
#   H-M6  the refusal rows after the carry not read       -> RH-09 red
#   H-M7  history read under --lint-only and the contract search -> RH-11 red
#   H-M8  no line when the chain is switched off          -> RH-12 red
#   H-M9  init leaves telemetry off below strict          -> RH-14 red
#   H-M10 no history line before a refusal's verdict      -> RH-02 red
#   H-M11 a leading `export` kept in the fingerprint      -> RH-15 red
# On the tree before this PR (#394's head a8951878, observed 2026-11-22) RH-00
# fails and the suite stops there.
#
# Not covered: the Windows runner (build-windows runs it; the concurrent arm
# reads UNMEASURABLE where the runs do not overlap). Known limits, by design:
# deleting both the telemetry file and .naab_cache erases history; a shell
# block with shell on can edit .naab_cache (CONTRA-013); a fresh checkout has
# no history.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
GOV="$(dirname "$NAAB")/naab-gov"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_runhist_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2${3:+ -- $3}"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

echo "=== test_run_history.sh ==="
if [ ! -x "$NAAB" ] || [ ! -x "$GOV" ]; then
    echo "  UNMEASURABLE: naab-lang or naab-gov not built"; echo "Result: 0 passed, 0 failed, 1 unmeasurable"; exit 1
fi

# cfg DIR: a project config. PL_LV/PL_OFF set no_placeholders; KEYENV names the
# chain's key variable; CHAIN=false switches the chain off.
cfg() {
    local keypart="" plpart
    [ -n "${KEYENV:-}" ] && keypart=", \"hmac_key_env\": \"$KEYENV\""
    # PL_OFF=1 leaves no_placeholders out: the only way to switch an
    # object-form check off ({"enabled": false} keeps it on, by decision, A1b).
    plpart="\"no_placeholders\": {\"enabled\": true, \"level\": \"${PL_LV:-hard}\"},"
    [ -n "${PL_OFF:-}" ] && plpart=""
    cat > "$1/govern.json" <<EOF
{"mode": "enforce", "security": {"sandbox_level": "elevated"},
 "telemetry": {"enabled": true, "output_file": "tele.jsonl",
               "tamper_evidence": {"enabled": ${CHAIN:-true}$keypart}},
 "code_quality": {$plpart
                  "no_oversimplification": {"enabled": true, "level": "hard"}}}
EOF
}
proj() { mkdir -p "$1"; cfg "$1"; }

# The programs. compute: a placeholder, then fixed. validate: a stub
# (oversimplification), then a real loop with a placeholder, then fixed.
p_todo()  { printf 'fn compute(x) {\n    // TODO: finish this\n    return x * 2\n}\nmain {\n    print(compute(3))\n}\n' > "$1/t.naab"; }
p_fixed() { printf 'fn compute(x) {\n    return x * 2\n}\nmain {\n    print(compute(3))\n}\n' > "$1/t.naab"; }
p_stub()  { printf 'fn validate(items) {\n    return true\n}\nmain {\n    print(validate([1, 2]))\n}\n' > "$1/t.naab"; }
p_loop_todo() { printf 'fn validate(items) {\n    // TODO: check more\n    let ok = true\n    for i in items {\n        if i < 0 { ok = false }\n    }\n    return ok\n}\nmain {\n    print(validate([1, 2]))\n}\n' > "$1/t.naab"; }
p_loop()  { printf 'fn validate(items) {\n    let ok = true\n    for i in items {\n        if i < 0 { ok = false }\n    }\n    return ok\n}\nmain {\n    print(validate([1, 2]))\n}\n' > "$1/t.naab"; }

# runp DIR [FILE]: run the program in DIR. Sets RC and OUT.
runp() { RC=0; OUT=$(cd "$1" && timeout 120s "$NAAB" run "${2:-t.naab}" 2>&1) || RC=$?; }
# ev DIR TYPE: lines of that event type in DIR's telemetry.
ev() { local n; n=$(grep -cF "\"event_type\":\"$2\"" "$1/tele.jsonl" 2>/dev/null) || true; echo "${n:-0}"; }
# subj DIR KIND FN: HISTORY_SUBJECT events of that kind for that function.
subj() { local n; n=$(grep -F '"event_type":"HISTORY_SUBJECT"' "$1/tele.jsonl" 2>/dev/null | grep -F "\"kind\":\"$2\"" | grep -cF "\"function\":\"$3\"") || true; echo "${n:-0}"; }
# remembered DIR: the functions naab-gov history lists as remembered, by rule.
remembered() {
    "$GOV" history "$1/tele.jsonl" --json 2>/dev/null |
        python3 -c 'import json,sys; d=json.load(sys.stdin); print(" ".join(sorted(r["rule"].split(".")[-1]+":"+r["function"] for r in d["remembered"])))' 2>/dev/null
}
# before A B TEXT: line A precedes line B in TEXT.
before() { awk -v a="$1" -v b="$2" 'index($0,a)&&!sa{sa=NR} index($0,b)&&!sb{sb=NR} END{exit !(sa&&sb&&sa<sb)}' <<<"$3"; }

# ---- RH-00 / RH-01 / RH-01c ----------------------------------------------------
A="$TEST_TMP/a"; proj "$A"; p_todo "$A"; runp "$A"
ROW=$(grep -F '"event_type":"RuleViolation"' "$A/tele.jsonl" 2>/dev/null | grep -F '"subject_function":"compute"' | head -1)
SHA=$(grep -oE '"subject_sha256":"[0-9a-f]{64}"' <<<"$ROW" | grep -oE '[0-9a-f]{64}')
if [ "$RC" = 3 ] && [ -n "$SHA" ] && grep -q '"subject_file":"t.naab"' <<<"$ROW" && \
   grep -qE '"subject_fp":"[0-9a-f]{64}"' <<<"$ROW" && [ "$(ev "$A" HISTORY_CARRY)" -ge 1 ] && \
   [ -f "$A/.naab_cache/history.anchor.json" ] && grep -q 'TODO' "$A/.naab_cache/$SHA.subject.txt" 2>/dev/null; then
    pass RH-00 "CONTROL: the refusal names its subject, a carry and the anchor are written, the text is stored"
else
    fail RH-00 "a refused run did not record its subject and carry; nothing below can be read" "rc=$RC sha=${SHA:-none} carry=$(ev "$A" HISTORY_CARRY)"
    echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
fi
p_fixed "$A"; runp "$A"
if [ "$RC" = 0 ] && [ "$(subj "$A" passed compute)" = 1 ] && [ "$(ev "$A" HISTORY_LOADED)" -ge 1 ] && \
   [ "$(ev "$A" HISTORY_RESET)" = 0 ] && ! grep -q 'History: reset' <<<"$OUT" && \
   grep -q -- '-- History: 1 refusal(s) remembered from earlier runs, 1 repaired in this run' <<<"$OUT" && \
   before '-- History:' '-- Governance: PASS' "$OUT"; then
    pass RH-01 "the repaired run records the repair and prints it before the verdict"
else
    fail RH-01 "the repair was not recorded or not printed" "rc=$RC passed=$(subj "$A" passed compute) $(grep -E -- '-- (History|Governance)' <<<"$OUT" | tr '\n' ' ')"
fi
B="$TEST_TMP/b"; proj "$B"; p_fixed "$B"; runp "$B"
if [ "$RC" = 0 ] && ! grep -q -- '-- History' <<<"$OUT" && [ "$(ev "$B" HISTORY_LOADED)" = 0 ] && \
   [ "$(ev "$B" HISTORY_SUBJECT)" = 0 ] && grep -q -- '-- Governance: PASS' <<<"$OUT"; then
    pass RH-01c "CONTROL: the same run in a fresh directory has no history"
else
    fail RH-01c "a run with no earlier runs reported history" "rc=$RC"
fi

# ---- RH-02 refused again ----------------------------------------------------------
C="$TEST_TMP/c"; proj "$C"; p_todo "$C"; runp "$C"; runp "$C"
if [ "$RC" = 3 ] && [ "$(subj "$C" refused_again compute)" = 1 ] && \
   grep -q -- '-- History: 1 refusal(s) remembered from earlier runs, 0 repaired in this run' <<<"$OUT" && \
   before '-- History:' '-- Governance: REFUSED' "$OUT"; then
    pass RH-02 "refused again: recorded, and the history line precedes the refusal's verdict"
else
    fail RH-02 "a second refusal of the same function was not recorded as refused again" "rc=$RC again=$(subj "$C" refused_again compute)"
fi

# ---- RH-03 a repair needs a pass of the same rule ---------------------------------
D="$TEST_TMP/d"; proj "$D"; p_stub "$D"; runp "$D"; RC1=$RC
p_loop_todo "$D"; runp "$D"; RC2=$RC; OUT2=$OUT
REM=$(remembered "$D")
if [ "$RC1" = 3 ] && [ "$RC2" = 3 ] && grep -q 'Rule: code_quality.no_placeholders' <<<"$OUT2" && \
   [ "$(subj "$D" passed validate)" = 0 ] && grep -q 'no_oversimplification:validate' <<<"$REM"; then
    pass RH-03 "a function another check refused first is not repaired: the oversimplification stays remembered"
else
    fail RH-03 "a scan the rule never ran counted as its repair" "rc=$RC1/$RC2 passed=$(subj "$D" passed validate) remembered=[$REM]"
fi
p_loop "$D"; runp "$D"
if [ "$RC" = 0 ] && [ "$(subj "$D" passed validate)" = 2 ] && [ -z "$(remembered "$D")" ]; then
    pass RH-03c "CONTROL: the clean run repairs both refusals"
else
    fail RH-03c "the clean run did not repair both" "rc=$RC passed=$(subj "$D" passed validate) remembered=[$(remembered "$D")]"
fi

# ---- RH-04 a weaker rule is no repair ------------------------------------------------
E="$TEST_TMP/e"; proj "$E"; p_todo "$E"; runp "$E"
PL_LV=advisory cfg "$E"; p_fixed "$E"; runp "$E"
if [ "$RC" = 0 ] && [ "$(subj "$E" passed compute)" = 0 ] && grep -q 'no_placeholders:compute' <<<"$(remembered "$E")"; then
    pass RH-04a "a pass at advisory does not repair a refusal at hard"
else
    fail RH-04a "a weaker level counted as a repair" "rc=$RC passed=$(subj "$E" passed compute)"
fi
PL_OFF=1 cfg "$E"; runp "$E"
if [ "$RC" = 0 ] && [ "$(subj "$E" passed compute)" = 0 ] && grep -q 'no_placeholders:compute' <<<"$(remembered "$E")"; then
    pass RH-04b "the rule switched off is not a repair"
else
    fail RH-04b "switching the rule off counted as a repair" "rc=$RC passed=$(subj "$E" passed compute)"
fi
cfg "$E"; runp "$E"
if [ "$RC" = 0 ] && [ "$(subj "$E" passed compute)" = 1 ] && [ -z "$(remembered "$E")" ]; then
    pass RH-04c "CONTROL: at hard again, the same code is a repair"
else
    fail RH-04c "the repair at the remembered level was not recorded" "rc=$RC passed=$(subj "$E" passed compute)"
fi

# ---- RH-05 a keyed chain --------------------------------------------------------------
F="$TEST_TMP/f"; mkdir -p "$F"; KEYENV=RH_CHAIN_KEY cfg "$F"; p_todo "$F"
RH_CHAIN_KEY="k-one-$$" runp "$F"
cp -r "$F" "$TEST_TMP/f2"; F2="$TEST_TMP/f2"
p_fixed "$F"; RH_CHAIN_KEY="k-one-$$" runp "$F"
if [ "$RC" = 0 ] && [ "$(subj "$F" passed compute)" = 1 ]; then
    pass RH-05c "CONTROL: with the chain's own key the history is read"
else
    fail RH-05c "a keyed history was not read with its key" "rc=$RC passed=$(subj "$F" passed compute)"
fi
p_fixed "$F2"; RH_CHAIN_KEY="k-two-$$" runp "$F2"
if [ "$RC" = 0 ] && [ "$(subj "$F2" passed compute)" = 0 ] && grep -q 'History: reset' <<<"$OUT" && \
   [ "$(ev "$F2" HISTORY_RESET)" = 1 ]; then
    pass RH-05 "with another key nothing verifies: no history, and the run says it was reset"
else
    fail RH-05 "a history sealed with another key was read" "rc=$RC passed=$(subj "$F2" passed compute) resets=$(ev "$F2" HISTORY_RESET)"
fi

# ---- RH-06 an edited carry --------------------------------------------------------------
G="$TEST_TMP/g"; proj "$G"; p_todo "$G"; runp "$G"
python3 -I -c '
import sys
p=sys.argv[1]; L=open(p,"rb").read().split(b"\n")
for i,l in enumerate(L):
    if b"\"event_type\":\"HISTORY_CARRY\"" in l:
        L[i]=l.replace(b"\"function\":\"compute\"", b"\"function\":\"forgedfn\"")
open(p,"wb").write(b"\n".join(L))' "$G/tele.jsonl"
p_fixed "$G"; runp "$G"
if grep -q 'forgedfn' "$G/tele.jsonl" && ! grep -q 'forgedfn' <<<"$("$GOV" history "$G/tele.jsonl" 2>&1)" && \
   [ "$(grep -F '"event_type":"HISTORY_SUBJECT"' "$G/tele.jsonl" 2>/dev/null | grep -cF forgedfn)" = 0 ] && \
   grep -q 'History: reset' <<<"$OUT"; then
    pass RH-06 "an edited carry line is never read, and the run says its history was reset"
else
    fail RH-06 "an edited carry was read" "$(grep -c forgedfn "$G/tele.jsonl")"
fi

# ---- RH-07 a truncated file ------------------------------------------------------------
H="$TEST_TMP/h"; proj "$H"; p_todo "$H"; runp "$H"
: > "$H/tele.jsonl"; p_fixed "$H"; runp "$H"
if [ "$RC" = 0 ] && grep -q 'History: reset -- the telemetry file is shorter than when the history was written' <<<"$OUT" && \
   [ "$(ev "$H" HISTORY_RESET)" = 1 ]; then
    pass RH-07 "a truncated telemetry file resets the history, and the run says so"
else
    fail RH-07 "a truncated file was not reported as a reset" "rc=$RC $(grep 'History' <<<"$OUT")"
fi

# ---- RH-08 a flood after the carry --------------------------------------------------------
flood() { python3 -I -c '
import sys
line=b"{\"event_type\":\"FILLER\",\"n\":\"" + b"x"*200 + b"\"}\n"
with open(sys.argv[1],"ab") as f:
    for _ in range((65*1024*1024)//len(line)+1): f.write(line)' "$1/tele.jsonl"; }
I="$TEST_TMP/i"; proj "$I"; p_todo "$I"; runp "$I"; flood "$I"
rm -f "$I/.naab_cache/history.anchor.json"; p_fixed "$I"; runp "$I"
if [ "$RC" = 0 ] && [ "$(subj "$I" passed compute)" = 0 ]; then
    pass RH-08c "CONTROL: without the anchor, a carry 65 MiB back is past the scan"
else
    fail RH-08c "the backward scan reached past its bound, so RH-08 measures nothing" "rc=$RC passed=$(subj "$I" passed compute)"
fi
rm -rf "$I"
I="$TEST_TMP/i2"; proj "$I"; p_todo "$I"; runp "$I"; flood "$I"; p_fixed "$I"; runp "$I"
if [ "$RC" = 0 ] && [ "$(subj "$I" passed compute)" = 1 ]; then
    pass RH-08 "with the anchor, the carry is found past 65 MiB of later lines"
else
    fail RH-08 "the anchor did not keep the history past a flood" "rc=$RC passed=$(subj "$I" passed compute)"
fi
rm -rf "$I"

# ---- RH-09 a killed run --------------------------------------------------------------------
two_fn() { printf 'fn alpha(x) {\n%b    return x * 2\n}\nfn beta(x) {\n%b    return x + 1\n}\nmain {\n    print(alpha(1) + beta(2))\n}\n' "$2" "$3" > "$1/t.naab"; }
for which in kept cut; do
    J="$TEST_TMP/j_$which"; proj "$J"; two_fn "$J" '    // TODO: a\n' ''; runp "$J"
    cp "$J/.naab_cache/history.anchor.json" "$J/anchor1"; SIZE1=$(wc -c < "$J/tele.jsonl")
    two_fn "$J" '' '    // TODO: b\n'; runp "$J"
    if [ "$which" = kept ]; then
        # killed after its refusal row: everything after beta's row is gone
        python3 -I -c '
import sys
p=sys.argv[1]; L=open(p,"rb").read().split(b"\n")
for i,l in enumerate(L):
    if b"\"event_type\":\"RuleViolation\"" in l and b"\"subject_function\":\"beta\"" in l:
        open(p,"wb").write(b"\n".join(L[:i+1])+b"\n"); break' "$J/tele.jsonl"
    else
        python3 -I -c 'import sys; p=sys.argv[1]; open(p,"r+b").truncate(int(sys.argv[2]))' "$J/tele.jsonl" "$SIZE1"
    fi
    cp "$J/anchor1" "$J/.naab_cache/history.anchor.json"
    two_fn "$J" '' ''; runp "$J"
    eval "BETA_$which=\$(subj \"\$J\" passed beta); RC_$which=\$RC"
done
if [ "$RC_kept" = 0 ] && [ "$BETA_kept" = 1 ]; then
    pass RH-09 "a run killed after its refusal row is still remembered (from the row)"
else
    fail RH-09 "a killed run's refusal was forgotten" "rc=$RC_kept beta_passed=$BETA_kept"
fi
if [ "$RC_cut" = 0 ] && [ "$BETA_cut" = 0 ]; then
    pass RH-09c "CONTROL: with the row cut too, nothing remembers beta"
else
    fail RH-09c "beta was remembered without its row" "rc=$RC_cut beta_passed=$BETA_cut"
fi

# ---- RH-10 two runs at once ------------------------------------------------------------------
conc_prog() { printf 'use time\nfn %s(x) {\n    // TODO: %s\n    return x * 2\n}\nmain {\n    time.sleep(%s)\n    print(%s(1))\n}\n' "$2" "$2" "$3" "$2" > "$1/$2.naab"; }
for mode in together apart; do
    K="$TEST_TMP/k_$mode"; mkdir -p "$K"; PL_LV=advisory cfg "$K"
    conc_prog "$K" alpha 2; conc_prog "$K" beta 2
    if [ "$mode" = together ]; then
        (cd "$K" && timeout 120s "$NAAB" run alpha.naab > a.out 2>&1) &
        P1=$!
        (cd "$K" && timeout 120s "$NAAB" run beta.naab > b.out 2>&1) &
        P2=$!
        wait $P1; wait $P2
    else
        (cd "$K" && timeout 120s "$NAAB" run alpha.naab > a.out 2>&1)
        (cd "$K" && timeout 120s "$NAAB" run beta.naab > b.out 2>&1)
    fi
    eval "REM_$mode=\"\$(remembered \"\$K\")\"; LOADED_$mode=\$(ev \"\$K\" HISTORY_LOADED)"
done
if [ "$REM_apart" = "no_placeholders:alpha no_placeholders:beta" ]; then
    pass RH-10c "CONTROL: one after the other, both advisory refusals are remembered"
else
    fail RH-10c "sequential runs do not remember both refusals, so RH-10 measures nothing" "remembered=[$REM_apart]"
fi
if [ "$LOADED_together" != 0 ]; then
    unm RH-10 "the two runs did not overlap (one read the other's carry)"
elif [ "$REM_together" = "no_placeholders:alpha no_placeholders:beta" ]; then
    pass RH-10 "two runs at once: the later carry keeps the other run's refusal"
else
    fail RH-10 "two runs at once: one run's refusal was forgotten" "remembered=[$REM_together]"
fi

# ---- RH-11 report-only callers -----------------------------------------------------------------
L="$TEST_TMP/l"; mkdir -p "$L"
cat > "$L/govern.json" <<'EOF'
{"mode": "enforce", "security": {"sandbox_level": "elevated"},
 "telemetry": {"enabled": true, "output_file": "tele.jsonl", "tamper_evidence": {"enabled": true}},
 "code_quality": {"no_placeholders": {"enabled": true, "level": "hard"}},
 "contracts": {"functions": {"compute": {"requires": ["x >= 0"], "ensures": ["result >= 0"]}}}}
EOF
p_todo "$L"; runp "$L"
hist_lines() { grep -cE '"event_type":"HISTORY_(LOADED|SUBJECT|CARRY|RESET)"' "$L/tele.jsonl" 2>/dev/null || true; }
H0=$(hist_lines); A0=$(cksum < "$L/.naab_cache/history.anchor.json")
p_fixed "$L"
(cd "$L" && "$GOV" lint t.naab >/dev/null 2>&1); (cd "$L" && "$NAAB" run --lint-only t.naab >/dev/null 2>&1)
(cd "$L" && timeout 120s "$GOV" contracts t.naab --cases 3 --naab "$NAAB" >/dev/null 2>&1)
if [ "$(hist_lines)" = "$H0" ] && [ "$(cksum < "$L/.naab_cache/history.anchor.json")" = "$A0" ] && [ "$H0" -ge 1 ]; then
    pass RH-11 "lint, --lint-only and the contract search neither read nor write history"
else
    fail RH-11 "a report-only caller touched the history" "history lines $H0 -> $(hist_lines)"
fi

# ---- RH-12 the chain switched off ---------------------------------------------------------------
M="$TEST_TMP/m"; proj "$M"; p_todo "$M"; runp "$M"
CHAIN=false cfg "$M"; p_fixed "$M"; runp "$M"
if grep -q 'History: not read -- telemetry or its chain is off' <<<"$OUT"; then
    pass RH-12 "chain off where history existed: one line says it is not read"
else
    fail RH-12 "history silently stopped" "$(grep -c 'History' <<<"$OUT")"
fi
N="$TEST_TMP/n"; mkdir -p "$N"; CHAIN=false cfg "$N"; p_fixed "$N"; runp "$N"
if [ "$RC" = 0 ] && ! grep -q 'History' <<<"$OUT"; then
    pass RH-12c "CONTROL: a project that never had history prints nothing about it"
else
    fail RH-12c "a project with no history was told about it" "rc=$RC"
fi

# ---- RH-13 naab-gov history -------------------------------------------------------------------------
R1=0; O1=$("$GOV" history "$A/tele.jsonl" 2>&1) || R1=$?
R2=0; "$GOV" history "$A/tele.jsonl" --hmac-key wrong >/dev/null 2>&1 || R2=$?
R3=0; "$GOV" history "$B/tele.jsonl" >/dev/null 2>&1 || R3=$?
R4=0; "$GOV" history "$TEST_TMP/none.jsonl" >/dev/null 2>&1 || R4=$?
if [ "$R1" = 0 ] && grep -q 'Repaired (1)' <<<"$O1" && grep -q 'fn compute' <<<"$O1" && \
   [ "$R2" = 2 ] && [ "$R3" = 2 ] && [ "$R4" = 4 ]; then
    pass RH-13 "naab-gov history: a report (0), exit 2 without the key or without history, 4 for no file"
else
    fail RH-13 "naab-gov history's outcomes" "report=$R1 wrongkey=$R2 nohistory=$R3 nofile=$R4"
fi

# ---- RH-14 init --------------------------------------------------------------------------------------
ok=1; bad=""
for preset in relaxed standard strict paranoid; do
    P="$TEST_TMP/init_$preset"; mkdir -p "$P"
    (cd "$P" && "$NAAB" init --governance-preset "$preset" >/dev/null 2>&1)
    if ! python3 -I -c '
import json,sys
d=json.load(open(sys.argv[1]))
t=d.get("telemetry",{})
sys.exit(0 if t.get("enabled") is True and t.get("tamper_evidence",{}).get("enabled") is True and d.get("history",{}).get("enabled") is True else 1)' "$P/govern.json" 2>/dev/null; then
        ok=0; bad="$bad $preset"
    fi
done
P="$TEST_TMP/init_standard"; p_todo "$P"; RC=0; (cd "$P" && timeout 120s "$NAAB" run t.naab >/dev/null 2>&1) || RC=$?
if [ "$ok" = 1 ] && [ "$RC" = 3 ] && [ "$(grep -c '"event_type":"HISTORY_CARRY"' "$P/telemetry.jsonl" 2>/dev/null)" -ge 1 ]; then
    pass RH-14 "init turns telemetry, its chain and history on, and a refusal there is remembered"
else
    fail RH-14 "init does not make history automatic" "presets missing it:${bad:- none} rc=$RC"
fi

# ---- RH-15 remembered code met again in an imported module ---------------------------------
# validate's stub is refused in the entry file, then moved unchanged into a
# module as `export fn`. The module text starts at `export`; the fingerprint
# must read through it, or moved code is never recognised.
p_mod_stub() { printf 'export fn validate(items) {\n    return true\n}\n' > "$1/mod.naab"
               printf 'import "mod.naab" as m\nmain {\n    print(m.validate([1, 2]))\n    print("RAN")\n}\n' > "$1/t.naab"; }
p_mod_loop() { printf 'export fn validate(items) {\n    let ok = true\n    for i in items {\n        if i < 0 { ok = false }\n    }\n    return ok\n}\n' > "$1/mod.naab"
               printf 'import "mod.naab" as m\nmain {\n    print(m.validate([1, 2]))\n    print("RAN")\n}\n' > "$1/t.naab"; }
M="$TEST_TMP/m"; proj "$M"; PL_OFF=1 cfg "$M"; p_stub "$M"; runp "$M"; M1=$RC
p_mod_stub "$M"; runp "$M"; M2=$RC; M2OUT=$OUT
MF="$TEST_TMP/m_fresh"; proj "$MF"; PL_OFF=1 cfg "$MF"; p_mod_stub "$MF"; runp "$MF"
MC="$TEST_TMP/m_other"; proj "$MC"; PL_OFF=1 cfg "$MC"; p_stub "$MC"; runp "$MC"; p_mod_loop "$MC"; runp "$MC"
MO=$("$GOV" history "$M/tele.jsonl" 2>&1)
if [ "$(subj "$MF" seen_in_module validate)" = 0 ] && [ "$(subj "$MC" seen_in_module validate)" = 0 ]; then
    pass RH-15c "CONTROL: no sighting in a fresh project, nor for other code under the same name"
else
    fail RH-15c "a sighting with nothing remembered, or for changed code, so RH-15 measures nothing" "fresh=$(subj "$MF" seen_in_module validate) other=$(subj "$MC" seen_in_module validate)"
fi
# The moved code's verdict is test_refusal_memory.sh's (PR 3 re-checks it, RM-01);
# this arm pins the sighting.
if [ "$M1" = 3 ] && [ "$(subj "$M" seen_in_module validate)" = 1 ] && grep -q 'met in an imported module' <<<"$MO"; then
    pass RH-15 "refused code moved unchanged into a module is recognised there (seen_in_module)"
else
    fail RH-15 "moved code was not recognised in the module" "refused=$M1 moved=$M2 seen=$(subj "$M" seen_in_module validate)"
fi

echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
