#!/usr/bin/env bash
# ============================================================
# test_capability_list_ratchet.sh -- a mid-run reload cannot loosen a
# capability LIST, and `extends` hands the child every capability list the
# parent sets
#
# WHY THIS EXISTS. The reload ratchet (checkRatchetViolation) compared only
# scalar fields from the day it was written (596dd798): limits, capability
# switches, enforcement levels, restriction flags. No allow/block list was
# compared, though every gate reads them live from the reloaded rules.
# Measured before the fix, under a signed config and a signed reload: removing
# a variable from env_vars.blocked_read was accepted with no violation and no
# notice, and the next env.get() printed the variable. The same held for every
# list below. Separately, the `extends` merge (f229dd9b) merged blocked_read /
# allowed_read / blocked_write / allowed_write and nothing else from
# env_vars or filesystem: a child silently dropped its parent's subprocess
# scrub, path lists and env_vars.read:false (measured: each held with the
# parent used directly and did nothing through `extends`).
#
# Lists are compared by EFFECTIVE access through the gate's own decision
# function, so the "accepted" arms below are as load-bearing as the refused
# ones: a raw diff of entries would refuse each of them.
#
#   R-00   CONTROL: the swap harness works and an unrelated tightening reload
#          is accepted (without it, every "accepted" arm could pass on a
#          harness that never reloads, and every "refused" arm on one that
#          refuses everything)
#   Reload, refused (each: the violation names the list AND the old policy
#   still holds afterwards -- the probe stays denied):
#   ER-01  env_vars.blocked_read: entry removed
#   ER-02  blocked_read -> merely not on allowed_read: HARD -> SOFT
#   ER-03  env_vars.allowed_read: entry added
#   ER-04  env_vars.allowed_read: allowlist removed
#   ER-05  env_vars.read false -> true (rejection only: env.get is also
#          refused by the startup sandbox, which a reload never touches)
#   EW-01  env_vars.blocked_write: entry removed
#   EW-02  env_vars.write false -> true
#   ES-01  blocked_subprocess_vars: entry removed       (child sees it)
#   ES-02  blocked_subprocess_prefixes: prefix removed  (child sees it)
#   ES-03  subprocess_scrub_mode allowlist -> default   (child sees it)
#   FP-01  filesystem.blocked_paths: entry removed
#   FP-02  filesystem.allowed_paths: entry added
#   FP-03  filesystem.allowed_paths: allowlist removed
#   SC-01  shell.blocked_commands: entry removed
#   LG-01  languages.blocked: entry removed
#   LG-02  languages.allowed: entry added
#   CG-01  codegen.blocked_languages: entry removed
#   AG-01  agents.<n>.blocked_paths: entry removed     (run as that agent)
#   AG-02  agents.<n>.blocked_languages: entry removed
#   NS-01  network.https_only true -> false
#   NH-01  network.blocked_hosts: entry removed (rejection only: the sandbox
#          copy is made at startup and a reload never refreshes it)
#   Reload, accepted (a raw entry diff would refuse each; effect checked):
#   ER-06  blocked_read entry re-cased (names compare case-insensitively)
#   ES-04  scrub blocklist [V] -> allowlist []  (V still withheld)
#   ES-05  blocked_subprocess_vars [V] -> blocked_subprocess_prefixes [V's prefix]
#   FP-04  redundant blocked_paths entry under a blocked directory removed
#   SC-02  blocked_commands entry replaced by a substring of itself
#   T-01   tightening: blocked_read entry added -> accepted AND enforced
#   T-02   tightening: languages.blocked entry added -> accepted AND enforced
#   Extends (each with the parent used directly as its control):
#   XE-01  blocked_subprocess_vars      XE-02  blocked_subprocess_prefixes
#   XE-03  subprocess_scrub_mode        XE-04  filesystem.blocked_paths
#   XE-05  filesystem.allowed_paths     XE-06  env_vars.read: false
#   XE-07  env_vars.write: false        XE-08  network.https_only
#   XE-09  network.blocked_hosts
#   XE-10  merge_arrays append: a child's own blocked_paths is ADDED to the
#          parent's (fails before the fix, which merged no path list at all)
#   XC-01  CONTROL: blocked_read was already inherited (extends is resolved)
#   XC-02  CONTROL: child_wins -- a child that sets read:true explicitly reads
#   XC-03  CONTROL: merge_arrays replace -- a child's own blocked_paths
#          replaces the parent's (passes before and after: the documented
#          replace semantics are unchanged)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -8 | sed 's/^/       | /'; }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }
report() { echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

echo "=== Capability lists: reload ratchet and extends merge ==="
if [ ! -x "$NAAB" ]; then
    skip "R-00" "naab-lang not built -- UNMEASURABLE, not a pass"; report; exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    skip "R-00" "python3 missing -- UNMEASURABLE"; report; exit 0
fi

source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
source "$REPO/tests/helpers/config_swap.sh"
W="$(mktemp -d)"
trap 'stop_swap_operators; teardown_isolated_trust; rm -rf "$W"' EXIT

"$NAAB" --keygen "$W/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$W/k.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$W/k.pem"
sign_dir() { (cd "$1" && "$NAAB" --sign-governance >/dev/null 2>&1); }

TOK="$$$RANDOM"
export NAAB_LR_SECRET="lrsecret_$TOK"     # env read probes
export NAAB_LR_OTHER="lrother_$TOK"
export NAAB_LR_SUB="lrsub_$TOK"           # subprocess scrub probes
export NAAB_LRP_VAR="lrpvar_$TOK"
VAULT="vaultdata_$TOK"
HDR='"version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" }'
# Every config names this agent so the agent arms can run as it; an agent
# entry is never contacted (loopback, discard port).
AGENT='"provider": "gemini", "model": "stub-model", "api_key_env": "NAAB_LR_NOKEY", "api_base": "http://127.0.0.1:9"'
# The swap's marker files, for configs whose allowed_paths is an allowlist.
SWAPFILES='"./.swap_req", "./.swap_req.ready", "./.swap_done"'

# ---- reload harness ---------------------------------------------------------
# reload_run NAME START NEXT PROBE [naab args...]
#   START / NEXT: top-level JSON members after the header, e.g.
#   '"capabilities": { ... }'. PROBE: NAAb statements run after the reload.
# The program swaps the config through an external operator, runs a <<sql>>
# block (the reload is checked before every polyglot block), prints
# TRIGGERED, runs PROBE, prints REACHED_END. Sets OUT and RC.
reload_run() {
    local name="$1" start="$2" next="$3" probe="$4"; shift 4
    local d="$W/$name" n="$W/$name.next"
    mkdir -p "$d/vault" "$d/data" "$n"
    echo "$VAULT" > "$d/vault/x.txt"
    printf '{ %s,\n  %s }\n' "$HDR" "$start" > "$d/govern.json"
    printf '{ %s,\n  %s }\n' "$HDR" "$next" > "$n/govern.json"
    sign_dir "$d"; sign_dir "$n"
    if [ ! -f "$d/govern.json.sig" ] || [ ! -f "$n/govern.json.sig" ]; then OUT="__NOSIGN__"; RC=99; return; fi
    cat > "$d/r.naab" <<EOF
use env
use file
use http
use codegen
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
time.sleep(1)
_swap("$n/govern.json.sig", "$d/govern.json.sig")
_swap("$n/govern.json", "$d/govern.json")
"swapped"
>>
    print(s)
    let trig = <<sql
SELECT 1 AS x
>>
    print("TRIGGERED")
$probe
    print("REACHED_END")
}
EOF
    start_swap_operator "$d"
    OUT="$(cd "$d" && timeout 120 "$NAAB" "$@" r.naab 2>&1)"; RC=$?
}

