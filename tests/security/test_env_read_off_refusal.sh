#!/usr/bin/env bash
# A183: `capabilities.env_vars.read: false` must refuse every env read as a
# HARD, recorded governance block -- not as a sandbox error a NAAb try/catch
# swallows.
#
# Before the fix, every env function asked the SANDBOX first (checkEnvSandbox),
# and main.cpp removes SYS_ENV from the sandbox when reads are off. The
# sandbox's refusal is an ordinary runtime error: `try { env.get("HOME") }
# catch` caught it, the run went on and ended "Governance: PASS", exit 0, with
# no check result recorded -- while blocking ONE variable by name
# (`blocked_read`) was a HARD block, exit 3. The stricter setting gave the
# weaker evidence. Governance already refused at HARD (checkEnvVarRead); it was
# never reached. Now governance is asked first at every env function; the
# sandbox stays as the second layer (the C-SB arm).
#
# Every refusal arm (R-*) has a control (OK-*) that runs the SAME program under
# a config with no env policy and must print RAN and AFTER, exit 0 -- so a
# program that cannot run, or a dead interpreter, is a FAIL, never a pass.
#
# Kill table (each mutant of src/stdlib/env_impl.cpp turns the named arms red;
# run by hand, revert-checked 2026-11-20):
#   M1 env.get: sandbox before governance again             -> R-get, R-get_default, AU
#   M2 env.has: sandbox before governance again             -> R-has
#   M3 get_all/list: no checkEnvGovernanceReadSwitch()      -> R-get_all, R-list, R-get_all_member
#   M4 load_dotenv: no checkEnvGovernanceReadSwitch()       -> R-load_dotenv, R-load_dotenv_path, RL
#   M5 get_int/get_float/get_bool: sandbox first again      -> R-get_int, R-get_float, R-get_bool
#   M6 the switch skips reloadIfChanged()                   -> RL
#   M7 checkEnvGovernanceRead() returns "" (no governance)  -> R-get, R-get_default, R-has,
#                                                              R-get_int, R-get_float, R-get_bool, AU, C-BR
# Not a mutant arm: C-BR, C-SB and WR pin behaviour that was already there.
set -u
NAAB="${NAAB:-$(cd "$(dirname "$0")/../.." && pwd)/build/naab-lang}"
W="$(mktemp -d)"
source "$(dirname "$0")/../helpers/trust_setup.sh"
setup_isolated_trust
source "$(dirname "$0")/../helpers/config_swap.sh"
trap 'stop_swap_operators; teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -20 | sed 's/^/      | /'; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] UNMEASURABLE: $2"; SKIP=$((SKIP+1)); }

if [ ! -x "$NAAB" ]; then echo "naab-lang not built: $NAAB"; exit 1; fi

export NAAB_E183_INT=7 NAAB_E183_FLOAT=2.5 NAAB_E183_BOOL=true

# cfg DIR KIND
#   open     enforce, elevated sandbox, no env policy
#   readoff  enforce, elevated sandbox, env_vars.read: false
#   audit    readoff, under mode audit
#   blocked  enforce, elevated sandbox, blocked_read on one variable
#   standard enforce, the standard sandbox (no SYS_ENV), no env policy
cfg() {
    local d="$1" kind="$2" mode=enforce level=elevated caps=''
    mkdir -p "$d"
    case "$kind" in
        open)     ;;
        readoff)  caps=',"capabilities":{"env_vars":{"read":false}}' ;;
        audit)    mode=audit; caps=',"capabilities":{"env_vars":{"read":false}}' ;;
        blocked)  caps=',"capabilities":{"env_vars":{"blocked_read":["NAAB_E183_INT"]}}' ;;
        standard) level=standard ;;
    esac
    printf '{"version":"5.0","mode":"%s","security":{"sandbox_level":"%s"}%s}\n' \
        "$mode" "$level" "$caps" > "$d/govern.json"
    printf 'NAAB_E183_DOT=1\n' > "$d/.env"
}

# prog CALL -- the call inside a try, then a line after it
prog() {
    cat <<EOF
use env
main {
    try {
        let v = $1
        print("RAN")
    } catch (e) {
        print("CAUGHT")
    }
    print("AFTER")
}
EOF
}

# go DIR PROGRAM -> OUT, RC
go() {
    printf '%s\n' "$2" > "$1/p.naab"
    OUT="$(cd "$1" && timeout 60 "$NAAB" run p.naab 2>&1)"; RC=$?
}
has() { [[ "$OUT" == *"$1"* ]]; }
rule_read() { grep -q 'Rule: capabilities.env_vars.read' <<<"$OUT"; }

CALLS=(
    "get|env.get(\"HOME\")"
    "get_default|env.get(\"HOME\", \"d\")"
    "has|env.has(\"HOME\")"
    "get_all|env.get_all()"
    "list|env.list()"
    "get_all_member|env.get_all"
    "load_dotenv|env.load_dotenv()"
    "load_dotenv_path|env.load_dotenv(\".env\")"
    "get_int|env.get_int(\"NAAB_E183_INT\")"
    "get_float|env.get_float(\"NAAB_E183_FLOAT\")"
    "get_bool|env.get_bool(\"NAAB_E183_BOOL\")"
)

