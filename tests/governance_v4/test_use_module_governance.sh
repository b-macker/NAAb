#!/usr/bin/env bash
# F33: `use module` must obey filesystem governance on BOTH engines.
#
# This is an ENGINE-PARITY defect, which is why the engine arms below are the
# point of the suite rather than decoration. There are two classes with the same
# method name -- ModuleResolver::parseModuleFile (module_resolver.cpp) and
# ModuleRegistry::parseModuleFile (module_system.cpp). A25/#213 gated the FIRST
# one, which the VM reaches, so the VM was already blocked; the tree-walker
# reaches the SECOND via modules.cpp -> ModuleRegistry::loadModule, which was
# ungated. So `use secret` under filesystem.mode:none blocked on the VM and
# sailed through on --tree-walk. A25's own commit message warned the fix had to
# land on both engines or it would reproduce the B11-class divergence; it did
# not, and this closes that half.
#
# UM-01 (VM) is the CONTROL that the gate exists at all -- it passed before this
# fix. UM-02 (--tree-walk) is the FIX: it failed before and passes now. Without
# UM-02 the suite would be green against the half-fixed tree. UM-03 proves the
# gate does not over-block a permitted `use` on the engine that was changed.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$HERE/../../build/naab-lang"
PASS=0; FAIL=0
ok(){ echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad(){ echo "  FAIL [$1] $2"; echo "        $3"; FAIL=$((FAIL+1)); }
[ -x "$NAAB" ] || { echo "no naab-lang"; exit 1; }
source "$HERE/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d)
trap 'teardown_isolated_trust 2>/dev/null; rm -rf "$W"' EXIT
echo 'export fn leaked() { return "USE_SECRET" }' > "$W/secret.naab"
cat > "$W/probe.naab" <<'J'
use secret
main { print("USED=" + string(secret.leaked())) }
J
cat > "$W/allow.json" <<'J'
{ "version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"} }
J
cat > "$W/deny.json" <<'J'
{ "version":"5.0","mode":"enforce","capabilities":{"filesystem":{"mode":"none"}} }
J
OUT=$(cd "$W" && cp allow.json govern.json && timeout 20 "$NAAB" probe.naab 2>&1); EC=$?
if echo "$OUT" | grep -q "USED=USE_SECRET"; then
    ok "UM-00" "CONTROL: use module under a permissive fs policy works (VM)"
else bad "UM-00" "use module broken under permissive policy" "exit $EC; $(echo "$OUT"|head -2)"; fi

OUT=$(cd "$W" && cp deny.json govern.json && timeout 20 "$NAAB" probe.naab 2>&1); EC=$?
if [ "$EC" = "3" ] && echo "$OUT" | grep -qi "Filesystem access is not allowed"; then
    ok "UM-01" "CONTROL (VM): use module under mode:none is blocked — gate exists (passed pre-fix via A25/ModuleResolver)"
else bad "UM-01" "VM path lost its gate" "exit $EC; $(echo "$OUT"|head -2)"; fi

# THE FIX: the tree-walker reaches ModuleRegistry, which was ungated.
OUT=$(cd "$W" && cp deny.json govern.json && timeout 20 "$NAAB" --tree-walk probe.naab 2>&1); EC=$?
if [ "$EC" != "0" ] && ! echo "$OUT" | grep -q "USED=USE_SECRET" && echo "$OUT" | grep -qi "Filesystem access is not allowed"; then
    ok "UM-02" "FIX (--tree-walk): use module under mode:none is blocked (was USED=USE_SECRET / PASS / exit 0)"
else bad "UM-02" "tree-walk still bypasses filesystem governance via ModuleRegistry" "exit $EC; $(echo "$OUT"|head -2)"; fi

OUT=$(cd "$W" && cp allow.json govern.json && timeout 20 "$NAAB" --tree-walk probe.naab 2>&1); EC=$?
if echo "$OUT" | grep -q "USED=USE_SECRET"; then
    ok "UM-03" "CONTROL (--tree-walk): a permitted use module still works (no over-block on the changed engine)"
else bad "UM-03" "the fix over-blocks a permitted use on tree-walk" "exit $EC; $(echo "$OUT"|head -2)"; fi

echo
echo "use-module governance: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
