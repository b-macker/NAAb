#!/usr/bin/env bash
# test_extends_floor.sh -- under `extends` a child cannot loosen its parent
# (A98, the owner's decision of 2026-11-06)
#
# The merge (mergeRules) decided each setting by the strategy: under
# child_wins a child's explicit value replaced the parent's, and under
# parent_wins a child's non-empty list still replaced the parent's
# (merge_arrays: replace) or widened it (append). Measured by
# tests/property/ratchet_property.cpp --extends on master: every strategy and
# array mode had pairs where the child allowed a request the parent refused
# (about 280 loosened probes in 1,918 pairs at seed 3). The extends floor
# (applyExtendsFloor) now makes every security setting at least as strict as
# the parent's: switches allowed only when both allow, blocked lists unioned,
# allow lists narrowed to what both admit (two that admit nothing in common
# refuse the config), every check on when either turns it on at the stricter
# level. Each arm runs the parent used directly first as its control.
#
#   XF-01   child_wins/replace: a child's own blocked_read does not replace
#           the parent's -- the parent's variable is still refused
#   XF-02   parent_wins/replace: the same (parent_wins did not protect it)
#   XF-03   append: a child's allowed_read entry the parent does not allow is
#           not admitted; XF-03c CONTROL: the entry both allow is read
#   XF-04   a child's explicit env_vars.write: true does not switch the
#           parent's false back on
#   XF-05   a child's allowed_paths entry inside the parent's blocked_paths
#           does not open it (the longest entry wins at the gate, so an allow
#           list alone punched the hole); XF-05c CONTROL: an exception the
#           parent itself made (its own allowed entry inside its own blocked
#           one) still opens
#   XF-06   languages: a language the child allows and the parent does not is
#           refused; XF-06c CONTROL: the language both allow runs
#   XF-07   a child's `"no_placeholders": "advisory"` does not lower the
#           parent's hard check. (A child's `"no_placeholders": false` never
#           switched it off: the shorthand false is not marked explicitly set,
#           so the merge inherited the parent's check -- observed on master,
#           which is why this arm lowers the level instead)
#   XF-08   two allow lists with nothing in common refuse the config (exit 4,
#           nothing runs, the key named), never read as unrestricted;
#           XF-08c CONTROL: overlapping lists load and run
#   XF-09   a child's `"no_pii": false` does not switch off the parent's PII
#           check (the value is built from fragments at run time)
#   XF-10c  CONTROL: a child still tightens -- its own blocked_read entry is
#           refused beside the parent's
#   XF-11c  CONTROL: a child still sets what is not a security setting -- its
#           own report path is written
#
# Kill table (each mutant rebuilt, this suite run; observed 2026-11-09):
#   applyExtendsFloor not called                  XF-01..XF-09 (XF-03c,
#                                                 XF-05c, XF-06c, XF-08c,
#                                                 XF-10c, XF-11c pass)
#   the path-hole step removed                    XF-05
#   meetAllowlist keeps the child's list          XF-03, XF-06, XF-08
#   the disjoint refusal removed (empty = allowed) XF-08
#   the parent's effective levels not used        tests/property/ratchet_property.cpp
#                                                 --extends (scan.pii); no arm
#                                                 here builds that pair
# Red first (observed, master's binary): XF-01..XF-09 fail; the controls pass.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_xfloor_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then echo "UNMEASURABLE: naab-lang not built"; exit 1; fi

TOK="xf-$$-$RANDOM"
export NAAB_XF_A="a-$TOK" NAAB_XF_B="b-$TOK"
HDR='"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}'

