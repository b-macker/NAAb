#!/usr/bin/env bash
# ============================================================
# test_schema_validator_noise.sh — the unknown-key warning must not cry wolf
#
# THE FAILURE THIS CATCHES
#
# checkSchemaValidation() warned on every top-level key outside VALID_TOP_KEYS,
# and got two whole classes of CORRECT config wrong:
#
#   1. "_comment_*" annotations. JSON has no comment syntax, so a sibling key
#      is the only way to annotate a config -- and both copies of
#      govern-template.json document every section that way. Measured: 55 of
#      the template's 69 load warnings were noise about its OWN COMMENTS.
#
#   2. "update_reason". It is read in GovernanceEngine::reloadIfChanged() and
#      carried in the CONFIG_ADJUSTMENT telemetry event -- the operator's note
#      on why a mid-run change was made. It was simply missing from the list,
#      so using it correctly earned a warning at every startup.
#
# Neither is a security defect. The cost is that a channel which cries wolf is
# one operators learn to ignore -- and A16 had just started using that same
# channel to report keys that genuinely enforce nothing.
#
# "update_reason" was the ONLY parsed root key missing from the list, and that
# is enumerated rather than noticed: every j.contains() on the root object,
# diffed against VALID_TOP_KEYS. SV-06 pins that so the next added key cannot
# reintroduce the gap silently.
#
#   SV-01  a _comment_* key does not warn
#   SV-02  POSITIVE CONTROL: a genuinely unknown key STILL warns. Without it,
#          a validator that skipped everything would pass SV-01/03/05.
#   SV-03  update_reason does not warn
#   SV-04  POSITIVE CONTROL: a near-miss still warns AND still suggests, so the
#          fix did not cost the typo-detection the check exists for
#   SV-05  a real section does not warn
#   SV-06  every top-level key the loader parses is in VALID_TOP_KEYS
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; FAILURES=""
ok()  { PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }

W="${TMPDIR:-/tmp}/naab-svnoise-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }

printf 'main { print("hi") }\n' > "$W/t.naab"

# python writes to stdout, the shell redirects -- see test_shell_path_handoff.sh
warns_for() {  # $1 = extra top-level key, $2 = value
    python3 -c "
import json,sys
d={'version':'1.0','mode':'enforce'}
d[sys.argv[1]]=json.loads(sys.argv[2])
json.dump(d, sys.stdout)
" "$1" "$2" > "$W/govern.json"
    ( cd "$W" && timeout 60 "$NAAB" t.naab 2>&1 ) | grep -c "Unknown key \"$1\"" || true
}

echo -e "${CYAN}=== Group SV: unknown-key warning accuracy ===${NC}"

[ "$(warns_for _comment_capabilities '"why this section exists"')" = "0" ] \
  && ok  "SV-01" "a _comment_* annotation does not warn" \
  || bad "SV-01" "a _comment_* annotation must not warn"

[ "$(warns_for totally_bogus_section '{"x":1}')" = "1" ] \
  && ok  "SV-02" "POSITIVE CONTROL: a genuinely unknown key still warns" \
  || bad "SV-02" "an unknown key must still warn" "the check has stopped checking"

[ "$(warns_for update_reason '"tightening shell after incident"')" = "0" ] \
  && ok  "SV-03" "update_reason does not warn (parsed by reloadIfChanged)" \
  || bad "SV-03" "update_reason is a working key and must not warn"

# A near-miss must still be caught AND still suggest the intended key.
python3 -c "
import json,sys
json.dump({'version':'1.0','mode':'enforce','capabilites':{}}, sys.stdout)
" > "$W/govern.json"
out=$( cd "$W" && timeout 60 "$NAAB" t.naab 2>&1 )
case "$out" in
    *'Unknown key "capabilites" — did you mean "capabilities"?'*)
        ok "SV-04" "POSITIVE CONTROL: a near-miss still warns and still suggests" ;;
    *'Unknown key "capabilites"'*)
        bad "SV-04" "near-miss warns but lost its suggestion" "suggestion missing" ;;
    *)  bad "SV-04" "a near-miss must still warn" "no warning at all" ;;
esac

[ "$(warns_for scanner '{"enabled":false}')" = "0" ] \
  && ok  "SV-05" "a real section does not warn" \
  || bad "SV-05" "a valid section must not warn"

# SV-06: enumerate from the SYSTEM. Every key read off the root json object must
# be in VALID_TOP_KEYS, or using it correctly earns a warning -- which is exactly
# how update_reason went unnoticed.
parsed=$(grep -oE '\bj\.contains\("[a-z_0-9]+"\)' "$REPO/src/runtime/governance_config.cpp" \
         | grep -oE '"[a-z_0-9]+"' | tr -d '"' | sort -u)
valid=$(sed -n '/VALID_TOP_KEYS = {/,/^    };/p' "$REPO/src/runtime/governance_checks.cpp" \
         | grep -oE '"[a-z_0-9]+"' | tr -d '"' | sort -u)
missing=$(comm -23 <(echo "$parsed") <(echo "$valid") | tr '\n' ' ' | sed 's/ *$//')
nparsed=$(echo "$parsed" | grep -c .)
if [ "$nparsed" -lt 20 ]; then
    bad "SV-06" "enumeration is broken, not passing" "only $nparsed root keys found — the grep stopped matching"
elif [ -z "$missing" ]; then
    ok "SV-06" "all $nparsed parsed root keys are in VALID_TOP_KEYS"
else
    bad "SV-06" "parsed root keys missing from VALID_TOP_KEYS" "$missing"
fi

echo
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}=== Results: $PASS passed, 0 failed ===${NC}"; exit 0
else
    echo -e "${RED}=== Results: $PASS passed, $FAIL failed ===${NC}"; echo -e "$FAILURES"; exit 1
fi
