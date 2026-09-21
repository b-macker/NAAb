#!/usr/bin/env bash
# ============================================================
# test_function_attribution.sh — which NAAb function is executing (FAS)
#
# WHY THIS EXISTS
#
# Nothing in the engine could name the executing function. RuntimeEvent carries
# `file` and `line` and no function, so a taint violation, a BSD event and a
# capability refusal could each name a line but not the function that owns the
# call. This is phase 1 of docs/plan-function-effects.md and it ships value on
# its own: the path-access refusal now says which function attempted it.
#
# THE TWO WAYS THIS BREAKS, both guarded here:
#
# 1. ENGINE DIVERGENCE. The tree-walker has TWO paths into a user function --
#    callFunction() and visit(CallExpr) -- as call_dispatch.cpp's own header
#    warns. Wiring one left attribution empty under --tree-walk while the VM
#    reported correctly. Every arm runs on BOTH engines for that reason.
#
# 2. STACK LEAK. The VM decrements frame_count_ at six sites plus the exception
#    unwind. A mirrored push/pop stack leaks at whichever one someone forgets --
#    the same shape as the try-handler cleanup bug. The VM therefore DERIVES the
#    stack from frames_ (authoritative by construction) while the tree-walker
#    uses an RAII guard. FA-04 and FA-05 are what prove neither drifts.
#
#   FA-01  a refusal inside a function names that function
#   FA-02  the innermost function is named, not the outermost
#   FA-03  POSITIVE CONTROL: a refusal at top level still reports, with no
#          bogus function name -- without this, "names a function" could be
#          satisfied by always printing the last thing seen
#   FA-04  attribution is correct AFTER an exception unwinds through a call
#   FA-05  attribution is correct after a `return` out of a `try`
#   FA-06  POSITIVE CONTROL: the permitted path still succeeds, so the suite
#          cannot pass by refusing everything
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; FAILURES=""
ok()  { PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }

W="${TMPDIR:-/tmp}/naab-fas-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W/vault" "$W/data"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }

python3 -c "
import json,sys
json.dump({'version':'1.0','mode':'enforce','security':{'sandbox_level':'elevated'},
 'capabilities':{'filesystem':{'mode':'write','blocked_paths':['vault/']}}}, sys.stdout)
" > "$W/govern.json"

# attributed function name for a run, or "" when none was reported
attrib() { ( cd "$W" && timeout 60 "$NAAB" ${2:-} "$1" 2>&1 ) \
    | grep -oE "in function '[^']*'" | head -1 \
    | sed "s/in function '//;s/'//"; }
# NOTE: the character class must accept ANY name. It was
# [a-zA-Z_][a-zA-Z0-9_]* and therefore could not match the VM's synthetic
# "<script>" frame, so a printed name read as "no name" and FA-03 passed for
# the wrong reason. Top level is now absent on both engines, but the pattern
# stays permissive so a future synthetic name is visible rather than silent.
blocked() { local o; o=$( cd "$W" && timeout 60 "$NAAB" ${2:-} "$1" 2>&1 ); \
    case "$o" in *"File path blocked"*) echo yes ;; *) echo no ;; esac; }

cat > "$W/simple.naab" <<'EOF'
fn save_report(data) {
  file.write("vault/secret.txt", data)
  return true
}
main { save_report("x") }
EOF

cat > "$W/nested.naab" <<'EOF'
fn inner_writer(d) {
  file.write("vault/secret.txt", d)
  return true
}
fn outer_caller(d) { return inner_writer(d) }
main { outer_caller("x") }
EOF

cat > "$W/toplevel.naab" <<'EOF'
main { file.write("vault/secret.txt", "x") }
EOF

# An exception unwinds THROUGH a call, then a violation happens elsewhere.
# A leaked frame would attribute the second violation to 'thrower'.
cat > "$W/unwind.naab" <<'EOF'
fn thrower(x) {
  throw "boom"
}
main {
  try { thrower(1) } catch (e) { }
  file.write("vault/secret.txt", "x")
}
EOF

# `return` out of a `try` — the compiler path that needed special handling.
cat > "$W/retry.naab" <<'EOF'
fn returns_from_try(x) {
  try { return 1 } catch (e) { return 2 }
}
main {
  returns_from_try(1)
  file.write("vault/secret.txt", "x")
}
EOF

cat > "$W/allowed.naab" <<'EOF'
fn save_ok(d) {
  file.write("data/ok.txt", d)
  return true
}
main { save_ok("x") print("WROTE_OK") }
EOF

echo -e "${CYAN}=== Group FA: function attribution, both engines ===${NC}"

for eng in "VM:" "tree-walk:--tree-walk"; do
    label="${eng%%:*}"; flag="${eng##*:}"

    got=$(attrib simple.naab "$flag")
    [ "$got" = "save_report" ] \
      && ok  "FA-01/$label" "refusal names the function ('save_report')" \
      || bad "FA-01/$label" "refusal must name the function" "got: '${got:-none}'"

    got=$(attrib nested.naab "$flag")
    [ "$got" = "inner_writer" ] \
      && ok  "FA-02/$label" "innermost function is named" \
      || bad "FA-02/$label" "must name the innermost function" "got: '${got:-none}'"

    # main IS a function in the AST, so naming it is correct; naming anything
    # from a previous run or an unrelated function is not.
    got=$(attrib toplevel.naab "$flag"); blk=$(blocked toplevel.naab "$flag")
    if [ "$blk" = "yes" ] && { [ "$got" = "main" ] || [ -z "$got" ]; }; then
        ok "FA-03/$label" "POSITIVE CONTROL: top level reports, no bogus name (${got:-none})"
    else
        bad "FA-03/$label" "top-level refusal must report without a stale name" \
            "blocked=$blk name='${got:-none}'"
    fi

    # The violation is at TOP LEVEL, shallower than the call that unwound. A
    # leaked frame would still be on top and would be named here; correct
    # behaviour names 'main' or nothing. Naming 'thrower' IS the leak.
    got=$(attrib unwind.naab "$flag")
    if [ "$got" = "main" ] || [ -z "$got" ]; then
        ok "FA-04/$label" "no frame leaked through an exception unwind (${got:-none})"
    else
        bad "FA-04/$label" "a frame leaked through the unwind" "attributed to '$got'"
    fi

    got=$(attrib retry.naab "$flag")
    if [ "$got" = "main" ] || [ -z "$got" ]; then
        ok "FA-05/$label" "no frame leaked through return-out-of-try (${got:-none})"
    else
        bad "FA-05/$label" "a frame leaked through return-out-of-try" "attributed to '$got'"
    fi

    out=$( cd "$W" && timeout 60 "$NAAB" $flag allowed.naab 2>&1 )
    case "$out" in
      *WROTE_OK*) ok "FA-06/$label" "POSITIVE CONTROL: permitted path still succeeds" ;;
      *) bad "FA-06/$label" "a permitted write must still work" "got: $(echo "$out" | head -2 | tr '\n' ' ')" ;;
    esac
done

echo
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}=== Results: $PASS passed, 0 failed ===${NC}"; exit 0
else
    echo -e "${RED}=== Results: $PASS passed, $FAIL failed ===${NC}"; echo -e "$FAILURES"; exit 1
fi
