#!/usr/bin/env bash
# ============================================================
# test_guidance_history.sh -- the `history` guidance source (Bundle 20 PR 4)
#
# {"type": "history"} is the engine's own facts from earlier runs of the
# project: one section per rule earlier runs refused, named for the rule, with
# the functions refused under it and those that later passed. Selected by the
# existing name matching (the automatic violation route at the terminal, and
# governance.guidance(rule, "history")); nothing ranks it by outcomes. Every
# value is shape-checked or the fact is dropped.
#
#   GH-01  the second refusal shows the first run's fact at the violation;
#          GH-01c the same run in a fresh directory shows none
#   GH-02  a violation of a rule history holds no section for gets nothing
#          from it (the noise control)
#   GH-03  a fact whose file name fails the shape check is dropped, though
#          history remembered the refusal
#   GH-04  the program pulls the section by the rule's name
#   GH-05  after a repair the section also says which function later passed
#   GH-06  --lint-only reads no history, so the source has no document
#   GH-07  history off: nothing, and the miss is recorded (no_history)
#
# Every multi-run arm keeps its telemetry and .naab_cache in one directory, and
# its control is the same run in a fresh directory (the method's "isolation
# can remove the property under test" -- a fresh directory is what has no
# history).
#
# KILL TABLE (each mutant applied alone, the suite re-run; the arms that
# turned red):
#   H1  buildHistoryGuidance() not called                 -> GH-01 GH-04 GH-05
#   H2  displayableRemembered() accepts any value         -> GH-03
#   H3  the repairs left out of the sections              -> GH-05
#   H4  the history source read as a document (no branch) -> GH-01 GH-04 GH-05 GH-07
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
TEST_TMP="$(mktemp -d)"

PASS_COUNT=0; FAIL_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"; }
trap cleanup EXIT
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }

echo "=== the history guidance source (Bundle 20 PR 4) ==="

# config DIR [history_enabled] [extra top-level keys]
config() {
    printf '{"version": "3.0", "mode": "enforce",\n "code_quality": {"no_placeholders": {"enabled": true, "level": "hard"}},\n "requirements": {"main_block": {"enabled": true, "level": "hard"}},\n "history": {"enabled": %s},\n "guidance": {"sources": [{"type": "history"}]},\n "telemetry": {"enabled": true, "output_file": "tele.jsonl", "tamper_evidence": {"enabled": true}}}\n' \
        "${2:-true}" > "$1/govern.json"
}
# A function the placeholder check refuses, and its fixed form.
STUB_FN='fn compute(x) {
    // TODO: implement this properly
    return x
}'
FIXED_FN='fn compute(x) {
    if x > 10 { return x * 2 }
    return x + 1
}'
prog() {  # $1 file, $2 function text, [$3 main body]
    printf '%s\nmain {\n%s\n    print(compute(3))\n}\n' "$2" "${3:-}" > "$1"
}
# run DIR FILE -> stdout+stderr in DIR/out.txt, exit status echoed
run() { (cd "$1" && timeout 60s "$NAAB" run "$2" > out.txt 2>&1); echo $?; }
HIST='Project guidance (history > code_quality.no_placeholders)'

# ------------------------------------------------------------------
# GH-01 -- the fact at the violation
# ------------------------------------------------------------------
d="$TEST_TMP/gh01"; mkdir -p "$d"; config "$d"; prog "$d/p.naab" "$STUB_FN"
rc1="$(run "$d" p.naab)"; cp "$d/out.txt" "$d/out1.txt"
rc2="$(run "$d" p.naab)"; cp "$d/out.txt" "$d/out2.txt"
dc="$TEST_TMP/gh01c"; mkdir -p "$dc"; config "$dc"; prog "$dc/p.naab" "$STUB_FN"
rcc="$(run "$dc" p.naab)"
# Bytes, not a path, cross to python3: under MSYS2 it is a native program
# that cannot open the shell's /tmp names (CLAUDE.md, output-channel gotcha).
FIRST_RUN="$(python3 -I -c 'import json,sys
for l in sys.stdin:
    e=json.loads(l)
    if e.get("event_type")=="RunStart": print(e["run_id"]); break' < "$d/tele.jsonl" 2>/dev/null)"
if [ "$rcc" = 3 ] && ! grep -q 'Project guidance (history' "$dc/out.txt" && [ "$rc1" = 3 ] && ! grep -q 'Project guidance (history' "$d/out1.txt"; then
    pass "GH-01c" "a run with no earlier history is refused and shows nothing from the history source"
else
    fail "GH-01c" "a run without history showed history guidance, or was not refused" "rc=$rcc/$rc1 $(grep -m2 'guidance' "$dc/out.txt")"
fi
if [ "$rc2" = 3 ] && grep -qF "$HIST" "$d/out2.txt" \
   && grep -q "fn compute (p.naab line 2) was refused in run ${FIRST_RUN:-NONE} at hard" "$d/out2.txt"; then
    pass "GH-01" "the second refusal shows, at the violation, which run refused the same function under the same rule"
else
    fail "GH-01" "the history fact is missing at the violation" "rc=$rc2 first=$FIRST_RUN $(grep -A3 'Project guidance' "$d/out2.txt" | head -5)"
fi

# ------------------------------------------------------------------
# GH-02 -- a rule history holds nothing for
# ------------------------------------------------------------------
# Same directory, so the history holds the placeholder refusal; this run is
# refused for having no main block, a rule with no section.
printf '%s\n' "$FIXED_FN" > "$d/nomain.naab"
rcn="$(run "$d" nomain.naab)"
if [ "$rcn" = 3 ] && grep -q 'requirements.main_block' "$d/out.txt" && ! grep -q 'Project guidance (history' "$d/out.txt"; then
    pass "GH-02" "a violation of a rule history has no section for gets nothing from it, though history is present"