# xrun ID PARENT_BODY CHILD_BODY STRATEGY ARRAYS PROGRAM -> PO/PRC (parent used
# directly), CO/CRC (the child). Bodies are JSON members after the header.
xrun() {
    local id="$1" pb="$2" cb="$3" strat="$4" arrays="$5" prog="$6"
    local d="$W/$id"
    mkdir -p "$d/base/vault/inner" "$d/child/vault/inner" "$d/child/data" "$d/base/data"
    for x in base child; do
        echo "V-$TOK" > "$d/$x/vault/inner/y.txt"
        printf '%s\n' "$prog" > "$d/$x/p.naab"
    done
    printf '{ %s%s }\n' "$HDR" "${pb:+, $pb}" > "$d/base/govern.json"
    printf '{ %s, "extends": "../base/govern.json",
  "meta": {"inheritance": {"merge_strategy": "%s", "merge_arrays": "%s"}}%s }\n' \
        "$HDR" "$strat" "$arrays" "${cb:+, $cb}" > "$d/child/govern.json"
    PRC=0; PO=$(cd "$d/base" && timeout 60 "$NAAB" p.naab 2>&1) || PRC=$?
    CRC=0; CO=$(cd "$d/child" && timeout 60 "$NAAB" p.naab 2>&1) || CRC=$?
}
# refused_both ID DESC MARK: the parent used directly refuses (exit 3, MARK
# not printed) -- else UNMEASURABLE -- and so must the child.
refused_both() {
    local id="$1" desc="$2" mark="$3"
    if [ "$PRC" -ne 3 ] || grep -q -F -- "$mark" <<<"$PO"; then
        unm "$id" "control failed: the parent used directly did not refuse (rc=$PRC)"; return
    fi
    if [ "$CRC" -eq 3 ] && ! grep -q -F -- "$mark" <<<"$CO"; then pass "$id" "$desc"
    else fail "$id" "$desc -- the child allowed it (rc=$CRC)" "$CO"; fi
}
# ran_child ID DESC MARK: the child runs the program to MARK, exit 0.
ran_child() {
    if [ "$CRC" -eq 0 ] && grep -q -F -- "$3" <<<"$CO"; then pass "$1" "$2"
    else fail "$1" "$2 (rc=$CRC)" "$CO"; fi
}

P_READ_A='use env
main {
    print("A=" + string(env.get("NAAB_XF_A")))
}'
P_READ_B='use env
main {
    print("B=" + string(env.get("NAAB_XF_B")))
}'
P_WRITE='use env
main {
    env.set_var("NAAB_XF_W", "x")
    print("WROTE")
}'
P_VAULT='use file
main {
    print("VAULT=" + file.read("vault/inner/y.txt"))
}'
P_JS='main {
    let j = <<javascript
6 * 7
>>
    print("JS=" + string(j))
}'
P_SQL='main {
    let s = <<sql
SELECT 42 AS x
>>
    print("SQL=" + string(s))
}'
P_PH='main {
    let x = 1
    // TODO finish this
    print("RAN=" + string(x))
}'

echo "=== under extends a child cannot loosen its parent (A98) ==="

xrun xf01 '"capabilities": {"env_vars": {"blocked_read": ["NAAB_XF_A"]}}' \
          '"capabilities": {"env_vars": {"blocked_read": ["NAAB_XF_OTHER"]}}' child_wins replace "$P_READ_A"
refused_both XF-01 "child_wins/replace: the parent's blocked_read still holds beside the child's" "A=a-$TOK"

xrun xf02 '"capabilities": {"env_vars": {"blocked_read": ["NAAB_XF_A"]}}' \
          '"capabilities": {"env_vars": {"blocked_read": ["NAAB_XF_OTHER"]}}' parent_wins replace "$P_READ_A"
refused_both XF-02 "parent_wins/replace: the parent's blocked_read still holds" "A=a-$TOK"

xrun xf03 '"capabilities": {"env_vars": {"allowed_read": ["NAAB_XF_A"]}}' \
          '"capabilities": {"env_vars": {"allowed_read": ["NAAB_XF_A", "NAAB_XF_B"]}}' child_wins append "$P_READ_B"
refused_both XF-03 "append: a child's allowed_read entry the parent does not allow is not admitted" "B=b-$TOK"
xrun xf03c '"capabilities": {"env_vars": {"allowed_read": ["NAAB_XF_A"]}}' \
           '"capabilities": {"env_vars": {"allowed_read": ["NAAB_XF_A", "NAAB_XF_B"]}}' child_wins append "$P_READ_A"
ran_child XF-03c "CONTROL: the variable both allow is read in the child" "A=a-$TOK"

xrun xf04 '"capabilities": {"env_vars": {"write": false}}' \
          '"capabilities": {"env_vars": {"write": true}}' child_wins replace "$P_WRITE"
if [ "$PRC" -eq 0 ] && grep -q WROTE <<<"$PO"; then
    unm XF-04 "control failed: the parent used directly let the write through"
elif ! grep -q WROTE <<<"$CO" && [ "$CRC" -ne 0 ]; then
    pass XF-04 "a child's explicit write: true does not switch the parent's false back on"
else fail XF-04 "the child wrote the variable (rc=$CRC)" "$CO"; fi

