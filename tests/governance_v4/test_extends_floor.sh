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
# (179 loosened request verdicts in 1,918 pairs at seed 3, observed). The extends floor
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
#   XF-12   the sandbox level: a parent's `restricted` (no polyglot) holds in a
#           child that sets `elevated`, and XF-12b in one that sets no level --
#           the merge never read it, so neither was held (observed on master);
#           XF-12c CONTROL: a child stricter than its parent keeps its level
#   XF-13   codegen: a child cannot enable runtime code generation its parent
#           does not; XF-13c CONTROL: enabled in both, it runs
#   XF-14   a child's own per_language entry for a language does not drop the
#           parent's banned imports for it
#   XF-15   integrity.blocked_flags: a child's own list does not drop a flag
#           its parent locks
#   XF-10c  CONTROL: a child still tightens -- its own blocked_read entry is
#           refused beside the parent's
#   XF-11c  CONTROL: a child still sets what is not a security setting -- its
#           own report path is written
#
# Kill table (each mutant applied alone, rebuilt, this suite and
# naab_ratchet_property --extends 800 --seed 3 run; observed 2026-11-09):
#   applyExtendsFloor not called                   XF-01..XF-09 (+ the harness)
#   the path-hole step removed                     XF-05 (+ the harness)
#   meetAllowlist keeps the child's list           XF-03, XF-08 (+ the harness)
#   the language allow list keeps the child's      XF-06 (+ the harness)
#   the disjoint refusal removed                   XF-08
#   the parent's raw levels, not its effective     the harness only (scan.pii)
#   the model-output scan level not floored        the harness only (scan.model_secret)
#   the sandbox level not floored                  XF-12, XF-12b
#   integrity.blocked_flags not unioned            XF-15
#   codegen not floored                            XF-13
#   per_language banned lists not unioned          XF-14
# Red first (observed, master's binary): the 14 subject arms fail; the 8
# controls pass. (The binary is from 977e7a40; governance_config.cpp is
# identical between it and master 169d6600.)
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

# The sandbox level is the arm's subject, so these configs set their own
# security section instead of the header's elevated one.
xsbx() {
    local id="$1" plevel="$2" cbody="$3" prog="$4"
    local d="$W/$id"; mkdir -p "$d/base" "$d/child"
    printf '%s\n' "$prog" > "$d/base/p.naab"; cp "$d/base/p.naab" "$d/child/p.naab"
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "%s"}}\n' "$plevel" > "$d/base/govern.json"
    printf '{"version": "5.0", "mode": "enforce", "extends": "../base/govern.json"%s}\n' "${cbody:+, $cbody}" > "$d/child/govern.json"
    PRC=0; PO=$(cd "$d/base" && timeout 60 "$NAAB" p.naab 2>&1) || PRC=$?
    CRC=0; CO=$(cd "$d/child" && timeout 60 "$NAAB" p.naab 2>&1) || CRC=$?
}
# blocked_js ID DESC: the parent used directly refuses the javascript block
# (UNMEASURABLE otherwise), and so does the child.
blocked_js() {
    if [ "$PRC" -eq 0 ] || grep -q "JS=42" <<<"$PO"; then
        unm "$1" "control failed: the parent used directly ran the block (rc=$PRC)"; return
    fi
    if [ "$CRC" -ne 0 ] && ! grep -q "JS=42" <<<"$CO"; then pass "$1" "$2"
    else fail "$1" "$2 -- the child ran the block (rc=$CRC)" "$CO"; fi
}
xsbx xf12 restricted '"security": {"sandbox_level": "elevated"}' "$P_JS"
blocked_js XF-12 "a parent's restricted sandbox holds in a child that sets elevated"
xsbx xf12b restricted '' "$P_JS"
blocked_js XF-12b "a parent's restricted sandbox holds in a child that sets no level"
xsbx xf12c elevated '"security": {"sandbox_level": "restricted"}' "$P_JS"
if [ "$PRC" -eq 0 ] && grep -q "JS=42" <<<"$PO"; then
    if [ "$CRC" -ne 0 ] && ! grep -q "JS=42" <<<"$CO"; then
        pass XF-12c "CONTROL: a child stricter than its parent keeps its own level"
    else fail XF-12c "the child's restricted level did not apply (rc=$CRC)" "$CO"; fi
else unm XF-12c "control failed: the elevated parent did not run the block (rc=$PRC)"; fi

P_CG='use codegen
main {
    let r = codegen.run("javascript", "6 * 7")
    print("CG=" + string(r))
}'
xrun xf13 '' '"codegen": {"enabled": true}' child_wins replace "$P_CG"
if ! grep -q '"output"' <<<"$PO" && grep -qi 'not enabled' <<<"$PO"; then
    if ! grep -q '"output"' <<<"$CO" && grep -qi 'not enabled' <<<"$CO"; then
        pass XF-13 "a child cannot enable codegen its parent does not"
    else fail XF-13 "the child ran codegen its parent does not enable (rc=$CRC)" "$CO"; fi
else unm XF-13 "control failed: the parent used directly did not refuse codegen (rc=$PRC)"; fi
xrun xf13c '"codegen": {"enabled": true}' '"codegen": {"enabled": true}' child_wins replace "$P_CG"
ran_child XF-13c "CONTROL: codegen enabled in both runs in the child" '"output"'

P_JSIMP='main {
    let j = <<javascript
const f = require("fs")
1
>>
    print("JSIMP=" + string(j))
}'
xrun xf14 '"languages": {"per_language": {"javascript": {"imports": {"blocked": ["fs"]}}}}' \
          '"languages": {"per_language": {"javascript": {"max_lines": 100}}}' child_wins replace "$P_JSIMP"
if [ "$PRC" -eq 3 ] && grep -q 'Blocked import' <<<"$PO"; then
    if [ "$CRC" -eq 3 ] && grep -q 'Blocked import' <<<"$CO"; then
        pass XF-14 "a child's own per_language entry does not drop the parent's banned imports"
    else fail XF-14 "the child's per_language entry dropped the parent's banned import (rc=$CRC)" "$CO"; fi
else unm XF-14 "control failed: the parent used directly did not refuse the import (rc=$PRC)"; fi

P_PLAIN='main {
    print("PLAIN-OK")
}'
xrun xf15 '"integrity": {"blocked_flags": ["--governance-override"]}' \
          '"integrity": {"blocked_flags": ["--sandbox-level"]}' child_wins replace "$P_PLAIN"
PRC=0; PO=$(cd "$W/xf15/base" && timeout 60 "$NAAB" --governance-override p.naab 2>&1) || PRC=$?
CRC=0; CO=$(cd "$W/xf15/child" && timeout 60 "$NAAB" --governance-override p.naab 2>&1) || CRC=$?
if [ "$PRC" -eq 3 ] && ! grep -q PLAIN-OK <<<"$PO"; then
    if [ "$CRC" -eq 3 ] && ! grep -q PLAIN-OK <<<"$CO"; then
        pass XF-15 "a child's own blocked_flags does not drop a flag its parent locks"
    else fail XF-15 "the child ran with the flag its parent locks (rc=$CRC)" "$CO"; fi
else unm XF-15 "control failed: the parent used directly did not refuse the flag (rc=$PRC)"; fi

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
