#!/usr/bin/env bash
# A53: a governance hook starts from the LOAD-TIME environment, not the one the
# program wrote.
#
# fireHook() used to look a bare command name up on getenv("PATH") when the
# hook fired, and the hook inherited the live environment minus three NAAb
# keys. The program writes the live environment (env.set; embedded Python's
# os.environ), so it chose the PATH the operator's hook was found on and set
# variables the hook then read. Hooks now take the load-time snapshot
# (include/naab/env_snapshot.h, operatorEnvironment() and findOnSnapshotPath(),
# the helpers command and MCP guidance sources use too), still keeping
# blocked_read values, as decided (a hook is the operator's command).
#
# Each run: the program writes the environment, then reads a blocked_read
# variable, a HARD violation that fires the on_violation hook. The hook writes
# what it saw to files.
#
#   HK-00  CONTROL: the hook fires (its marker is written)
#   HK-01  a bare command name is found on the load-time PATH, not on a
#          directory the program put first on PATH          (red on c1312132)
#   HK-02  a variable the program set does not reach the hook (red on c1312132)
#   HK-02c CONTROL: a variable set before the run does reach it
#   HK-03  NAAB_SIGNING_KEY (a throwaway value) is still withheld from the hook
#   HK-03c CONTROL: with inherit_governance_keys the hook receives it
#
# Kill table (mutants of src/runtime/governance_reports.cpp, run by hand,
# revert-checked 2026-11-20):
#   H1 the bare name looked up on getenv("PATH") again      -> HK-01
#   H2 execv (the live environment) instead of execve       -> HK-02, HK-03
#   H3 operatorEnvironment(true) whatever the hook says      -> HK-03
# POSIX only: on Windows the hook path is built the same way, from the same
# helpers, but only build-windows can show it, and this suite reports
# UNMEASURABLE there.
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
W="$(mktemp -d)"
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -12 | sed 's/^/      | /'; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] UNMEASURABLE: $2"; SKIP=$((SKIP+1)); }
[ -x "$NAAB" ] || { echo "not built: $NAAB"; exit 1; }
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*)
    for a in HK-00 HK-01 HK-02 HK-02c HK-03 HK-03c; do skip "$a" "POSIX hooks only here"; done
    echo; echo "Results: 0 passed, 0 failed, 6 unmeasurable"; exit 0 ;;
esac

# Two programs of one bare name: the operator's, on the load-time PATH, and a
# planted one the program puts first on PATH. Each writes its own marker.
mkdir -p "$W/opbin" "$W/planted"
printf '#!/bin/sh\necho operator > "%s/which.txt"\nenv > "%s/env.txt"\n' "$W" "$W" > "$W/opbin/naab_a53_hook"
printf '#!/bin/sh\necho planted > "%s/which.txt"\n' "$W" > "$W/planted/naab_a53_hook"
chmod +x "$W/opbin/naab_a53_hook" "$W/planted/naab_a53_hook"

# go ID INHERIT_KEYS COMMAND -> RC, OUT; the hook's files under $W
go() {
    local d="$W/$1"; mkdir -p "$d"; rm -f "$W/which.txt" "$W/env.txt"
    cat > "$d/govern.json" <<EOF
{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "capabilities": {"env_vars": {"blocked_read": ["NAAB_A53_BLOCKED"]}},
 "hooks": {"on_violation": {"command": "$3", "args": [], "timeout": 5,
                            "inherit_governance_keys": $2}}}
EOF
    cat > "$d/p.naab" <<EOF
use env
main {
    env.set("NAAB_A53_PLANTED", "planted-by-program")
    let s = <<python
import os
os.environ["PATH"] = "$W/planted:" + os.environ.get("PATH", "")
"set"
>>
    print("SET:" + s)
    let v = env.get("NAAB_A53_BLOCKED")
    print("READ")
}
EOF
    OUT="$(cd "$d" && PATH="$W/opbin:$PATH" NAAB_A53_LOADTIME=loadtime-value \
            NAAB_A53_BLOCKED=blocked-value NAAB_SIGNING_KEY="throwaway-$$" \
            timeout 60 "$NAAB" run p.naab 2>&1)"; RC=$?
    for _ in 1 2 3 4 5 6 7 8 9 10; do [ -f "$W/which.txt" ] && break; sleep 0.2; done
}
hookenv() { [ -f "$W/env.txt" ] && grep -q "^$1=" "$W/env.txt"; }

# HK-00/HK-01: the bare name. HK-02/HK-03 run the operator's program by its
# full path, so they measure the environment whatever HK-01 finds.
go k0 false naab_a53_hook
if ! grep -q "SET:set" <<<"$OUT"; then
    for a in HK-00 HK-01 HK-02 HK-02c HK-03 HK-03c; do skip "$a" "the <<python>> block did not run here (rc=$RC)"; done
else
    if [ "$RC" -eq 3 ] && ! grep -q READ <<<"$OUT" && [ -f "$W/which.txt" ]; then
        ok HK-00 "the violation fires the hook (exit 3)"
        w="$(cat "$W/which.txt")"
        if [ "$w" = operator ]; then ok HK-01 "the bare name ran the operator's program from the load-time PATH"
        else bad HK-01 "the bare name ran the program's planted copy ($w)"; fi
    else bad HK-00 "the hook did not fire (rc=$RC)" "$OUT"; skip HK-01 "HK-00 did not fire"; fi

    go k2 false "$W/opbin/naab_a53_hook"
    if [ ! -f "$W/env.txt" ]; then
        for a in HK-02 HK-02c HK-03; do skip "$a" "the hook did not run by its full path (rc=$RC)"; done
    else
        if ! hookenv NAAB_A53_PLANTED; then ok HK-02 "a variable the program set does not reach the hook"
        else bad HK-02 "the hook saw NAAB_A53_PLANTED, set by the program"; fi
        if hookenv NAAB_A53_LOADTIME; then ok HK-02c "a variable set before the run reaches the hook"
        else bad HK-02c "the hook did not see a load-time variable"; fi
        if ! hookenv NAAB_SIGNING_KEY; then ok HK-03 "NAAB_SIGNING_KEY is withheld from the hook"
        else bad HK-03 "the hook received NAAB_SIGNING_KEY"; fi
    fi

    go k1 true "$W/opbin/naab_a53_hook"
    if [ -f "$W/env.txt" ] && hookenv NAAB_SIGNING_KEY; then ok HK-03c "with inherit_governance_keys the hook receives NAAB_SIGNING_KEY"
    elif [ ! -f "$W/env.txt" ]; then skip HK-03c "the hook did not run (rc=$RC)"
    else bad HK-03c "inherit_governance_keys did not pass NAAB_SIGNING_KEY"; fi
fi

echo
echo "Results: $PASS passed, $FAIL failed, $SKIP unmeasurable"
[ "$FAIL" -eq 0 ]
