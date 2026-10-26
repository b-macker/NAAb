#!/usr/bin/env bash
# ============================================================
# test_function_capability_composition.sh — F8: intersection down the stack
#
# WHAT THIS CLOSES
#
# Consulting only the innermost function makes capabilities.functions escapable
# by refactoring: a caller declaring [FS_READ] calls a helper declaring
# [FS_WRITE], and the write goes through. That is the split-delegation /
# confused-deputy pattern agent_review's own prompt hunts for -- and it means
# `git mv` defeats the feature, which is advice rather than a boundary.
#
# Effective permission is now the INTERSECTION of every function on the call
# stack. Same monotonic narrowing the engine already applies for
# role-subset-of-program, extended to depth: a caller can narrow a callee, never
# widen it.
#
# A frame with no entry and no "default" is UNRESTRICTED and contributes the
# universal set. Without that, naming a few functions would accidentally deny
# everything else.
#
#   FC-01  the escape is closed: restricted caller + permissive callee reports
#   FC-02  the error names the CALLER that narrowed it. Under intersection the
#          blocking declaration is usually NOT the function that attempted the
#          call, and without provenance the operator edits the callee and
#          nothing changes
#   FC-03  POSITIVE CONTROL: permissive caller + permissive callee is SILENT.
#          Without this, "always deny" passes FC-01
#   FC-04  POSITIVE CONTROL: a frame with no entry and no default narrows
#          nothing. Without this, "deny unless explicitly listed everywhere"
#          passes FC-01 and FC-03
#   FC-05  the OUTERMOST constraint is the one reported, since that is the one
#          the operator must relax
#   FC-06  THE REMEDY WORKS: applying the suggested declaration to the entry the
#          message names silences it
#   Every arm runs on BOTH engines: the VM under enforce, the tree-walker under
#   audit (FC-TW: under enforce --tree-walk is refused before it runs).
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

W="${TMPDIR:-/tmp}/naab-fccomp-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }

cat > "$W/t.naab" <<'EOF'
fn write_helper(d) { file.write("out.txt", d) return 1 }
fn restricted_caller(d) { return write_helper(d) }
main { restricted_caller("x") print("DONE") }
EOF

# three levels: outer narrows hardest, so it must be the one reported
cat > "$W/deep.naab" <<'EOF'
fn leaf(d) { file.write("out.txt", d) return 1 }
fn middle(d) { return leaf(d) }
fn outer(d) { return middle(d) }
main { outer("x") print("DONE") }
EOF

# python writes to stdout, the shell redirects (see test_shell_path_handoff.sh)
# The tree-walker is refused under enforce (FC-TW), so it runs in $W/aud beside
# the same config in mode audit. Under audit the AUDIT line carries only a
# message's first line; the JSON report keeps all of it, so run() prints both.
mkdir -p "$W/aud"
cfg() {
    local m
    for m in enforce audit; do
        python3 -c "
import json,sys
json.dump({'version':'1.0','mode':sys.argv[2],'security':{'sandbox_level':'elevated'},
 'capabilities':{'filesystem':{'mode':'write'},
   'functions':json.loads(sys.argv[1])}}, sys.stdout)
" "$1" "$m" > "$( [ $m = enforce ] && echo "$W" || echo "$W/aud" )/govern.json"
    done
}
run() { local d="$W"; [ -n "${2:-}" ] && d="$W/aud"; cp "$W/$1" "$d/$1" 2>/dev/null; rm -f "$d/fc_rep.json"
        ( cd "$d" && timeout 60 "$NAAB" run ${2:-} --governance-report fc_rep.json "$1" 2>&1; cat fc_rep.json 2>/dev/null ); }
warns() { run "$1" "$2" | grep -c "Undeclared action" || true; }

BOTH='{"default":{"allowed_actions":["FS_READ","FS_WRITE"]},"restricted_caller":{"allowed_actions":["FS_READ"]},"write_helper":{"allowed_actions":["FS_READ","FS_WRITE"]}}'
OPEN='{"default":{"allowed_actions":["FS_READ","FS_WRITE"]}}'
NODEF='{"write_helper":{"allowed_actions":["FS_READ","FS_WRITE"]}}'

