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

# The old form probed `[ -d <termux tmp> ]` first. That probe is unsound off
# Android: the project's own runners created that directory on Linux, so the
# branch was taken on a box where the path is root-owned 755 -- and a non-root
# user got "mktemp: Permission denied". TMPDIR is what Termux actually sets,
# so consulting it needs no Termux-specific branch at all.
_TRUST_SYSTMP="${TMPDIR:-/tmp}"

setup_isolated_trust() {
    export NAAB_TRUST_STORE_DIR="$(mktemp -d "${_TRUST_SYSTMP}/trust-XXXXXX")"
}

teardown_isolated_trust() {
    [ -n "${NAAB_TRUST_STORE_DIR:-}" ] && rm -rf "$NAAB_TRUST_STORE_DIR"
    unset NAAB_TRUST_STORE_DIR
}

# isolate_trust_store -- for a suite that writes an UNSIGNED govern.json and
# installs no key of its own. With any key trusted, an unsigned govern.json is an
# INTEGRITY BLOCK (exit 3), so such a suite used to pass only on a machine whose
# store was empty (D8: 50 of 81 changed verdict with one key installed). This
# points the store at a directory that does not exist: the engine reads a
# missing store as empty (TrustStore::hasKeys/loadKeys test is_directory first),
# and nothing creates it, so there is nothing to remove -- no EXIT trap, which
# would replace the caller's own (42 of those 50 set one). A suite that installs
# keys calls setup_isolated_trust instead.
isolate_trust_store() {
    local d
    while :; do
        d="${_TRUST_SYSTMP}/naab-no-trust-store-$$-${RANDOM}${RANDOM}"
        [ -e "$d" ] || break
    done
    export NAAB_TRUST_STORE_DIR="$d"
}
