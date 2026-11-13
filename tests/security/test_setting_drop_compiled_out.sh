#!/usr/bin/env bash
# ============================================================
# test_setting_drop_compiled_out.sh -- NAAB_DROP_SETTING does nothing in a normal build
#
# A TEST build (cmake -DNAAB_CONFIG_MUTATION=ON) lets NAAB_DROP_SETTING delete a
# governance setting from every config it loads, so tools/testrunner/
# setting_drop.py can ask whether any test notices a setting stop working. In a
# shipped binary the same variable would be a governance bypass: set it, and
# a blocked path is readable. So the hook must be compiled out of every normal
# build, and this suite checks that on the build CI ships.
#
# The subject is capabilities.filesystem.blocked_paths + file.read, decided
# inside NAAb's standard library. It was a <<sh>> block under
# capabilities.shell.enabled, which build-windows cannot run at all -- so the
# control failed there for a reason that had nothing to do with the drop.
#
#   SD-01  normal build: the variable's name is not in the binary at all, so no
#          code can be reading it
#   SD-02  normal build: secret.txt blocked + NAAB_DROP_SETTING naming exactly
#          that setting -> still blocked (exit 3), not read, nothing dropped
#   SD-02c CONTROL: the same program with nothing blocked reads the file, so
#          SD-02's block is the setting's doing and a drop would be visible
#   SD-03  test build (when one is given): it DOES honour the variable -- the
#          same drop lets the read through and logs one drop. Proves the
#          harness's probe is real. UNMEASURABLE without a test build (normal
#          on CI). Give one with NAAB_MUT_BINARY=/path/to/naab-lang.
#
# govern.json has a SECOND reader: the scanner (naab-lang --scan) opens the file
# itself and reads its "scanner" section, never passing loadFromJson. The hook
# is one header both readers call (include/naab/testing/config_mutation.h), so
# the same check is made on that path, and on naab-gov, which shares the
# governance loader:
#
#   SD-01g normal build: the variable's name is not in naab-gov either
#   SD-04  normal build: a scanner check switched off in govern.json + the drop
#          naming exactly that key -> still off, nothing dropped
#   SD-04c CONTROL: with the key absent the scanner reports the finding, so
#          SD-04's silence is the setting's doing and a drop would be visible
#   SD-05  test build (when given): the scanner DOES honour the drop -- the
#          finding comes back and one drop is logged. UNMEASURABLE without one.
#
# Kill table (each mutant run 2026-11-13, each turns the arm red):
#   M1 normal build compiled with -DNAAB_CONFIG_MUTATION=ON ..... SD-01 SD-01g SD-02 SD-04
#   M2 scanner.cpp without its applySettingDrop call ............ SD-05
#   M3 SD-04's config without the "enabled": false key ......... SD-04 (harness)
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
    for id in SD-01 SD-01g SD-02 SD-02c SD-03 SD-04 SD-04c SD-05; do skip "$id" "naab-lang not built (UNMEASURABLE)"; done
    echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-sdrop.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
# The configs below are unsigned. With a populated trust store an unsigned
# govern.json is an INTEGRITY BLOCK (exit 3) -- which is exactly the exit SD-02
# expects, so the store must be isolated or SD-02 could pass for the wrong reason.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

# off = secret.txt blocked, on = nothing blocked
for d in off on; do
    mkdir -p "$W/$d"
    b='["secret.txt"]'; [ "$d" = on ] && b='[]'
    printf '{"mode":"enforce","security":{"sandbox_level":"elevated"},"capabilities":{"filesystem":{"mode":"read","blocked_paths":%s}}}\n' "$b" > "$W/$d/govern.json"
    echo "POLICY_SECRET" > "$W/$d/secret.txt"
    printf 'use file\nmain {\n  print("r=" + file.read("secret.txt"))\n}\n' > "$W/$d/p.naab"
done
DROP=capabilities.filesystem.blocked_paths
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
run "$NAAB" off "$DROP"
case "$OUT" in *r=POLICY_SECRET*) ran=yes ;; *) ran=no ;; esac
if [ "$RC" -eq 3 ] && [ "$ran" = no ] && [ ! -s "$W/off.drop" ]; then
    ok "SD-02" "with the drop requested, the path stays blocked (exit 3) and nothing was dropped"
else
    bad "SD-02" "the normal build honoured NAAB_DROP_SETTING" "rc=$RC read=$ran drops=$(wc -l < "$W/off.drop" 2>/dev/null || echo 0)"
fi
run "$NAAB" on
case "$OUT" in
    *r=POLICY_SECRET*) ok "SD-02c" "CONTROL: with nothing blocked the file is read, so SD-02's block is the setting's" ;;
    *) bad "SD-02c" "the file was not read even with nothing blocked -- SD-02 cannot tell a drop from a broken program" "rc=$RC" ;;
