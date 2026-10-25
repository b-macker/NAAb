#!/usr/bin/env bash
# ============================================================
# test_flag_authority.sh — the authority to use a flag the owner locked is
# possession of the PRIVATE key that signed govern.json, never the public one
#
# THE FAILURE THIS CATCHES
#
# integrity.blocked_flags (and, for a signed enforce config, the implicit
# blocks on the two bypass flags) are skipped when the run has "authority".
# main.cpp's preflight computed it by handing the NAAB_SIGNING_KEY file to
# CryptoUtils::ed25519Verify(), which reads only a PUBLIC key
# (PEM_read_bio_PUBKEY) -- while the variable names the PRIVATE key everywhere
# else (`--keygen` prints `export NAAB_SIGNING_KEY=<private key>`). It also took
# everything after "ed25519:" as the signature, so the timestamped envelope
# `--sign-governance` writes ("ed25519:<sig>:<ts>", signed over content:ts) never
# decoded. Observed before the fix (2026-10-25, master 6abb3201):
#   current signature + private key -> blocked (the owner has no authority)
#   current signature + public key  -> blocked
#   legacy signature (no timestamp) + public key  -> RAN: the public key, which
#     anyone can read, unlocked the owner's lock
#   legacy signature + private key -> blocked
# F1 (29982dc6) meant to validate the key "against project signature"; the
# verifier had rejected private-key PEMs since ee7e8611, two weeks earlier, so
# the Ed25519 branch never worked as written.
#
# Restoring the owner's authority loosens nothing: the private-key holder can
# edit and re-sign govern.json to remove the lock anyway.
#
#   FA-00  CONTROL: an unlocked --governance-override runs (the flag itself works)
#   FA-01  locked, no key: INTEGRITY BLOCK, the program does not run
#   FA-02  locked, public key, current signature: blocked
#   FA-03  locked, public key, legacy signature: blocked
#   FA-04  locked, private key, current signature: runs
#   FA-05  locked, private key, legacy signature: runs
#   FA-06  locked, a trusted private key that did not sign the file: blocked
#   FA-07  locked, an untrusted private key: blocked
#   FA-08  HMAC (NAAB_GOVERN_KEY) path: the signing key runs, a wrong one is blocked
#   FA-09  CONTROL: the legacy signature is accepted as a signature at all
#   FA-10  signed enforce config, unlisted --governance-override: implicitly
#          blocked without a key, runs with the private key
#   FA-11  no trusted key installed (any Ed25519 signature loads, with a
#          warning): the key that signed the file gives no authority
# FA-03 and FA-05 need openssl to write a legacy signature; without it they
# are UNMEASURABLE.
#
# Kill table (each mutant rebuilt and run, 2026-10-25):
#   key file read as the public key (the old code)  FA-02 FA-03 FA-04 FA-05 FA-10
#   no trust-store check                            FA-11
#   timestamp ignored in the payload                FA-04 FA-10
#   the preflight never asks                        FA-04 FA-05 FA-10
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"

if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2
    exit 1
fi

PASS=0; FAIL=0; UNM=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm() { UNM=$((UNM+1)); echo "  UNMEASURABLE [$1] $2"; }

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-flagauth-XXXXXX")"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT

# Owner key (trusted, signs), a second trusted key, and an untrusted key.
"$NAAB" --keygen "$W/owner.pem" >/dev/null 2>&1 \
    && "$NAAB" --trust-key "$W/owner.pem.pub" >/dev/null 2>&1 \
    && "$NAAB" --keygen "$W/other.pem" >/dev/null 2>&1 \
    && NAAB_SIGNING_KEY="$W/owner.pem" "$NAAB" --sign-key "$W/other.pem.pub" >/dev/null 2>&1 \
    && "$NAAB" --trust-key "$W/other.pem.pub" --authorized-by "$W/other.pem.pub.sig" >/dev/null 2>&1 \
    && "$NAAB" --keygen "$W/stranger.pem" >/dev/null 2>&1 \
    || { bad "SETUP" "could not create and trust the signing keys"; echo ""; echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1; }

printf 'main { print("RAN") }\n' > "$W/t.naab"

# project DIR CONFIG -> writes govern.json, signs it with the owner key
project() {
    mkdir -p "$1"; cp "$W/t.naab" "$1/"
    printf '%s\n' "$2" > "$1/govern.json"
    NAAB_SIGNING_KEY="$W/owner.pem" "$NAAB" --sign-governance "$1/govern.json" >/dev/null 2>&1
}
LOCKED='{"version":"5.0","mode":"enforce","integrity":{"blocked_flags":["--governance-override"]}}'
OPEN='{"version":"5.0","mode":"enforce"}'

