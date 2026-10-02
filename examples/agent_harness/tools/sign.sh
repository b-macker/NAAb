#!/usr/bin/env bash
# sign.sh -- lock src/govern.json with an Ed25519 key you hold.
#
#   tools/sign.sh [PRIVATE_KEY]
#
# PRIVATE_KEY defaults to ~/.naab/agent_harness/signing-key.pem and is created
# on first use. It must live OUTSIDE this directory: only the public half is
# committed (keys/harness-signing.pub), next to the signature
# (src/govern.json.sig). Whoever holds the private key owns the lock.
#
# Order of work this script enforces:
#   1. every key in govern.json is one the engine reads (check_govern_keys.py)
#   2. sign
#   3. verify the result the way a run will (tools/lock_check.sh)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="${NAAB_REPO:-$(cd "$HERE/../.." && pwd)}"
NAAB="${NAAB:-$REPO/build/naab-lang}"
KEY="${1:-$HOME/.naab/agent_harness/signing-key.pem}"

[ -x "$NAAB" ] || { echo "naab-lang not found at $NAAB" >&2; exit 2; }
KEY_ABS="$(cd "$(dirname "$KEY")" 2>/dev/null && pwd)/$(basename "$KEY")" || KEY_ABS="$KEY"
case "$KEY_ABS" in "$HERE"/*) echo "refusing: the private key would sit inside the example and get committed" >&2; exit 2 ;; esac

python3 "$HERE/tools/check_govern_keys.py" "$HERE/src/govern.json" --repo "$REPO" \
    || { echo "refusing to sign: fix the keys above first" >&2; exit 1; }

if [ ! -f "$KEY" ]; then
    mkdir -p "$(dirname "$KEY")"
    "$NAAB" --keygen "$KEY" 2>&1 | grep -E 'Private key|Fingerprint'
fi
mkdir -p "$HERE/keys"
cp "$KEY.pub" "$HERE/keys/harness-signing.pub"
NAAB_SIGNING_KEY="$KEY" "$NAAB" --sign-governance "$HERE/src/govern.json"
"$HERE/tools/lock_check.sh"
