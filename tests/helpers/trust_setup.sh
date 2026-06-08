#!/usr/bin/env bash
# tests/helpers/trust_setup.sh — Shared trust store isolation for test scripts.
# Source this file, then call setup_isolated_trust / teardown_isolated_trust.
#
# Usage:
#   source "$(dirname "$0")/../helpers/trust_setup.sh"   # adjust path as needed
#   setup_isolated_trust
#   trap teardown_isolated_trust EXIT
#   "$NAAB" --keygen "$WORKDIR/test-key.pem" 2>/dev/null
#   "$NAAB" --trust-key "$WORKDIR/test-key.pem.pub" 2>/dev/null
#   export NAAB_SIGNING_KEY="$WORKDIR/test-key.pem"

_TRUST_SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"

setup_isolated_trust() {
    export NAAB_TRUST_STORE_DIR="$(mktemp -d "${_TRUST_SYSTMP}/trust-XXXXXX")"
}

teardown_isolated_trust() {
    [ -n "${NAAB_TRUST_STORE_DIR:-}" ] && rm -rf "$NAAB_TRUST_STORE_DIR"
    unset NAAB_TRUST_STORE_DIR
}
