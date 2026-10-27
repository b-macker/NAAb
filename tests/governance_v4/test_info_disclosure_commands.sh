#!/usr/bin/env bash
# ============================================================
# test_info_disclosure_commands.sh -- process listing and system information
# under restrictions.information_disclosure, on both doors (A17 F8a, Bundle 3)
#
# block_process_listing held two source patterns, `ps aux` and `ps -ef`, so
# `ps -A`, `ps -e` and `ps ax` passed a <<shell>> block (A17 F8a, traced), and
# process.run("ps", ["aux"]) never passed through the scan at all. The listing
# forms are now every option that selects all processes (BSD a/x in the first
# argument, UNIX -e/-A/-a/-x in any), found by one scan of the block's source
# and of process.run's command line; `ps -p PID` still passes.
# block_system_info_leak (uname -a, cat /etc/passwd) shares the test.
#
#   IL-00  CONTROL: <<shell>> `ps aux` is refused under the restriction
#   IL-01  <<shell>> `ps -A` is refused (a form the old table missed)
#   IL-02  process.run("ps", ["aux"]) is refused (exit 3) and prints no listing
#   IL-03  process.run("ps", ["-A"]) is refused
#   IL-04  process.run("/bin/ps", ["-e"]) is refused (a path, not a bare name)
#   IL-05  CONTROL: process.run("ps", ["-p", PID]) runs -- one process, not a listing
#   IL-06  CONTROL: with the restriction off, process.run("ps", ["aux"]) runs
#   IL-07  process.run("uname", ["-a"]) is refused (block_system_info_leak)
#   IL-08  CONTROL: block_process_listing false runs `ps aux` while uname -a,
#          its own flag on, is still refused
#   IL-09  process.run("ps", ["-o", "pid", "-e"]) is refused (the listing
#          option after another option)
#   IL-10  CONTROL: a 64 KB argument after `ps -p 1` runs -- the scan neither
#          crashes on long input (a candidate regex did) nor calls it a listing
#
# Kill table (each mutant rebuilt, this suite run):
#   the checkInfoDisclosureCommand() call removed from process.run  IL-02/03/04/07/08/09
#   the listing test back to the regexes `ps aux` / `ps -ef`         IL-01, IL-03, IL-04, IL-09
#   the UNIX-option test limited to the first argument              IL-09
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

PASS=0; FAIL=0; UNM=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -6 | sed 's/^/       | /'; }
unm() { UNM=$((UNM+1)); echo "  UNMEASURABLE [$1] $2"; }

echo "=== information disclosure: process listing and system info ==="
if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2; exit 1
fi
PS_BIN="$(command -v ps)"; UNAME_BIN="$(command -v uname)"
if [ -z "$PS_BIN" ] || [ -z "$UNAME_BIN" ]; then
    echo "  UNMEASURABLE [IL] ps or uname not installed"; exit 0
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-infodisc-XXXXXX")"
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

# run NAME RESTRICTION_JSON PROGRAM -> OUT RC
run() {
    mkdir -p "$W/$1"
    printf '{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" }, "capabilities": { "shell": { "enabled": true } }, "restrictions": { "information_disclosure": %s } }\n' "$2" > "$W/$1/govern.json"
    printf '%s\n' "$3" > "$W/$1/p.naab"
    OUT="$(cd "$W/$1" && timeout 60 "$NAAB" run p.naab 2>&1)"; RC=$?
}
# prun CMD ARGS_JSON -> a program that runs it and prints its exit code
prun() { printf 'use process\nmain {\n    let r = process.run("%s", %s)\n    print("RAN=" + string(r["exit_code"]))\n}\n' "$1" "$2"; }
refused() { [ "$RC" = 3 ] && [[ "$OUT" != *RAN=* ]] && [[ "$OUT" == *information_disclosure* || "$OUT" == *"Information disclosure"* ]]; }
ran()     { [ "$RC" = 0 ] && [[ "$OUT" == *RAN=0* ]]; }

ON='{ "enabled": true, "level": "hard" }'
SHELL_PS='main {
    let r = <<shell
%s > /dev/null; echo LISTED
>>
    print(r)
}'

