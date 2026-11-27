#!/usr/bin/env bash
# test_module_exports_only.sh -- an importer reaches only the functions a
# module exports, on both engines (A216, the owner's decision of 2026-11-26).
#
# With m.naab holding `export fn pub()` and `fn hidden()`, the VM made
# hidden() callable by its BARE name after `use m` (every module global is
# merged into the importer so the module's own functions can call their
# helpers by name, 8669f246 -- whose message meant them to stay "invisible"),
# and the tree-walker returned it through `m.hidden()`, `import {hidden}` and
# the `import * as` dict, though its own error text says "Only exported
# functions are visible from other files" (observed on 30962bcc). Now:
#   VM            a bare-name read of a module's unexported function from code
#                 that is not that module's is refused (it was already refused
#                 through m.hidden, the import dict and a named import);
#   tree-walker   m.hidden(), `import {hidden}` and the `import * as` dict
#                 refuse it.
# A module with no `export` at all keeps everything public, as the VM's module
# dict always has. Only functions are restricted: exported variables, structs
# and enums are as before.
#
#   EO-00  CONTROL: pub(), which calls hidden() inside the module, returns on
#          the VM (bare) and on the tree-walker (m.pub()).
#   EO-01  VM: hidden() by its bare name after `use m` is refused.
#   EO-02  VM: the same from inside an async fn of the program is refused.
#   EO-03  VM: module a's function calling module b's unexported function by
#          bare name is refused; EO-03c CONTROL: calling b's export works.
#   EO-04  CONTROL: the program's own fn of the same name is the program's.
#   EO-05  CONTROL: a module with no export keeps its functions public on
#          both engines (bare on the VM, m.helper() on the tree-walker).
#   EO-06  tree-walker: m.hidden() is refused.
#   EO-07  tree-walker: import {hidden} is refused.
#   EO-08  tree-walker: the `import * as` and `import ... as` dicts hold no
#          hidden.
#   EO-09  VM (unchanged, pinned): m.hidden(), import {hidden} and the
#          `import * as` dict refuse it.
#   EO-10  A231: two modules each with a private helper -- each module's code
#          calls its own, on both engines (the VM called the last one merged).
#   EO-11  A231: beside the program's own helper, the program calls its own
#          and the module its own.
#   EO-12  A231: the same from a module's async fn (run by its export).
#   EO-13  A216: after a module is imported a second time, its private helper
#          is still refused; EO-13c CONTROL: the second import works.
#
# Kill table (mutants of src/vm/vm.cpp, src/interpreter/modules.cpp and
# call_dispatch.cpp, run by hand):
#   P1 OP_GET_GLOBAL does not ask refusePrivateModuleRead()  -> EO-01, EO-03
#   P2 importModule() records no private function           -> EO-01, EO-02, EO-03
#   P3 the owner check is dropped (every frame refused)     -> EO-00
#   P4 the async VM does not take the record                -> EO-02
#   P5 a function from another module counts as private     -> EO-03 (the
#      importing module re-records b's helper as its own, so a reaches it;
#      predicted EO-03c, observed EO-03)
#   P6 privateModuleFunctions() ignores the exports         -> EO-05 (a module
#      with no export)
#   P7 the tree-walker's member route does not ask it       -> EO-06
#   P8 the named-import route does not ask it               -> EO-07
#   P9 the import dicts keep private functions              -> EO-08
#   P10 the value check is dropped (a same-named function   -> EO-04
#       of the program is refused)
#   P11 OP_GET_GLOBAL does not ask ownModuleFunction()      -> EO-10, EO-11, EO-12
#   P12 importModule() sets no module_functions             -> EO-10, EO-11, EO-12
#   P13 the async copy drops module_functions               -> EO-12
#   P14 one private record per name (the shape before)      -> EO-13

