#!/usr/bin/env bash
# ============================================================
# test_privilege_escalation_patterns.sh — does the check catch what it claims? (A20)
#
# checkPrivilegeEscalation() builds four patterns from four config flags:
#
#     block_sudo        \bsudo\s
#     block_su          \bsu\s+-            <-- requires a hyphen
#     block_chmod_suid  chmod\s+[ugo]*s     <-- matches no real chmod
#     block_setuid      \bsetuid\b
#
# Two of the four do not do what their flag name says.
#
# block_su misses `su root` and bare `su`, because it insists on a hyphen. The
# hyphenated forms it does catch (su -, su -l, su -c) are the ones an operator
# is least likely to rely on as their only spelling.
#
# block_chmod_suid catches NOTHING. Read it against "chmod u+s": after the
# space, [ugo]* takes "u", then the pattern needs a literal "s" and finds "+";
# backtrack to empty, needs "s", finds "u". No match. The same failure holds for
# +s, ug+s, and every octal form (4755, 2755). The register recorded this as
# "octal SUID is missed"; measurement shows the symbolic forms the pattern was
# presumably written FOR are missed too. An inert sub-check, not a partial gap.
#
# WHY THE FALSE-POSITIVE ARMS MATTER AS MUCH AS THE GAPS. A privilege-escalation
# check that blocks `chmod 755` or the word `sudoku` gets disabled by whoever
# hits it, and then catches nothing at all. PE-20..23 pin that, and they pass
# BEFORE the fix -- they exist to keep the fix from buying coverage with noise.
#
# BOUNDED PATTERNS ONLY. tests/security/test_secret_scan_redos.sh exists because
# an unbounded `[\s\S]*` between two literals in SECRET_PATTERNS crashed the
# interpreter via stack exhaustion. Every pattern added here is a bounded
# character class followed by literals -- no `.*` between required text.
#
#   PE-01..02  sudo family still blocked (control: the check runs at all)
#   PE-03..05  hyphenated su still blocked (must not regress)
#   PE-06..07  THE FIX: `su root` and bare `su`
#   PE-10..14  THE FIX: chmod SUID, symbolic and octal
#   PE-15      setuid still blocked (control)
#   PE-20..23  NEGATIVE CONTROLS: benign commands and lookalike words
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

W="${TMPDIR:-/tmp}/naab-privesc-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
cat > "$W/govern.json" <<'JSON'
{ "version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
  "restrictions":{"privilege_escalation":{"enabled":true,"level":"hard"}},
  "capabilities":{"shell":{"enabled":true}} }
JSON

# ASSERT ON THE GOVERNANCE VERDICT, NOT ON WHETHER THE SHELL RAN.
#
# The first version of the negative controls required the command to EXECUTE
# (they looked for PE_RAN). That asks the wrong question. What is under test is
# "did the privilege-escalation check block this?", and the check is a STATIC
# scan that runs BEFORE execution -- so its verdict is available on a platform
# with no shell executor at all.
#
# On the Windows runner there is no shell executor (the same run reports
# "SKIPPED (no python/shell executor)" elsewhere), so every benign command came
# back `unmeasurable` and all four negative controls failed while PE-01..15
# passed. The fix was fine; the controls were measuring the environment.
#
# Absence of the block message is only meaningful if the scanner actually ran.
# THAT IS GUARDED BY THE THIRTEEN must_block ARMS, not by a marker inside this
# function. An earlier draft required "[governance] Loaded" in the captured
# output; that string is captured inside $( ) and never reaches the CI log, so
# it could not be verified cross-platform and would have silently turned every
# arm unmeasurable if it were ever absent.
#
# The suite as a whole is the calibration: if anything stopped the privilege
# scan from running, PE-01..15 fail loudly, and a reader knows the four negative
# arms are void. That is a stronger guarantee than a marker string, because it
# is asserted thirteen times against known-blocking input.
verdict() {
    printf 'main {\n  let r = <<shell\necho %s\n>>\n  print("PE_RAN")\n}\n' "$1" > "$W/t.naab"
    local o; o=$( cd "$W" && timeout 60 "$NAAB" t.naab 2>&1 )
    case "$o" in
        *"Privilege escalation"*) echo blocked ;;
        *)                        echo not-blocked ;;
    esac
}
must_block() {  # id, label, command
    local v; v=$(verdict "$3")
    [ "$v" = blocked ] && ok "$1" "$2" || bad "$1" "$2" "got '$v' for: $3"
}
must_run()   {  # id, label, command  -- negative control: must NOT be blocked
    local v; v=$(verdict "$3")
    [ "$v" = not-blocked ] && ok "$1" "$2" \
        || bad "$1" "$2" "got '$v' for: $3 — FALSE POSITIVE (see PE-01..15: if those passed, the scanner is live)"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Privilege escalation: does the check catch what it claims?  |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

echo -e "${CYAN}--- sudo (control: the check is live) ---${NC}"
must_block "PE-01" "sudo <cmd> is blocked"            'sudo rm -rf /'
must_block "PE-02" "sudo -u <user> is blocked"        'sudo -u root id'

echo -e "${CYAN}--- su, hyphenated forms (must not regress) ---${NC}"
must_block "PE-03" "su - <user> is blocked"           'su - root'
must_block "PE-04" "su -l is blocked"                 'su -l root'
must_block "PE-05" "su -c is blocked"                 'su -c id'

echo -e "${CYAN}--- su, the forms the hyphen requirement missed ---${NC}"
must_block "PE-06" "THE FIX: su <user> with no hyphen" 'su root'
must_block "PE-07" "THE FIX: bare su"                  'su'

echo -e "${CYAN}--- chmod SUID/SGID: the sub-check that caught nothing ---${NC}"
must_block "PE-10" "THE FIX: chmod u+s"                'chmod u+s /bin/sh'
must_block "PE-11" "THE FIX: chmod +s"                 'chmod +s /bin/sh'
must_block "PE-12" "THE FIX: chmod ug+s"               'chmod ug+s /bin/sh'
must_block "PE-13" "THE FIX: octal SUID chmod 4755"    'chmod 4755 /bin/sh'
must_block "PE-14" "THE FIX: octal SGID chmod 2755"    'chmod 2755 /bin/sh'

echo -e "${CYAN}--- setuid (control) ---${NC}"
must_block "PE-15" "setuid() is blocked"               'setuid(0)'

echo -e "${CYAN}--- NEGATIVE CONTROLS: a noisy check gets switched off ---${NC}"
must_run   "PE-20" "benign chmod 755 is NOT blocked"   'chmod 755 /tmp/x'
must_run   "PE-21" "benign chmod 644 is NOT blocked"   'chmod 644 /tmp/x'
must_run   "PE-22" "the word sudoku is NOT blocked"    'sudoku game'
must_run   "PE-23" "the word issue is NOT blocked"     'issue report'

echo ""
echo -e "${CYAN}--------------------------------------------------------------${NC}"
echo -e "  Passed: ${GREEN}${PASS}${NC}   Failed: ${RED}${FAIL}${NC}"
[ "$FAIL" -gt 0 ] && { echo -e "${RED}FAILURES:${NC}${FAILURES}"; echo ""; exit 1; }
echo -e "  ${GREEN}ALL PASSED${NC}"; echo ""; exit 0
