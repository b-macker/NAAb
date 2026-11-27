#!/usr/bin/env bash
# test_module_export_taint.sh -- a module's exported variable keeps its taint
# across the module boundary, on both engines (A224, the owner's decision of
# 2026-11-26).
#
# Under taint_tracking (source env.get, sink print, level hard), a module
# holding `export let SECRET = env.get(...)` and a main that printed
# `m.SECRET` ran to "Governance: PASS", exit 0, the value printed, while the
# same value read with env.get in main was refused (observed on 30962bcc). The
# VM recorded the module's global as tainted by name only, and the importer
# read it out of a fresh copy of the module dict; the tree-walker restores the
# caller's taint set after loading a module (BUG-O, 03a7cae8, kept: a module's
# internal names must not taint the caller's), which dropped the export's
# taint with them.
#
# Now: a module dict holding a tainted export is a dict holding tainted data,
# tainted as a whole as any such dict is on each engine (its other members
# read tainted; on the VM a call of one of its functions is decided by its
# arguments, on the tree-walker by its receiver too, as for any dict); a named
# import is decided per name. Only the binding the importer makes is marked,
# so BUG-O still holds.
#
#   ME-00  CONTROL: env.get printed directly in main is refused (exit 3), so
#          the fixture taints and the sink fires.
#   ME-01  `use m` then m.SECRET is refused (VM).
#   ME-02  `import "./m.naab" as mm` then mm.SECRET is refused.
#   ME-03  `import {SECRET} from` is refused; ME-03a the same under an alias.
#   ME-04  `import * as mm from` then mm.SECRET is refused.
#   ME-05  the whole module value (string(m)) is refused.
#   ME-06  CONTROL: a named import of a CLEAN export of the same module
#          prints (a named import is per name).
#   ME-07  CONTROL: a call of the module's function with clean arguments
#          prints on the VM (decided by its arguments, as for any dict).
#   ME-08  PINNED, the dict rule: m.LABEL, a clean export beside a tainted
#          one, is refused; ME-08c CONTROL: the same read from a module with
#          no tainted export prints.
#   ME-09  CONTROL (BUG-O): main's own variable named like the module's
#          tainted export is clean, on both engines.
#   ME-10  tree-walker (audit): each route records the sink violation;
#          ME-10c CONTROL: a clean named import and the clean module print.
#
# Kill table (mutants of src/vm/vm.cpp and src/interpreter/modules.cpp, run
# by hand):
#   K1 taintImportedModuleDict() does nothing          -> ME-01, ME-02, ME-04, ME-05, ME-08
#   K2 OP_IMPORT_NAME ignores the export's taint       -> ME-03, ME-03a
#   K3 importedExportTainted() is true for any name    -> ME-06
#   K4 importModule() records no tainted export        -> ME-01..ME-05
#   K5 the cached-module path does not taint the dict  -> ME-02 (its second import)
#   K6 markImportBindingTainted() does nothing         -> ME-10
#   K7 the tree-walker marks a named import whatever   -> ME-10c
#      its name
# Not reachable from a program, so not in the table: the wildcard opcode
# without an alias (the parser refuses `import *` without `as`), and the
# container-identity half (a module's top level runs only declarations).