set -uo pipefail
PASS=0; FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
W="$(mktemp -d "${TMPDIR:-/tmp}/naab_modexp.XXXXXX")"
trap 'rm -rf "$W"' EXIT
ok()  { echo "  PASS $1: $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL $1: $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/      /'; FAIL=$((FAIL+1)); }
[ -x "$NAAB" ] || { echo "UNMEASURABLE: $NAAB not built"; exit 1; }

cat > "$W/m.naab" <<'EOF'
fn hidden() {
    return "H"
}
export fn pub() {
    return "P" + hidden()
}
EOF
cat > "$W/open.naab" <<'EOF'
fn helper() {
    return "O"
}
EOF
cat > "$W/b.naab" <<'EOF'
fn b_private() {
    return "bp"
}
export fn b_pub() {
    return "bP"
}
EOF
cat > "$W/a.naab" <<'EOF'
use b
export fn a_calls_pub() {
    return b_pub()
}
export fn a_calls_priv() {
    return b_private()
}
EOF

# prog NAME HEADER EXPR [BODY-BEFORE]
prog() { printf '%s\nmain {\n%s    print("OUT=" + %s)\n}\n' "$2" "${4:-}" "$3" > "$W/$1.naab"; }
# run NAME [flags] -> OUT RC (no govern.json in the directory: --no-governance)
run() { local n=$1; shift; OUT=$(cd "$W" && timeout 60 "$NAAB" run "$n.naab" --no-governance "$@" 2>&1); RC=$?; }
got() { [ "$RC" -eq 0 ] && grep -qx "OUT=$1" <<<"$OUT"; }
refused() { [ "$RC" -ne 0 ] && ! grep -q '^OUT=' <<<"$OUT" && grep -q "$1" <<<"$OUT"; }

prog pub 'use m' 'pub()'; run pub; vm_ok=0; got PH && vm_ok=1
prog tw_pub 'use m' 'm.pub()'; run tw_pub --tree-walk
if [ "$vm_ok" = 1 ] && got PH; then ok EO-00 "CONTROL: pub(), which calls hidden() inside the module, returns on both engines"
else bad EO-00 "the module's own call of its helper failed (tree-walker rc=$RC) -- every arm is UNMEASURABLE" "$OUT"; exit 1; fi

prog bare 'use m' 'hidden()'; run bare
if refused "is a function of the module"; then ok EO-01 "VM: hidden() by its bare name is refused"
else bad EO-01 "VM: the bare-name call reached hidden (rc=$RC)" "$OUT"; fi

printf 'use m\nasync fn w() {\n    return hidden()\n}\nmain {\n    print("OUT=" + await w())\n}\n' > "$W/async.naab"; run async
if [ "$RC" -ne 0 ] && ! grep -q '^OUT=H' <<<"$OUT"; then ok EO-02 "VM: the same from an async fn of the program is refused"
else bad EO-02 "VM: the async fn reached hidden (rc=$RC)" "$OUT"; fi

prog a_priv 'use a' 'a.a_calls_priv()'; run a_priv; a_priv_rc=$RC; a_priv_out=$OUT
prog a_pub 'use a' 'a.a_calls_pub()'; run a_pub
if [ "$a_priv_rc" -ne 0 ] && grep -q "is a function of the module" <<<"$a_priv_out"; then
    ok EO-03 "VM: module a calling module b's unexported function is refused"
else bad EO-03 "VM: a reached b's unexported function (rc=$a_priv_rc)" "$a_priv_out"; fi
if got bP; then ok EO-03c "CONTROL: module a calling module b's export works"
else bad EO-03c "a's call of b's export failed (rc=$RC)" "$OUT"; fi

prog own 'use m
fn hidden() {
    return "MINE"
}' 'hidden()'; run own
if got MINE; then ok EO-04 "CONTROL: the program's own fn of the same name is the program's"
else bad EO-04 "the program's own function was refused (rc=$RC)" "$OUT"; fi

prog open_bare 'use open' 'helper()'; run open_bare; ob=0; got O && ob=1
prog open_mem 'use open' 'open.helper()'; run open_mem --tree-walk
if [ "$ob" = 1 ] && got O; then ok EO-05 "CONTROL: a module with no export keeps its functions public on both engines"
else bad EO-05 "a no-export module's function was refused (tree-walker rc=$RC)" "$OUT"; fi

prog mem 'use m' 'm.hidden()'; run mem --tree-walk
if refused "has no member 'hidden'"; then ok EO-06 "tree-walker: m.hidden() is refused"
else bad EO-06 "tree-walker: m.hidden() returned (rc=$RC)" "$OUT"; fi

prog named 'import {hidden} from "./m.naab"' 'hidden()'; run named --tree-walk
if refused "does not export"; then ok EO-07 "tree-walker: import {hidden} is refused"
else bad EO-07 "tree-walker: the named import reached hidden (rc=$RC)" "$OUT"; fi

prog wild 'import * as mm from "./m.naab"' 'mm.hidden()'; run wild --tree-walk; w_rc=$RC; w_out=$OUT
prog alias 'import "./m.naab" as mi' 'mi.hidden()'; run alias --tree-walk
if [ "$w_rc" -ne 0 ] && ! grep -q '^OUT=H' <<<"$w_out" && [ "$RC" -ne 0 ] && ! grep -q '^OUT=H' <<<"$OUT"; then
    ok EO-08 "tree-walker: the import dicts hold no hidden"
else bad EO-08 "tree-walker: an import dict held hidden (wildcard rc=$w_rc, alias rc=$RC)" "$w_out
$OUT"; fi

miss=""
for t in mem named wild; do run "$t"; grep -q '^OUT=H' <<<"$OUT" && miss="$miss $t"; [ "$RC" -eq 0 ] && miss="$miss $t(rc=0)"; done
if [ -z "$miss" ]; then ok EO-09 "VM (pinned): m.hidden, import {hidden} and the import dict refuse it"
else bad EO-09 "VM reached hidden through:$miss" "$OUT"; fi

# --- A231: a module's code calls its own functions --------------------------
# Every module global is merged into the importer's one table, so on the VM a
# module's own call of its helper read whichever file's function of that
# name was merged last: with two modules each defining a private helper, a's
# code called b's (observed on 30962bcc: a_use->B), and beside a program's
# own helper both modules called the program's. The tree-walker, which keeps
# a namespace per module, is right throughout and is the oracle here.
cat > "$W/ca.naab" <<'EOF'
fn helper() {
    return "A"
}
export fn a_use() {
    return helper()
}
async fn a_worker() {
    return helper()
}
export fn a_async() {
    return await a_worker()
}
EOF
cat > "$W/cb.naab" <<'EOF'
fn helper() {
    return "B"
}
export fn b_use() {
    return helper()
}
EOF
TWO='import "./ca.naab" as ca
import "./cb.naab" as cb'

prog two "$TWO" 'ca.a_use() + "," + cb.b_use()'; run two; t_vm=0; got 'A,B' && t_vm=1; t_out=$OUT
run two --tree-walk
if [ "$t_vm" = 1 ] && got 'A,B'; then ok EO-10 "two modules with a private helper each: each module's code calls its own, as on the tree-walker"
else bad EO-10 "a module's code called another module's helper (VM ok=$t_vm, tree-walker rc=$RC)" "$t_out"; fi

prog mine "$TWO
fn helper() {
    return \"MAIN\"
}" 'helper() + "," + ca.a_use()'; run mine; m_vm=0; got 'MAIN,A' && m_vm=1; m_out=$OUT
run mine --tree-walk
if [ "$m_vm" = 1 ] && got 'MAIN,A'; then ok EO-11 "beside the program's own helper: the program calls its own and the module its own"
else bad EO-11 "the program's or the module's call went to the other's helper (VM ok=$m_vm, tree-walker rc=$RC)" "$m_out"; fi

prog asy "$TWO" 'ca.a_async()'; run asy
if got A; then ok EO-12 "the same from a module's async fn (the async VM's copy of the function keeps it)"
else bad EO-12 "the module's async fn called another module's helper (rc=$RC)" "$OUT"; fi

# --- A216: every module's private function stays refused ---------------------
# The record kept one function per name: after `import ca; import cb`, a
# second import of ca (from the cache) put ca's helper back in the slot while
# the record still named cb's, and the program's read of helper() passed
# (observed on 80175946: OUT=A).
prog again "$TWO
import \"./ca.naab\" as ca2" 'helper()'; run again
if refused "is a function of the module"; then ok EO-13 "after a module is imported a second time, its private helper is still refused"
else bad EO-13 "the program reached a module's private helper after a re-import (rc=$RC)" "$OUT"; fi
prog again_ok "$TWO
import \"./ca.naab\" as ca2" 'ca2.a_use()'; run again_ok
if got A; then ok EO-13c "CONTROL: the module imported again works through its new name"
else bad EO-13c "the second import failed (rc=$RC)" "$OUT"; fi

echo
echo "module exports only: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