# The shell arms need a <<shell>> executor; ask with the restriction OFF.
run probe '{ "enabled": false }' "$(printf "$SHELL_PS" "ps aux")"
SHELL_OK=0; [ "$RC" = 0 ] && [[ "$OUT" == *LISTED* ]] && SHELL_OK=1

if [ "$SHELL_OK" = 1 ]; then
    run il0 "$ON" "$(printf "$SHELL_PS" "ps aux")"
    if [ "$RC" = 3 ] && [[ "$OUT" != *LISTED* ]]; then ok "IL-00" "control: <<shell>> ps aux is refused under the restriction"
    else bad "IL-00" "the fixture does not turn the rule on" "rc=$RC $OUT"; fi
    run il1 "$ON" "$(printf "$SHELL_PS" "ps -A")"
    if [ "$RC" = 3 ] && [[ "$OUT" != *LISTED* ]]; then ok "IL-01" "<<shell>> ps -A is refused (a form the old table missed)"
    else bad "IL-01" "<<shell>> ps -A ran under the restriction" "rc=$RC $OUT"; fi
else
    unm "IL-00" "no <<shell>> executor runs here"; unm "IL-01" "no <<shell>> executor runs here"
fi

run il2 "$ON" "$(prun ps '["aux"]')"
if refused; then ok "IL-02" 'process.run("ps", ["aux"]) is refused'
else bad "IL-02" 'process.run("ps", ["aux"]) ran under the restriction' "rc=$RC $OUT"; fi

run il3 "$ON" "$(prun ps '["-A"]')"
if refused; then ok "IL-03" 'process.run("ps", ["-A"]) is refused'
else bad "IL-03" 'process.run("ps", ["-A"]) ran under the restriction' "rc=$RC $OUT"; fi

run il4 "$ON" "$(prun "$PS_BIN" '["-e"]')"
if refused; then ok "IL-04" "process.run(\"$PS_BIN\", [\"-e\"]) is refused"
else bad "IL-04" "process.run with ps by path ran under the restriction" "rc=$RC $OUT"; fi

run il5 "$ON" "$(prun ps '["-p", "1"]')"
if ran; then ok "IL-05" 'control: process.run("ps", ["-p", "1"]) runs -- one process is not a listing'
else bad "IL-05" "the listing rule refuses a single-process query" "rc=$RC $OUT"; fi

run il6 '{ "enabled": false }' "$(prun ps '["aux"]')"
if ran; then ok "IL-06" 'control: with the restriction off, process.run("ps", ["aux"]) runs'
else bad "IL-06" "process.run(\"ps\") did not run with the restriction off" "rc=$RC $OUT"; fi

run il7 "$ON" "$(prun uname '["-a"]')"
if refused; then ok "IL-07" 'process.run("uname", ["-a"]) is refused'
else bad "IL-07" 'process.run("uname", ["-a"]) ran under the restriction' "rc=$RC $OUT"; fi

run il8p '{ "enabled": true, "level": "hard", "block_process_listing": false }' "$(prun ps '["aux"]')"
P="$RC:$(ran && echo ran || echo not)"
run il8u '{ "enabled": true, "level": "hard", "block_process_listing": false }' "$(prun uname '["-a"]')"
if [ "$P" = "0:ran" ] && refused; then ok "IL-08" "control: block_process_listing false runs ps aux; uname -a, its own flag on, is refused"
else bad "IL-08" "the flags did not split (ps: $P, uname: rc=$RC)" "$OUT"; fi

run il9 "$ON" "$(prun ps '["-o", "pid", "-e"]')"
if refused; then ok "IL-09" 'process.run("ps", ["-o", "pid", "-e"]) is refused'
else bad "IL-09" "a listing option after another option ran under the restriction" "rc=$RC $OUT"; fi

run il10 "$ON" 'use process
main {
    let s = "a"
    for i in 0..16 {
        s = s + s
    }
    let r = process.run("ps", ["-p", "1", s])
    print("RAN=" + string(r["exit_code"]) + " LEN=" + string(s.length()))
}'
if [ "$RC" = 0 ] && [[ "$OUT" == *RAN=* ]] && [[ "$OUT" == *LEN=65536* ]]; then ok "IL-10" "control: a 64 KB argument after ps -p 1 runs (no crash, not a listing)"
else bad "IL-10" "a long non-listing command line did not run" "rc=$RC $OUT"; fi

echo ""
echo "  Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
