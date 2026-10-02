#!/usr/bin/env bash
# lock_check.sh -- does src/govern.json still verify against the committed key?
#
#   tools/lock_check.sh            prints LOCK|ok, LOCK|stale or LOCK|broken
#
# Exit 0: verified.  Exit 1: broken -- the file was edited after signing, or
# was signed by another key.  Exit 2: stale -- a valid signature older than
# trust.max_signature_age_days (re-review and re-sign).  Exit 3: unmeasurable
# (no binary, no signature, no public key).
#
# The check runs the real engine (--lint-only) against a COPY of src/ with a
# throwaway trust store holding only keys/harness-signing.pub, so neither your
# personal trust store nor the working tree can influence the answer.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="${NAAB_REPO:-$(cd "$HERE/../.." && pwd)}"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PUB="$HERE/keys/harness-signing.pub"

[ -x "$NAAB" ] || { echo "LOCK|unmeasurable (no naab-lang at $NAAB)"; exit 3; }
[ -f "$HERE/src/govern.json.sig" ] || { echo "LOCK|unmeasurable (src/govern.json is not signed -- run tools/sign.sh)"; exit 3; }
[ -f "$PUB" ] || { echo "LOCK|unmeasurable (no $PUB)"; exit 3; }

W="$(mktemp -d "${TMPDIR:-/tmp}/ah-lock-XXXXXX")"
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/src" "$W/ts"
cp "$HERE/src/govern.json" "$HERE/src/govern.json.sig" "$HERE/src/harness.naab" "$W/src/"
NAAB_TRUST_STORE_DIR="$W/ts" "$NAAB" --trust-key "$PUB" >/dev/null 2>&1 \
    || { echo "LOCK|unmeasurable (could not install $PUB)"; exit 3; }
# GEMINI_API_KEY only satisfies the prerequisites check; --lint-only makes no call.
out="$(cd "$W/src" && env -u NAAB_SIGNING_KEY NAAB_TRUST_STORE_DIR="$W/ts" \
       GEMINI_API_KEY=lock-check "$NAAB" --lint-only harness.naab 2>&1)"
rc=$?
# STALE first: every integrity refusal, stale included, also prints the generic
# "tamper-protected" line, so matching that first would call a stale lock broken.
case "$out" in
    *"STALE SIGNATURE"*)
        echo "LOCK|stale"; printf '%s\n' "$out" | grep -m1 'STALE'; exit 2 ;;
    *"does not match any trusted key"*|*"INTEGRITY BLOCK"*)
        echo "LOCK|broken"; printf '%s\n' "$out" | grep -m3 'INTEGRITY'; exit 1 ;;
esac
if [ "$rc" -eq 0 ]; then echo "LOCK|ok"; exit 0; fi
echo "LOCK|broken (rc=$rc)"; printf '%s\n' "$out" | grep -v '^\s*$' | tail -5; exit 1