xrun xf05 '"capabilities": {"filesystem": {"blocked_paths": ["vault/"]}}' \
          '"capabilities": {"filesystem": {"allowed_paths": ["vault/inner/", "data/"]}}' child_wins replace "$P_VAULT"
refused_both XF-05 "a child's allowed entry inside the parent's blocked_paths does not open it" "VAULT=V-$TOK"
xrun xf05c '"capabilities": {"filesystem": {"allowed_paths": ["vault/inner/", "data/"], "blocked_paths": ["vault/"]}}' \
           '"capabilities": {"filesystem": {"allowed_paths": ["vault/inner/", "data/"]}}' child_wins replace "$P_VAULT"
if [ "$PRC" -eq 0 ] && grep -q "VAULT=V-$TOK" <<<"$PO"; then
    ran_child XF-05c "CONTROL: the parent's own exception (allowed inside its blocked) still opens" "VAULT=V-$TOK"
else unm XF-05c "control failed: the parent used directly did not read its own exception (rc=$PRC)"; fi

xrun xf06 '"languages": {"allowed": ["sql"]}' '"languages": {"allowed": ["sql", "javascript"]}' child_wins replace "$P_JS"
refused_both XF-06 "a language the child allows and the parent does not is refused" "JS=42"
xrun xf06c '"languages": {"allowed": ["sql"]}' '"languages": {"allowed": ["sql", "javascript"]}' child_wins replace "$P_SQL"
ran_child XF-06c "CONTROL: the language both allow runs in the child" "SQL="

xrun xf07 '"code_quality": {"no_placeholders": {"level": "hard"}}' \
          '"code_quality": {"no_placeholders": "advisory"}' child_wins replace "$P_PH"
refused_both XF-07 "a child's no_placeholders: advisory does not lower the parent's hard check" "RAN=1"

xrun xf08 '"capabilities": {"env_vars": {"allowed_read": ["NAAB_XF_A"]}}' \
          '"capabilities": {"env_vars": {"allowed_read": ["NAAB_XF_B"]}}' child_wins replace "$P_READ_B"
if [ "$CRC" -eq 4 ] && ! grep -q "B=" <<<"$CO" && grep -q 'capabilities.env_vars.allowed_read' <<<"$CO"; then
    pass XF-08 "two allow lists with nothing in common refuse the config (exit 4, the key named)"
else fail XF-08 "disjoint allow lists were not refused at load (rc=$CRC)" "$CO"; fi
xrun xf08c '"capabilities": {"env_vars": {"allowed_read": ["NAAB_XF_A", "NAAB_XF_B"]}}' \
           '"capabilities": {"env_vars": {"allowed_read": ["NAAB_XF_B"]}}' child_wins replace "$P_READ_B"
ran_child XF-08c "CONTROL: overlapping allow lists load, and the shared entry is read" "B=b-$TOK"

# The PII value is assembled here, never written in this file whole.
SSN="$((100 + 23))-$((40 + 5))-$((6000 + 789))"
P_PII="main {
    print(\"ID $SSN\")
}"
xrun xf09 '"code_quality": {"no_pii": "hard"}' '"code_quality": {"no_pii": false}' child_wins replace "$P_PII"
refused_both XF-09 "a child's no_pii: false does not switch off the parent's PII check" "ID $SSN"

xrun xf10 '"capabilities": {"env_vars": {"blocked_read": ["NAAB_XF_A"]}}' \
          '"capabilities": {"env_vars": {"blocked_read": ["NAAB_XF_B"]}}' child_wins replace "$P_READ_B"
if [ "$PRC" -eq 0 ] && grep -q "B=b-$TOK" <<<"$PO"; then
    if [ "$CRC" -eq 3 ] && ! grep -q "B=b-$TOK" <<<"$CO"; then
        pass XF-10c "CONTROL: the child's own blocked_read entry is refused too (a child still tightens)"
    else fail XF-10c "the child's own blocked_read entry did not apply (rc=$CRC)" "$CO"; fi
else unm XF-10c "control failed: the parent used directly did not read the variable (rc=$PRC)"; fi

xrun xf11 '"output": {"file_output": {"report_json": "parent-report.json"}}' \
          '"output": {"file_output": {"report_json": "child-report.json"}}' child_wins replace "$P_SQL"
if [ "$CRC" -eq 0 ] && [ -s "$W/xf11/child/child-report.json" ]; then
    pass XF-11c "CONTROL: the child's own report path (not a security setting) is written"
else fail XF-11c "the child's report path was not used (rc=$CRC)" "$CO"; fi

echo ""
echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$UNM"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
