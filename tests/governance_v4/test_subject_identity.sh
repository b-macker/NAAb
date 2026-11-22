#!/usr/bin/env bash
# ============================================================
# test_subject_identity.sh -- what a refusal records about its subject (Bundle 20 PR 2)
#
# A refusal of a function-body check (no_placeholders, no_oversimplification,
# no_incomplete_logic, complexity_floor) names its subject on the chained
# RuleViolation row: the function, its file (relative to govern.json, '/'
# separators), the SHA-256 of its text with CRLF read as LF, and a code
# fingerprint -- the run's own lexer tokens with comments and whitespace
# dropped, local names renamed in order of use, the function's own name
# replaced and literal values reduced to their kind. The refused text is kept
# in .naab_cache/<sha256>.subject.txt, which the program may not write.
#
# A body that matches a secret or PII pattern is never stored and its text is
# never hashed (telemetry is forwarded off the machine, and a hash of a short
# body can be searched for); its fingerprint, which holds no literal value or
# comment, is kept, so a PII-shaped comment does not take a function out of
# the history.
#
#   SI-00  CONTROL: a refused function's row carries a 64-hex sha256 and
#          fingerprint, and the stored text is its text
#   SI-01  renamed function and locals: the same fingerprint, another sha256
#   SI-02  one operator changed: another fingerprint
#   SI-03  CRLF line endings: the same sha256 and fingerprint as LF
#   SI-04  a key-shaped literal in the body: no sha256, no stored text; the
#          fingerprint is kept and is the same as without the literal value's
#          content (SI-04c the same function without it is stored)
#   SI-05  an SSN-shaped comment in the body: no sha256, no stored text, the
#          fingerprint kept
#   SI-06  the program may not write the store (governance.state_files);
#          SI-06c it writes elsewhere
#
# Every secret- or PII-shaped value is assembled by this script from
# fragments; none is written in this file.
#
# Kill table (each mutant applied alone, rebuilt, this suite run; observed
# 2026-11-21 on the default build):
#   S-M1  local names not renamed                       -> SI-01 red
#   S-M2  literal values kept in the fingerprint         -> SI-04 red
#   S-M3  CRLF not read as LF                            -> SI-03 red
#   S-M4  a secret- or PII-shaped body hashed and stored -> SI-04 SI-05 red
#   S-M5  such a body loses its fingerprint              -> SI-04 SI-05 red
# On the tree before this PR (#394's head a8951878, observed 2026-11-22) SI-00
# fails and the suite stops there.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_subject_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2${3:+ -- $3}"; }

echo "=== test_subject_identity.sh ==="
if [ ! -x "$NAAB" ]; then echo "  UNMEASURABLE: naab-lang not built"; echo "Result: 0 passed, 0 failed, 1 unmeasurable"; exit 1; fi

# refuse ID TEXT: run TEXT (a program) in a fresh project whose no_placeholders
# is hard. Sets D, RC, OUT, SHA and FP (from the RuleViolation row).
refuse() {
    D="$TEST_TMP/$1"; mkdir -p "$D"
    cat > "$D/govern.json" <<'EOF'
{"mode": "enforce", "security": {"sandbox_level": "elevated"},
 "capabilities": {"filesystem": {"mode": "write"}},
 "telemetry": {"enabled": true, "output_file": "tele.jsonl", "tamper_evidence": {"enabled": true}},
 "code_quality": {"no_placeholders": {"enabled": true, "level": "hard"}}}
EOF
    printf '%s' "$2" > "$D/t.naab"
    RC=0; OUT=$(cd "$D" && timeout 60s "$NAAB" run t.naab 2>&1) || RC=$?
    local row
    row=$(grep -F '"event_type":"RuleViolation"' "$D/tele.jsonl" 2>/dev/null | grep -F '"subject_function"' | head -1)
    SHA=$(grep -oE '"subject_sha256":"[0-9a-f]{64}"' <<<"$row" | grep -oE '[0-9a-f]{64}')
    FP=$(grep -oE '"subject_fp":"[0-9a-f]{64}"' <<<"$row" | grep -oE '[0-9a-f]{64}')
    FN=$(grep -oE '"subject_function":"[a-z_]+"' <<<"$row" | cut -d'"' -f4)
}
stored() { ls "$D/.naab_cache/"*.subject.txt 2>/dev/null | wc -l | tr -d ' '; }

MAIN=$'main {\n    print(compute(3))\n}\n'
BASE=$'fn compute(x) {\n    // TODO: finish this\n    let y = x * 2\n    return y\n}\n'"$MAIN"

refuse si00 "$BASE"; SHA0=$SHA; FP0=$FP
if [ "$RC" = 3 ] && [ -n "$SHA0" ] && [ -n "$FP0" ] && [ "$FN" = compute ] && \
   grep -q 'let y = x \* 2' "$D/.naab_cache/$SHA0.subject.txt" 2>/dev/null; then
    pass SI-00 "CONTROL: the row names the subject and the stored text is the function's"
