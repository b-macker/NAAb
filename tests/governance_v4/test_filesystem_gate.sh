#!/usr/bin/env bash
# ============================================================
# test_filesystem_gate.sh — csv and log were not behind the filesystem gate
#
# THE DEFECT
#
# Both engines gated mod=="file" and neither gated csv or log, so
# capabilities.filesystem.mode:"none" and an allowed_actions matrix without
# FS_WRITE were both bypassable. Measured: csv.write("govern.json") exited 0 and
# OVERWROTE the file in the same process where file.write("govern.json") was
# blocked at exit 3. csv_impl.cpp opens ifstream/ofstream directly and
# log_impl.cpp opens an ofstream on the first write after set_output, so the
# sandbox was the only thing in the path — and the sandbox does not carry the
# governance policy. Under --sandbox-level on the CLI it does not even carry the
# capability erasure, because that is applied only when the level changes.
#
# THIS IS NOT SHIPPED BEHIND observe MODE, unlike the BSD repairs, and the
# distinction is deliberate. Those enabled detections the operator never had.
# This enforces a restriction the operator explicitly WROTE: a config saying
# filesystem.mode "none" already asked for filesystem writes to stop. Closing
# the leak delivers the stated policy rather than adding a new one. Programs
# relying on the gap were relying on a defect. That is still a behaviour change
# and is called out in the commit, not buried here.
#
# Classification lives in GovernanceEngine::filesystemAccessMode(), shared by
# vm.cpp and call_dispatch.cpp — they are independent implementations of the
# same gate, and this gap existed because the module list was written twice and
# only one copy was ever tested.
#
#   FG-01  csv.write is BLOCKED under filesystem.mode "none"
#   FG-02  PERMIT CONTROL: the same call SUCCEEDS with no restriction, and the
#          file appears on disk. Without this, FG-01 passes on a build that
#          blocks csv unconditionally
#   FG-03  both directions hold under --tree-walk too (the gap was on both
#          engines; fixing one would leave the other open)
#   FG-04  csv.read is blocked under "none" and permitted under "read" — the
#          read/write split is real, not a blanket deny
#   FG-05  log.set_output to a FILE is gated, and log.set_output("stderr")
#          still works. The second half is the load-bearing one: stderr and
#          stdout name no file and must not be caught by the gate
#   FG-06  govern.json self-protection now holds against csv, under the exact
#          conditions that reproduced the bypass (--sandbox-level on the CLI)
#   FG-07  the allowed_actions matrix gates csv as well, with its permit case
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/naab-fs-gate-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
cleanup() { rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"
W="$TEST_TMP/w"; mkdir -p "$W"

# $1=config json  $2=program  $3=extra CLI args -> "<rc>|<marker seen>"
run() {
    printf '%s\n' "$1" > "$W/govern.json"
    printf '%s\n' "$2" > "$W/t.naab"
    rm -f "$W/out.csv" "$W/app.log"
    local o rc
    o=$(cd "$W" && timeout 60s "$NAAB" ${3:-} t.naab 2>&1); rc=$?
    if echo "$o" | grep -q "MARKER"; then echo "$rc|ran"; else echo "$rc|blocked"; fi
}

DENY='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "none" } } }'
READONLY='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "read" } } }'
OPEN='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" } }'

P_WRITE='use csv
main { csv.write("out.csv", [["x"]]) print("MARKER") }'
P_READ='use csv
main { let r = csv.read("seed.csv") print("MARKER") }'

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  csv and log were outside the filesystem gate                 |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""
echo -e "${CYAN}--- FG-01..03: csv.write, both directions, both engines ---${NC}"

R_DENY="$(run "$DENY" "$P_WRITE" "")"
if [ "${R_DENY#*|}" = "blocked" ] && [ "${R_DENY%%|*}" = "3" ]; then
    pass "FG-01" "csv.write blocked under filesystem.mode \"none\" (exit 3)"
else
    fail "FG-01" "csv.write not blocked" "got rc=${R_DENY%%|*} ${R_DENY#*|}"
fi

R_OPEN="$(run "$OPEN" "$P_WRITE" "")"
if [ "${R_OPEN#*|}" = "ran" ] && [ -f "$W/out.csv" ]; then
    pass "FG-02" "PERMIT CONTROL: unrestricted csv.write succeeds and writes the file"
else
    fail "FG-02" "csv.write blocked when it should be permitted" "got rc=${R_OPEN%%|*} ${R_OPEN#*|}; FG-01 would pass on a blanket deny"
fi

R_DENY_TW="$(run "$DENY" "$P_WRITE" "--tree-walk")"
R_OPEN_TW="$(run "$OPEN" "$P_WRITE" "--tree-walk")"
if [ "${R_DENY_TW#*|}" = "blocked" ] && [ "${R_OPEN_TW#*|}" = "ran" ]; then
    pass "FG-03" "--tree-walk enforces and permits identically to the VM"
else
    fail "FG-03" "engine parity broken" "deny=${R_DENY_TW} open=${R_OPEN_TW}"
fi

echo -e "${CYAN}--- FG-04: the read/write split is real ---${NC}"
printf 'a,b\n1,2\n' > "$W/seed.csv"
R_RD_DENY="$(run "$DENY" "$P_READ" "")"
R_RD_OK="$(run "$READONLY" "$P_READ" "")"
if [ "${R_RD_DENY#*|}" = "blocked" ] && [ "${R_RD_OK#*|}" = "ran" ]; then
    pass "FG-04" "csv.read blocked under \"none\", permitted under \"read\""
else
    fail "FG-04" "read/write split wrong" "none=${R_RD_DENY} read=${R_RD_OK}"
fi

echo -e "${CYAN}--- FG-05: log, and the sinks that are not files ---${NC}"
P_LOGFILE='use log
main { log.set_output("app.log") log.info("hi") print("MARKER") }'
P_LOGERR='use log
main { log.set_output("stderr") log.info("hi") print("MARKER") }'
R_LF="$(run "$DENY" "$P_LOGFILE" "")"
R_LE="$(run "$DENY" "$P_LOGERR" "")"
if [ "${R_LF#*|}" = "blocked" ] && [ "${R_LE#*|}" = "ran" ]; then
    pass "FG-05" "log to a file is gated; log to stderr is not"
else
    fail "FG-05" "log gating wrong" "file=${R_LF} stderr=${R_LE} — gating stderr would break ordinary logging"
fi

echo -e "${CYAN}--- FG-06: govern.json self-protection against csv ---${NC}"
printf '%s\n' "$DENY" > "$W/govern.json"
cp "$W/govern.json" "$W/govern.expected"
printf 'use csv\nmain { csv.write("govern.json", [["PWNED"]]) print("MARKER") }\n' > "$W/t.naab"
(cd "$W" && timeout 60s "$NAAB" --sandbox-level elevated t.naab >/dev/null 2>&1) || true
if diff -q "$W/govern.json" "$W/govern.expected" >/dev/null 2>&1; then
    pass "FG-06" "csv.write cannot overwrite govern.json (--sandbox-level path)"
else
    fail "FG-06" "govern.json was overwritten by csv.write" "the self-protection bypass is back"
fi

echo -e "${CYAN}--- FG-07: the allowed_actions matrix gates csv too ---${NC}"
NOFS='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "agents": { "w": { "provider": "gemini", "model": "m", "api_key_env": "K",
      "allowed_actions": ["SHELL_EXEC"] } } }'