# A reload arm is measurable only if the swap happened.
swapped() { case "$OUT" in *swapped*) return 0 ;; *) return 1 ;; esac; }

# refused ID DESC KEYTEXT LEAK [HELD]
#   PASS iff the reload was refused naming KEYTEXT, LEAK never appears (the
#   old policy still held for the probe), and HELD (if given) appears.
refused() {
    local id="$1" desc="$2" key="$3" leak="$4" held="${5:-}"
    if [ "$OUT" = "__NOSIGN__" ]; then skip "$id" "signing unavailable -- UNMEASURABLE"; return; fi
    if ! swapped; then skip "$id" "config swap did not happen -- UNMEASURABLE"; return; fi
    local rej=0 kept=1
    case "$OUT" in *"Reload rejected: ratchet violation"*) case "$OUT" in *"$key"*) rej=1 ;; esac ;; esac
    case "$OUT" in *"$leak"*) kept=0 ;; esac
    if [ -n "$held" ]; then case "$OUT" in *"$held"*) ;; *) kept=0 ;; esac; fi
    if [ $rej -eq 1 ] && [ $kept -eq 1 ]; then ok "$id" "$desc"
    elif [ $rej -eq 0 ]; then bad "$id" "$desc -- reload NOT refused for '$key'" "$OUT"
    else bad "$id" "$desc -- refused, but the old policy did not hold" "$OUT"; fi
}

