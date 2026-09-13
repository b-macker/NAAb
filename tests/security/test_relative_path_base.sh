#!/usr/bin/env bash
# ============================================================
# test_relative_path_base.sh — a relative path entry resolved against the
# PROCESS CWD, so the same config gave different verdicts for the same file
#
# THE DEFECT, and the direction matters
#
# checkPathAccess() canonicalised each govern.json path entry with
# std::filesystem::weakly_canonical(), which resolves a RELATIVE path against
# the process working directory. A project config saying
#
#     "blocked_paths": ["shared"]
#
# therefore meant "./shared relative to wherever naab-lang was started", not
# "the shared directory in this project". Measured on 2785654, identical
# config and an ABSOLUTE target path so both arms address the same bytes:
#
#     cwd = project dir   exit 3, blocked
#     cwd = /tmp          exit 0, READ:SECRET_DATA
#
# It FAILS OPEN. A reviewer running the suite from the project root sees the
# block and concludes the policy works; the same policy is absent for a
# service started from /, a CI runner with a different workdir, or a cron job.
# That is the worst direction for a path policy, and it is invisible from the
# place people test.
#
# THE FIX. Resolve a relative entry against govern_json_dir_ -- the directory
# the config was loaded from, which is the project root and the only stable
# base available. Inline configs keep the old behaviour: govern_json_dir_ is
# empty there and there is no project to be relative to.
#
#   RP-01  POSITIVE CONTROL: from the project dir, a relative blocked_paths
#          entry blocks. This passed BEFORE the fix -- it is the arm that
#          proves the policy fires at all
#   RP-02  THE FIX: the same config blocks the same file from a DIFFERENT cwd
#   RP-03  the two arms AGREE. Stated separately so a change that merely
#          starts blocking everything cannot pass as a fix
#   RP-04  NEGATIVE CONTROL: a file NOT under the blocked entry is still
#          readable from both cwds. Without this, "deny everything" passes
#          RP-01 through RP-03
#   RP-05  NEGATIVE CONTROL: an ABSOLUTE blocked_paths entry still works from
#          both cwds — the fix must not disturb the form that was never broken
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

P="${TMPDIR:-/tmp}/naab-relpath-$$"
OTHER="${TMPDIR:-/tmp}/naab-relpath-other-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$P" "$OTHER"; }
trap cleanup EXIT
mkdir -p "$P/shared" "$P/public" "$OTHER"
echo "SECRET_DATA" > "$P/shared/secret.txt"
echo "PUBLIC_DATA" > "$P/public/open.txt"

# $1 = blocked_paths JSON fragment
write_cfg() {
    cat > "$P/govern.json" <<JSON_EOF
{ "version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
  "capabilities":{"filesystem":{"mode":"write","blocked_paths":[$1]}}}
JSON_EOF
}
# $1 = absolute file to read -> writes the program
write_prog() {
    printf 'use file\nmain { let x = file.read("%s") print("READ:" + x) }\n' "$1" > "$P/t.naab"
}
# $1 = cwd -> ran|blocked
from() {
    local o
    o=$(cd "$1" && timeout 30s "$NAAB" "$P/t.naab" 2>&1)
    case "$o" in *READ:*) echo ran ;; *) echo blocked ;; esac
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  relative path entries resolved against the process CWD       |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

write_cfg '"shared"'
write_prog "$P/shared/secret.txt"
A=$(from "$P"); B=$(from "$OTHER")
echo "  relative entry \"shared\", absolute target:  project-cwd=$A  other-cwd=$B"
echo ""

[ "$A" = blocked ] && ok "RP-01" "POSITIVE CONTROL: blocks from the project dir" \
  || bad "RP-01" "POSITIVE CONTROL: blocks from the project dir" \
         "got '$A' — the policy is not firing; every arm below is void"

[ "$B" = blocked ] && ok "RP-02" "THE FIX: blocks from a different cwd" \
  || bad "RP-02" "THE FIX: blocks from a different cwd" \
         "got '$B' — FAILS OPEN: the same config read the same file when started elsewhere"

[ "$A" = "$B" ] && ok "RP-03" "the verdict does not depend on the process cwd" \
  || bad "RP-03" "the verdict does not depend on the process cwd" "project-cwd=$A other-cwd=$B"

write_prog "$P/public/open.txt"
A=$(from "$P"); B=$(from "$OTHER")
if [ "$A" = ran ] && [ "$B" = ran ]; then
    ok "RP-04" "NEGATIVE CONTROL: an unblocked file is readable from both cwds"
else
    bad "RP-04" "NEGATIVE CONTROL: an unblocked file is readable from both cwds" \
        "project-cwd=$A other-cwd=$B — denying everything would pass RP-01..03"
fi

write_cfg "\"$P/shared\""
write_prog "$P/shared/secret.txt"
A=$(from "$P"); B=$(from "$OTHER")
if [ "$A" = blocked ] && [ "$B" = blocked ]; then
    ok "RP-05" "NEGATIVE CONTROL: an absolute entry still works from both cwds"
else
    bad "RP-05" "NEGATIVE CONTROL: an absolute entry still works from both cwds" \
        "project-cwd=$A other-cwd=$B — the fix disturbed the form that was never broken"
fi

echo ""
echo -e "${CYAN}--------------------------------------------------------------${NC}"
echo -e "  Passed: ${GREEN}${PASS}${NC}   Failed: ${RED}${FAIL}${NC}"
[ "$FAIL" -gt 0 ] && { echo -e "${RED}FAILURES:${NC}${FAILURES}"; echo ""; exit 1; }
echo -e "  ${GREEN}ALL PASSED${NC}"; echo ""; exit 0
