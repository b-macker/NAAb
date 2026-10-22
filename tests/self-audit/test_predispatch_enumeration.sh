#!/usr/bin/env bash
# ============================================================
# test_predispatch_enumeration.sh -- every check a stdlib dispatcher makes before
# (and around) a NAAb stdlib call is classified (roadmap item 6)
#
# tools/codemap/predispatch_enum.py lists, from the code (libclang, through the
# code map's own extractor), every engine check the stdlib dispatchers -- the
# VM's VM::run and VM::callValue, the tree-walker's Interpreter::visit overloads
# -- call on their stdlib routes, with the module/method literals guarding each
# call, and every ROUTE: each call that runs a stdlib function under a stdlib
# marker, counted, so a new route fails here even when it runs no check at all.
# tools/codemap/predispatch_checks.txt classifies each one (gate, detector,
# post-call, context). The pre-dispatch checks were first found by grepping for
# names already known, which is exactly what misses a check under a new name; this
# suite makes a new check FAIL until someone classifies it, and makes the list
# fail when the code stops making a call it lists.
#
#   PE-00  harness: libclang loads and build/compile_commands.json compiles both
#          dispatcher files; otherwise UNMEASURABLE
#   PE-01  floor: the enumeration parsed both files without errors, found the four
#          dispatchers, and holds four facts traced by hand (the filesystem gate on
#          the VM, the network gate guarded by `http` on the tree-walker, the BSD
#          pre-check on the VM, the VM's two `__stdlib_module__` routes) -- an
#          empty or partial enumeration is not a result
#   PE-02  the code and the classified list agree, in both directions, and every
#          class matches what tools/codemap/store.py models ([predispatch] for a
#          gate, [precheck] for a detector)
#   Controls, each a doctored copy of the list or of the enumeration run through
#   the SAME comparison, which must name the difference:
#   PE-03c a call the code makes that the list lacks -> UNCLASSIFIED
#   PE-04c a listed call the code does not make -> STALE
#   PE-05c a modelled gate classed as context -> MODELLED GATE CLASSED
#   PE-06c a modelled detector classed as post-call -> MODELLED DETECTOR CLASSED
#   PE-07c a third dispatcher -> UNLISTED DISPATCHER
#   PE-08c an empty enumeration -> UNMEASURABLE (exit 2), never a pass
#   PE-09c a third route under the same guard (count 2 -> 3) -> UNCLASSIFIED and
#          STALE route rows: a new route is a new row, whatever it checks
#
# Needs python3 and libclang (python3-clang); missing either is UNMEASURABLE.
# Reads the repository; writes only to its own temporary directory.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$REPO/tests/helpers/native_path.sh"
export PYTHONDONTWRITEBYTECODE=1

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | sed 's/^/       | /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }
finish() { echo "Result: $PASS passed, $FAIL failed, $SKIP skipped"; [ "$FAIL" -eq 0 ]; exit $?; }

echo "=== every pre-dispatch check is classified ==="
if ! command -v python3 >/dev/null 2>&1; then
    skip PE-ALL "python3 not found"; finish
fi

ENUM="$(native_path "$REPO/tools/codemap/predispatch_enum.py")"
LIST="$(native_path "$REPO/tools/codemap/predispatch_checks.txt")"
NREPO="$(native_path "$REPO")"
W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
NW="$(native_path "$W")"

# ---------------------------------------------------------------- PE-00
LIBCLANG="$(python3 -c "
import clang.cindex as ci
ci.Index.create()
print('LIBCLANG_OK')" 2>&1)"
if ! grep -q LIBCLANG_OK <<<"$LIBCLANG"; then
    skip PE-00 "libclang Python bindings unavailable (apt-get install python3-clang)"; finish
fi
CDB="$REPO/build/compile_commands.json"
if [ ! -f "$CDB" ] || ! grep -q 'src/vm/vm.cpp' "$CDB" || ! grep -q 'src/interpreter/call_dispatch.cpp' "$CDB"; then
    skip PE-00 "build/compile_commands.json missing or does not compile both dispatcher files (run cmake in build/)"; finish
fi
ok PE-00 "libclang loads; the compile database holds vm.cpp and call_dispatch.cpp"