# accepted ID DESC EFFECT
#   PASS iff no reload was refused and EFFECT appears.
accepted() {
    local id="$1" desc="$2" effect="$3"
    if [ "$OUT" = "__NOSIGN__" ]; then skip "$id" "signing unavailable -- UNMEASURABLE"; return; fi
    if ! swapped; then skip "$id" "config swap did not happen -- UNMEASURABLE"; return; fi
    case "$OUT" in
        *"Reload rejected"*) bad "$id" "$desc -- the reload was refused" "$OUT" ;;
        *"$effect"*) ok "$id" "$desc" ;;
        *) bad "$id" "$desc -- expected '$effect'" "$OUT" ;;
    esac
}

# Probes. Each prints a token only when the access it tests succeeded.
P_READ='    print("READ=" + env.get("NAAB_LR_SECRET"))'
P_WRITE="    env.set_var(\"NAAB_LR_W\", \"x\")
    print(\"WROTE_$TOK\")"
P_SUB='    let sub = <<shell
echo "SUB=[${NAAB_LR_SUB}][${NAAB_LRP_VAR}]"
>>
    print(sub)'
P_VAULT='    print("VAULT=" + file.read("vault/x.txt"))'
P_CMD='    let c = <<shell
true naab_lr_cmd_marker
echo "RAN-$((6*7))"
>>
    print(c)'
P_CG='    print("CG=" + codegen.run("sql", "SELECT 1 AS x"))'
P_HTTP='    print("H=" + http.get("http://naab-lr.invalid/"))'
P_NOP='    print("NOP")'

caps() { printf '"capabilities": { %s }, "agents": { "worker": { %s } }' "$1" "$AGENT"; }

# ---- R-00 -----------------------------------------------------------------
reload_run R00 "$(caps '"env_vars": { "blocked_read": [] }')" \
    "\"limits\": { \"timeout\": { \"global\": 600 } }, $(caps '"env_vars": { "blocked_read": [] }')" "$P_NOP"
if [ "$OUT" = "__NOSIGN__" ]; then
    skip "R-00" "signing unavailable -- UNMEASURABLE"
elif ! swapped; then
    skip "R-00" "config swap did not happen -- every reload arm is UNMEASURABLE"
else
    case "$OUT" in
        *"Reload rejected"*) bad "R-00" "control: an unrelated tightening reload was refused" "$OUT" ;;
        *REACHED_END*) ok "R-00" "control: the swap harness reloads, and a tightening reload is accepted" ;;
        *) bad "R-00" "control: program did not finish" "$OUT" ;;
    esac
fi

# ---- env read lists ---------------------------------------------------------
reload_run ER01 "$(caps '"env_vars": { "blocked_read": ["NAAB_LR_SECRET"] }')" \
    "$(caps '"env_vars": { "blocked_read": [] }')" "$P_READ"
refused ER-01 "blocked_read entry removed" "env_vars (read lists)" "$NAAB_LR_SECRET" "is blocked by policy"

reload_run ER02 "$(caps '"env_vars": { "blocked_read": ["NAAB_LR_SECRET"], "allowed_read": ["NAAB_LR_OTHER"] }')" \
    "$(caps '"env_vars": { "blocked_read": [], "allowed_read": ["NAAB_LR_OTHER"] }')" "$P_READ"
