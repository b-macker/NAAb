#!/usr/bin/env bash
# ============================================================
# test_inert_capability_keys.sh — capability knobs that enforce nothing (A16)
#
# Three keys are parsed (two of them RATCHETED) and emitted by
# `naab governance init`, and read by no check:
#
#   capabilities.process.spawn / signals / max_processes / allow_daemon
#   capabilities.filesystem.allow_hidden_files / allow_absolute_paths
#   capabilities.filesystem.blocked_extensions
#
# That is FALSE CONFIDENCE, not a new hole: the real controls exist and work.
# An operator who sets "spawn": false believes process execution is blocked, and
# so never sets capabilities.shell.enabled, which actually blocks it. The engine
# now says so at load.
#
# THE WARNING MUST BE TRUE, not merely present. This mirrors LD-08 in
# test_inert_limits.sh: for every key the engine calls inert, prove the inert key
# does NOT block while a live key on the SAME file DOES. Without the paired
# control, "inert" could mean the probe was broken and every arm would agree with
# it. If someone later wires one of these keys up, its IK arm fails and the
# warning stops being a lie -- which is the point of pinning it.
#
#   IC-01..03  the three fs keys are inert, each against a blocked_paths control
#   IC-04      capabilities.process.* is inert, against a shell.enabled control
#   IC-05..06  the warnings actually fire for restrictive values
#   IC-07      NEGATIVE CONTROL: permissive defaults warn about NOTHING -- every
#              generated config sets these, so warning there would train
#              operators to ignore the channel
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
source "$SCRIPT_DIR/../helpers/native_path.sh"
setup_isolated_trust

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; FAILURES=""
ok()  { PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }

W="${TMPDIR:-/tmp}/naab-inertcap-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
echo "HIDDEN_DATA" > "$W/.hidden_secret"
echo "PLAIN_DATA"  > "$W/plain.txt"

# $1 = capabilities object  $2 = program body -> ran|blocked|sandbox-refused|...
# Four outcomes: a governance block and a sandbox refusal are different events
# with different causes, and collapsing them lets one masquerade as the other.
run() {
    cat > "$W/govern.json" <<JSON
{ "version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
  "capabilities": $1 }
JSON
    printf '%s\n' "$2" > "$W/t.naab"
    local o; o=$( cd "$W" && timeout 60 "$NAAB" t.naab 2>&1 )
    case "$o" in
        *INTEGRITY*)               echo "CONFOUNDED-trust" ;;
        *READ_OK*|*RAN*)           echo ran ;;
        *"blocked by governance"*) echo blocked ;;
        *"SANDBOX VIOLATION"*)     echo sandbox-refused ;;
        *)                         echo unmeasurable ;;
    esac
}
warns() {  # $1 = capabilities object -> count of inert-key warnings
    cat > "$W/govern.json" <<JSON
{ "version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
  "capabilities": $1 }
JSON
    printf 'main { print("OK") }\n' > "$W/t.naab"
    ( cd "$W" && timeout 60 "$NAAB" t.naab 2>&1 ) | grep -c "not enforced" || true
}

# PATH VOCABULARY. The first version of this suite wrote absolute "$W/..." paths
# into the program. naab-lang under MSYS2 is a NATIVE build and cannot open an
# MSYS /tmp/... path, so IC-01..03 failed on build-windows while passing on
# Linux -- the same defect fixed in test_relative_path_base.sh EARLIER THE SAME
# DAY, reintroduced here. Hence tests/helpers/native_path.sh.
#
# IC-01 and IC-03 use RELATIVE paths: run() cd's into $W, the blocked_paths
# entries are already relative, and a relative name goes through NAAb's own
# canonicaliser on both sides. That is strictly more robust than converting.
#
# IC-02 is the exception and MUST stay absolute -- it tests allow_absolute_paths,
# and a relative path would not test the thing the arm names. So it converts.
ABS_PLAIN=$(native_path "$W/plain.txt")

