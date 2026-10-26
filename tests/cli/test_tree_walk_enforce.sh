#!/usr/bin/env bash
# ============================================================
# test_tree_walk_enforce.sh -- under mode enforce, --tree-walk is refused
# (Bundle 2 item 2.1; owner's decision 2026-10-25)
#
# WHY
#
# Governance is enforced on the default engine, the VM. The tree-walker is a
# second implementation of the same rules, and it has fallen behind more than
# once (B11: it records no stdlib sequence events; A55: its parallel polyglot
# path skips checks; A57: its async fn builds an engine without the startup
# overlays; A43(a): its error paths lose the advisory summary). Under enforce,
# a run of the same program must not depend on which engine a command line
# picked, so the CLI refuses --tree-walk there (exit 4, the config-error code)
# and runs nothing. Audit and off, and a run with no govern.json, are
# unchanged.
#
# "Under enforce" is the mode the run would have once an environment is laid
# over the config: --env NAME when given, else governance.default_env. The
# tree-walker never applied default_env (traced), so a config whose base mode
# OR whose default environment is enforce refuses -- either way one engine
# would have enforced.
#
#   TW-01   enforce, flag after the file: refused, exit 4, the program did not run
#   TW-01b  enforce, flag before the file: refused the same way
#   TW-02   CONTROL audit: the tree-walker runs the program
#   TW-03   CONTROL off: the tree-walker runs the program
#   TW-04   CONTROL no govern.json (waived): the tree-walker runs the program
#   TW-05   CONTROL enforce without the flag: the VM runs the program
#   TW-06   audit base, --env prod (enforce): refused
#   TW-07   audit base, governance.default_env prod (enforce): refused
#   TW-08   enforce base, governance.default_env dev (audit): refused
#   TW-09   CONTROL enforce base, --env dev (audit): runs (the operator's
#           environment sets the mode for both engines)
#   TW-10   the refusal names the flag and gives the remedy
#
# Kill table (each mutant rebuilt, this suite run):
#   refusal removed                         TW-01/01b/06/07/08 (and TW-10)
#   only the base mode consulted            TW-06/07/09
#   only the --env/default_env mode         TW-08
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2
    exit 1
fi

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       | /'; }

# An unsigned govern.json with a key in the trust store is an INTEGRITY BLOCK:
# isolate the store so the result does not depend on this machine's keys.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-twenf-XXXXXX")"
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PROG='main {
    print("RAN-" + string(6 * 7))
}'

# mk DIR JSON: a project directory with that govern.json (empty JSON = none).
mk() {
    mkdir -p "$W/$1"
    printf '%s\n' "$PROG" > "$W/$1/p.naab"
    [ -n "$2" ] && printf '%s\n' "$2" > "$W/$1/govern.json"
    return 0
}
mk enforce '{"version":"5.0","mode":"enforce"}'
mk audit   '{"version":"5.0","mode":"audit"}'
mk off     '{"version":"5.0","mode":"off"}'
mk none    ''
mk envprod '{"version":"5.0","mode":"audit","environments":{"prod":{"mode":"enforce"}}}'
mk defprod '{"version":"5.0","mode":"audit","governance":{"default_env":"prod"},"environments":{"prod":{"mode":"enforce"}}}'
mk defdev  '{"version":"5.0","mode":"enforce","governance":{"default_env":"dev"},"environments":{"dev":{"mode":"audit"}}}'
mk envdev  '{"version":"5.0","mode":"enforce","environments":{"dev":{"mode":"audit"}}}'

# run DIR ARGS... -> OUT, RC (from inside DIR, so discovery finds its config)
run() {
    local d="$1"; shift
    OUT="$(cd "$W/$d" && timeout 30 "$NAAB" "$@" 2>&1)"; RC=$?
}
ran()     { grep -qx 'RAN-42' <<<"$OUT"; }
refused() { [ "$RC" = 4 ] && ! ran && grep -qi 'tree-walk' <<<"$OUT"; }

run enforce p.naab --tree-walk
if refused; then ok "TW-01" "enforce: --tree-walk after the file is refused (exit 4, nothing ran)"
else bad "TW-01" "enforce: --tree-walk should be refused with exit 4" "rc=$RC $OUT"; fi
TW01_OUT="$OUT"

run enforce --tree-walk p.naab
if refused; then ok "TW-01b" "enforce: --tree-walk before the file is refused the same way"
else bad "TW-01b" "enforce: --tree-walk before the file should be refused" "rc=$RC $OUT"; fi

run audit p.naab --tree-walk
if [ "$RC" = 0 ] && ran; then ok "TW-02" "control: audit runs the tree-walker"
else bad "TW-02" "audit should run the tree-walker" "rc=$RC $OUT"; fi

run off p.naab --tree-walk
if [ "$RC" = 0 ] && ran; then ok "TW-03" "control: off runs the tree-walker"
else bad "TW-03" "off should run the tree-walker" "rc=$RC $OUT"; fi

run none p.naab --tree-walk --no-governance
if [ "$RC" = 0 ] && ran; then ok "TW-04" "control: with no govern.json (waived) the tree-walker runs"
else bad "TW-04" "no govern.json should run the tree-walker" "rc=$RC $OUT"; fi

run enforce p.naab
if [ "$RC" = 0 ] && ran; then ok "TW-05" "control: enforce without the flag runs on the VM"
else bad "TW-05" "enforce without --tree-walk should run" "rc=$RC $OUT"; fi

run envprod p.naab --env prod --tree-walk
if refused; then ok "TW-06" "audit base with --env prod (enforce) is refused"
else bad "TW-06" "--env prod (enforce) should refuse the tree-walker" "rc=$RC $OUT"; fi

run defprod p.naab --tree-walk
if refused; then ok "TW-07" "audit base with default_env prod (enforce) is refused"
else bad "TW-07" "default_env prod (enforce) should refuse the tree-walker" "rc=$RC $OUT"; fi

run defdev p.naab --tree-walk
if refused; then ok "TW-08" "enforce base with default_env dev (audit) is refused (the tree-walker ignores default_env)"
else bad "TW-08" "an enforce base should refuse the tree-walker whatever default_env says" "rc=$RC $OUT"; fi

run envdev p.naab --env dev --tree-walk
if [ "$RC" = 0 ] && ran; then ok "TW-09" "control: --env dev (audit) over an enforce base runs the tree-walker"
else bad "TW-09" "--env dev (audit) should run the tree-walker" "rc=$RC $OUT"; fi

if grep -q -- '--tree-walk' <<<"$TW01_OUT" && grep -qi 'enforce' <<<"$TW01_OUT" && grep -q 'Help:' <<<"$TW01_OUT"; then
    ok "TW-10" "the refusal names --tree-walk, the mode, and a Help section"
else
    bad "TW-10" "the refusal should name --tree-walk and enforce and carry Help" "$TW01_OUT"
fi

echo ""
echo "  Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