else
    fail "GH-02" "history guidance was shown for an unrelated rule" "rc=$rcn $(grep -m3 'main_block\|guidance' "$d/out.txt")"
fi

# ------------------------------------------------------------------
# GH-03 -- a value that fails the shape check is dropped
# ------------------------------------------------------------------
d3="$TEST_TMP/gh03"; mkdir -p "$d3"; config "$d3"; prog "$d3/odd name.naab" "$STUB_FN"
run "$d3" "odd name.naab" >/dev/null
r3="$(run "$d3" "odd name.naab")"
if [ "$r3" = 3 ] && grep -q 'History: 1 refusal(s) remembered' "$d3/out.txt" && ! grep -q 'Project guidance (history' "$d3/out.txt"; then
    pass "GH-03" "history remembered the refusal, but its file name ('odd name.naab') fails the shape check, so the fact is not shown"
else
    fail "GH-03" "a fact with an unplain file name was shown, or history did not remember it" "rc=$r3 $(grep -m3 'History\|guidance' "$d3/out.txt")"
fi

# ------------------------------------------------------------------
# GH-04 / GH-05 -- pulled by name; a repair is a fact too
# ------------------------------------------------------------------
d4="$TEST_TMP/gh04"; mkdir -p "$d4"; config "$d4"; prog "$d4/p.naab" "$STUB_FN"
run "$d4" p.naab >/dev/null
printf 'use governance\n%s\nmain {\n    let g = governance.guidance("no_placeholders", "history")\n    if len(g) > 0 { print("PULLED " + g[0]["heading"] + " :: " + g[0]["text"]) } else { print("PULLED nothing") }\n    print(compute(3))\n}\n' "$FIXED_FN" > "$d4/p.naab"
r4="$(run "$d4" p.naab)"; cp "$d4/out.txt" "$d4/out2.txt"
d4c="$TEST_TMP/gh04c"; mkdir -p "$d4c"; config "$d4c"; cp "$d4/p.naab" "$d4c/p.naab"
r4c="$(run "$d4c" p.naab)"
if [ "$r4c" = 0 ] && grep -q 'PULLED nothing' "$d4c/out.txt" && [ "$r4" = 0 ] \
   && grep -q 'PULLED code_quality.no_placeholders :: Earlier runs of this project refused this rule:' "$d4/out2.txt" \
   && grep -q 'fn compute (p.naab line 2) was refused in run' "$d4/out2.txt"; then
    pass "GH-04" "the passing program pulls the section by the rule's name; with no history, the same pull returns nothing"
else
    fail "GH-04" "the pull did not return the history section" "rc=$r4/$r4c $(grep PULLED "$d4/out2.txt" "$d4c/out.txt")"
fi
# The second run passed the rule for the same function: a repair. A third
# run, refused again, shows both facts.
prog "$d4/p.naab" "$STUB_FN"
r5="$(run "$d4" p.naab)"
if [ "$r5" = 3 ] && grep -qF "$HIST" "$d4/out.txt" && grep -q 'Earlier runs passed it after a refusal:' "$d4/out.txt" \
   && grep -q 'fn compute (p.naab) was refused in run .* and passed in run .* at hard or stricter' "$d4/out.txt"; then
    pass "GH-05" "after a repair the section also says the function was refused in one run and passed in a later one"
else
    fail "GH-05" "the repair is not in the history section" "rc=$r5 $(grep -A6 'Project guidance' "$d4/out.txt" | head -8)"
fi

# ------------------------------------------------------------------
# GH-06 -- --lint-only reads no history
# ------------------------------------------------------------------
r6="$( (cd "$d" && timeout 60s "$NAAB" run --lint-only p.naab > out6.txt 2>&1); echo $?)"
if [ "$r6" = 3 ] && grep -q 'no_placeholders' "$d/out6.txt" && ! grep -q 'Project guidance (history' "$d/out6.txt"; then
    pass "GH-06" "--lint-only is refused as the run is, and shows nothing from history (it reads none)"
else
    fail "GH-06" "--lint-only showed history guidance, or was not refused" "rc=$r6 $(grep -m3 'guidance\|placeholder' "$d/out6.txt")"
fi

# ------------------------------------------------------------------
# GH-07 -- history off
# ------------------------------------------------------------------
d7="$TEST_TMP/gh07"; mkdir -p "$d7"; config "$d7"; prog "$d7/p.naab" "$STUB_FN"
run "$d7" p.naab >/dev/null
config "$d7" false
r7="$(run "$d7" p.naab)"
MISS="$(python3 -I -c 'import json,sys
print(sum(1 for l in sys.stdin if (lambda e: e.get("event_type")=="GUIDANCE_WITHHELD" and e.get("reason")=="no_history")(json.loads(l))))' < "$d7/tele.jsonl" 2>/dev/null)"
if [ "$r7" = 3 ] && ! grep -q 'Project guidance (history' "$d7/out.txt" && [ "${MISS:-0}" -ge 1 ]; then
    pass "GH-07" "with history off the source has no document: nothing is shown and the miss is recorded (no_history)"
else
    fail "GH-07" "history guidance with history off, or the miss not recorded" "rc=$r7 misses=$MISS"
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, 0 skipped"
[ "$FAIL_COUNT" -eq 0 ]