# run DIR KEYFILE ARGS... -> OUT, RC (KEYFILE "" = no NAAB_SIGNING_KEY)
run() {
    local d=$1 key=$2; shift 2
    if [ -n "$key" ]; then
        OUT=$(cd "$d" && env -u NAAB_GOVERN_KEY NAAB_SIGNING_KEY="$key" timeout 30 "$NAAB" "$@" 2>&1); RC=$?
    else
        OUT=$(cd "$d" && env -u NAAB_GOVERN_KEY -u NAAB_SIGNING_KEY timeout 30 "$NAAB" "$@" 2>&1); RC=$?
    fi
}
ran()     { [ "$RC" = 0 ] && grep -qx 'RAN' <<<"$OUT"; }
blocked() { [ "$RC" = 3 ] && grep -q 'INTEGRITY BLOCK' <<<"$OUT" && ! grep -qx 'RAN' <<<"$OUT"; }

project "$W/open" "$OPEN"
project "$W/locked" "$LOCKED"

# FA-00 needs an unsigned, unlocked project, so its own empty trust store (an
# unsigned file with trusted keys installed is an integrity block of its own).
mkdir -p "$W/unsigned" "$W/empty-trust"; cp "$W/t.naab" "$W/unsigned/"
printf '%s\n' "$OPEN" > "$W/unsigned/govern.json"
OUT=$(cd "$W/unsigned" && env -u NAAB_GOVERN_KEY -u NAAB_SIGNING_KEY NAAB_TRUST_STORE_DIR="$W/empty-trust" \
      timeout 30 "$NAAB" t.naab --governance-override 2>&1); RC=$?
if ran; then ok "FA-00" "control: an unlocked --governance-override on an unsigned project runs"
else bad "FA-00" "an unlocked flag should run" "rc=$RC $(grep -m1 -E 'BLOCK|Error' <<<"$OUT")"; fi

run "$W/locked" "" t.naab --governance-override
if blocked; then ok "FA-01" "locked flag, no key: INTEGRITY BLOCK"
else bad "FA-01" "a locked flag without a key should be blocked" "rc=$RC"; fi

run "$W/locked" "$W/owner.pem.pub" t.naab --governance-override
if blocked; then ok "FA-02" "the public key gives no authority (current signature)"
else bad "FA-02" "the public key must not unlock a locked flag" "rc=$RC"; fi

run "$W/locked" "$W/owner.pem" t.naab --governance-override
if ran; then ok "FA-04" "the private key that signed govern.json gives authority (current signature)"
else bad "FA-04" "the owner's private key should unlock the flag" "rc=$RC $(grep -m1 'BLOCK' <<<"$OUT")"; fi

run "$W/locked" "$W/other.pem" t.naab --governance-override
if blocked; then ok "FA-06" "a trusted key that did not sign the file gives no authority"
else bad "FA-06" "only the key that signed the file has authority" "rc=$RC"; fi

run "$W/locked" "$W/stranger.pem" t.naab --governance-override
if blocked; then ok "FA-07" "an untrusted private key gives no authority"
else bad "FA-07" "an untrusted key must not unlock the flag" "rc=$RC"; fi

# Legacy signature: "ed25519:<sig>" over the content alone, as releases before
# the timestamp wrote it. The engine still accepts it (FA-09).
if command -v openssl >/dev/null 2>&1; then
    mkdir -p "$W/legacy"; cp "$W/t.naab" "$W/legacy/"
    printf '%s\n' "$LOCKED" > "$W/legacy/govern.json"
    sig=$(openssl pkeyutl -sign -rawin -inkey "$W/owner.pem" -in "$W/legacy/govern.json" 2>/dev/null | base64 | tr -d '\n')
    if [ -n "$sig" ]; then
        printf 'ed25519:%s' "$sig" > "$W/legacy/govern.json.sig"
        run "$W/legacy" "" t.naab
        if ran; then
            ok "FA-09" "control: the legacy signature verifies (the program runs without the flag)"
            run "$W/legacy" "$W/owner.pem.pub" t.naab --governance-override
            if blocked; then ok "FA-03" "the public key gives no authority (legacy signature)"
            else bad "FA-03" "the public key must not unlock a locked flag (legacy signature)" "rc=$RC"; fi
            run "$W/legacy" "$W/owner.pem" t.naab --governance-override
            if ran; then ok "FA-05" "the signing private key gives authority (legacy signature)"
            else bad "FA-05" "the owner's private key should unlock the flag (legacy signature)" "rc=$RC"; fi
        else
            bad "FA-09" "the legacy signature should verify" "rc=$RC $(grep -m1 'BLOCK' <<<"$OUT")"
        fi
    else
        unm "FA-03" "openssl could not sign with the Ed25519 key"; unm "FA-05" "openssl could not sign"; unm "FA-09" "openssl could not sign"
    fi