set -uo pipefail
PASS=0; FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
W="$(mktemp -d "${TMPDIR:-/tmp}/naab_modtaint.XXXXXX")"
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT
ok()  { echo "  PASS $1: $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL $1: $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/      /'; FAIL=$((FAIL+1)); }
[ -x "$NAAB" ] || { echo "UNMEASURABLE: $NAAB not built"; exit 1; }

mkdir -p "$W/vm" "$W/tw"
CFG='"taint_tracking":{"enabled":true,"level":"hard","sources":["env.get"],"sinks":["print"]}'
printf '{"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"},%s}\n' "$CFG" > "$W/vm/govern.json"
printf '{"version":"3.0","mode":"audit","security":{"sandbox_level":"elevated"},%s}\n' "$CFG" > "$W/tw/govern.json"
for d in vm tw; do
    python3 -c 'import json,sys; json.load(sys.stdin)' < "$W/$d/govern.json" || { echo "UNMEASURABLE: bad fixture"; exit 1; }
    cat > "$W/$d/m.naab" <<'EOF'
use env
export let SECRET = env.get("ME_T")
export let LABEL = "clean-label"
export fn greet(x) {
    return "hi " + x
}
EOF
    cat > "$W/$d/clean.naab" <<'EOF'
export let LABEL = "clean-label"
EOF
done

# prog DIR NAME HEADER EXPR [BODY-BEFORE]
prog() { printf '%s\nmain {\n%s    print("OUT=" + %s)\n}\n' "$3" "${5:-}" "$4" > "$W/$1/$2.naab"; }
# run DIR NAME [flags] -> OUT, RC
run() { local d=$1 n=$2; shift 2; OUT=$(cd "$W/$d" && ME_T=tval timeout 60 "$NAAB" run "$n.naab" "$@" 2>&1); RC=$?; }
refused_vm() { [ "$RC" -eq 3 ] && ! grep -q 'tval' <<<"$OUT" && grep -q 'aint' <<<"$OUT"; }
printed() { [ "$RC" -eq 0 ] && grep -qx "OUT=$1" <<<"$OUT"; }

prog vm direct 'use env' 'env.get("ME_T")'; run vm direct
if refused_vm; then ok ME-00 "CONTROL: env.get printed directly is refused (exit 3)"
else bad ME-00 "the direct read was not refused (rc=$RC) -- every arm is UNMEASURABLE" "$OUT"; exit 1; fi

prog vm u_var 'use m' 'm.SECRET'; run vm u_var
if refused_vm; then ok ME-01 "use m: m.SECRET is refused"
else bad ME-01 "use m: m.SECRET reached the sink (rc=$RC)" "$OUT"; fi

# Two imports of one module: the second takes the cached path.
prog vm i_var 'import "./m.naab" as m1
import "./m.naab" as mm' 'mm.SECRET'; run vm i_var
if refused_vm; then ok ME-02 "import as (the module's second, cached import): mm.SECRET is refused"
else bad ME-02 "import as: mm.SECRET reached the sink (rc=$RC)" "$OUT"; fi

prog vm n_var 'import {SECRET} from "./m.naab"' 'SECRET'; run vm n_var
if refused_vm; then ok ME-03 "import {SECRET}: refused"
else bad ME-03 "a named import of the tainted export reached the sink (rc=$RC)" "$OUT"; fi
prog vm n_alias 'import {SECRET as S} from "./m.naab"' 'S'; run vm n_alias
if refused_vm; then ok ME-03a "import {SECRET as S}: refused"
else bad ME-03a "an aliased named import reached the sink (rc=$RC)" "$OUT"; fi

prog vm w_var 'import * as mm from "./m.naab"' 'mm.SECRET'; run vm w_var
if refused_vm; then ok ME-04 "import * as mm: mm.SECRET is refused"
else bad ME-04 "a wildcard alias reached the sink (rc=$RC)" "$OUT"; fi

prog vm whole 'use m' 'string(m)'; run vm whole
if refused_vm; then ok ME-05 "the whole module value is refused"
else bad ME-05 "string(m) reached the sink (rc=$RC)" "$OUT"; fi

prog vm n_label 'import {LABEL} from "./m.naab"' 'LABEL'; run vm n_label
if printed clean-label; then ok ME-06 "CONTROL: a named import of a clean export of the same module prints"
else bad ME-06 "a clean named import was refused (rc=$RC)" "$OUT"; fi

prog vm u_call 'use m' 'm.greet("x")'; run vm u_call
if printed 'hi x'; then ok ME-07 "CONTROL: a call of the module's function with clean arguments prints (VM)"
else bad ME-07 "the clean call was refused (rc=$RC)" "$OUT"; fi

prog vm u_label 'use m' 'm.LABEL'; run vm u_label
if refused_vm; then ok ME-08 "PINNED (the dict rule): a clean export beside a tainted one reads tainted"
else bad ME-08 "m.LABEL printed (rc=$RC): the module dict is not tainted as a whole" "$OUT"; fi
prog vm c_label 'use clean' 'clean.LABEL'; run vm c_label
if printed clean-label; then ok ME-08c "CONTROL: the same read from a module with no tainted export prints"
else bad ME-08c "a clean module's export was refused (rc=$RC)" "$OUT"; fi

prog vm shadow 'use m' 'SECRET' '    let SECRET = "safe-local"
'; run vm shadow
vm_shadow_rc=$RC; vm_shadow_out=$OUT
prog tw shadow 'use m' 'SECRET' '    let SECRET = "safe-local"
'; run tw shadow --tree-walk
if [ "$vm_shadow_rc" -eq 0 ] && grep -qx 'OUT=safe-local' <<<"$vm_shadow_out" && printed safe-local; then
    ok ME-09 "CONTROL (BUG-O): main's own variable named like the export is clean, on both engines"
else bad ME-09 "a same-named local was refused (vm rc=$vm_shadow_rc, tw rc=$RC)" "$vm_shadow_out
$OUT"; fi

# ---- tree-walker (audit): each route records the sink violation ----
tw_flagged() { grep -q 'Taint tracking violation' <<<"$OUT" && ! grep -q '^OUT=tval' <<<"$OUT"; }
miss=""
for spec in 'u_var|use m|m.SECRET' 'i_var|import "./m.naab" as mm|mm.SECRET' \
            'n_var|import {SECRET} from "./m.naab"|SECRET' 'w_var|import * as mm from "./m.naab"|mm.SECRET'; do
    IFS='|' read -r n h e <<<"$spec"
    prog tw "$n" "$h" "$e"; run tw "$n" --tree-walk
    tw_flagged || miss="$miss $n(rc=$RC)"
done
if [ -z "$miss" ]; then ok ME-10 "tree-walker: every route records the sink violation"
else bad ME-10 "tree-walker routes not flagged:$miss" "$OUT"; fi

prog tw n_label 'import {LABEL} from "./m.naab"' 'LABEL'; run tw n_label --tree-walk; tw_n=$RC; tw_n_out=$OUT
prog tw c_label 'use clean' 'clean.LABEL'; run tw c_label --tree-walk
if [ "$tw_n" -eq 0 ] && grep -qx 'OUT=clean-label' <<<"$tw_n_out" && ! grep -q 'Taint tracking violation' <<<"$tw_n_out" && printed clean-label; then
    ok ME-10c "CONTROL: on the tree-walker a clean named import and a clean module print"
else bad ME-10c "a clean read was flagged on the tree-walker (named rc=$tw_n, module rc=$RC)" "$tw_n_out
$OUT"; fi

echo
echo "module export taint: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
