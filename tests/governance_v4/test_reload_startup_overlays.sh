#!/usr/bin/env bash
# ============================================================
# test_reload_startup_overlays.sh -- an accepted mid-run reload keeps what the
# run laid over govern.json at startup (A37)
#
# WHY THIS EXISTS. reloadIfChanged() builds the new rules from govern.json
# alone. Everything the run had written into its rules at startup and that
# govern.json does not hold was gone after the first accepted reload, or,
# where the ratchet compares it, every reload was refused:
#   - the --agent-id role (applyAgentRole: languages intersected with the
#     role's, shell denied for a shell-less role)
#   - the --env environment (applyEnvironment: mode, report paths, ...)
#   - project-context rules from CLAUDE.md and friends (applyToRules)
#   - --governance-record-baselines / --governance-check-baselines
#   - governance plugins: loadPlugins() marks each plugin loaded IN the rules,
#     so the reloaded rules held no loaded plugin and no plugin rule ran again
# None of these was decided: every one predates reload (596dd798), whose
# commit names none of them.
#
# Every reload here is a no-op unless the arm says otherwise: the next file
# differs from the first only by "update_reason". Reloads are checked before a
# polyglot block, so every program runs one after the swap.
#
#   R-00   CONTROL: the swap harness works and a no-op reload is accepted
#   Role, languages (--agent-id worker, role allowed_languages [python]):
#   RL-c   no --agent-id: the javascript block runs (the role is what refuses)
#   RL-01  the no-op reload is ACCEPTED (before: refused, "languages")
#   RL-02  after it, javascript is still refused, by the same rule as with no
#          reload; RL-02t the same under --tree-walk
#   RL-03  signed config and signed reload: accepted, still refused
#   RL-04  a reload that tightens (blocked_paths) is accepted and applied
#   RL-05  a reload that widens the ROLE's languages is still refused
#   Role, shell (role shell_allowed false):
#   RS-c   no --agent-id: the shell block runs
#   RS-01  after a no-op reload, shell is refused with the startup message
#          (before: a different check, the per-role one, refused it)
#   RS-02  process.run behaves as it did at startup (A54 is a separate
#          decision: the overlay must not start stripping SYS_EXEC at reload)
#   Environment (--env):
#   EP-c   base mode audit, no --env: a protected read is allowed
#   EP-01  --env prod (enforce): the reload is accepted and the read refused
#          (before: refused, "governance mode: enforce -> audit")
#   ED-c   --env dev (audit), no reload: the read is allowed
#   ED-01  --env dev, no-op reload: still allowed (before: the run silently
#          returned to enforce and the read ended it)
#   ED-02  a reload that sets environments.dev.mode to enforce is accepted
#          and applied -- the environment is re-read from the new file
#   ER-01  --env setting a report path: after a reload the report is written
#          there
#   EN-01  a reload that removes the environment says so, and the run goes on
#          under the new file
#   Project context (CLAUDE.md "Never use subprocess.getoutput()"):
#   PC-c   project_context off: the block runs
#   PC-c1  on, no reload: the block is refused (banned function)
#   PC-01  on, no-op reload: still refused (before: ran, nothing refused)
#   PC-02  the program rewrites CLAUDE.md before the reload: the ban still
#          holds -- a reload replays what startup read, it does not re-read
#   Baselines flags (tree-walker: the VM checks no output baselines):
#   BL-c   no flag: a changed output runs
#   BL-c1  the flag, unchanged output: runs
#   BL-c2  the flag WITH a report flag refuses a changed output (the planted
#          baseline is read; the only form that worked before the fix)
#   BL-01  --governance-check-baselines with no report flag refuses a changed
#          output (before: the flag did nothing without a report flag)
#   BL-02  the same after a no-op reload
#   Plugins (a plugin rule refuses eval):
#   PL-c   no plugin: the eval block runs
#   PL-c1  plugin, no reload: refused by the plugin rule; PL-c1v on the VM
#   PL-01  plugin, no-op reload: still refused (before: ran); PL-01v the VM
#   PL-02  a reload that ADDS a plugin file: its rule runs (the loaded files
#          are remembered, and the new one is loaded on the next check)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -10 | sed 's/^/       | /'; }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }
report() { echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

echo "=== Reload keeps the startup overlays (A37) ==="
if [ ! -x "$NAAB" ]; then
    skip "R-00" "naab-lang not built -- UNMEASURABLE, not a pass"; report; exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    skip "R-00" "python3 missing -- UNMEASURABLE"; report; exit 0
fi

source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
SIGNED_STORE="$NAAB_TRUST_STORE_DIR"
source "$REPO/tests/helpers/config_swap.sh"
W="$(mktemp -d)"
EMPTY_STORE="$(mktemp -d)"
trap 'stop_swap_operators; teardown_isolated_trust; rm -rf "$W" "$EMPTY_STORE"' EXIT

"$NAAB" --keygen "$W/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$W/k.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$W/k.pem"
sign_dir() { (cd "$1" && "$NAAB" --sign-governance >/dev/null 2>&1); }

HDR='"version": "4.0", "security": { "sandbox_level": "elevated" }'

# setup NAME START NEXT TAIL
#   START / NEXT: top-level JSON members after the header ("mode" included).
#   NEXT empty = no reload; NEXT "=" = the no-op reload (START plus
#   update_reason). The program runs one <<python>> block that swaps the
#   config when .do_swap exists -- the text is the same either way, so a
#   block's line and hash do not move between runs -- then TAIL.
#   SIGN=1 signs both files (and runs against the trust store holding the
#   key); otherwise both are unsigned and the trust store is empty.
setup() {
    local name="$1" start="$2" next="$3" tail="$4"
    local d="$W/$name" n="$W/$name.next"
    mkdir -p "$d" "$n"
    printf '{ %s,\n  %s }\n' "$HDR" "$start" > "$d/govern.json"
    rm -f "$d/.do_swap"
    if [ -n "$next" ]; then
        [ "$next" = "=" ] && next="$start"
        printf '{ %s,\n  %s,\n  "update_reason": "a37 reload" }\n' "$HDR" "$next" > "$n/govern.json"
        touch "$d/.do_swap"
    fi
    if [ "${SIGN:-0}" = 1 ]; then
        sign_dir "$d"; [ -n "$next" ] && sign_dir "$n"
    fi
    cat > "$d/r.naab" <<EOF
use process
use file
main {
    let s = <<python
import os, time
def _swap(src, dst):
    if os.path.exists(".swap_done"):
        os.remove(".swap_done")
    with open(".swap_req", "w") as f:
        f.write(src + "\t" + dst + "\n")
    open(".swap_req.ready", "w").close()
    for _ in range(1200):
        if os.path.exists(".swap_done"):
            return
        time.sleep(0.05)
    raise RuntimeError("config swap operator did not respond")
if os.path.exists(".do_swap"):
    time.sleep(1.1)
    if os.path.exists("$n/govern.json.sig"):
        _swap("$n/govern.json.sig", "$d/govern.json.sig")
    _swap("$n/govern.json", "$d/govern.json")
"step1"
>>
    print("STEP1:" + s)
$tail
    print("REACHED_END")
}
EOF
}

# go NAME [naab args...] -- run r.naab in NAME's directory; sets OUT, RC
go() {
    local name="$1"; shift
    local d="$W/$name" store="$EMPTY_STORE"
    [ "${SIGN:-0}" = 1 ] && store="$SIGNED_STORE"
    [ -f "$d/.do_swap" ] && start_swap_operator "$d"
    OUT="$(cd "$d" && NAAB_TRUST_STORE_DIR="$store" timeout 120 "$NAAB" run "$@" r.naab 2>&1)"; RC=$?
}
reloaded() { [[ "$OUT" == *"Config reloaded mid-run"* ]]; }
rejected() { [[ "$OUT" == *"Reload rejected"* ]]; }
ran()      { [[ "$OUT" == *"STEP1:step1"* ]]; }
# The governance rule that refused the run (the first "Rule:" line), or "".
rule_of()  { grep -m1 -o 'Rule: [^ ]*' <<<"$1" || true; }

# expect_reload ID DESC -- the no-op reload must have been accepted
expect_reload() {
    if ! ran; then bad "$1" "$2 -- the program did not start" "$OUT"; return 1; fi
    if rejected; then bad "$1" "$2 -- the reload was REFUSED" "$OUT"; return 1; fi
    if ! reloaded; then bad "$1" "$2 -- no reload happened" "$OUT"; return 1; fi
    return 0
}

# The <<python>> block first: the reload is checked before a block, and on
# the tree-walker javascript takes the parallel-group path, which never checks
# for one (polyglot.cpp: only the sequential path calls reloadIfChanged()).
JS_TAIL='    let jp = <<python
"p"
>>
    let j = <<javascript
1 + 1
>>
    print("JS_RAN:" + j)'
SH_TAIL='    let o = <<shell
echo SHELL_RAN
>>
    print("SH:" + o)'
PROC_TAIL='    let pj = <<python
"x"
>>
    let pr = process.run("/bin/echo", ["PROC_RAN"])
    print("PROC:" + pr["stdout"])'
READ_TAIL='    let t = <<sql
SELECT 1 AS x
>>
    let g = file.read("govern.json")
    print("READ_OK")'

# ---- R-00: the harness ------------------------------------------------------
setup r00 '"mode": "enforce"' "=" "$JS_TAIL"
go r00
if ran && reloaded && ! rejected && [[ "$OUT" == *JS_RAN:2* ]] && [[ "$OUT" == *REACHED_END* ]]; then
    ok "R-00" "the swap harness works and a no-op reload is accepted"
else
    bad "R-00" "harness control failed -- every arm below is UNMEASURABLE" "$OUT"
    report; exit 1
fi

# ---- Role: languages --------------------------------------------------------
ROLE_L='"mode": "enforce", "agents": { "worker": { "allowed_languages": ["python"] } }'
setup rl_c "$ROLE_L" "" "$JS_TAIL"
go rl_c
if [[ "$OUT" == *JS_RAN:2* ]]; then ok "RL-c" "no --agent-id: javascript runs"
else bad "RL-c" "no --agent-id: javascript did not run" "$OUT"; fi

setup rl_n "$ROLE_L" "" "$JS_TAIL"
go rl_n --agent-id worker
RULE_RL_STARTUP="$(rule_of "$OUT")"
if [ "$RC" -ne 3 ] || [[ "$OUT" == *JS_RAN* ]] || [ -z "$RULE_RL_STARTUP" ]; then
    bad "RL-n" "the role did not refuse javascript at startup -- RL-02/03 UNMEASURABLE" "$OUT"
    RULE_RL_STARTUP="__none__"
fi

for eng in vm tw; do
    id="RL-02"; args=(--agent-id worker); [ "$eng" = tw ] && { id="RL-02t"; args=(--tree-walk --agent-id worker); }
    setup "rl_$eng" "$ROLE_L" "=" "$JS_TAIL"
    go "rl_$eng" "${args[@]}"
    if [ "$eng" = vm ]; then
        if expect_reload "RL-01" "role languages: a no-op reload"; then ok "RL-01" "role languages: the no-op reload is accepted"; fi
    fi
    if [ "$RC" -eq 3 ] && [[ "$OUT" != *JS_RAN* ]] && [ "$(rule_of "$OUT")" = "$RULE_RL_STARTUP" ] && reloaded; then
        ok "$id" "after the reload javascript is refused by the startup rule ($RULE_RL_STARTUP)"
    else
        bad "$id" "after the reload: rc=$RC, rule '$(rule_of "$OUT")' (startup: '$RULE_RL_STARTUP')" "$OUT"
    fi
done

SIGN=1 setup rl_s "$ROLE_L" "=" "$JS_TAIL"
SIGN=1 go rl_s --agent-id worker
if [ ! -f "$W/rl_s/govern.json.sig" ]; then skip "RL-03" "signing unavailable -- UNMEASURABLE"
elif expect_reload "RL-03" "signed"; then
    if [ "$RC" -eq 3 ] && [ "$(rule_of "$OUT")" = "$RULE_RL_STARTUP" ]; then ok "RL-03" "signed: accepted, javascript still refused"
    else bad "RL-03" "signed: rc=$RC rule '$(rule_of "$OUT")'" "$OUT"; fi
fi

TIGHT='"mode": "enforce", "agents": { "worker": { "allowed_languages": ["python"] } }, "capabilities": { "filesystem": { "mode": "write", "blocked_paths": ["secret.txt"] } }'
setup rl_t "$ROLE_L" "$TIGHT" '    let t2 = <<python
"x"
>>
    let sx = file.read("secret.txt")
    print("SECRET_READ")'
echo hush > "$W/rl_t/secret.txt"
go rl_t --agent-id worker
if expect_reload "RL-04" "tightening"; then
    if [ "$RC" -eq 3 ] && [[ "$OUT" != *SECRET_READ* ]] && [[ "$OUT" == *blocked_paths* ]]; then
        ok "RL-04" "a tightening reload is accepted and applied"
    else bad "RL-04" "tightening not applied: rc=$RC" "$OUT"; fi
fi

WIDE='"mode": "enforce", "agents": { "worker": { "allowed_languages": ["python", "javascript"] } }'
setup rl_w "$ROLE_L" "$WIDE" "$JS_TAIL"
go rl_w --agent-id worker
if ran && rejected && [ "$RC" -eq 3 ] && [[ "$OUT" != *JS_RAN* ]]; then
    ok "RL-05" "widening the role's languages is still refused"
else bad "RL-05" "widening the role: rc=$RC" "$OUT"; fi

# ---- Role: shell ------------------------------------------------------------
ROLE_S='"mode": "enforce", "agents": { "worker": { "shell_allowed": false } }'
setup rs_c "$ROLE_S" "" "$SH_TAIL"
go rs_c
if [[ "$OUT" == *SH:SHELL_RAN* ]]; then ok "RS-c" "no --agent-id: shell runs"
else bad "RS-c" "no --agent-id: shell did not run" "$OUT"; fi

setup rs_n "$ROLE_S" "" "$SH_TAIL"
go rs_n --agent-id worker
RS_STARTUP="$(grep -m1 -E 'Shell execution|not allowed to execute' <<<"$OUT" || true)"
setup rs_1 "$ROLE_S" "=" "$SH_TAIL"
go rs_1 --agent-id worker
if [ -z "$RS_STARTUP" ]; then
    bad "RS-01" "the role did not refuse shell at startup -- UNMEASURABLE" "$OUT"
elif expect_reload "RS-01" "role shell"; then
    RS_AFTER="$(grep -m1 -E 'Shell execution|not allowed to execute' <<<"$OUT" || true)"
    if [ "$RC" -eq 3 ] && [ "$RS_AFTER" = "$RS_STARTUP" ]; then
        ok "RS-01" "after the reload shell is refused as at startup"
    else bad "RS-01" "after the reload: '$RS_AFTER' (startup: '$RS_STARTUP')" "$OUT"; fi
fi

setup rs_p0 "$ROLE_S" "" "$PROC_TAIL"
go rs_p0 --agent-id worker
P_STARTUP="$RC:$([[ "$OUT" == *PROC:PROC_RAN* ]] && echo ran || echo refused)"
setup rs_p1 "$ROLE_S" "=" "$PROC_TAIL"
go rs_p1 --agent-id worker
P_AFTER="$RC:$([[ "$OUT" == *PROC:PROC_RAN* ]] && echo ran || echo refused)"
if expect_reload "RS-02" "process.run"; then
    if [ "$P_AFTER" = "$P_STARTUP" ]; then ok "RS-02" "process.run after the reload is as at startup ($P_STARTUP)"
    else bad "RS-02" "process.run: startup $P_STARTUP, after the reload $P_AFTER" "$OUT"; fi
fi

# ---- Environment ------------------------------------------------------------
ENV_P='"mode": "audit", "environments": { "prod": { "mode": "enforce" } }'
setup ep_c "$ENV_P" "" "$READ_TAIL"
go ep_c
if [[ "$OUT" == *READ_OK* ]]; then ok "EP-c" "mode audit: the protected read is allowed"
else bad "EP-c" "mode audit: the read was refused" "$OUT"; fi
setup ep_1 "$ENV_P" "=" "$READ_TAIL"
go ep_1 --env prod
if expect_reload "EP-01" "--env prod"; then
    if [ "$RC" -eq 3 ] && [[ "$OUT" != *READ_OK* ]]; then ok "EP-01" "--env prod: accepted, and the read is refused under enforce"
    else bad "EP-01" "--env prod: rc=$RC" "$OUT"; fi
fi

ENV_D='"mode": "enforce", "environments": { "dev": { "mode": "audit" } }'
setup ed_c "$ENV_D" "" "$READ_TAIL"
go ed_c --env dev
if [[ "$OUT" == *READ_OK* ]] && [[ "$OUT" == *"Applied environment: dev"* ]]; then
    ok "ED-c" "--env dev (audit), no reload: the read is allowed"
else bad "ED-c" "--env dev with no reload: the read was refused -- ED-01 UNMEASURABLE" "$OUT"; fi
setup ed_1 "$ENV_D" "=" "$READ_TAIL"
go ed_1 --env dev
if expect_reload "ED-01" "--env dev"; then
    if [[ "$OUT" == *READ_OK* ]] && [ "$RC" -eq 0 ]; then ok "ED-01" "--env dev: still audit after the reload"
    else bad "ED-01" "--env dev: the reload changed the mode (rc=$RC)" "$OUT"; fi
fi
ENV_D2='"mode": "enforce", "environments": { "dev": { "mode": "enforce" } }'
setup ed_2 "$ENV_D" "$ENV_D2" "$READ_TAIL"
go ed_2 --env dev
if expect_reload "ED-02" "dev -> enforce"; then
    if [ "$RC" -eq 3 ] && [[ "$OUT" != *READ_OK* ]]; then ok "ED-02" "an environment tightened by the reload is applied"
    else bad "ED-02" "environments.dev.mode enforce not applied (rc=$RC)" "$OUT"; fi
fi

ENV_R='"mode": "enforce", "environments": { "dev": { "output.file_output.report_json": "env-report.json" } }'
setup er_1 "$ENV_R" "=" '    let t = <<sql
SELECT 1 AS x
>>'
go er_1 --env dev
if expect_reload "ER-01" "env report path"; then
    if [ -s "$W/er_1/env-report.json" ]; then ok "ER-01" "the environment's report path holds after a reload"
    else bad "ER-01" "no report at the environment's path" "$OUT"; fi
fi

setup en_1 "$ENV_D" '"mode": "enforce"' "$READ_TAIL"
go en_1 --env dev
if expect_reload "EN-01" "environment removed"; then
    if [[ "$OUT" == *"environment 'dev' is not in the new govern.json"* ]] && [ "$RC" -eq 3 ] && [[ "$OUT" != *READ_OK* ]]; then
        ok "EN-01" "a reload that removes the environment says so; the new file's enforce applies"
    else bad "EN-01" "environment removed: rc=$RC, no notice" "$OUT"; fi
fi

# ---- Project context --------------------------------------------------------
PC_TAIL='    let p = <<python
import subprocess
f = subprocess.getoutput
"pc_ran"
>>
    print("PC:" + p)'
PC_ON='"mode": "enforce", "project_context": { "enabled": true }'
setup pc_c '"mode": "enforce"' "" "$PC_TAIL"
printf 'Never use subprocess.getoutput() in this project.\n' > "$W/pc_c/CLAUDE.md"
go pc_c
if [[ "$OUT" == *PC:pc_ran* ]]; then ok "PC-c" "project_context off: the block runs"
else bad "PC-c" "project_context off: the block did not run" "$OUT"; fi
setup pc_n "$PC_ON" "" "$PC_TAIL"
printf 'Never use subprocess.getoutput() in this project.\n' > "$W/pc_n/CLAUDE.md"
go pc_n
if [ "$RC" -eq 3 ] && [[ "$OUT" == *banned_functions* ]]; then ok "PC-c1" "project_context on: the banned function is refused"
else bad "PC-c1" "project_context on, no reload: not refused (rc=$RC) -- PC-01 UNMEASURABLE" "$OUT"; fi
setup pc_1 "$PC_ON" "=" "$PC_TAIL"
printf 'Never use subprocess.getoutput() in this project.\n' > "$W/pc_1/CLAUDE.md"
go pc_1
if expect_reload "PC-01" "project context"; then
    if [ "$RC" -eq 3 ] && [[ "$OUT" == *banned_functions* ]] && [[ "$OUT" != *PC:pc_ran* ]]; then
        ok "PC-01" "after the reload the project-context ban still holds"
    else bad "PC-01" "after the reload the banned function ran (rc=$RC)" "$OUT"; fi
fi

setup pc_2 "$PC_ON" "=" '    file.write("CLAUDE.md", "Nothing is banned here.\n")
'"$PC_TAIL"
printf 'Never use subprocess.getoutput() in this project.\n' > "$W/pc_2/CLAUDE.md"
go pc_2
if expect_reload "PC-02" "CLAUDE.md rewritten"; then
    if [ "$RC" -eq 3 ] && [[ "$OUT" == *banned_functions* ]] && grep -q "Nothing is banned" "$W/pc_2/CLAUDE.md"; then
        ok "PC-02" "a CLAUDE.md the program rewrote does not change what the reload applies"
    else bad "PC-02" "after the program rewrote CLAUDE.md the ban did not hold (rc=$RC)" "$OUT"; fi
fi

# ---- Baselines flags (tree-walker) -------------------------------------------
BL_TAIL='    let b = <<python
open("val.txt").read().strip()
>>
    print("BL:" + b)'
# One directory, one program: the baseline key is language:hash:line, so the
# block must not move between runs. hash_keys false makes the key
# "python::LINE", so the baseline can be planted: a CLI run never SAVES one
# (saveBaselines() runs only in the engine's destructor, which _exit skips --
# recorded in docs/open-investigations.md, not fixed here).
BL_CFG='"mode": "enforce", "baselines": { "hash_keys": false }'
setup bl "$BL_CFG" "" "$BL_TAIL"
B="$W/bl"
BL_LINE="$(grep -n 'let b = <<python' "$B/r.naab" | cut -d: -f1)"
mkdir -p "$B/.naab"
printf '{ "entries": { "python::%s": { "output": "A", "type": "string" } } }\n' "$BL_LINE" > "$B/.naab/baselines.json"
echo B > "$B/val.txt"
go bl --tree-walk
if [[ "$OUT" == *BL:B* ]]; then ok "BL-c" "no flag: a changed output runs"; else bad "BL-c" "no flag: refused" "$OUT"; fi
echo A > "$B/val.txt"
go bl --tree-walk --governance-check-baselines
if [[ "$OUT" == *BL:A* ]] && [ "$RC" -eq 0 ]; then ok "BL-c1" "the flag, unchanged output: runs"
else bad "BL-c1" "the flag refused an unchanged output (rc=$RC)" "$OUT"; fi
echo B > "$B/val.txt"
go bl --tree-walk --governance-check-baselines --governance-report chk0.json
if [ "$RC" -eq 3 ] && [[ "$OUT" != *BL:B* ]] && [[ "$OUT" == *"Baseline mismatch"* ]]; then
    ok "BL-c2" "the flag with a report flag refuses a changed output (the planted baseline is read)"
    BL_LIVE=1
else bad "BL-c2" "the flag with a report flag did not refuse -- BL-01/02 UNMEASURABLE (rc=$RC)" "$OUT"; BL_LIVE=0; fi
go bl --tree-walk --governance-check-baselines
if [ "$RC" -eq 3 ] && [[ "$OUT" == *"Baseline mismatch"* ]]; then ok "BL-01" "--governance-check-baselines alone refuses a changed output"
else bad "BL-01" "--governance-check-baselines alone: rc=$RC" "$OUT"; fi
# The same program, now swapping a no-op config in before the block.
printf '{ %s,\n  %s,\n  "update_reason": "a37 reload" }\n' "$HDR" "$BL_CFG" > "$W/bl.next/govern.json"
touch "$B/.do_swap"; cp "$B/govern.json" "$B/govern.json.orig"
go bl --tree-walk --governance-check-baselines --governance-report chk.json
cp "$B/govern.json.orig" "$B/govern.json"; rm -f "$B/.do_swap"
if [ "$BL_LIVE" = 1 ] && expect_reload "BL-02" "baselines"; then
    if [ "$RC" -eq 3 ] && [[ "$OUT" == *"Baseline mismatch"* ]]; then ok "BL-02" "after a reload the flag still refuses a changed output"
    else bad "BL-02" "after the reload the changed output ran (rc=$RC)" "$OUT"; fi
fi

# ---- Plugins ----------------------------------------------------------------
EVAL_TAIL='    let e = <<python
str(eval("1 + 1"))
>>
    print("EVAL:" + e)'
PLUG='"mode": "enforce", "governance_plugins": [ { "file": "plugin.naab", "rules": [ { "id": "A37-PLUG", "function": "check_no_eval", "description": "no eval", "level": "hard", "trigger": "polyglot_block", "languages": ["python"], "message": "eval() is not allowed here", "help": "do not use eval" } ] } ]'
mkplugin() {
    cat > "$1/plugin.naab" <<'EOF'
use string

export function check_no_eval(ctx) {
    let code = ctx["code"]
    if string.contains(code, "eval(") {
        return { "pass": false, "message": "eval() is forbidden by project policy" }
    }
    return { "pass": true }
}
EOF
}
setup pl_c '"mode": "enforce"' "" "$EVAL_TAIL"
go pl_c
if [[ "$OUT" == *EVAL:2* ]]; then ok "PL-c" "no plugin: the eval block runs"
else bad "PL-c" "no plugin: the eval block did not run -- PL-* UNMEASURABLE" "$OUT"; fi
for eng in tw vm; do
    sfx=""; args=(--tree-walk); [ "$eng" = vm ] && { sfx="v"; args=(); }
    setup "pl_n$eng" "$PLUG" "" "$EVAL_TAIL"; mkplugin "$W/pl_n$eng"
    go "pl_n$eng" "${args[@]}"
    if [ "$RC" -eq 3 ] && [[ "$OUT" == *A37-PLUG* ]]; then ok "PL-c1$sfx" "plugin, no reload: refused by the plugin rule"
    else bad "PL-c1$sfx" "plugin, no reload: not refused by it (rc=$RC)" "$OUT"; fi
    setup "pl_1$eng" "$PLUG" "=" "$EVAL_TAIL"; mkplugin "$W/pl_1$eng"
    go "pl_1$eng" "${args[@]}"
    if expect_reload "PL-01$sfx" "plugins"; then
        if [ "$RC" -eq 3 ] && [[ "$OUT" == *A37-PLUG* ]]; then ok "PL-01$sfx" "after the reload the plugin rule still refuses eval"
        else bad "PL-01$sfx" "after the reload the plugin rule did not run (rc=$RC)" "$OUT"; fi
    fi
done

COMPILE_TAIL='    let c = <<python
str(compile("1", "f", "eval") is not None)
>>
    print("COMPILE:" + c)'
PLUG2='"mode": "enforce", "governance_plugins": [ { "file": "plugin.naab", "rules": [ { "id": "A37-PLUG", "function": "check_no_eval", "description": "no eval", "level": "hard", "trigger": "polyglot_block", "languages": ["python"], "message": "m", "help": "h" } ] }, { "file": "plugin2.naab", "rules": [ { "id": "A37-PLUG2", "function": "check_no_compile", "description": "no compile", "level": "hard", "trigger": "polyglot_block", "languages": ["python"], "message": "m", "help": "h" } ] } ]'
setup pl_2 "$PLUG" "$PLUG2" "$COMPILE_TAIL"; mkplugin "$W/pl_2"
cat > "$W/pl_2/plugin2.naab" <<'EOF'
use string

export function check_no_compile(ctx) {
    if string.contains(ctx["code"], "compile(") {
        return { "pass": false, "message": "compile() is forbidden by project policy" }
    }
    return { "pass": true }
}
EOF
go pl_2
if expect_reload "PL-02" "plugin added"; then
    if [ "$RC" -eq 3 ] && [[ "$OUT" == *A37-PLUG2* ]]; then ok "PL-02" "a plugin file the reload adds is loaded and its rule runs"
    else bad "PL-02" "the added plugin's rule did not run (rc=$RC)" "$OUT"; fi
fi

report
[ -n "${A37_KEEP:-}" ] && echo "workdir: $W" && trap - EXIT
[ "$FAIL" -eq 0 ]
