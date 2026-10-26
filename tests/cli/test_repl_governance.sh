#!/usr/bin/env bash
# ============================================================
# test_repl_governance.sh -- the REPL does not start beside a govern.json it
# would not apply (Bundle 2 item 2.3; owner's decision 2026-10-26)
#
# Every REPL front end runs a tree-walker that loads no govern.json. Measured
# 2026-10-26 before the fix: beside an enforce config blocking NAABPROBE,
# code piped into `naab-lang` printed the variable and exited 0, while the
# same code as a file was a HARD block (exit 3). The REPL now refuses (exit 4)
# beside a config in mode enforce or audit, and runs beside mode off or with
# no config found (include/naab/repl_gate.h).
#
#   RG-01   enforce, code piped into `naab-lang`: refused, exit 4, nothing ran
#   RG-01b  enforce, `naab-lang --repl`: refused the same way
#   RG-02   audit: refused (the REPL cannot report under it)
#   RG-03   CONTROL off: the REPL runs the code
#   RG-04   CONTROL no govern.json anywhere up the tree: the REPL runs
#   RG-05   a subdirectory of an enforce project: refused (discovery walks up)
#   RG-06   the measured leak: a blocked_read variable's value never prints
#   RG-07   --no-governance does not lift it
#   RG-08   the standalone naab-repl binaries refuse too (UNMEASURABLE if
#           not built)
#   RG-09   the refusal says what to run instead
#   RG-10   a govern.json that does not parse: refused, exit 4, as the run
#           path refuses it (RG-10b: an empty file; RG-10c CONTROL: a file run
#           there exits 4 too, so the arm compares like with like)
#
# Kill table (each mutant rebuilt, this suite run):
#   gate call removed from naab::repl::run()    RG-01/01b/02/05/06/07/09/10/10b
#   audit allowed (enforce only)                RG-02
#   a config that fails to load allowed         RG-10/10b
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
BUILD_DIR="$(dirname "$NAAB")"

if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2
    exit 1
fi

PASS=0; FAIL=0; UNM=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       | /'; }
unm() { UNM=$((UNM+1)); echo "  UNMEASURABLE [$1] $2"; }

# An unsigned govern.json with a key in the trust store is an INTEGRITY BLOCK:
# isolate the store so the result does not depend on this machine's keys.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-replgov-XXXXXX")"
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

# A directory with no govern.json anywhere above it is needed for RG-04.
has_config_above() {
    local d="$1"
    while :; do
        [ -f "$d/govern.json" ] && return 0
        [ "$d" = "/" ] && return 1
        d="$(dirname "$d")"
    done
}

mk() { mkdir -p "$W/$1"; [ -n "$2" ] && printf '%s\n' "$2" > "$W/$1/govern.json"; return 0; }
mk enforce '{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},"capabilities":{"env_vars":{"blocked_read":["NAABPROBE"]}}}'
mk audit   '{"version":"5.0","mode":"audit"}'
mk off     '{"version":"5.0","mode":"off"}'
mk none    ''
mk broken  '{ "mode": "enforce", '
mkdir -p "$W/empty"; : > "$W/empty/govern.json"
mkdir -p "$W/enforce/sub/deeper"

CODE='print("REPL-" + string(6 * 7))'
LEAK='use env
print("PROBE=" + string(env.get("NAABPROBE")))'

# repl DIR CODE [ARGS...] -> OUT, RC
repl() {
    local d="$1" code="$2"; shift 2
    OUT="$(cd "$W/$d" && printf '%s\n' "$code" | NAABPROBE=leaked-value timeout 20 "$NAAB" "$@" 2>&1)"; RC=$?
}
ran()     { grep -q 'REPL-42' <<<"$OUT"; }
refused() { [ "$RC" = 4 ] && ! ran && grep -q 'REPL cannot run' <<<"$OUT"; }

repl enforce "$CODE"
if refused; then ok "RG-01" "enforce: code piped into naab-lang is refused (exit 4, nothing ran)"
else bad "RG-01" "enforce: the REPL should refuse with exit 4" "rc=$RC $OUT"; fi
RG01_OUT="$OUT"

