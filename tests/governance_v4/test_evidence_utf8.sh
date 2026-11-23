#!/usr/bin/env bash
# ============================================================
# test_evidence_utf8.sh -- evidence is written when text is not UTF-8 (A207)
#
# Every evidence writer serialises with nlohmann, whose strict handler throws
# on a byte that is not UTF-8. Before Bundle 20 PR 5 a program whose refused
# function held one Latin-1 byte in a comment lost every piece of its
# evidence -- no telemetry line, no JSON report, no SARIF -- and an
# advisory-only run ended exit 1 instead of 0 (observed). A HARD block kept its
# exit 3 but recorded nothing, so the next run's history had nothing to
# remember. The writers now pass every field through naab::text::validUtf8()
# (include/naab/utf8_text.h): one U+FFFD per invalid byte.
#
#   EU-00c  the ASCII program: advisory, exit 0, every file written
#   EU-01   the same program with one Latin-1 byte: exit 0, every file written,
#           the chain verifies, the byte recorded as U+FFFD
#   EU-02   HARD: exit 3, every file written, and the next run remembers the
#           refusal (run history); EU-02c the ASCII control remembers it too
#   EU-03   a guidance section holding the byte is delivered by a pull with
#           telemetry on (it ended the run with exit 1), and its recorded
#           text hashes to recorded_sha256
#
# KILL TABLE (each mutant applied alone, rebuilt, the suite re-run; the arms
# that turned red):
#   U1  writeTelemetry dumps strictly (the RuleViolation rows)       -> EU-01 EU-02
#   U2  generateJsonReport dumps strictly                            -> EU-01 EU-02
#   U3  writeAgentTelemetry fields not made valid (strict dump)      -> EU-03
#   U4  recorded_sha256 over the text before validUtf8()             -> EU-03
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
NAAB_GOV="${NAAB_GOV:-$REPO/build/naab-gov}"
TEST_TMP="$(mktemp -d)"

PASS_COUNT=0; FAIL_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"; }
trap cleanup EXIT
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }
[ -x "$NAAB_GOV" ] || { echo "naab-gov not built: UNMEASURABLE, not a pass"; exit 1; }

echo "=== evidence written when text is not UTF-8 (A207) ==="