esac

# --- SD-03 -------------------------------------------------------------------------
if [ -z "$MUT" ] || [ ! -x "$MUT" ]; then
    skip "SD-03" "no test build given (NAAB_MUT_BINARY) -- UNMEASURABLE, expected on CI"
else
    run "$MUT" off "$DROP"
    drops=$(wc -l < "$W/off.drop" 2>/dev/null | tr -d ' ')
    case "$OUT" in
        *r=POLICY_SECRET*) [ "${drops:-0}" -ge 1 ] && ok "SD-03" "the test build honours the drop: the file was read and $drops drop(s) logged" \
                                               || bad "SD-03" "the file was read but no drop was logged" ;;
        *) bad "SD-03" "the test build did not honour NAAB_DROP_SETTING" "rc=$RC drops=${drops:-0}" ;;
    esac
fi

# --- SD-01g ----------------------------------------------------------------------
GOV="$SCRIPT_DIR/../../build/naab-gov"
[ -x "$GOV" ] || GOV="$GOV.exe"
if [ ! -x "$GOV" ]; then
    skip "SD-01g" "naab-gov not built (UNMEASURABLE)"
elif LC_ALL=C grep -q 'NAAB_DROP_SETTING' "$GOV"; then
    bad "SD-01g" "the normal naab-gov contains the string NAAB_DROP_SETTING -- the hook is compiled in"
else
    ok "SD-01g" "the variable's name is not in the normal naab-gov"
fi

# --- SD-04 / SD-04c / SD-05: the scanner's own reader ------------------------------
# soff = the empty_catch check switched off, son = the key absent (default: on)
for d in soff son; do
    mkdir -p "$W/$d"
    cq='{"empty_catch":{"enabled":false}}'; [ "$d" = son ] && cq='{}'
    printf '{"version":"4.0","mode":"audit","scanner":{"version":"1.0","mode":"enforce","code_quality":%s}}\n' "$cq" > "$W/$d/govern.json"
    printf 'fn risky(a) {\n  try {\n    return a + 1\n  } catch (err) {\n  }\n  return 0\n}\nmain {\n  print(risky(1))\n}\n' > "$W/$d/p.naab"
done
SDROP=scanner.code_quality.empty_catch.enabled
# scan <binary> <dir> [drop] -> sets HITS (report lines naming the check), OUT
scan() {
    local bin="$1" dir="$2" drop="${3:-}"
    rm -f "$W/$dir.drop"
    OUT=$(cd "$W/$dir" && NAAB_DROP_SETTING="$drop" NAAB_DROP_LOG="$W/$dir.drop" "$bin" --scan p.naab naab 2>&1)
    HITS=$(grep -c 'code_quality\.empty_catch' <<<"$OUT")
    SCANNED=no; grep -q 'Scanned: 1 files' <<<"$OUT" && SCANNED=yes
}
scan "$NAAB" son
if [ "$SCANNED" = yes ] && [ "$HITS" -ge 1 ]; then
    ok "SD-04c" "CONTROL: with the key absent the scanner reports the empty catch"
    scan "$NAAB" soff "$SDROP"
    if [ "$SCANNED" = yes ] && [ "$HITS" -eq 0 ] && [ ! -s "$W/soff.drop" ]; then
        ok "SD-04" "with the drop requested, the scanner check stays off and nothing was dropped"
    else
        bad "SD-04" "the normal build's scanner honoured NAAB_DROP_SETTING" "scanned=$SCANNED hits=$HITS drops=$(wc -l < "$W/soff.drop" 2>/dev/null || echo 0)"
    fi
else
    bad "SD-04c" "the scanner did not report the empty catch with the key absent -- SD-04 cannot tell a drop from a scanner that reports nothing" "scanned=$SCANNED hits=$HITS"
    skip "SD-04" "its control failed (UNMEASURABLE)"
fi
if [ -z "$MUT" ] || [ ! -x "$MUT" ]; then
    skip "SD-05" "no test build given (NAAB_MUT_BINARY) -- UNMEASURABLE, expected on CI"
else
    scan "$MUT" soff "$SDROP"
    drops=$(wc -l < "$W/soff.drop" 2>/dev/null | tr -d ' ')
    if [ "$SCANNED" = yes ] && [ "$HITS" -ge 1 ] && [ "${drops:-0}" -ge 1 ]; then
        ok "SD-05" "the test build's scanner honours the drop: the finding is back and $drops drop(s) logged"
    else
        bad "SD-05" "the test build's scanner did not honour NAAB_DROP_SETTING" "scanned=$SCANNED hits=$HITS drops=${drops:-0}"
    fi
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