refused ER-02 "blocked_read -> merely not allowlisted (HARD -> SOFT)" "blocked -> not allowlisted" "not in the allowed read list" "is blocked by policy"

reload_run ER03 "$(caps '"env_vars": { "allowed_read": ["NAAB_LR_OTHER"] }')" \
    "$(caps '"env_vars": { "allowed_read": ["NAAB_LR_OTHER", "NAAB_LR_SECRET"] }')" "$P_READ"
refused ER-03 "allowed_read entry added" "env_vars (read lists)" "$NAAB_LR_SECRET" "not in the allowed read list"

reload_run ER04 "$(caps '"env_vars": { "allowed_read": ["NAAB_LR_OTHER"] }')" \
    "$(caps '"env_vars": { "allowed_read": [] }')" "$P_READ"
refused ER-04 "allowed_read allowlist removed" "env_vars (read lists)" "$NAAB_LR_SECRET" "not in the allowed read list"

reload_run ER05 "$(caps '"env_vars": { "read": false }')" "$(caps '"env_vars": { "read": true }')" "$P_READ"
refused ER-05 "env_vars.read false -> true" "capabilities.env_vars.read" "$NAAB_LR_SECRET"

reload_run ER06 "$(caps '"env_vars": { "blocked_read": ["naab_lr_secret"] }')" \
    "$(caps '"env_vars": { "blocked_read": ["NAAB_LR_SECRET"] }')" "$P_READ"
accepted ER-06 "a re-cased blocked_read entry is the same entry" "is blocked by policy"

reload_run T01 "$(caps '"env_vars": { "blocked_read": [] }')" \
    "$(caps '"env_vars": { "blocked_read": ["NAAB_LR_SECRET"] }')" "$P_READ"
accepted T-01 "tightening: an added blocked_read entry is accepted and enforced" "is blocked by policy"

# ---- env write --------------------------------------------------------------
reload_run EW01 "$(caps '"env_vars": { "blocked_write": ["NAAB_LR_W"] }')" \
    "$(caps '"env_vars": { "blocked_write": [] }')" "$P_WRITE"
refused EW-01 "blocked_write entry removed" "env_vars (write lists)" "WROTE_$TOK" "is blocked by policy"

reload_run EW02 "$(caps '"env_vars": { "write": false }')" "$(caps '"env_vars": { "write": true }')" "$P_WRITE"
refused EW-02 "env_vars.write false -> true" "capabilities.env_vars.write" "WROTE_$TOK"

# ---- subprocess scrub -------------------------------------------------------
reload_run ES01 "$(caps '"env_vars": { "blocked_subprocess_vars": ["NAAB_LR_SUB"] }')" \
    "$(caps '"env_vars": { "blocked_subprocess_vars": [] }')" "$P_SUB"
refused ES-01 "blocked_subprocess_vars entry removed" "subprocess scrub" "$NAAB_LR_SUB" "SUB=[]"

reload_run ES02 "$(caps '"env_vars": { "blocked_subprocess_prefixes": ["NAAB_LRP_"] }')" \
    "$(caps '"env_vars": { "blocked_subprocess_prefixes": [] }')" "$P_SUB"
refused ES-02 "blocked_subprocess_prefixes entry removed" "subprocess scrub" "$NAAB_LRP_VAR" "][]"

reload_run ES03 "$(caps '"env_vars": { "subprocess_scrub_mode": "allowlist" }')" \
    "$(caps '"env_vars": { }')" "$P_SUB"
refused ES-03 "scrub allowlist -> default blocklist" "subprocess scrub" "$NAAB_LR_SUB" "SUB=[]"

reload_run ES04 "$(caps '"env_vars": { "blocked_subprocess_vars": ["NAAB_LR_SUB"] }')" \
    "$(caps '"env_vars": { "subprocess_scrub_mode": "allowlist" }')" "$P_SUB"
accepted ES-04 "blocklist [V] -> allowlist [] is a tightening (V still withheld)" "SUB=[]"