# ---------------------------------------------------------------- PE-01
python3 "$ENUM" "$NREPO" > "$W/enum.txt" 2> "$W/enum.err"; rc=$?
OUT="$(cat "$W/enum.txt")"
ERRS="$(grep -c '^# parse error' "$W/enum.txt")"
if [ "$rc" -ne 0 ] || [ "$ERRS" -ne 0 ]; then
    skip PE-01 "the enumeration did not parse cleanly (rc=$rc, $ERRS parse errors)"
    head -5 "$W/enum.err" "$W/enum.txt" | sed 's/^/       | /'
    finish
fi
TAB="$(printf '\t')"
missing=""
for anchor in "VM::run()${TAB}GovernanceEngine::checkPathAccess${TAB}*" \
              "Interpreter::visit(CallExpr&)${TAB}GovernanceEngine::checkNetworkAllowed${TAB}http" \
              "VM::run()${TAB}GovernanceEngine::checkPreExecution${TAB}*" \
              "route${TAB}VM::run()${TAB}VM::callStdlibMethod${TAB}__stdlib_module__${TAB}2"; do
    grep -qxF "$anchor" <<<"$OUT" || missing="$missing [$anchor]"
done
ROWS="$(grep -vc '^#' <<<"$OUT")"
if grep -qxF '# dispatchers: Interpreter::visit(CallExpr&); Interpreter::visit(MemberExpr&); VM::callValue(NaabVal, int); VM::run()' <<<"$OUT" \
   && [ -z "$missing" ] && [ "$ROWS" -ge 20 ]; then
    ok PE-01 "the four dispatchers found, $ROWS calls and routes enumerated, the four hand-traced facts among them"
else
    bad PE-01 "the enumeration is not the one the code makes (rows=$ROWS, missing:${missing:- none})" "$(head -5 <<<"$OUT")"
    finish
fi

check() {   # check BASELINE [ENUMERATION] -> output, rc in CHK_RC
    CHK_OUT="$(python3 "$ENUM" "$NREPO" --check "$1" --enum "${2:-$NW/enum.txt}" 2>&1)"; CHK_RC=$?
}

# ---------------------------------------------------------------- PE-02
check "$LIST"
if [ "$CHK_RC" -eq 0 ] && grep -q ' 0 differences$' <<<"$CHK_OUT"; then
    ok PE-02 "the code and tools/codemap/predispatch_checks.txt agree ($(tail -1 <<<"$CHK_OUT"))"
else
    bad PE-02 "the code and the classified list differ: classify each UNCLASSIFIED call, drop each STALE row (rc=$CHK_RC)" "$CHK_OUT"
fi

# ---------------------------------------------------------------- controls
# Each doctored list is the real one with one change, so a control that does not
# fire means the comparison cannot see that kind of difference.
row_of() { grep -m1 -F "$1" "$REPO/tools/codemap/predispatch_checks.txt"; }

# PE-03c: drop a listed call the code makes.
DROP="$(row_of "VM::run()${TAB}GovernanceEngine::checkStdlibRate${TAB}")"
grep -vxF "$DROP" "$REPO/tools/codemap/predispatch_checks.txt" > "$W/drop.txt"
check "$NW/drop.txt"
if [ "$CHK_RC" -eq 1 ] && grep -qF "UNCLASSIFIED VM::run()${TAB}GovernanceEngine::checkStdlibRate${TAB}*" <<<"$CHK_OUT"; then
    ok PE-03c "a call missing from the list is named UNCLASSIFIED (control)"
else
    bad PE-03c "removing a row did not produce UNCLASSIFIED (rc=$CHK_RC)" "$CHK_OUT"
fi

# PE-04c: list a call no dispatcher makes.
{ cat "$REPO/tools/codemap/predispatch_checks.txt"
  printf 'VM::run()\tGovernanceEngine::checkNoSuchThing\t*\tgate\tfabricated\n'; } > "$W/extra.txt"
check "$NW/extra.txt"
if [ "$CHK_RC" -eq 1 ] && grep -qF "STALE VM::run()${TAB}GovernanceEngine::checkNoSuchThing" <<<"$CHK_OUT"; then
    ok PE-04c "a listed call the code does not make is named STALE (control)"
else
    bad PE-04c "a fabricated row was not reported STALE (rc=$CHK_RC)" "$CHK_OUT"
fi