READ_HIDDEN='use file
main { let x = file.read(".hidden_secret") print("READ_OK") }'
READ_PLAIN='use file
main { let x = file.read("plain.txt") print("READ_OK") }'
READ_PLAIN_ABS='use file
main { let x = file.read("'"$ABS_PLAIN"'") print("READ_OK") }'
DO_EXEC='use process
main { let r = process.run("echo", ["hi"]) print("RAN") }'

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Inert capability keys: parsed, ratcheted, enforcing nothing |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# --- the keys are inert, each paired with a control on the SAME file ---
pair() {  # $1=id $2=label $3=inert-caps $4=control-caps $5=program
    local p c
    p=$(run "$3" "$5"); c=$(run "$4" "$5")
    if [ "$p" = ran ] && [ "$c" = blocked ]; then
        ok "$1" "$2"
    elif [ "$c" != blocked ]; then
        bad "$1" "$2" "CONTROL did not block (got '$c') — the probe proves nothing"
    else
        bad "$1" "$2" "key now BLOCKS (got '$p') — it is live; delete the warning and this arm"
    fi
}

pair "IC-01" "allow_hidden_files:false is inert (blocked_paths is not)" \
     '{"filesystem":{"mode":"write","allow_hidden_files":false}}' \
     '{"filesystem":{"mode":"write","blocked_paths":[".hidden_secret"]}}' \
     "$READ_HIDDEN"

pair "IC-02" "allow_absolute_paths:false is inert (blocked_paths is not)" \
     '{"filesystem":{"mode":"write","allow_absolute_paths":false}}' \
     '{"filesystem":{"mode":"write","blocked_paths":["plain.txt"]}}' \
     "$READ_PLAIN_ABS"

pair "IC-03" "blocked_extensions is inert (blocked_paths is not)" \
     '{"filesystem":{"mode":"write","blocked_extensions":[".txt"]}}' \
     '{"filesystem":{"mode":"write","blocked_paths":["plain.txt"]}}' \
     "$READ_PLAIN"

# process.* uses a different control: shell.enabled, the gate that really fires.
p=$(run '{"shell":{"enabled":true},"process":{"spawn":false,"max_processes":0}}' "$DO_EXEC")
c=$(run '{"shell":{"enabled":false},"process":{"spawn":true}}' "$DO_EXEC")
if [ "$p" = ran ] && [ "$c" = sandbox-refused ]; then
    ok "IC-04" "capabilities.process.* is inert (shell.enabled is the real gate)"
elif [ "$c" != sandbox-refused ]; then
    bad "IC-04" "capabilities.process.* is inert (shell.enabled is the real gate)" \
        "CONTROL did not refuse (got '$c') — the probe proves nothing"
else
    bad "IC-04" "capabilities.process.* is inert (shell.enabled is the real gate)" \
        "process.* now BLOCKS (got '$p') — it is live; delete the warning and this arm"
fi

# --- the warnings fire ---
n=$(warns '{"filesystem":{"mode":"read","allow_hidden_files":false,"allow_absolute_paths":false,"blocked_extensions":[".exe"]}}')
[ "${n:-0}" -ge 3 ] \
  && ok "IC-05" "all three filesystem keys warn when set restrictively ($n)" \
  || bad "IC-05" "all three filesystem keys warn when set restrictively" "got $n warnings, expected >= 3"

n=$(warns '{"process":{"spawn":false,"signals":false,"max_processes":5,"allow_daemon":false}}')
[ "${n:-0}" -ge 4 ] \
  && ok "IC-06" "each capabilities.process sub-key warns ($n)" \
  || bad "IC-06" "each capabilities.process sub-key warns" "got $n warnings, expected >= 4"

# --- the control that keeps the channel worth reading ---
n=$(warns '{"filesystem":{"mode":"read","allow_hidden_files":true,"allow_absolute_paths":true,"blocked_extensions":[]}}')
[ "${n:-0}" -eq 0 ] \
  && ok "IC-07" "NEGATIVE CONTROL: permissive defaults warn about nothing" \
  || bad "IC-07" "NEGATIVE CONTROL: permissive defaults warn about nothing" \
         "got $n warnings — every generated config sets these; this trains operators to ignore the channel"

echo ""
echo -e "${CYAN}--------------------------------------------------------------${NC}"
echo -e "  Passed: ${GREEN}${PASS}${NC}   Failed: ${RED}${FAIL}${NC}"
[ "$FAIL" -gt 0 ] && { echo -e "${RED}FAILURES:${NC}${FAILURES}"; echo ""; exit 1; }
echo -e "  ${GREEN}ALL PASSED${NC}"; echo ""; exit 0