echo -e "${CYAN}=== Group FC: capability composition across the call stack ===${NC}"

source "$SCRIPT_DIR/../helpers/tree_walk_refusal.sh"
cfg "$BOTH"; out="$( cd "$W" && timeout 60 "$NAAB" --tree-walk t.naab 2>&1 )"; rc=$?
if tw_refused "$rc" "$out"; then ok "FC-TW" "under the enforce config --tree-walk is refused before it runs"
else bad "FC-TW" "--tree-walk ran under an enforce config" "exit $rc"; fi

for eng in "VM:" "tree-walk:--tree-walk"; do
    label="${eng%%:*}"; flag="${eng##*:}"

    cfg "$BOTH"; out=$(run t.naab "$flag")
    case "$out" in
      *"Undeclared action"*) ok "FC-01/$label" "split-delegation escape is closed" ;;
      *) bad "FC-01/$label" "a restricted caller must not gain FS_WRITE via a helper" \
             "the helper's own declaration was used instead of the intersection" ;;
    esac

    case "$out" in
      *"Narrowed by caller 'restricted_caller'"*)
          ok "FC-02/$label" "names the CALLER that narrowed the set" ;;
      *) bad "FC-02/$label" "the narrowing frame must be named" \
             "without it the operator edits the callee and nothing changes" ;;
    esac

    cfg "$OPEN"
    [ "$(warns t.naab "$flag")" = "0" ] \
      && ok  "FC-03/$label" "POSITIVE CONTROL: permissive caller + callee is silent" \
      || bad "FC-03/$label" "intersection must not deny a permitted chain" "'always deny' would pass FC-01"

    cfg "$NODEF"
    [ "$(warns t.naab "$flag")" = "0" ] \
      && ok  "FC-04/$label" "POSITIVE CONTROL: an unlisted frame narrows nothing" \
      || bad "FC-04/$label" "a frame with no entry and no default is unrestricted" \
             "naming a few functions must not deny everything else"

    # outer is the most restrictive; it must be the reported entry
    cfg '{"default":{"allowed_actions":["FS_READ","FS_WRITE"]},"outer":{"allowed_actions":["FS_READ"]},"middle":{"allowed_actions":["FS_READ","FS_WRITE"]},"leaf":{"allowed_actions":["FS_READ","FS_WRITE"]}}'
    out=$(run deep.naab "$flag")
    case "$out" in
      *"capabilities.functions.outer.allowed_actions"*)
          ok "FC-05/$label" "the outermost constraint is the one reported" ;;
      *) bad "FC-05/$label" "must report the outermost narrowing frame" \
             "rule: $(echo "$out" | grep -oE 'capabilities\.functions\.[a-z_]+' | head -1)" ;;
    esac

    # FC-06: apply the suggestion to the entry the message named
    cfg "$BOTH"; out=$(run t.naab "$flag")
    entry=$(echo "$out" | grep -oE 'capabilities\.functions\.[a-zA-Z_]+\.allowed_actions' | head -1 | sed 's/capabilities\.functions\.//;s/\.allowed_actions//')
    arr=$(echo "$out" | grep -oE '"allowed_actions": \[[^]]*\]' | head -1 | sed 's/.*\[/[/')
    if [ -n "$entry" ] && [ -n "$arr" ]; then
        cfg "$(python3 -c "
import json,sys
d=json.loads(sys.argv[1]); d[sys.argv[2]]={'allowed_actions':json.loads(sys.argv[3])}
json.dump(d,sys.stdout)" "$BOTH" "$entry" "$arr")"
        [ "$(warns t.naab "$flag")" = "0" ] \
          && ok  "FC-06/$label" "applying the suggestion to '$entry' silences it" \
          || bad "FC-06/$label" "the recommended remedy must work" "still warns after applying $arr to $entry"
    else
        bad "FC-06/$label" "the message must name an entry and an array" "entry='$entry' arr='$arr'"
    fi
done

echo
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}=== Results: $PASS passed, 0 failed ===${NC}"; exit 0
else
    echo -e "${RED}=== Results: $PASS passed, $FAIL failed ===${NC}"; echo -e "$FAILURES"; exit 1
fi