# PE-05c: a gate the map models, classed as context.
GATE="$(row_of "Interpreter::visit(CallExpr&)${TAB}GovernanceEngine::checkNetworkAllowed${TAB}http${TAB}gate")"
GATE_AS_CTX="$(printf '%s' "$GATE" | sed "s/${TAB}gate${TAB}/${TAB}context${TAB}/")"
{ grep -vxF "$GATE" "$REPO/tools/codemap/predispatch_checks.txt"; printf '%s\n' "$GATE_AS_CTX"; } > "$W/gate.txt"
check "$NW/gate.txt"
if ! grep -qxF "$GATE_AS_CTX" "$W/gate.txt"; then
    bad PE-05c "the doctored list was not written (fixture)" "$GATE"
elif [ "$CHK_RC" -eq 1 ] && grep -qF "MODELLED GATE CLASSED context Interpreter::visit(CallExpr&)${TAB}GovernanceEngine::checkNetworkAllowed" <<<"$CHK_OUT"; then
    ok PE-05c "a modelled gate classed otherwise is named (control)"
else
    bad PE-05c "reclassing a gate went unreported (rc=$CHK_RC)" "$CHK_OUT"
fi

# PE-06c: a detector the map models, classed as post-call.
DET="$(row_of "VM::run()${TAB}GovernanceEngine::checkPreExecution${TAB}*${TAB}detector")"
DET_AS_POST="$(printf '%s' "$DET" | sed "s/${TAB}detector${TAB}/${TAB}post-call${TAB}/")"
{ grep -vxF "$DET" "$REPO/tools/codemap/predispatch_checks.txt"; printf '%s\n' "$DET_AS_POST"; } > "$W/det.txt"
check "$NW/det.txt"
if [ "$CHK_RC" -eq 1 ] && grep -qF "MODELLED DETECTOR CLASSED post-call VM::run()${TAB}GovernanceEngine::checkPreExecution" <<<"$CHK_OUT"; then
    ok PE-06c "a modelled detector classed otherwise is named (control)"
else
    bad PE-06c "reclassing a detector went unreported (rc=$CHK_RC)" "$CHK_OUT"
fi

# PE-07c: the enumeration finds a dispatcher the list does not name.
{ sed 's/^# dispatchers: .*/&; NewEngine::dispatch()/' "$W/enum.txt"
  printf 'NewEngine::dispatch()\tGovernanceEngine::checkPathAccess\t*\n'; } > "$W/enum3.txt"
check "$LIST" "$NW/enum3.txt"
if [ "$CHK_RC" -eq 1 ] && grep -qxF "UNLISTED DISPATCHER NewEngine::dispatch()" <<<"$CHK_OUT"; then
    ok PE-07c "a dispatcher the list does not name is reported (control)"
else
    bad PE-07c "a new dispatcher went unreported (rc=$CHK_RC)" "$CHK_OUT"
fi

# PE-08c: an enumeration that found nothing is not agreement.
printf '# dispatchers: \n' > "$W/empty.txt"
check "$LIST" "$NW/empty.txt"
if [ "$CHK_RC" -eq 2 ] && grep -q '^UNMEASURABLE' <<<"$CHK_OUT"; then
    ok PE-08c "an empty enumeration is UNMEASURABLE, not a pass (control)"
else
    bad PE-08c "an empty enumeration was judged (rc=$CHK_RC)" "$CHK_OUT"
fi

# PE-09c: a third route under a guard two routes already share.
sed "s/^route${TAB}VM::run()${TAB}VM::callStdlibMethod${TAB}__stdlib_module__${TAB}2\$/route${TAB}VM::run()${TAB}VM::callStdlibMethod${TAB}__stdlib_module__${TAB}3/" "$W/enum.txt" > "$W/enum_route.txt"
check "$LIST" "$NW/enum_route.txt"
if ! grep -q "__stdlib_module__${TAB}3$" "$W/enum_route.txt"; then
    bad PE-09c "the doctored enumeration was not written (fixture)"
elif [ "$CHK_RC" -eq 1 ] && grep -qF "UNCLASSIFIED route${TAB}VM::run()${TAB}VM::callStdlibMethod${TAB}__stdlib_module__${TAB}3" <<<"$CHK_OUT" \
     && grep -qF "STALE route${TAB}VM::run()${TAB}VM::callStdlibMethod${TAB}__stdlib_module__${TAB}2" <<<"$CHK_OUT"; then
    ok PE-09c "a new route under an existing guard changes its row's count and is reported (control)"
else
    bad PE-09c "a third route went unreported (rc=$CHK_RC)" "$CHK_OUT"
fi

finish
