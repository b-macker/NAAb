#!/usr/bin/env bash
# ============================================================
# test_function_capabilities.sh — capabilities.functions (phase 3)
#
# The third rung of the scope ladder: program -> role -> function. Enforced at
# ONE gate (checkFilesystemAllowed) and ADVISORY only, which is deliberate --
# composition across the call stack (F8 in docs/plan-function-effects.md) is NOT
# implemented, so a restricted caller can still reach the gate through a
# permissive callee. Advisory until that lands.
#
# "default" covers any function without its own entry. NOT "absent means
# unrestricted", which would protect only what someone remembered to list.
#
#   CF-01  an undeclared action is reported, naming the function
#   CF-02  POSITIVE CONTROL: a declared action is silent, so CF-01 cannot be
#          satisfied by warning about everything
#   CF-03  advisory does NOT block -- execution continues, exit 0
#   CF-04  "default" applies to a function with no entry of its own
#   CF-05  THE SUGGESTED JSON IS VALID AND WORKS. The message prints a config
#          fragment to paste; this parses it AND applies it and asserts the
#          warning goes away. Guidance that does not survive being followed is
#          not guidance -- and the first version of this message emitted
#          ["FS_READ, FS_WRITE"], one element containing a comma, which is JSON
#          an operator cannot paste
#   CF-06  BACKWARD COMPAT: no capabilities.functions key -> no restriction
#   CF-07  an unknown action name warns at load rather than silently removing a
#          permission from an allowlist
#   Every arm runs on BOTH engines.
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

W="${TMPDIR:-/tmp}/naab-fncap-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }

cat > "$W/t.naab" <<'EOF'
fn save_report(d) { file.write("ok.txt", d) return 1 }
fn sneaky(d) { file.write("bad.txt", d) return 1 }
main { save_report("a") sneaky("b") print("DONE") }
EOF

# python writes to stdout, the shell redirects (see test_shell_path_handoff.sh)
cfg() {  # $1 = JSON for capabilities.functions, or "" to omit the key
    python3 -c "
import json,sys
d={'version':'1.0','mode':'enforce','security':{'sandbox_level':'elevated'},
   'capabilities':{'filesystem':{'mode':'write'}}}
if sys.argv[1]: d['capabilities']['functions']=json.loads(sys.argv[1])
json.dump(d, sys.stdout)
" "$1" > "$W/govern.json"
}
run() { ( cd "$W" && timeout 60 "$NAAB" ${1:-} t.naab 2>&1 ); }

BASE='{"default":{"allowed_actions":["FS_READ"]},"save_report":{"allowed_actions":["FS_READ","FS_WRITE"]}}'

echo -e "${CYAN}=== Group CF: capabilities.functions ===${NC}"

for eng in "VM:" "tree-walk:--tree-walk"; do
    label="${eng%%:*}"; flag="${eng##*:}"

    cfg "$BASE"; out=$(run "$flag")
    case "$out" in
      *"Undeclared action in 'sneaky': FS_WRITE"*)
          ok "CF-01/$label" "undeclared action reported, naming the function" ;;
      *) bad "CF-01/$label" "an undeclared action must be reported" \
             "got: $(echo "$out" | grep -oE 'Undeclared[^\"]*' | head -1)" ;;
    esac

    case "$out" in
      *"Undeclared action in 'save_report'"*)
          bad "CF-02/$label" "a DECLARED action must not warn" "save_report declares FS_WRITE" ;;
      *)  ok "CF-02/$label" "POSITIVE CONTROL: declared action is silent" ;;
    esac

    ( cd "$W" && timeout 60 "$NAAB" $flag t.naab >/dev/null 2>&1 ); rc=$?
    if [ "$rc" = "0" ] && [ -n "$(echo "$out" | grep -o DONE)" ]; then
        ok "CF-03/$label" "advisory does not block (exit 0, execution continued)"
    else
        bad "CF-03/$label" "this tier must stay advisory" "exit $rc"
    fi

    # sneaky has no entry; it must be governed by "default"
    case "$out" in
      *"capabilities.functions.default.allowed_actions"*)
          ok "CF-04/$label" "'default' covers a function with no entry" ;;
      *) bad "CF-04/$label" "default must apply to unlisted functions" \
             "rule line: $(echo "$out" | grep -oE 'capabilities\.functions[^ ]*' | head -1)" ;;
    esac

    # CF-05: extract the suggested array, prove it is valid JSON, then APPLY it
    frag=$(echo "$out" | grep -oE '"allowed_actions": \[[^]]*\]' | head -1)
    arr=$(echo "$frag" | sed 's/.*\[/[/')
    if printf '%s' "$arr" | python3 -c "import json,sys; json.load(sys.stdin)" 2>/dev/null; then
        cfg "$(python3 -c "
import json,sys
base=json.loads(sys.argv[1]); base['default']={'allowed_actions':json.loads(sys.argv[2])}
json.dump(base,sys.stdout)" "$BASE" "$arr")"
        out2=$(run "$flag")
        case "$out2" in
          *"Undeclared action"*) bad "CF-05/$label" "applying the suggestion must silence it" \
                                     "still warns after applying $arr" ;;
          *) ok "CF-05/$label" "the suggested JSON is valid AND applying it works ($arr)" ;;
        esac
    else
        bad "CF-05/$label" "the suggested JSON must be pasteable" "got: ${arr:-<none>}"
    fi

    cfg ""; out3=$(run "$flag")
    case "$out3" in
      *"Undeclared action"*) bad "CF-06/$label" "no key must mean no restriction" "warned anyway" ;;
      *DONE*) ok "CF-06/$label" "BACKWARD COMPAT: absent key -> no restriction" ;;
      *) bad "CF-06/$label" "program must still run" "got: $(echo "$out3" | head -2 | tr '\n' ' ')" ;;
    esac

    cfg '{"default":{"allowed_actions":["FS_READ","NOT_AN_ACTION"]}}'
    case "$(run "$flag")" in
      *'unknown action "NOT_AN_ACTION"'*)
          ok "CF-07/$label" "unknown action name warns at load" ;;
      *) bad "CF-07/$label" "a typo in an allowlist must not be silent" \
             "it would silently remove a permission" ;;
    esac
done

echo
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}=== Results: $PASS passed, 0 failed ===${NC}"; exit 0
else
    echo -e "${RED}=== Results: $PASS passed, $FAIL failed ===${NC}"; echo -e "$FAILURES"; exit 1
fi