reload_run ES05 "$(caps '"env_vars": { "blocked_subprocess_vars": ["NAAB_LRP_VAR"] }')" \
    "$(caps '"env_vars": { "blocked_subprocess_prefixes": ["NAAB_LRP_"] }')" "$P_SUB"
accepted ES-05 "a blocked var replaced by a prefix covering it" "][]"

# ---- filesystem paths -------------------------------------------------------
reload_run FP01 "$(caps '"filesystem": { "blocked_paths": ["vault/"] }')" \
    "$(caps '"filesystem": { "blocked_paths": [] }')" "$P_VAULT"
refused FP-01 "blocked_paths entry removed" "filesystem paths" "$VAULT" "File path blocked by governance"

reload_run FP02 "$(caps "\"filesystem\": { \"allowed_paths\": [$SWAPFILES, \"./data\"] }")" \
    "$(caps "\"filesystem\": { \"allowed_paths\": [$SWAPFILES, \"./data\", \"./vault\"] }")" "$P_VAULT"
refused FP-02 "allowed_paths entry added" "filesystem paths" "$VAULT" "not in allowed paths"

reload_run FP03 "$(caps "\"filesystem\": { \"allowed_paths\": [$SWAPFILES, \"./data\"] }")" \
    "$(caps '"filesystem": { "allowed_paths": [] }')" "$P_VAULT"
refused FP-03 "allowed_paths allowlist removed" "filesystem paths" "$VAULT" "not in allowed paths"

reload_run FP04 "$(caps '"filesystem": { "blocked_paths": ["vault/", "vault/x.txt"] }')" \
    "$(caps '"filesystem": { "blocked_paths": ["vault/"] }')" "$P_VAULT"
accepted FP-04 "a redundant entry under a blocked directory removed" "File path blocked by governance"

# ---- shell.blocked_commands -------------------------------------------------
reload_run SC01 "$(caps '"shell": { "blocked_commands": ["naab_lr_cmd_marker"] }')" \
    "$(caps '"shell": { "blocked_commands": [] }')" "$P_CMD"
refused SC-01 "blocked_commands entry removed" "blocked_commands" "RAN-42" "Blocked shell command"

reload_run SC02 "$(caps '"shell": { "blocked_commands": ["naab_lr_cmd_marker"] }')" \
    "$(caps '"shell": { "blocked_commands": ["lr_cmd_mark"] }')" "$P_CMD"
accepted SC-02 "an entry replaced by a substring of itself blocks more" "Blocked shell command"

# ---- languages --------------------------------------------------------------
# The reload trigger is itself a <<sql>> block, so for these arms the trigger
# IS the probe: TRIGGERED prints only if sql ran.
reload_run LG01 "\"languages\": { \"blocked\": [\"sql\"] }, $(caps '"env_vars": { }')" \
    "\"languages\": { \"blocked\": [] }, $(caps '"env_vars": { }')" "$P_NOP"
refused LG-01 "languages.blocked entry removed" "languages:" "TRIGGERED"

reload_run LG02 "\"languages\": { \"allowed\": [\"python\"] }, $(caps '"env_vars": { }')" \
    "\"languages\": { \"allowed\": [\"python\", \"sql\"] }, $(caps '"env_vars": { }')" "$P_NOP"
refused LG-02 "languages.allowed entry added" "languages:" "TRIGGERED"

reload_run T02 "\"languages\": { \"blocked\": [] }, $(caps '"env_vars": { }')" \
    "\"languages\": { \"blocked\": [\"sql\"] }, $(caps '"env_vars": { }')" "$P_NOP"
case "$OUT" in
    *"Reload rejected"*) bad "T-02" "tightening: an added languages.blocked entry was refused" "$OUT" ;;
    *TRIGGERED*) bad "T-02" "tightening: accepted but sql still ran" "$OUT" ;;
    *swapped*) ok "T-02" "tightening: an added languages.blocked entry is accepted and enforced" ;;
    *) skip "T-02" "config swap did not happen -- UNMEASURABLE" ;;
esac

reload_run CG01 "\"codegen\": { \"enabled\": true, \"blocked_languages\": [\"sql\"] }, $(caps '"env_vars": { }')" \
    "\"codegen\": { \"enabled\": true, \"blocked_languages\": [] }, $(caps '"env_vars": { }')" "$P_CG"
