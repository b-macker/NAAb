#!/usr/bin/env bash
# ============================================================
# test_process_run_role_gate.sh -- process.run answers to the same shell gate
# as a <<shell>> block (A54, Bundle 3)
#
# checkShellAllowed() -- the per-role shell_allowed, the role's action matrix
# (SHELL_EXEC) and the function-scope SHELL_EXEC tier -- gated <<shell>> blocks
# and prerequisite commands only. process.run answered to the sandbox's SYS_EXEC
# and the project's capabilities.shell, so a role with shell_allowed: false ran
# any command through it. Measured before the fix (A54, observed): with
# `--agent-id worker` and agents.worker.shell_allowed: false, <<shell>> was
# refused (exit 3) and process.run("/bin/echo", ...) ran (exit 0).
#
# Every arm observes a marker file the command creates, not the exit code alone.
#
#   PR-00  CONTROL: no --agent-id, shell enabled -- process.run runs (the
#          change must not refuse ordinary process.run)
#   PR-00s CONTROL: the role's <<shell>> block is refused -- the role fixture
#          really withholds shell
#   PR-01  a role with shell_allowed: false: process.run is refused (exit 3)
#          and the command did not run
#   PR-01c CONTROL: the same role with shell_allowed: true runs process.run
#   PR-02  a role whose allowed_actions lack SHELL_EXEC: refused
#   PR-02c CONTROL: allowed_actions including SHELL_EXEC: runs
#   PR-03  capabilities.functions default without SHELL_EXEC: process.run in a
#          function is refused
#   PR-03c CONTROL: the default including SHELL_EXEC: runs
#
# Kill table (each mutant rebuilt, this suite run):
#   the checkShellAllowed() call removed from process.run   PR-01, PR-02, PR-03
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

PASS=0; FAIL=0; UNM=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -6 | sed 's/^/       | /'; }
unm() { UNM=$((UNM+1)); echo "  UNMEASURABLE [$1] $2"; }

echo "=== process.run and the per-role shell gate ==="
if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2; exit 1
fi
TOUCH="$(command -v touch)"
if [ -z "$TOUCH" ]; then echo "  UNMEASURABLE [PR] no touch binary"; exit 0; fi

# An unsigned govern.json with a key in the trust store is an INTEGRITY BLOCK:
# isolate the store so the result does not depend on this machine's keys.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-prrole-XXXXXX")"
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

# case NAME CONFIG_TAIL PROGRAM -- writes DIR/govern.json and DIR/p.naab
case_dir() {
    mkdir -p "$W/$1"
    printf '{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" }%s }\n' "$2" > "$W/$1/govern.json"
    printf '%s\n' "$3" > "$W/$1/p.naab"
}
# go NAME [ARGS...] -> OUT RC; MADE=yes when the marker exists afterwards
go() {
    local d="$W/$1"; shift; rm -f "$d/marker"
    OUT="$(cd "$d" && timeout 60 "$NAAB" run "$@" p.naab 2>&1)"; RC=$?
    MADE=no; [ -e "$d/marker" ] && MADE=yes
}
RUN_PROG="use process
main {
    let r = process.run(\"$TOUCH\", [\"marker\"])
    print(\"EXIT=\" + string(r[\"exit_code\"]))
}"
SH_PROG='main {
    let r = <<shell
echo SHELL_RAN
>>
    print(r)
}'
FN_PROG="use process
fn spawn() {
    let r = process.run(\"$TOUCH\", [\"marker\"])
    return r[\"exit_code\"]
}
main { print(\"EXIT=\" + string(spawn())) }"

refused() { [ "$RC" = 3 ] && [ "$MADE" = no ]; }
ran()     { [ "$RC" = 0 ] && [ "$MADE" = yes ]; }

case_dir base ', "capabilities": { "shell": { "enabled": true } }' "$RUN_PROG"
go base
if ran; then ok "PR-00" "control: with no role, process.run runs (rc=0, marker made)"
else bad "PR-00" "process.run did not run with shell enabled and no role -- every arm below is unmeasured" "rc=$RC $OUT"; fi

ROLE_OFF=', "capabilities": { "shell": { "enabled": true } }, "agents": { "worker": { "shell_allowed": false } }'
case_dir role_sh "$ROLE_OFF" "$SH_PROG"
go role_sh --agent-id worker
if [ "$RC" = 3 ] && [[ "$OUT" != *SHELL_RAN* ]]; then ok "PR-00s" "control: the shell-less role's <<shell>> block is refused"
else bad "PR-00s" "the role fixture does not withhold shell" "rc=$RC $OUT"; fi

case_dir role_off "$ROLE_OFF" "$RUN_PROG"
go role_off --agent-id worker
if refused; then ok "PR-01" "a role with shell_allowed: false: process.run is refused and the command did not run"
else bad "PR-01" "a shell-less role ran a command through process.run" "rc=$RC made=$MADE $OUT"; fi

case_dir role_on ', "capabilities": { "shell": { "enabled": true } }, "agents": { "worker": { "shell_allowed": true } }' "$RUN_PROG"
go role_on --agent-id worker
if ran; then ok "PR-01c" "control: the same role with shell_allowed: true runs process.run"
else bad "PR-01c" "a role allowed shell could not use process.run" "rc=$RC made=$MADE $OUT"; fi

case_dir matrix_off ', "capabilities": { "shell": { "enabled": true } }, "agents": { "worker": { "allowed_actions": ["FS_READ"] } }' "$RUN_PROG"
go matrix_off --agent-id worker
if refused; then ok "PR-02" "a role whose allowed_actions lack SHELL_EXEC: process.run is refused"
else bad "PR-02" "a role without SHELL_EXEC ran a command through process.run" "rc=$RC made=$MADE $OUT"; fi

case_dir matrix_on ', "capabilities": { "shell": { "enabled": true } }, "agents": { "worker": { "allowed_actions": ["FS_READ", "SHELL_EXEC"] } }' "$RUN_PROG"
go matrix_on --agent-id worker
if ran; then ok "PR-02c" "control: allowed_actions with SHELL_EXEC run process.run"
else bad "PR-02c" "a role granted SHELL_EXEC could not use process.run" "rc=$RC made=$MADE $OUT"; fi

case_dir fn_off ', "capabilities": { "shell": { "enabled": true }, "functions": { "level": "hard", "default": { "allowed_actions": ["FS_READ"] } } }' "$FN_PROG"
go fn_off
if refused; then ok "PR-03" "a function granted no SHELL_EXEC: process.run inside it is refused"
else bad "PR-03" "a function without SHELL_EXEC ran a command through process.run" "rc=$RC made=$MADE $OUT"; fi

case_dir fn_on ', "capabilities": { "shell": { "enabled": true }, "functions": { "level": "hard", "default": { "allowed_actions": ["FS_READ", "SHELL_EXEC"] } } }' "$FN_PROG"
go fn_on
if ran; then ok "PR-03c" "control: a function granted SHELL_EXEC runs process.run"
else bad "PR-03c" "a function granted SHELL_EXEC could not use process.run" "rc=$RC made=$MADE $OUT"; fi

echo ""
echo "  Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