config() {  # $1 dir, $2 placeholder level
    printf '{"version": "3.0", "mode": "enforce",\n "code_quality": {"no_placeholders": {"enabled": true, "level": "%s"}},\n "telemetry": {"enabled": true, "output_file": "tele.jsonl", "tamper_evidence": {"enabled": true}}}\n' \
        "$2" > "$1/govern.json"
}
prog() {  # $1 file, $2 the byte or letter in the comment
    printf 'fn compute(x) {\n    // TODO caf%b later\n    return x\n}\nmain {\n    print(compute(1))\n}\n' "$2" > "$1"
}
# run DIR -> exit status; stdout+stderr in DIR/out.txt
run() { (cd "$1" && timeout 60s "$NAAB" run --governance-report rep.json --governance-sarif rep.sarif p.naab > out.txt 2>&1); echo $?; }
files_ok() { [ -s "$1/tele.jsonl" ] && [ -s "$1/rep.json" ] && [ -s "$1/rep.sarif" ]; }
chain_ok() { (cd "$1" && "$NAAB" --verify-telemetry-chain tele.jsonl > chain.txt 2>&1); }
# U+FFFD (EF BF BD) recorded where the byte was, in the RuleViolation row.
replaced() { python3 -I -c 'import json,sys
print(any(e.get("event_type") == "RuleViolation" and "caf� later" in json.dumps(e, ensure_ascii=False) for e in map(json.loads, sys.stdin)))' < "$1/tele.jsonl" 2>/dev/null; }

# ------------------------------------------------------------------
# EU-00c / EU-01 -- advisory
# ------------------------------------------------------------------
d0="$TEST_TMP/eu00c"; mkdir -p "$d0"; config "$d0" advisory; prog "$d0/p.naab" 'e'
r0="$(run "$d0")"
if [ "$r0" = 0 ] && files_ok "$d0" && chain_ok "$d0"; then
    pass "EU-00c" "the ASCII program: advisory, exit 0, telemetry, JSON report and SARIF written, the chain verifies"
else
    fail "EU-00c" "the control run -- EU-01 measures nothing" "rc=$r0 $(tail -2 "$d0/out.txt")"
fi
d1="$TEST_TMP/eu01"; mkdir -p "$d1"; config "$d1" advisory; prog "$d1/p.naab" '\xe9'
r1="$(run "$d1")"
if [ "$r1" = 0 ] && files_ok "$d1" && chain_ok "$d1" && [ "$(replaced "$d1")" = True ]; then
    pass "EU-01" "one Latin-1 byte in the refused function: exit 0, every file written, the chain verifies, the byte recorded as U+FFFD"
else
    fail "EU-01" "a byte that is not UTF-8 lost the evidence or changed the exit" "rc=$r1 files=$(ls "$d1" | tr '\n' ' ') $(grep -m1 -o 'json.exception[^]]*\]' "$d1/out.txt")"
fi

# ------------------------------------------------------------------
# EU-02 -- HARD, and the history that depends on the evidence
# ------------------------------------------------------------------
hard_pair() {  # $1 dir, $2 byte -> "rc1 files rc2 remembered"
    local d="$1"; mkdir -p "$d"; config "$d" hard; prog "$d/p.naab" "$2"
    local a b f=no m=no
    a="$(run "$d")"; files_ok "$d" && f=yes
    b="$(run "$d")"; grep -q 'History: 1 refusal(s) remembered' "$d/out.txt" && m=yes
    echo "$a $f $b $m"
}
H2C="$(hard_pair "$TEST_TMP/eu02c" 'e')"
H2="$(hard_pair "$TEST_TMP/eu02" '\xe9')"
if [ "$H2C" = "3 yes 3 yes" ]; then
    pass "EU-02c" "the ASCII HARD refusal: exit 3, files written, the next run remembers it"
else
    fail "EU-02c" "the HARD control -- EU-02 measures nothing" "$H2C"
fi
if [ "$H2" = "3 yes 3 yes" ]; then
    pass "EU-02" "with the Latin-1 byte: exit 3, files written, and the next run still remembers the refusal"
else
    fail "EU-02" "a HARD refusal holding a non-UTF-8 byte left no evidence or no memory" "$H2"
fi

# ------------------------------------------------------------------
# EU-03 -- a guidance section holding the byte
# ------------------------------------------------------------------
d3="$TEST_TMP/eu03"; mkdir -p "$d3/docs"
printf '# Playbook\n\n## No placeholders\nCaf\xe9 rule: write the real logic.\n' > "$d3/docs/playbook.md"
printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},\n "telemetry": {"enabled": true, "output_file": "tele.jsonl", "tamper_evidence": {"enabled": true}},\n "guidance": {"sources": ["docs/playbook.md"]}}\n' > "$d3/govern.json"
printf 'use governance\nmain {\n    let g = governance.guidance("no placeholders")\n    print("GOT " + string(len(g)))\n}\n' > "$d3/p.naab"
r3="$( (cd "$d3" && timeout 60s "$NAAB" p.naab > out.txt 2>&1); echo $?)"
AGREE="$(python3 -I -c 'import hashlib,json,sys
ok=[e for e in map(json.loads, sys.stdin) if e.get("event_type") == "GUIDANCE_DELIVERED"]
print(len(ok) == 1 and "�" in ok[0]["text"] and hashlib.sha256(ok[0]["text"].encode("utf-8")).hexdigest() == ok[0]["recorded_sha256"])' < "$d3/tele.jsonl" 2>/dev/null)"
DIV="$( (cd "$d3" && "$NAAB_GOV" replay tele.jsonl --json 2>/dev/null) | python3 -I -c 'import json,sys; print(json.load(sys.stdin)["checks"]["guidance"]["diverged"])' 2>/dev/null)"
if [ "$r3" = 0 ] && grep -q 'GOT 1' "$d3/out.txt" && [ "$AGREE" = True ] && [ "$DIV" = 0 ]; then
    pass "EU-03" "a section holding the byte is delivered with telemetry on, and its recorded text hashes to recorded_sha256"
else
    fail "EU-03" "the guidance pull failed, or the recorded text and its hash disagree" "rc=$r3 agree=$AGREE diverged=$DIV $(grep -m1 -o 'json.exception[^]]*\]' "$d3/out.txt")"
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, 0 skipped"
[ "$FAIL_COUNT" -eq 0 ]
