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
#
# TWO PLATFORM LESSONS, both learned the hard way on this suite's own CI.
#
# 1. THE VERDICT IS THREE-VALUED, not two. from() used to print "ran" when it
#    saw READ: and "blocked" for EVERYTHING else — so "the binary could not
#    open the file at all" was redistributed into the blocked bucket, which is
#    the PASS verdict for three of the five arms. On Windows that is exactly
#    what happened: RP-01, RP-02 and RP-03 passed vacuously while the file was
#    never read, and RP-04 (the only arm expecting a successful read) was the
#    single thing standing between that and a green run. A probe needs a third
#    outcome, and "unmeasurable" must fail loudly rather than resolve to
#    either verdict.
#
# 2. AN ABSOLUTE PATH IS IN THE SHELL'S VOCABULARY. Under MSYS2 the shell says
#    /tmp/xxx and naab-lang.exe is a NATIVE build that cannot open it. The
#    sibling suite test_path_precedence.sh dodges this by making every path
#    relative; that is NOT available here, because a relative TARGET resolves
#    differently per cwd and the whole point of RP-01..03 is that both arms
#    address the same bytes. So the paths handed to the binary are converted
#    with cygpath, and RP-00 checks up front that the binary can actually
#    reach the fixture — a usability check on the instrument, separate from
#    the result it reports.
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
# Paths the NATIVE binary will see. Identity everywhere except MSYS2, where
# the shell's /tmp/xxx is meaningless to naab-lang.exe. cygpath -m yields
# C:/... with forward slashes, which survives being pasted into a NAAb string
# literal; -w would yield backslashes and be read as escapes.
native() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi
}
NP=$(native "$P")

# $1 = absolute file to read (native vocabulary) -> writes the program
write_prog() {
    printf 'use file\nmain { let x = file.read("%s") print("READ:" + x) }\n' "$1" > "$P/t.naab"
}
# $1 = cwd -> ran|blocked|unmeasurable
#
# Three outcomes on purpose. Classifying anything-that-is-not-READ as "blocked"
# makes an unreadable fixture look like a working policy, and "blocked" is the
# expected value for three of the five arms — so the instrument failing reads
# as the system passing. Governance blocks and open failures are distinguished
# by their own messages rather than by exit code, since both are non-zero.
from() {
    local o
    o=$(cd "$1" && timeout 30s "$NAAB" "$NP/t.naab" 2>&1)
    case "$o" in
        *READ:*)                     echo ran ;;
        *"blocked by governance"*)   echo blocked ;;
        # Everything else is UNMEASURABLE, and the catch-all defaulting to
        # "blocked" is the same redistribution one level down: a script the
        # binary cannot even locate is not evidence that a policy fired. With
        # the catch-all set to "blocked", a simulated vocabulary mismatch left
        # RP-01..03 passing vacuously even after RP-00 was added.
        *)                           echo unmeasurable ;;
    esac
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  relative path entries resolved against the process CWD       |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# RP-00 -- CAN THE BINARY REACH THE FIXTURE AT ALL? Runs with no blocked_paths,
# so the only thing it can report is whether the program text, the script path
# and the file all resolve for the native binary. If this fails every arm below
# is void, and saying so beats three vacuous passes.
write_cfg ''
write_prog "$NP/public/open.txt"
REACH=$(from "$P")
if [ "$REACH" = ran ]; then
    ok "RP-00" "the binary can open the fixture (paths are in its vocabulary)"
else
    bad "RP-00" "the binary can open the fixture (paths are in its vocabulary)" \
        "got '$REACH' with an EMPTY blocked_paths — measuring path vocabulary, not path policy; every arm below is void"
fi

write_cfg '"shared"'
write_prog "$NP/shared/secret.txt"
A=$(from "$P"); B=$(from "$OTHER")
echo "  relative entry \"shared\", absolute target:  project-cwd=$A  other-cwd=$B"
echo ""

[ "$A" = blocked ] && ok "RP-01" "POSITIVE CONTROL: blocks from the project dir" \
  || bad "RP-01" "POSITIVE CONTROL: blocks from the project dir" \
         "got '$A' — the policy is not firing; every arm below is void"

if [ "$B" = blocked ]; then
    ok "RP-02" "THE FIX: blocks from a different cwd"
elif [ "$B" = ran ]; then
    bad "RP-02" "THE FIX: blocks from a different cwd" \
        "got 'ran' — FAILS OPEN: the same config read the same file when started elsewhere"
else
    bad "RP-02" "THE FIX: blocks from a different cwd" \
        "got '$B' — not a verdict: the probe could not reach the file, so this says nothing about policy"
fi

if [ "$A" = unmeasurable ] || [ "$B" = unmeasurable ]; then
    bad "RP-03" "the verdict does not depend on the process cwd" \
        "project-cwd=$A other-cwd=$B — two unmeasurable runs agree about nothing"
elif [ "$A" = "$B" ]; then
    ok "RP-03" "the verdict does not depend on the process cwd"
else
    bad "RP-03" "the verdict does not depend on the process cwd" "project-cwd=$A other-cwd=$B"
fi

write_prog "$NP/public/open.txt"
A=$(from "$P"); B=$(from "$OTHER")
if [ "$A" = ran ] && [ "$B" = ran ]; then
    ok "RP-04" "NEGATIVE CONTROL: an unblocked file is readable from both cwds"
else
    bad "RP-04" "NEGATIVE CONTROL: an unblocked file is readable from both cwds" \
        "project-cwd=$A other-cwd=$B — denying everything would pass RP-01..03"
fi

write_cfg "\"$NP/shared\""
write_prog "$NP/shared/secret.txt"
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