else
    fail SI-00 "the refusal did not record its subject; nothing below can be read" "rc=$RC sha=${SHA0:-none} fp=${FP0:-none} fn=${FN:-none}"
    echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
fi

RENAMED=$'fn calculate(value) {\n    // TODO: finish this one later\n    let doubled = value * 2\n    return doubled\n}\nmain {\n    print(calculate(3))\n}\n'
refuse si01 "$RENAMED"
if [ "$RC" = 3 ] && [ "$FP" = "$FP0" ] && [ -n "$SHA" ] && [ "$SHA" != "$SHA0" ]; then
    pass SI-01 "renamed function and locals: the same fingerprint, another text hash"
else
    fail SI-01 "renaming changed the fingerprint" "fp=$FP vs $FP0 sha=$SHA"
fi

CHANGED=$'fn compute(x) {\n    // TODO: finish this\n    let y = x + 2\n    return y\n}\n'"$MAIN"
refuse si02 "$CHANGED"
if [ "$RC" = 3 ] && [ -n "$FP" ] && [ "$FP" != "$FP0" ]; then
    pass SI-02 "one operator changed: another fingerprint"
else
    fail SI-02 "a changed statement kept the fingerprint" "fp=$FP"
fi

refuse si03 "$(printf '%s' "$BASE" | sed 's/$/\r/')"
if [ "$RC" = 3 ] && [ "$SHA" = "$SHA0" ] && [ "$FP" = "$FP0" ]; then
    pass SI-03 "CRLF line endings: the same text hash and fingerprint as LF"
else
    fail SI-03 "CRLF changed the subject's identity" "sha=$SHA fp=$FP"
fi

# A key shape assembled from fragments (AKIA + 16 upper-case letters).
K1="AKI"; K2="AQWERTYUIOP"; K3="ASDFGH"; KEYLIKE="$K1$K2$K3"
SECRET=$'fn compute(x) {\n    // TODO: finish this\n    let y = x * 2\n    let k = "'"$KEYLIKE"$'"\n    return y\n}\n'"$MAIN"
PLAIN=$'fn compute(x) {\n    // TODO: finish this\n    let y = x * 2\n    let k = "plainvalue"\n    return y\n}\n'"$MAIN"
refuse si04c "$PLAIN"; FP_PLAIN=$FP
if [ "$RC" = 3 ] && [ -n "$SHA" ] && [ "$(stored)" = 1 ]; then
    pass SI-04c "CONTROL: the same function with a plain literal is hashed and stored"
else
    fail SI-04c "the plain variant was not stored, so SI-04 measures nothing" "rc=$RC sha=$SHA stored=$(stored)"
fi
refuse si04 "$SECRET"
if [ "$RC" = 3 ] && [ "$FN" = compute ] && [ -z "$SHA" ] && [ "$(stored)" = 0 ] && \
   [ "$FP" = "$FP_PLAIN" ] && ! grep -qF "$KEYLIKE" "$D/tele.jsonl"; then
    pass SI-04 "a key-shaped literal: named, fingerprinted without its value, never hashed or stored"
else
    fail SI-04 "a secret-bearing body was hashed or stored" "sha=${SHA:-none} stored=$(stored) fp=$FP vs $FP_PLAIN"
fi

# An SSN shape assembled from fragments, in a comment.
S1="123"; S2="45"; S3="6789"
PII=$'fn compute(x) {\n    // TODO: finish this\n    // ref '"$S1-$S2-$S3"$'\n    let y = x * 2\n    return y\n}\n'"$MAIN"
refuse si05 "$PII"
if [ "$RC" = 3 ] && [ "$FN" = compute ] && [ -z "$SHA" ] && [ "$(stored)" = 0 ] && [ "$FP" = "$FP0" ]; then
    pass SI-05 "a PII-shaped comment: not hashed or stored, and the fingerprint is the plain body's"
else
    fail SI-05 "a PII-bearing body was hashed or stored, or lost its fingerprint" "sha=${SHA:-none} stored=$(stored) fp=$FP"
fi

# SI-06: the program may not write the store.
WR=$'use file\nmain {\n    file.write(".naab_cache/planted.subject.txt", "x")\n    print("WROTE")\n}\n'
refuse si06 "$WR"
if [ "$RC" = 3 ] && grep -q 'governance.state_files' <<<"$OUT" && [ ! -f "$D/.naab_cache/planted.subject.txt" ]; then
    pass SI-06 "the program may not write the subject store"
else
    fail SI-06 "the program wrote into .naab_cache" "rc=$RC"
fi
WR2=$'use file\nmain {\n    file.write("planted.txt", "x")\n    print("WROTE")\n}\n'
refuse si06c "$WR2"
if [ "$RC" = 0 ] && grep -q 'WROTE' <<<"$OUT" && [ -f "$D/planted.txt" ]; then
    pass SI-06c "CONTROL: the same write elsewhere succeeds"
else
    fail SI-06c "the write control failed, so SI-06 measures nothing" "rc=$RC"
fi

echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