else
    unm "FA-03" "openssl not available"; unm "FA-05" "openssl not available"; unm "FA-09" "openssl not available"
fi

# HMAC path, in its own empty trust store (an HMAC signature is checked only
# when no Ed25519 key is trusted).
HK="hmac-$RANDOM-$RANDOM-$$"
(
    export NAAB_TRUST_STORE_DIR="$W/hmac-trust"; mkdir -p "$NAAB_TRUST_STORE_DIR" "$W/hmac"
    cp "$W/t.naab" "$W/hmac/"; printf '%s\n' "$LOCKED" > "$W/hmac/govern.json"
    env -u NAAB_SIGNING_KEY NAAB_GOVERN_KEY="$HK" "$NAAB" --sign-governance "$W/hmac/govern.json" >/dev/null 2>&1
    o=$(cd "$W/hmac" && env -u NAAB_SIGNING_KEY NAAB_GOVERN_KEY="$HK" timeout 30 "$NAAB" t.naab --governance-override 2>&1); r=$?
    o2=$(cd "$W/hmac" && env -u NAAB_SIGNING_KEY NAAB_GOVERN_KEY="$HK-wrong" timeout 30 "$NAAB" t.naab --governance-override 2>&1); r2=$?
    printf '%s\n%s\n' "$r:$(grep -cx RAN <<<"$o")" "$r2:$(grep -c 'BLOCK' <<<"$o2"):$(grep -cx RAN <<<"$o2")"
) > "$W/hmac.res"
hm1=$(sed -n 1p "$W/hmac.res"); hm2=$(sed -n 2p "$W/hmac.res")
if [ "$hm1" = "0:1" ] && [ "${hm2%%:*}" != 0 ] && [ "${hm2##*:}" = 0 ]; then
    ok "FA-08" "HMAC path unchanged: the signing key runs, a wrong key does not"
else
    bad "FA-08" "the HMAC authority path changed" "right key: $hm1 (rc:RAN), wrong key: $hm2 (rc:BLOCK:RAN)"
fi

project "$W/signed" "$OPEN"
run "$W/signed" "" t.naab --governance-override
if blocked; then
    run "$W/signed" "$W/owner.pem" t.naab --governance-override
    if ran; then ok "FA-10" "signed enforce config: the implicit block holds without a key and lifts for the owner"
    else bad "FA-10" "the owner's private key should lift the implicit block" "rc=$RC"; fi
else
    bad "FA-10" "a signed enforce config should implicitly block --governance-override" "rc=$RC"
fi

# FA-11: with NO trusted key installed the engine accepts any Ed25519 signature
# with a warning (verifySignatureImpl), so a key anyone can make would sign the
# file. Authority still needs a TRUSTED key.
mkdir -p "$W/selfsigned" "$W/empty-trust2"; cp "$W/t.naab" "$W/selfsigned/"
printf '%s\n' "$LOCKED" > "$W/selfsigned/govern.json"
NAAB_TRUST_STORE_DIR="$W/empty-trust2" NAAB_SIGNING_KEY="$W/stranger.pem" \
    "$NAAB" --sign-governance "$W/selfsigned/govern.json" >/dev/null 2>&1
OUT=$(cd "$W/selfsigned" && env -u NAAB_GOVERN_KEY NAAB_TRUST_STORE_DIR="$W/empty-trust2" \
      timeout 30 "$NAAB" t.naab 2>&1); RC=$?
if ran; then
    OUT=$(cd "$W/selfsigned" && env -u NAAB_GOVERN_KEY NAAB_TRUST_STORE_DIR="$W/empty-trust2" \
          NAAB_SIGNING_KEY="$W/stranger.pem" timeout 30 "$NAAB" t.naab --governance-override 2>&1); RC=$?
    if blocked; then ok "FA-11" "no trusted key installed: the key that signed the file still gives no authority"
    else bad "FA-11" "an untrusted signer must not unlock the flag" "rc=$RC"; fi
else
    bad "FA-11" "control: a file signed with no trusted key installed should load (with a warning)" "rc=$RC $(grep -m1 -E 'BLOCK|Error' <<<"$OUT")"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
