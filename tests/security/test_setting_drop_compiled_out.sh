#!/usr/bin/env bash
# ============================================================
# test_setting_drop_compiled_out.sh -- NAAB_DROP_SETTING does nothing in a normal build
#
# A TEST build (cmake -DNAAB_CONFIG_MUTATION=ON) lets NAAB_DROP_SETTING delete a
# governance setting from every config it loads, so tools/testrunner/
# setting_drop.py can ask whether any test notices a setting stop working. In a
# shipped binary the same variable would be a governance bypass: set it, and
# capabilities.shell.enabled:false is gone. So the hook must be compiled out of
# every normal build, and this suite checks that on the build CI ships.
#
#   SD-01  normal build: the variable's name is not in the binary at all, so no
#          code can be reading it
#   SD-02  normal build: shell disabled + NAAB_DROP_SETTING naming exactly that
#          setting -> still blocked (exit 3), shell did not run, nothing dropped
#   SD-02c CONTROL: the same program with shell ENABLED runs the shell block,
#          so SD-02's block is the setting's doing and a drop would be visible
#   SD-03  test build (when one is given): it DOES honour the variable -- the
#          same drop lets the block run and logs one drop. Proves the harness's
#          probe is real. UNMEASURABLE without a test build (normal on CI).
#          Give one with NAAB_MUT_BINARY=/path/to/naab-lang.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
MUT="${NAAB_MUT_BINARY:-}"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== NAAB_DROP_SETTING is compiled out of normal builds ==="

if [ ! -x "$NAAB" ]; then
    for id in SD-01 SD-02 SD-02c SD-03; do skip "$id" "naab-lang not built (UNMEASURABLE)"; done
    echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-sdrop.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
trap 'rm -rf "$W"' EXIT

for d in off on; do
    mkdir -p "$W/$d"
    v=false; [ "$d" = on ] && v=true
    printf '{"mode":"enforce","security":{"sandbox_level":"elevated"},"capabilities":{"shell":{"enabled":%s}}}\n' "$v" > "$W/$d/govern.json"
    printf 'main {\n  let r = <<sh\necho SHELL_RAN\n>>\n  print("r=" + string(r))\n}\n' > "$W/$d/p.naab"
done
# run <binary> <dir> [drop] -> sets RC, OUT; drop log in $W/<dir>.drop
run() {
    local bin="$1" dir="$2" drop="${3:-}"
    rm -f "$W/$dir.drop"
    OUT=$(cd "$W/$dir" && NAAB_DROP_SETTING="$drop" NAAB_DROP_LOG="$W/$dir.drop" "$bin" p.naab 2>&1); RC=$?
}

# --- SD-01 ---------------------------------------------------------------------
if LC_ALL=C grep -q 'NAAB_DROP_SETTING' "$NAAB"; then
    bad "SD-01" "the normal binary contains the string NAAB_DROP_SETTING -- the hook is compiled in"
else
    ok "SD-01" "the variable's name is not in the normal binary"
fi

# --- SD-02 / SD-02c -------------------------------------------------------------
run "$NAAB" off capabilities.shell.enabled
case "$OUT" in *SHELL_RAN*) ran=yes ;; *) ran=no ;; esac
if [ "$RC" -eq 3 ] && [ "$ran" = no ] && [ ! -s "$W/off.drop" ]; then
    ok "SD-02" "with the drop requested, shell stays blocked (exit 3) and nothing was dropped"
else
    bad "SD-02" "the normal build honoured NAAB_DROP_SETTING" "rc=$RC shell_ran=$ran drops=$(wc -l < "$W/off.drop" 2>/dev/null || echo 0)"
fi
run "$NAAB" on
case "$OUT" in
    *SHELL_RAN*) ok "SD-02c" "CONTROL: with shell enabled the block runs, so SD-02's block is the setting's" ;;
    *) bad "SD-02c" "the shell block did not run even when enabled -- SD-02 cannot tell a drop from a broken program" "rc=$RC" ;;
esac

# --- SD-03 -------------------------------------------------------------------------
if [ -z "$MUT" ] || [ ! -x "$MUT" ]; then
    skip "SD-03" "no test build given (NAAB_MUT_BINARY) -- UNMEASURABLE, expected on CI"
else
    run "$MUT" off capabilities.shell.enabled
    drops=$(wc -l < "$W/off.drop" 2>/dev/null | tr -d ' ')
    case "$OUT" in
        *SHELL_RAN*) [ "${drops:-0}" -ge 1 ] && ok "SD-03" "the test build honours the drop: shell ran and $drops drop(s) logged" \
                                         || bad "SD-03" "shell ran but no drop was logged" ;;
        *) bad "SD-03" "the test build did not honour NAAB_DROP_SETTING" "rc=$RC drops=${drops:-0}" ;;
    esac
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
