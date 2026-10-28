#!/usr/bin/env bash
# ============================================================
# test_native_encoding_exfil.sh -- crypto.base64_* / hex_* answer to
# restrictions.data_exfiltration's encoding flags (F19, Bundle 3)
#
# block_base64_encode_secrets refuses base64.b64encode( / b64decode( in a
# polyglot block and block_hex_encode_secrets binascii.hexlify( / unhexlify(
# -- by regex over SOURCE. crypto.base64_encode() and its siblings do the same
# encoding natively and never passed through that scan. Measured before the
# fix (A19/F19, observed): under the restriction the polyglot spelling was
# refused (exit 3) while crypto.base64_encode("password123") printed
# cGFzc3dvcmQxMjM= (exit 0).
#
#   NE-00  CONTROL: the polyglot base64.b64encode( is refused under the
#          restriction -- the fixture turns the rule on
#   NE-01  crypto.base64_encode is refused (exit 3) and prints no encoding
#   NE-02  crypto.base64_decode is refused (the source rule covers b64decode)
#   NE-03  crypto.hex_encode is refused (block_hex_encode_secrets)
#   NE-04  crypto.hex_decode is refused
#   NE-05  CONTROL: with the restriction off, crypto.base64_encode runs
#   NE-06  CONTROL: block_base64_encode_secrets false leaves base64 running,
#          while hex (its own flag still on) is refused
#   NE-07  CONTROL: operator patterns replace the built-in ones, as for source,
#          so the native encoders run
#   NE-08  level advisory: the call runs and a warning is printed (cfg.level,
#          as the source rule)
#
# Kill table (each mutant rebuilt, this suite run):
#   the checkEncodingGovernance() calls removed           NE-01..04, NE-06, NE-08
#   the patterns.empty() condition removed                 NE-07
#   the per-flag test removed (any flag gates all four)    NE-06
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -6 | sed 's/^/       | /'; }

echo "=== native encoders under restrictions.data_exfiltration ==="
if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2; exit 1
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-natenc-XXXXXX")"
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

# run NAME RESTRICTION_JSON PROGRAM -> OUT RC
run() {
    mkdir -p "$W/$1"
    printf '{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" }, "restrictions": { "data_exfiltration": %s } }\n' "$2" > "$W/$1/govern.json"
    printf '%s\n' "$3" > "$W/$1/p.naab"
    OUT="$(cd "$W/$1" && timeout 60 "$NAAB" run p.naab 2>&1)"; RC=$?
}
call() { printf 'use crypto\nmain {\n    print("OUT=" + crypto.%s("%s"))\n}\n' "$1" "$2"; }
refused() { [ "$RC" = 3 ] && [[ "$OUT" != *OUT=* ]] && [[ "$OUT" == *restrictions.data_exfiltration* || "$OUT" == *"Data exfiltration"* ]]; }

ON='{ "enabled": true, "level": "hard" }'

run ne0 "$ON" 'main {
    let r = <<python
import base64
base64.b64encode(b"x").decode()
>>
    print("OUT=" + r)
}'
if [ "$RC" = 3 ] && [[ "$OUT" != *OUT=* ]]; then ok "NE-00" "control: the polyglot base64.b64encode( is refused under the restriction"
else bad "NE-00" "the fixture does not turn the rule on -- every arm below is unmeasured" "rc=$RC $OUT"; fi

run ne1 "$ON" "$(call base64_encode password123)"
if refused; then ok "NE-01" "crypto.base64_encode is refused and prints no encoding"
else bad "NE-01" "crypto.base64_encode ran under the restriction" "rc=$RC $OUT"; fi

run ne2 "$ON" "$(call base64_decode cGFzc3dvcmQxMjM=)"
if refused; then ok "NE-02" "crypto.base64_decode is refused"
else bad "NE-02" "crypto.base64_decode ran under the restriction" "rc=$RC $OUT"; fi

run ne3 "$ON" "$(call hex_encode password123)"
if refused; then ok "NE-03" "crypto.hex_encode is refused"
else bad "NE-03" "crypto.hex_encode ran under the restriction" "rc=$RC $OUT"; fi

run ne4 "$ON" "$(call hex_decode 70617373)"
if refused; then ok "NE-04" "crypto.hex_decode is refused"
else bad "NE-04" "crypto.hex_decode ran under the restriction" "rc=$RC $OUT"; fi

run ne5 '{ "enabled": false }' "$(call base64_encode password123)"
if [ "$RC" = 0 ] && [[ "$OUT" == *OUT=cGFzc3dvcmQxMjM=* ]]; then ok "NE-05" "control: with the restriction off, crypto.base64_encode runs"
else bad "NE-05" "crypto.base64_encode did not run with the restriction off" "rc=$RC $OUT"; fi

run ne6b '{ "enabled": true, "level": "hard", "block_base64_encode_secrets": false }' "$(call base64_encode password123)"
B="$RC:$([[ "$OUT" == *OUT=cGFzc3dvcmQxMjM=* ]] && echo ran || echo not)"
run ne6h '{ "enabled": true, "level": "hard", "block_base64_encode_secrets": false }' "$(call hex_encode password123)"
if [ "$B" = "0:ran" ] && refused; then ok "NE-06" "control: block_base64_encode_secrets false leaves base64 running; hex, its own flag on, is refused"
else bad "NE-06" "the flags did not split (base64: $B, hex: rc=$RC)" "$OUT"; fi

run ne7 '{ "enabled": true, "level": "hard", "patterns": ["never_matches_this_text"] }' "$(call base64_encode password123)"
if [ "$RC" = 0 ] && [[ "$OUT" == *OUT=cGFzc3dvcmQxMjM=* ]]; then ok "NE-07" "control: operator patterns replace the built-in ones, so the native encoders run"
else bad "NE-07" "operator patterns did not replace the built-in encoding rule" "rc=$RC $OUT"; fi

run ne8 '{ "enabled": true, "level": "advisory" }' "$(call base64_encode password123)"
if [ "$RC" = 0 ] && [[ "$OUT" == *OUT=cGFzc3dvcmQxMjM=* ]] && [[ "$OUT" == *"Data exfiltration"* || "$OUT" == *data_exfiltration* ]]; then
    ok "NE-08" "level advisory: the call runs and the finding is reported"
else bad "NE-08" "advisory did not run-and-report" "rc=$RC $OUT"; fi

echo ""
echo "  Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
