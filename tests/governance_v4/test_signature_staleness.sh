#!/usr/bin/env bash
# ============================================================
# test_signature_staleness.sh — trust.max_signature_age_days applies at STARTUP
#
# THE FAILURE THIS CATCHES
#
# loadFromFile() parses govern.json into a NEW rules object, verifies the
# signature, and only then installs the rules. The staleness check inside
# verification read rules().trust_policy -- the ACTIVE rules, which at startup
# are still the empty pre-load config -- so max_signature_age_days read 0 and a
# signature of any age was accepted. Authority decay never fired on a fresh run.
# Found building examples/agent_harness: a valid 90-day-old signature under a
# 30-day HARD limit loaded with exit 0.
#
# Nothing caught it because the one existing arm (gorilla naab-29 AU-04)
# backdated the .sig file's MTIME, which the engine never reads -- the age is
# the timestamp signed INTO the signature -- and it passed on any exit that was
# not a crash.
#
# The old signature here is forged properly: the engine signs
# content + ":" + timestamp, so openssl signs that payload with a past
# timestamp, producing a signature that is VALID and OLD.
#
#   ST-01  CONTROL: a fresh signature under a 30-day HARD limit loads (exit 0)
#   ST-02  a valid 90-day-old signature under a 30-day HARD limit is refused
#          at startup (exit 3, STALE SIGNATURE BLOCK)
#   ST-03  the same old signature with max_signature_age_days 0 (disabled)
#          loads -- the refusal in ST-02 is the age limit, not the signature
#   ST-04  the same old signature at ADVISORY loads with a warning
#   ST-05  CONTROL: an edited file is still an INTEGRITY BLOCK (not "stale"),
#          so ST-02 is not a verification failure in disguise
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; SKIP=0; FAILURES=""
ok()   { PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

W="${TMPDIR:-/tmp}/naab-sigstale-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }
unset NAAB_SIGNING_KEY NAAB_GOVERN_KEY

echo -e "${CYAN}=== Group ST: signature age at startup ===${NC}"

if ! command -v openssl >/dev/null 2>&1; then
    for id in ST-01 ST-02 ST-03 ST-04 ST-05; do skip "$id" "openssl not available (UNMEASURABLE)"; done
    echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

K="$W/key.pem"
"$NAAB" --keygen "$K" >/dev/null 2>&1 && "$NAAB" --trust-key "$K.pub" >/dev/null 2>&1 \
    || { echo "  SKIP: could not create a signing key"; exit 0; }

printf 'main { print("ran") }\n' > "$W/t.naab"

cfg() {  # $1 = max_signature_age_days  $2 = stale_signature_level
    printf '{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},"trust":{"max_signature_age_days":%s,"stale_signature_level":"%s"}}\n' \
        "$1" "$2" > "$W/govern.json"
}
sign_fresh() { NAAB_SIGNING_KEY="$K" "$NAAB" --sign-governance "$W/govern.json" >/dev/null 2>&1; }
sign_old() {  # $1 = days ago
    local ts=$(( $(date +%s) - $1 * 86400 ))
    { cat "$W/govern.json"; printf ':%s' "$ts"; } > "$W/payload"
    local sig
    sig=$(openssl pkeyutl -sign -rawin -inkey "$K" -in "$W/payload" 2>/dev/null | base64 | tr -d '\n')
    [ -n "$sig" ] || return 1
    printf 'ed25519:%s:%s' "$sig" "$ts" > "$W/govern.json.sig"
}
run() { ( cd "$W" && timeout 30 "$NAAB" t.naab 2>&1 ); }

cfg 30 hard; sign_fresh
out=$(run); rc=$?
if [ $rc -eq 0 ] && grep -q '^ran' <<<"$out"; then ok "ST-01" "CONTROL: a fresh signature loads"
else bad "ST-01" "fresh signature refused (rc=$rc)" "$(grep -m2 -E 'BLOCK|Error' <<<"$out")"; fi

cfg 30 hard
if ! sign_old 90; then
    for id in ST-02 ST-03 ST-04 ST-05; do skip "$id" "openssl cannot sign Ed25519 here (UNMEASURABLE)"; done
else
    out=$(run); rc=$?
    if [ $rc -eq 3 ] && grep -q 'STALE SIGNATURE BLOCK' <<<"$out" && ! grep -q '^ran' <<<"$out"; then
        ok "ST-02" "a valid 90-day-old signature under a 30-day HARD limit is refused at startup"
    else bad "ST-02" "stale signature accepted at startup (rc=$rc)" "$(grep -m2 -E 'BLOCK|STALE|ran' <<<"$out")"; fi

    cfg 0 hard; sign_old 90
    out=$(run); rc=$?
    if [ $rc -eq 0 ] && grep -q '^ran' <<<"$out"; then ok "ST-03" "with the age limit disabled the same old signature loads"
    else bad "ST-03" "old signature refused with the limit disabled (rc=$rc)" "$(grep -m2 -E 'BLOCK|STALE' <<<"$out")"; fi

    cfg 30 advisory; sign_old 90
    out=$(run); rc=$?
    if [ $rc -eq 0 ] && grep -q 'days old' <<<"$out"; then ok "ST-04" "at advisory the old signature loads with a warning"
    else bad "ST-04" "advisory staleness wrong (rc=$rc)" "$(grep -m2 -E 'BLOCK|days old' <<<"$out")"; fi

    cfg 30 hard; sign_fresh
    sed -i.bak 's/"elevated"/"standard"/' "$W/govern.json" && rm -f "$W/govern.json.bak"
    out=$(run); rc=$?
    if [ $rc -eq 3 ] && grep -q 'does not match any trusted key' <<<"$out" && ! grep -q 'STALE' <<<"$out"; then
        ok "ST-05" "CONTROL: an edited file is an INTEGRITY BLOCK, not a stale one"
    else bad "ST-05" "edited file not refused as tampered (rc=$rc)" "$(grep -m2 -E 'BLOCK|STALE' <<<"$out")"; fi
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
if [ $FAIL -gt 0 ]; then echo -e "Failures:$FAILURES"; exit 1; fi
exit 0
