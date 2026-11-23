#!/usr/bin/env bash
# ============================================================
# test_claim_reader.sh -- the claim reader's table (Bundle 20 PR 5)
#
# Every row of claim_reader.psv through `naab-gov claims`, which calls the
# function the engine reads replies with (include/naab/claim_reader.h).
#
#   CR-00  the table covers every rule the header lists (each row id present,
#          each row well formed); a row that cannot be read fails, it does not
#          skip
#   CR-<id> one arm per row: the claims read, and what the reply says per tool
#   CR-90  the command refuses a missing file and an unknown option (exit 4)
#
# KILL TABLE (each mutant applied alone to src/runtime/claim_reader.cpp,
# naab-gov rebuilt, the suite re-run; the rows that turned red):
#   C1  negation ignored (negated always false)          -> R02 R03 R04 R05 R28 R33 R42
#   C2  questions read                                     -> R08 R37
#   C3  conditionals read                                  -> R09 R10
#   C4  quotes and inline code read                        -> R11 R12 R32
#   C5  fenced blocks and block quotes read                -> R13 R14
#   C6  the infinitive rule removed                        -> R15 R29
#   C7  no modal: predictions read as success              -> R16 R17 R41 R42
#   C8  the fixed-clause object rule removed               -> R06 R07
#   C9  tool names split at '_' and '-' (the old S19)      -> R01 R06 R11 R12 R21 R22 R23 R24 R25 R28 R31 R32 R33 R36 R39
#   C10 the next-sentence fallback removed                 -> R24
#   C11 the other-tool guard on the fallback removed       -> R25
#   C12 failure verbs negate failure words                 -> R27
#   C13 a negation window of four words                    -> R35
#   C14 clause boundaries removed                          -> R34
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
GOV="${NAAB_GOV:-$REPO/build/naab-gov}"
TABLE="$SCRIPT_DIR/claim_reader.psv"

PASS_COUNT=0; FAIL_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

[ -x "$GOV" ] || { echo "naab-gov not built ($GOV): UNMEASURABLE, not a pass"; exit 1; }
[ -f "$TABLE" ] || { echo "claim_reader.psv missing"; exit 1; }

echo "=== the claim reader (Bundle 20 PR 5) ==="

# The JSON report, as one line: the claim tokens, then '|', then the tools.
# Python reads stdin only: under MSYS2 it is a native program that cannot open
# the shell's paths (CLAUDE.md, output-channel gotcha).
SUMMARISE='import json,sys
d=json.load(sys.stdin)
cl=" ".join(c["kind"]+("+" if c["positive"] else "-")+("!" if c["negated"] else "")+c["word"].lower() for c in d["claims"]) or "-"
tl=",".join(k+"="+v for k,v in sorted(d["tools"].items())) or "-"
sys.stdout.write(cl+"|"+tl)'

# ------------------------------------------------------------------
# CR-00 -- the table is readable and complete
# ------------------------------------------------------------------
ROWS=0; BAD=""
while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    n="$(awk -F'|' '{print NF}' <<<"$line")"
    if [ "$n" -ne 6 ]; then BAD="$BAD ${line%%|*}($n)"; fi
    ROWS=$((ROWS + 1))
done < "$TABLE"
if [ "$ROWS" -ge 42 ] && [ -z "$BAD" ]; then
    pass "CR-00" "the table has $ROWS well-formed rows"
else
    fail "CR-00" "the table is short or has malformed rows" "rows=$ROWS bad:$BAD"
fi

# ------------------------------------------------------------------
# One arm per row
# ------------------------------------------------------------------
while IFS='|' read -r id text tools want_claims want_tools why; do
    case "$id" in ''|'#'*) continue ;; esac
    args=()
    [ "$tools" != "-" ] && args=(--tools "$tools")
    got="$(printf '%b' "$text" | "$GOV" claims - "${args[@]}" --json 2>/dev/null | python3 -I -c "$SUMMARISE" 2>/dev/null)"
    want="$want_claims|$want_tools"
    if [ "$got" = "$want" ]; then
        pass "CR-$id" "$why"
    else
        fail "CR-$id" "$why" "want [$want] got [$got]"
    fi
done < "$TABLE"

# ------------------------------------------------------------------
# CR-90 -- usage errors are exit 4, never a report
# ------------------------------------------------------------------
"$GOV" claims /nonexistent/reply.txt > /dev/null 2>&1; r1=$?
"$GOV" claims - --bogus < /dev/null > /dev/null 2>&1; r2=$?
printf 'It worked.' | "$GOV" claims - > /dev/null 2>&1; r3=$?
if [ "$r1" = 4 ] && [ "$r2" = 4 ] && [ "$r3" = 0 ]; then
    pass "CR-90" "a missing file and an unknown option exit 4; a readable reply exits 0"
else
    fail "CR-90" "usage exit codes" "missing=$r1 bogus=$r2 ok=$r3"
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, 0 skipped"
[ "$FAIL_COUNT" -eq 0 ]