refused CG-01 "codegen.blocked_languages entry removed" "codegen languages" "CG=" "is blocked for dynamic code"

# ---- per-agent --------------------------------------------------------------
reload_run AG01 "\"capabilities\": { }, \"agents\": { \"worker\": { $AGENT, \"blocked_paths\": [\"vault/\"] } }" \
    "\"capabilities\": { }, \"agents\": { \"worker\": { $AGENT } }" "$P_VAULT" --agent-id worker
refused AG-01 "agents.worker.blocked_paths entry removed" "agent.worker paths" "$VAULT" "blocked_paths"

reload_run AG02 "\"capabilities\": { }, \"agents\": { \"worker\": { $AGENT, \"blocked_languages\": [\"sql\"] } }" \
    "\"capabilities\": { }, \"agents\": { \"worker\": { $AGENT } }" "$P_NOP" --agent-id worker
refused AG-02 "agents.worker.blocked_languages entry removed" "agent.worker languages" "TRIGGERED"

# ---- network ------------------------------------------------------------------
reload_run NS01 "$(caps '"network": { "https_only": true }')" \
    "$(caps '"network": { "https_only": false }')" "$P_HTTP"
refused NS-01 "network.https_only true -> false" "capabilities.network.https_only" "Couldn't resolve" "plaintext HTTP request denied"

reload_run NH01 "$(caps '"network": { "blocked_hosts": ["naab-lr-blocked.invalid"] }')" \
    "$(caps '"network": { "blocked_hosts": [] }')" "$P_NOP"
refused NH-01 "network.blocked_hosts entry removed" "blocked_hosts" "__never__"

# ---- extends ------------------------------------------------------------------
# ext_run NAME PARENT_MEMBERS CHILD_MEMBERS PROBE: runs PROBE under the parent
# directly (PO/PRC) and under a child that extends it (CO/CRC).
ext_run() {
    local name="$1" parent="$2" child="$3" probe="$4"
    local p="$W/x_$name/base" c="$W/x_$name/child"
    mkdir -p "$p/vault" "$c/vault"
    echo "$VAULT" > "$p/vault/x.txt"; echo "$VAULT" > "$c/vault/x.txt"
    printf '{ %s,\n  %s }\n' "$HDR" "$parent" > "$p/govern.json"
    if [ -n "$child" ]; then
        printf '{ %s, "extends": "../base/govern.json",\n  %s }\n' "$HDR" "$child" > "$c/govern.json"
    else
        printf '{ %s, "extends": "../base/govern.json" }\n' "$HDR" > "$c/govern.json"
    fi
    sign_dir "$p"; sign_dir "$c"
    printf 'use env\nuse file\nuse http\nmain {\n%s\n    print("REACHED_END")\n}\n' "$probe" > "$p/r.naab"
    cp "$p/r.naab" "$c/r.naab"
    PO="$(cd "$p" && timeout 60 "$NAAB" r.naab 2>&1)"; PRC=$?
    CO="$(cd "$c" && timeout 60 "$NAAB" r.naab 2>&1)"; CRC=$?
}
# inherited ID DESC LEAK HELD: the parent used directly must hold (no LEAK,
# HELD present) -- else the arm is UNMEASURABLE -- and so must the child.
inherited() {
    local id="$1" desc="$2" leak="$3" held="$4"
    case "$PO" in *"$leak"*) skip "$id" "control failed: the parent itself did not hold -- UNMEASURABLE"; return ;; esac
    case "$PO" in *"$held"*) ;; *) skip "$id" "control failed: parent output lacks '$held' -- UNMEASURABLE"; return ;; esac
    case "$CO" in
        *"$leak"*) bad "$id" "$desc -- the child did not inherit it" "$CO" ;;
        *"$held"*) ok "$id" "$desc" ;;
        *) bad "$id" "$desc -- child output lacks '$held'" "$CO" ;;
    esac
}