echo "== A183: env.* under capabilities.env_vars.read: false"
for entry in "${CALLS[@]}"; do
    id="${entry%%|*}"; call="${entry#*|}"
    # Control: the same program runs where reads are allowed.
    d="$W/ok_$id"; cfg "$d" open; go "$d" "$(prog "$call")"
    if [ "$RC" -eq 0 ] && has RAN && has AFTER && ! has CAUGHT; then
        ok "OK-$id" "$call runs with no env policy (exit 0)"
        control_ok=1
    else
        bad "OK-$id" "$call did not run with no env policy (rc=$RC)" "$OUT"
        control_ok=0
    fi
    d="$W/ro_$id"; cfg "$d" readoff; go "$d" "$(prog "$call")"
    if [ "$control_ok" -ne 1 ]; then
        skip "R-$id" "its control did not run"
    elif [ "$RC" -eq 3 ] && rule_read && ! has CAUGHT && ! has AFTER && ! has RAN; then
        ok "R-$id" "$call is a HARD block naming capabilities.env_vars.read, not caught (exit 3)"
    else
        bad "R-$id" "$call under read:false: rc=$RC, expected an uncatchable HARD block" "$OUT"
    fi
done

echo "== audit mode records the refusal"
d="$W/au"; cfg "$d" audit; go "$d" "$(prog 'env.get("HOME")')"
if [ "$RC" -eq 0 ] && grep -q 'AUDIT capabilities.env_vars.read' <<<"$OUT" && has AFTER; then
    ok "AU" "audit records capabilities.env_vars.read and the run continues"
else
    bad "AU" "audit mode: rc=$RC, expected an AUDIT capabilities.env_vars.read line" "$OUT"
fi

echo "== behaviour that was already there (controls, not the fix)"
d="$W/br"; cfg "$d" blocked; go "$d" "$(prog 'env.get_int("NAAB_E183_INT")')"
if [ "$RC" -eq 3 ] && grep -q 'Rule: capabilities.env_vars.blocked_read' <<<"$OUT" && ! has AFTER; then
    ok "C-BR" "blocked_read is still a HARD block (exit 3)"
else
    bad "C-BR" "blocked_read: rc=$RC, expected exit 3 naming blocked_read" "$OUT"
fi
d="$W/sb"; cfg "$d" standard; go "$d" "$(prog 'env.get("HOME")')"
if [ "$RC" -eq 0 ] && has CAUGHT && has AFTER && has 'SANDBOX VIOLATION'; then
    ok "C-SB" "the standard sandbox still refuses env.get with no env policy (the second layer)"
else
    bad "C-SB" "standard sandbox: rc=$RC, expected a caught sandbox refusal" "$OUT"
fi
# Writes are not refused by read:false. They are still refused -- by the
# sandbox, which main.cpp stripped of SYS_ENV -- as a catchable error; pinned,
# recorded in A183, since the write does not happen either way.
d="$W/wr"; cfg "$d" readoff; go "$d" "$(prog 'env.set_var("NAAB_E183_W", "1")')"
if [ "$RC" -eq 0 ] && has CAUGHT && has AFTER && ! has RAN; then
    ok "WR" "env.set_var under read:false is refused by the sandbox (caught; PINNED, see A183)"
else
    bad "WR" "env.set_var under read:false: rc=$RC" "$OUT"
fi

echo "== a reload that switches reads off is judged at the next call"
# rl DIR SWAP(0/1): start with reads on; with SWAP=1 the program asks the
# external operator to install a read:false config before calling load_dotenv.
rl() {
    local d="$1" swap="$2" n="$1.next"
    mkdir -p "$n"; cfg "$d" open; cfg "$n" readoff
    rm -f "$d/.do_swap"; [ "$swap" = 1 ] && touch "$d/.do_swap"
    cat > "$d/p.naab" <<EOF
use env
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
    _swap("$n/govern.json", "$d/govern.json")
"step1"
>>
    print("STEP1:" + s)
    try {
        let v = env.load_dotenv()
        print("RAN")
    } catch (e) {
        print("CAUGHT")
    }
    print("AFTER")
}
EOF
    [ "$swap" = 1 ] && start_swap_operator "$d"
    OUT="$(cd "$d" && timeout 120 "$NAAB" run p.naab 2>&1)"; RC=$?
}
rl "$W/rlc" 0
if [ "$RC" -eq 0 ] && has STEP1:step1 && has RAN && has AFTER; then
    ok "RLc" "with no reload, load_dotenv runs after the block (exit 0)"; rl_control=1
elif ! has STEP1:step1; then
    skip "RLc" "the <<python>> block did not run here"; rl_control=0
else
    bad "RLc" "no reload: rc=$RC" "$OUT"; rl_control=0
fi
rl "$W/rl" 1
if [ "$rl_control" -ne 1 ]; then
    skip "RL" "its control did not run"
elif ! has 'Config reloaded mid-run'; then
    skip "RL" "the reload was not accepted, so the arm measures nothing"
elif [ "$RC" -eq 3 ] && rule_read && ! has AFTER; then
    ok "RL" "after a reload to read:false, load_dotenv is a HARD block (exit 3)"
else
    bad "RL" "after a reload to read:false: rc=$RC" "$OUT"
fi

echo
echo "Results: $PASS passed, $FAIL failed, $SKIP unmeasurable"
[ "$FAIL" -eq 0 ] && [ "$SKIP" -eq 0 ]