repl enforce "$CODE" --repl
if refused; then ok "RG-01b" "enforce: naab-lang --repl is refused the same way"
else bad "RG-01b" "enforce: --repl should refuse" "rc=$RC $OUT"; fi

repl audit "$CODE"
if refused; then ok "RG-02" "audit: the REPL is refused"
else bad "RG-02" "audit: the REPL should refuse" "rc=$RC $OUT"; fi

repl off "$CODE"
if [ "$RC" = 0 ] && ran; then ok "RG-03" "control: beside mode off the REPL runs the code"
else bad "RG-03" "mode off should run the REPL" "rc=$RC $OUT"; fi

if has_config_above "$W/none"; then
    unm "RG-04" "a govern.json sits above $W -- no config-free directory to run from"
else
    repl none "$CODE"
    if [ "$RC" = 0 ] && ran; then ok "RG-04" "control: with no govern.json the REPL runs the code"
    else bad "RG-04" "with no govern.json the REPL should run" "rc=$RC $OUT"; fi
fi

repl enforce/sub/deeper "$CODE"
if refused; then ok "RG-05" "a subdirectory of an enforce project is refused (discovery walks up)"
else bad "RG-05" "a subdirectory of an enforce project should refuse" "rc=$RC $OUT"; fi

repl enforce "$LEAK"
if [ "$RC" != 0 ] && ! grep -q 'leaked-value' <<<"$OUT"; then
    ok "RG-06" "the blocked_read variable's value never prints (rc=$RC)"
else
    bad "RG-06" "the REPL printed a variable govern.json blocks" "rc=$RC $OUT"
fi

repl enforce "$CODE" --no-governance --repl
if refused; then ok "RG-07" "--no-governance does not lift the refusal"
else bad "RG-07" "--no-governance should not lift the refusal" "rc=$RC $OUT"; fi

measured_any=0
for bin in naab-repl naab-repl-opt naab-repl-rl; do
    [ -x "$BUILD_DIR/$bin" ] || continue
    measured_any=1
    OUT="$(cd "$W/enforce" && printf '%s\n' "$CODE" | timeout 20 "$BUILD_DIR/$bin" 2>&1)"; RC=$?
    if refused; then ok "RG-08/$bin" "$bin refuses beside an enforce config"
    else bad "RG-08/$bin" "$bin should refuse beside an enforce config" "rc=$RC $OUT"; fi
done
[ "$measured_any" = 1 ] || unm "RG-08" "no naab-repl* binary built in $BUILD_DIR"

if grep -q 'naab-lang run' <<<"$RG01_OUT" && grep -q 'Help:' <<<"$RG01_OUT" && grep -q 'mode enforce' <<<"$RG01_OUT"; then
    ok "RG-09" "the refusal names the mode and says to run the code as a file"
else
    bad "RG-09" "the refusal should name the mode and point to naab-lang run" "$RG01_OUT"
fi

# A config found but unreadable: the run path refuses it (exit 4), so the REPL
# must not start there either -- it would run code beside a policy no engine
# could read. Measured before the fix: the REPL ran the code, exit 0.
repl broken "$CODE"
if refused; then ok "RG-10" "a govern.json that does not parse: the REPL is refused (exit 4, nothing ran)"
else bad "RG-10" "the REPL should refuse beside a govern.json that does not load" "rc=$RC $OUT"; fi
repl empty "$CODE"
if refused; then ok "RG-10b" "an empty govern.json: the REPL is refused the same way"
else bad "RG-10b" "the REPL should refuse beside an empty govern.json" "rc=$RC $OUT"; fi
printf '%s\n' 'main { print("FILE-RAN") }' > "$W/broken/p.naab"
FOUT="$(cd "$W/broken" && timeout 20 "$NAAB" run p.naab 2>&1)"; FRC=$?
if [ "$FRC" = 4 ] && ! grep -q 'FILE-RAN' <<<"$FOUT"; then
    ok "RG-10c" "control: a file run beside the same config is refused with exit 4 too"
else
    bad "RG-10c" "the run path should refuse a govern.json that does not parse" "rc=$FRC $FOUT"
fi

echo ""
echo "  Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