WITHFS='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "agents": { "w": { "provider": "gemini", "model": "m", "api_key_env": "K",
      "allowed_actions": ["SHELL_EXEC", "FS_WRITE"] } } }'
printf '%s\n' "$NOFS" > "$W/govern.json"; printf '%s\n' "$P_WRITE" > "$W/t.naab"; rm -f "$W/out.csv"
(cd "$W" && timeout 60s "$NAAB" --agent-id w t.naab >/dev/null 2>&1) || true
NOFS_WROTE=$([ -f "$W/out.csv" ] && echo yes || echo no)
printf '%s\n' "$WITHFS" > "$W/govern.json"; rm -f "$W/out.csv"
(cd "$W" && timeout 60s "$NAAB" --agent-id w t.naab >/dev/null 2>&1) || true
WITHFS_WROTE=$([ -f "$W/out.csv" ] && echo yes || echo no)
if [ "$NOFS_WROTE" = "no" ] && [ "$WITHFS_WROTE" = "yes" ]; then
    pass "FG-07" "csv obeys allowed_actions (blocked without FS_WRITE, permitted with)"
else
    fail "FG-07" "action matrix does not gate csv correctly" "without FS_WRITE wrote=$NOFS_WROTE, with FS_WRITE wrote=$WITHFS_WROTE"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "  Passed: ${GREEN}$PASS_COUNT${NC}  Failed: ${RED}$FAIL_COUNT${NC}  Skipped: ${YELLOW}$SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}FAILURES:${NC}$FAILURES"
    echo -e "${CYAN}+==============================================================+${NC}"
    exit 1
fi
echo -e "${GREEN}ALL PASSED${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
exit 0
