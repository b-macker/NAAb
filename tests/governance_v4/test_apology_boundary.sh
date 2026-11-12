#!/usr/bin/env bash
# test_apology_boundary.sh -- the apologetic-language patterns match words, not
# pieces of words (A149), and naab-gov lint says which of its findings a run
# does not enforce (A132's follow-up; the owner's decisions of 2026-11-11)
#
# A149. code_quality.no_apologetic_language searched its patterns anywhere in
# the text, so "oops" matched inside "Loops": a Python block whose only comment
# was "# Loops over the items" was refused (observed on master 93d2e099, SOFT,
# exit 3). The other patterns had the same shape through their leading "I" /
# "my" ("API didn't check", "UI should have checked", "army apologies"). Each
# pattern now starts and ends at a word boundary. "whoops" is kept as its own
# word, so nothing a person would call an apology stops being flagged.
#
# The lint label. naab-gov lint applies 15 whole-file checks that a run applies
# inside polyglot blocks only (A132: kept that way by decision). A finding of
# one of them is now followed by a note naming the rules and saying a run does
# not enforce them on NAAb code. The note changes no verdict and no exit code.
#
# Arms
#   AB-00   control, real run: a block comment that says "oops" is refused
#   AB-01   real run: a block comment that says "Loops" runs, and its value
#           reaches the program
#   AB-02   still flagged, one per pattern: oops, Oops!, whoops, yikes, uh oh,
#           I'm sorry, Im very sorry, I apologize, my apologies, I'll fix it
#           right away, I didn't check, I should have tested
#   AB-03   no longer flagged, the word inside another: Loops, loops, hoops,
#           snoops, API didn't check, UI should have checked, army apologies,
#           wifi apologize
#   LL-01   lint, a lint-only finding in NAAb text: the note names the rule
#   LL-01c  the same file RUN is not refused and reports no such finding
#   LL-02   lint, a clean file: no note
#   LL-03   lint, a finding of a check a run does apply (a placeholder): no note
#   LL-04   the note changes no exit code: at hard level lint still exits 3,
#           with the note printed (the hard finding throws; the note follows)
#   LL-05   two lint-only rules fire (audit mode, where the first finding does
#           not end the scan): both are named
#
# Kill table (each mutant observed to turn its arm red; run by hand)
#   the boundary removed from "oops"                      -> AB-01, AB-03
#   the leading boundary removed from the "I ..." patterns -> AB-03
#   "whoops" dropped from the pattern                     -> AB-02
#   the note never printed                                -> LL-01, LL-04, LL-05
#   the note printed for every finding                    -> LL-03
#   the note printed with no finding                      -> LL-02, LL-03
#   a throwing check's finding not collected              -> LL-01, LL-04
#   lint exits 0 once the note is printed                 -> LL-04
# Survived, and left out: the note's rule list not de-duplicated. Each check
# returns at its first finding, so no rule is recorded twice by one lint today;
# the de-duplication is a guard, not a tested behaviour.
#
# Red first (observed, master 93d2e099): AB-01, AB-03, LL-01, LL-04 and LL-05
# fail; AB-00, AB-02, LL-01c, LL-02 and LL-03 pass.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
GOV="${NAAB_GOV:-$(dirname "$NAAB")/naab-gov}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_apol_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       -> /'; }
skip() { UNM=$((UNM+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }
has() { grep -qF -- "$2" <<<"$1"; }
RULE="code_quality.no_apologetic_language"
NOTE="not enforced when the program runs"

if [ ! -x "$NAAB" ] || [ ! -x "$GOV" ]; then
    echo "  FAIL [setup] naab-lang or naab-gov not built at $(dirname "$NAAB")"; echo "Results: 0 passed, 1 failed, 0 skipped"; exit 1
fi

# A config that switches the check on. $1 = its level.
config() {
    printf '{"mode": "enforce", "security": {"sandbox_level": "elevated"},\n "code_quality": {"no_apologetic_language": {"level": "%s"}}}\n' "$1"
}

# --- real runs --------------------------------------------------------------
run_block() {   # $1 = dir name, $2 = the comment line; sets OUT, RC
    local d="$W/$1"; mkdir -p "$d"
    config soft > "$d/govern.json"
    cat > "$d/p.naab" <<EOF
main {
    let r = <<python
$2
total = 0
for i in [1, 2, 3]:
    total = total + i
total
>>
    print("VALUE=" + r)
}
EOF
    OUT="$(cd "$d" && "$NAAB" run p.naab 2>&1)"; RC=$?
}

PY_OK=1
run_block probe "# adds the numbers"
if [ "$RC" != 0 ] || ! has "$OUT" "VALUE=6"; then PY_OK=0; fi

if [ "$PY_OK" = 0 ]; then
    skip AB-00 "a plain Python block does not run here (rc=$RC)"
    skip AB-01 "a plain Python block does not run here (rc=$RC)"
else
    run_block ab00 "# oops, this adds the numbers"
    if [ "$RC" = 3 ] && has "$OUT" "$RULE" && ! has "$OUT" "VALUE=6"; then
        pass AB-00 "control: a block comment that says \"oops\" is refused (exit 3)"
    else fail AB-00 "a block comment that says \"oops\" was not refused (rc=$RC)" "$OUT"; fi

    run_block ab01 "# Loops over the items"
    if [ "$RC" = 0 ] && has "$OUT" "VALUE=6" && ! has "$OUT" "$RULE"; then
        pass AB-01 "a block comment that says \"Loops\" runs, and its value reaches the program"
    else fail AB-01 "a block comment that says \"Loops\" was refused or did not run (rc=$RC)" "$OUT"; fi
fi

# --- the patterns, through naab-gov check -------------------------------------
mkdir -p "$W/chk"; config soft > "$W/chk/govern.json"
flagged() {   # $1 = comment text; 0 when the rule fires
    local out
    out="$(printf '# %s\nx = 1\nprint(x)\n' "$1" | "$GOV" check --language python --config "$W/chk/govern.json" 2>&1)"
    has "$out" "$RULE"
}
# Is the checker judging at all? (a dead naab-gov would pass every "not flagged" row)
if ! flagged "I apologize for this"; then
    fail AB-02 "the checker does not flag \"I apologize\": it is not judging" ""
    fail AB-03 "the checker is not judging, so an unflagged row means nothing" ""
else
    missed=""
    while IFS= read -r t; do
        [ -z "$t" ] && continue
        flagged "$t" || missed="$missed [$t]"
    done <<'EOF'
oops
Oops! forgot the edge case
whoops, off by one
yikes this is slow
uh oh, this breaks on empty input
I'm sorry, this is a basic version
Im very sorry about this
I apologize for the mess
my apologies for the hack
I'll fix it right away
I didn't check the empty case
I should have tested this
EOF
    if [ -z "$missed" ]; then pass AB-02 "every apology is still flagged (12 phrasings)"
    else fail AB-02 "no longer flagged:$missed" ""; fi

    wrong=""
    while IFS= read -r t; do
        [ -z "$t" ] && continue
        flagged "$t" && wrong="$wrong [$t]"
    done <<'EOF'
Loops over the items
both loops share one counter
jump through hoops to keep the order
snoops on the socket for a reply
the API didn't check the header
the UI should have checked the box
army apologies are out of scope here
wifi apologize handler
EOF
    if [ -z "$wrong" ]; then pass AB-03 "a pattern inside another word is not flagged (8 phrasings)"
    else fail AB-03 "flagged though the word is part of another:$wrong" ""; fi
fi

# --- the lint label ------------------------------------------------------------
lint_dir() {   # $1 = name, $2 = level, $3 = extra code_quality JSON (or ""), stdin = program
    local d="$W/$1"; mkdir -p "$d"
    printf '{"mode": "%s", "security": {"sandbox_level": "elevated"},\n "code_quality": {"no_apologetic_language": {"level": "%s"}%s}}\n' "${LINT_MODE:-enforce}" "$2" "$3" > "$d/govern.json"
    cat > "$d/p.naab"
    OUT="$(cd "$d" && "$GOV" lint p.naab 2>&1)"; RC=$?
}

lint_dir ll01 advisory "" <<'EOF'
// I apologize for the shape of this
main {
    let total = 2 + 3
    print("TOTAL=" + total)
}
EOF
if has "$OUT" "$NOTE" && has "$(grep -F -A3 -- "$NOTE" <<<"$OUT")" "$RULE"; then
    pass LL-01 "a lint-only finding in NAAb text is followed by the note, which names the rule"
else fail LL-01 "no note naming $RULE after a lint-only finding (rc=$RC)" "$OUT"; fi

ROUT="$(cd "$W/ll01" && "$NAAB" run p.naab 2>&1)"; RRC=$?
if [ "$RRC" = 0 ] && has "$ROUT" "TOTAL=5" && ! has "$ROUT" "$RULE"; then
    pass LL-01c "the same file run is not refused and reports no such finding (the note is true)"
else fail LL-01c "the run refused the file or reported the finding (rc=$RRC)" "$ROUT"; fi

lint_dir ll02 advisory "" <<'EOF'
main {
    let total = 2 + 3
    print("TOTAL=" + total)
}
EOF
if [ "$RC" = 0 ] && ! has "$OUT" "$NOTE"; then pass LL-02 "a clean file gets no note"
else fail LL-02 "a clean file got the note (rc=$RC)" "$OUT"; fi

lint_dir ll03 advisory ', "no_placeholders": {"level": "advisory"}' <<'EOF'
main {
    // TODO: implement the real total
    let total = 2 + 3
    print("TOTAL=" + total)
}
EOF
if has "$OUT" "code_quality.no_placeholders" && ! has "$OUT" "$NOTE"; then
    pass LL-03 "a finding of a check a run also applies (a placeholder) gets no note"
elif ! has "$OUT" "code_quality.no_placeholders"; then
    fail LL-03 "the placeholder control was not flagged, so the arm shows nothing" "$OUT"
else fail LL-03 "the note was printed for a check a run applies" "$OUT"; fi

lint_dir ll04 hard "" <<'EOF'
// I apologize for the shape of this
main {
    print("x")
}
EOF
if [ "$RC" = 3 ] && has "$OUT" "$NOTE"; then
    pass LL-04 "the note changes no exit code: at hard level lint still exits 3, with the note"
else fail LL-04 "hard level: expected exit 3 with the note (rc=$RC)" "$OUT"; fi

# Under enforce the first blocking finding ends lint's scan, so two rules are
# seen together only where nothing blocks: audit mode.
LINT_MODE=audit lint_dir ll05 advisory ', "no_simulation_markers": {"level": "advisory"}' <<'EOF'
// I apologize for the shape of this
// my apologies again
main {
    // the result is simulated here
    print("x")
}
EOF
NB="$(grep -F -A3 -- "$NOTE" <<<"$OUT")"
if has "$NB" "$RULE" && has "$NB" "code_quality.no_simulation_markers" &&
   [ "$(grep -oF -- "$RULE" <<<"$NB" | wc -l)" = 1 ]; then
    pass LL-05 "two lint-only rules fire: both are named"
else fail LL-05 "the note does not name both rules once each" "$OUT"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $UNM skipped"
[ "$FAIL" = 0 ]
