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
#
# trust.require_fresh_signature (A175; parsed and read by nothing until
# 2026-11-18). The owner's decision: "require" means refuse -- a verified
# signature that carries no signing time fails like one that does not verify,
# at HARD, whatever stale_signature_level says. Two signatures carry no time:
# an Ed25519 one made before e00f78b8 (it signs the content alone, so openssl
# makes a valid one here) and every HMAC one.
#   ST-06  an undated Ed25519 signature, require true: exit 3, STALE SIGNATURE
#          BLOCK naming the missing signing time, the program does not run
#   ST-07  the same at stale_signature_level advisory: still refused
#   ST-08  CONTROL: the same undated signature without require loads -- the
#          signature verifies, so ST-06's refusal is the requirement
#   ST-09  CONTROL: a dated (fresh) signature with require true loads
#   ST-10  an HMAC signature (a throwaway key the suite makes; no Ed25519 key
#          installed), require true: exit 3
#   ST-10c CONTROL: the same HMAC signature without require loads
#
# Red first: ST-06, ST-07 and ST-10 fail on a build of ec009c5e; the other
# arms pass there (observed 2026-11-18).
# KILL TABLE (each mutant built, the suite run, the arm that FAILED; observed
# 2026-11-18):
#   F1  the undated branch removed from signatureAgeAcceptable() .. ST-06, 07, 10
#   F2  the undated branch judged at stale_signature_level ........ ST-07
#   F3  the HMAC path returns true instead of judging the age ...... ST-10
#   F4  require refuses every signature, dated or not .............. ST-09
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

echo -e "${CYAN}=== Group ST: trust.require_fresh_signature ===${NC}"

cfgf() {  # $1 = require_fresh_signature  $2 = stale_signature_level
    printf '{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},"trust":{"require_fresh_signature":%s,"stale_signature_level":"%s"}}\n' \
        "$1" "$2" > "$W/govern.json"
}
sign_undated() {  # an Ed25519 signature over the content alone (before e00f78b8)
    local sig
    sig=$(openssl pkeyutl -sign -rawin -inkey "$K" -in "$W/govern.json" 2>/dev/null | base64 | tr -d '\n')
    [ -n "$sig" ] || return 1
    printf 'ed25519:%s' "$sig" > "$W/govern.json.sig"
}
refused_undated() {  # $1 = id  $2 = what
    if [ $rc -eq 3 ] && grep -q 'STALE SIGNATURE BLOCK' <<<"$out" && grep -q 'carries no signing time' <<<"$out" \
       && ! grep -q '^ran' <<<"$out"; then ok "$1" "$2"
    else bad "$1" "an undated signature was not refused (rc=$rc)" "$(grep -m2 -E 'BLOCK|STALE|WARNING|ran' <<<"$out")"; fi
}
loaded() {  # $1 = id  $2 = what
    if [ $rc -eq 0 ] && grep -q '^ran' <<<"$out"; then ok "$1" "$2"
    else bad "$1" "refused (rc=$rc)" "$(grep -m2 -E 'BLOCK|STALE|Error' <<<"$out")"; fi
}

cfgf true hard
if ! sign_undated; then
    for id in ST-06 ST-07 ST-08; do skip "$id" "openssl cannot sign Ed25519 here (UNMEASURABLE)"; done
else
    out=$(run); rc=$?
    refused_undated ST-06 "an undated signature is refused when a fresh one is required"
    cfgf true advisory; sign_undated
    out=$(run); rc=$?
    refused_undated ST-07 "refused at HARD whatever stale_signature_level says"
    cfgf false hard; sign_undated
    out=$(run); rc=$?
    loaded ST-08 "CONTROL: the same undated signature loads without the requirement"
fi

cfgf true hard; sign_fresh
out=$(run); rc=$?
loaded ST-09 "CONTROL: a dated signature loads with the requirement"

# HMAC: no Ed25519 key may be installed (a trusted key makes an HMAC signature
# an INTEGRITY BLOCK of its own), so these two run with a second, empty trust
# store. The key is one this suite makes; it is no one's.
HK="st-throwaway-$$-$RANDOM"
sign_hmac() {
    local mac
    mac=$(NAAB_ST_HK="$HK" python3 -c 'import hmac,hashlib,os,sys; sys.stdout.write(hmac.new(os.environ["NAAB_ST_HK"].encode(), sys.stdin.buffer.read(), hashlib.sha256).hexdigest())' < "$W/govern.json")
    [ -n "$mac" ] || return 1
    printf 'hmac:%s' "$mac" > "$W/govern.json.sig"
}
EMPTY_STORE="$W/empty-trust"; mkdir -p "$EMPTY_STORE"
run_hmac() { ( cd "$W" && NAAB_TRUST_STORE_DIR="$EMPTY_STORE" NAAB_GOVERN_KEY="$HK" timeout 30 "$NAAB" t.naab 2>&1 ); }
if ! command -v python3 >/dev/null 2>&1; then
    for id in ST-10 ST-10c; do skip "$id" "python3 not available to compute the HMAC (UNMEASURABLE)"; done
else
    cfgf false hard; sign_hmac
    out=$(run_hmac); rc=$?
    loaded ST-10c "CONTROL: an HMAC signature loads without the requirement"
    cfgf true hard; sign_hmac
    out=$(run_hmac); rc=$?
    refused_undated ST-10 "an HMAC signature (no signing time) is refused when a fresh one is required"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
if [ $FAIL -gt 0 ]; then echo -e "Failures:$FAILURES"; exit 1; fi
exit 0