ext_run XE01 "$(caps '"env_vars": { "blocked_subprocess_vars": ["NAAB_LR_SUB"] }')" "" "$P_SUB"
inherited XE-01 "blocked_subprocess_vars inherited" "$NAAB_LR_SUB" "SUB=[]"
ext_run XE02 "$(caps '"env_vars": { "blocked_subprocess_prefixes": ["NAAB_LRP_"] }')" "" "$P_SUB"
inherited XE-02 "blocked_subprocess_prefixes inherited" "$NAAB_LRP_VAR" "SUB=["
ext_run XE03 "$(caps '"env_vars": { "subprocess_scrub_mode": "allowlist" }')" "" "$P_SUB"
inherited XE-03 "subprocess_scrub_mode allowlist inherited" "$NAAB_LR_SUB" "SUB=[]"
ext_run XE04 "$(caps '"filesystem": { "blocked_paths": ["vault/"] }')" "" "$P_VAULT"
inherited XE-04 "filesystem.blocked_paths inherited" "$VAULT" "File path blocked by governance"
ext_run XE05 "$(caps '"filesystem": { "allowed_paths": ["./data"] }')" "" "$P_VAULT"
inherited XE-05 "filesystem.allowed_paths inherited" "$VAULT" "not in allowed paths"
ext_run XE06 "$(caps '"env_vars": { "read": false }')" "" "$P_READ"
inherited XE-06 "env_vars.read: false inherited" "$NAAB_LR_SECRET" "denied by sandbox"
ext_run XE07 "$(caps '"env_vars": { "write": false }')" "" "$P_WRITE"
inherited XE-07 "env_vars.write: false inherited" "WROTE_$TOK" "Writing environment variables is disabled"
ext_run XE08 "$(caps '"network": { "https_only": true }')" "" "$P_HTTP"
inherited XE-08 "network.https_only inherited" "Couldn't resolve" "plaintext HTTP request denied"
ext_run XE09 "$(caps '"network": { "blocked_hosts": ["naab-lr.invalid"] }')" "" "$P_HTTP"
inherited XE-09 "network.blocked_hosts inherited" "Couldn't resolve" "HTTP request denied by sandbox"

ext_run XC01 "$(caps '"env_vars": { "blocked_read": ["NAAB_LR_SECRET"] }')" "" "$P_READ"
inherited XC-01 "control: blocked_read (merged before this change) is inherited" "$NAAB_LR_SECRET" "is blocked by policy"

ext_run XC02 "$(caps '"env_vars": { "read": false }')" "$(caps '"env_vars": { "read": true }')" "$P_READ"
case "$PO" in
    *"$NAAB_LR_SECRET"*) skip "XC-02" "control failed: parent itself read the variable -- UNMEASURABLE" ;;
    *) case "$CO" in
           *"READ=$NAAB_LR_SECRET"*) ok "XC-02" "control: a child that sets read:true explicitly keeps it (child_wins)" ;;
           *) bad "XC-02" "control: child_wins child could not read" "$CO" ;;
       esac ;;
esac

ext_run XC03 "$(caps '"filesystem": { "blocked_paths": ["vault/"] }')" \
    "$(caps '"filesystem": { "blocked_paths": ["data/"] }')" "$P_VAULT"
case "$CO" in
    *"VAULT=$VAULT"*) ok "XC-03" "control: merge_arrays replace -- the child's own blocked_paths replaces the parent's" ;;
    *) bad "XC-03" "control: replace semantics changed -- child could not read vault/" "$CO" ;;
esac

ext_run XE10 "$(caps '"filesystem": { "blocked_paths": ["vault/"] }')" \
    "\"meta\": { \"inheritance\": { \"merge_arrays\": \"append\" } }, $(caps '"filesystem": { "blocked_paths": ["data/"] }')" "$P_VAULT"
case "$CO" in
    *"VAULT=$VAULT"*) bad "XE-10" "merge_arrays append dropped the parent's blocked_paths" "$CO" ;;
    *"File path blocked by governance"*) ok "XE-10" "merge_arrays append keeps the parent's blocked_paths" ;;
    *) bad "XE-10" "unexpected output" "$CO" ;;
esac

unset NAAB_SIGNING_KEY
report
[ $FAIL -eq 0 ] || exit 1
exit 0
