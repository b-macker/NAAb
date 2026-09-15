#!/usr/bin/env bash
# ============================================================
# test_sandbox_level_precedence.sh — can the CLI out-rank govern.json?
#
# TWO COMPOUNDING GAPS, recorded as A13.
#
# (1) ESCALATION. applyGovernanceSandbox() (main.cpp) applies the configured
#     security.sandbox_level only under
#
#         if (!rules.sandbox_level_config.empty() && sandbox_level == "unrestricted")
#
#     i.e. only when NO --sandbox-level flag was passed, since the flag is what
#     moves that variable off its "unrestricted" default. So a project that
#     configures `restricted` is overridden by `--sandbox-level elevated` on the
#     command line. The fail-closed clamp below it is gated the same way, so it
#     does not fire either. The direction is the bad one: the CLI can only be
#     made to LOOSEN a configured policy, never to tighten it beyond one.
#
# (2) THE LOCK THAT DOES NOT LOCK. integrity.blocked_flags exists precisely so an
#     owner can forbid a flag. But the preflight calls checkBlocked() on a
#     HARDCODED list of five flag names; isBlockedFlag() consults the operator's
#     full list and nothing calls it for anything else. Any other flag an owner
#     lists -- --sandbox-level among them -- is never consulted. There are also
#     TWO such preflights with DIFFERENT lists (the second omits --tree-walk),
#     so which flags are lockable depends on which execution path you take.
#
# So the one control that could have contained gap (1) is itself inert for the
# flag in question.
#
# TIGHTENING MUST KEEP WORKING. The fix is not "ignore the flag": --sandbox-level
# is legitimate for making a run MORE restrictive than its config, the same way
# --timeout takes the max. SP-05 pins that, and without it "CLI never wins" would
# pass every other arm while removing a facility people use.
#
#   SP-00  POSITIVE CONTROL: flag locking works at all (a flag on the hardcoded
#          list is refused), or every other verdict here is void
#   SP-01  BASELINE: a configured sandbox_level really does restrict
#   SP-02  THE ESCALATION: --sandbox-level elevated overrides configured restricted
#   SP-03  THE INERT LOCK: blocked_flags:["--sandbox-level"] does not stop it
#   SP-04  NEGATIVE CONTROL: with no flag passed, the configured level still wins
#   SP-05  NEGATIVE CONTROL: the CLI may still TIGHTEN below the configured level
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
source "$SCRIPT_DIR/../helpers/native_path.sh"
setup_isolated_trust

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; FAILURES=""
ok()  { PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }

W="${TMPDIR:-/tmp}/naab-sblevel-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W" "${OUTSIDE:-}"; }
trap cleanup EXIT
mkdir -p "$W"

# THE DISCRIMINATOR HAS TO DISCRIMINATE, AND IT HAS TO EXIST EVERYWHERE.
#
# Two mistakes here, both caught by running rather than reasoning.
#
# FIRST: a fixture under $W (i.e. under /tmp) does not discriminate at all.
# createEnterpriseConfig() -- the "standard" level -- allows exactly cwd and the
# temp dir, so standard and elevated both permitted the read, the baseline arm
# failed, and two further arms failed for reasons unrelated to A13.
#
# SECOND: /etc/hostname fixed that on Linux and DOES NOT EXIST under MSYS2, so
# the whole suite hit its own skip guard and exited 0 -- which run-all-tests.sh
# reports as "ALL PASSED". A13 was therefore completely unverified on Windows
# while the run looked green. That is the vacuous-pass failure this very suite
# exists to prevent, committed by the suite itself.
#
# The fix is a fixture WE create, outside both cwd and the temp dir, in $HOME --
# which exists on Linux and MSYS2 alike. Measured across all four levels before
# being trusted (identically for /etc/hostname and for the $HOME fixture):
#     restricted   -> SANDBOX VIOLATION      elevated     -> READ_OK
#     standard     -> SANDBOX VIOLATION      unrestricted -> READ_OK
#
# There is deliberately NO skip guard now. If the discriminator ever stops
# discriminating, SP-01 (the baseline) FAILS rather than the suite quietly
# passing -- an instrument that cannot measure must not report success.
OUTSIDE="${HOME:-/root}/naab-sblevel-outside-$$"
mkdir -p "$OUTSIDE"
echo "OUTSIDE_DATA" > "$OUTSIDE/secret.txt"
TARGET=$(native_path "$OUTSIDE/secret.txt")
# An EMPTY or unreadable TARGET makes every "expects refused" arm pass for free:
# a failed read is classified as refused, which is their expected value. That
# happened -- native_path was used before its helper was sourced, TARGET came
# back empty, and SP-01..03 passed against file.read(""). Assert the instrument
# before trusting any verdict from it.
[ -n "$TARGET" ] || { echo -e "  ${RED}ABORT${NC}: TARGET is empty — native_path unavailable"; exit 1; }
[ -r "$OUTSIDE/secret.txt" ] || { echo -e "  ${RED}ABORT${NC}: fixture unreadable at $OUTSIDE"; exit 1; }

# $1 = security block  $2 = integrity block
cfg() {
    cat > "$W/govern.json" <<JSON
{ "version":"5.0","mode":"enforce",
  "security": $1,
  "integrity": $2,
  "capabilities":{"filesystem":{"mode":"readwrite"}} }
JSON
}

# Reading an ABSOLUTE path is the discriminator: `standard` refuses it and
# `elevated` permits it, so the same program separates the two levels without
# needing a governance rule in the picture at all.
prog() { printf 'use file\nmain { let x = file.read("%s") print("READ_OK") }\n' "$TARGET" > "$W/t.naab"; }

# read | refused | integrity-block | unmeasurable -- four outcomes, never two.
# "refused" and "integrity-block" are different events with different causes and
# collapsing them would let an integrity block masquerade as a sandbox refusal.
run() {
    local out rc
    out=$( cd "$W" && timeout 60 "$NAAB" "$@" "$W/t.naab" 2>&1 ); rc=$?
    case "$out" in
        *READ_OK*)              echo read ;;
        *"INTEGRITY BLOCK"*)    echo integrity-block ;;
        *"SANDBOX VIOLATION"*|*"blocked by governance"*|*"not allowed"*|*"denied"*)
                                echo refused ;;
        *)                      echo "unmeasurable(rc=$rc)" ;;
    esac
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  sandbox_level: does the CLI out-rank govern.json?           |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

prog

# SP-00 -- flag locking is a real mechanism. Uses --governance-override, which IS
# on the hardcoded preflight list. Without this arm, SP-03's "not blocked" could
# mean blocked_flags does nothing at all rather than that it misses this flag.
cfg '{"sandbox_level":"standard"}' '{"blocked_flags":["--governance-override"]}'
r=$(run --governance-override)
[ "$r" = integrity-block ] \
  && ok "SP-00" "POSITIVE CONTROL: a flag on the hardcoded list IS locked" \
  || bad "SP-00" "POSITIVE CONTROL: a flag on the hardcoded list IS locked" \
         "got '$r' — flag locking is not working at all; SP-03 would prove nothing"

# SP-01 -- the configured level actually restricts something.
cfg '{"sandbox_level":"standard"}' '{}'
r=$(run)
[ "$r" = refused ] \
  && ok "SP-01" "BASELINE: configured 'standard' refuses the out-of-sandbox read" \
  || bad "SP-01" "BASELINE: configured 'standard' refuses the out-of-sandbox read" \
         "got '$r' — the level is not restricting, so SP-02 cannot show escalation"

# SP-02 -- THE DEFECT.
cfg '{"sandbox_level":"standard"}' '{}'
r=$(run --sandbox-level elevated)
[ "$r" = refused ] \
  && ok "SP-02" "THE FIX: --sandbox-level cannot escalate above the configured level" \
  || bad "SP-02" "THE FIX: --sandbox-level cannot escalate above the configured level" \
         "got '$r' — CLI overrode govern.json and LOOSENED the sandbox"

# SP-03 -- the lock that should have contained SP-02.
cfg '{"sandbox_level":"standard"}' '{"blocked_flags":["--sandbox-level"]}'
r=$(run --sandbox-level elevated)
[ "$r" = integrity-block ] \
  && ok "SP-03" "an owner can LOCK --sandbox-level via integrity.blocked_flags" \
  || bad "SP-03" "an owner can LOCK --sandbox-level via integrity.blocked_flags" \
         "got '$r' — blocked_flags is inert for any flag off the hardcoded five"

# SP-04 -- the configured level must still apply when no flag is passed.
cfg '{"sandbox_level":"elevated"}' '{}'
r=$(run)
[ "$r" = read ] \
  && ok "SP-04" "NEGATIVE CONTROL: configured 'elevated' still permits the read" \
  || bad "SP-04" "NEGATIVE CONTROL: configured 'elevated' still permits the read" \
         "got '$r' — the fix broke the ordinary configured-level path"

# SP-05 -- tightening from the CLI must survive the fix. Without this arm, a fix
# that simply ignored --sandbox-level would pass SP-02 and SP-03 while removing
# a legitimate facility.
cfg '{"sandbox_level":"elevated"}' '{}'
r=$(run --sandbox-level standard)
[ "$r" = refused ] \
  && ok "SP-05" "NEGATIVE CONTROL: the CLI may still TIGHTEN below the config" \
  || bad "SP-05" "NEGATIVE CONTROL: the CLI may still TIGHTEN below the config" \
         "got '$r' — tighten-only was lost; --sandbox-level should still restrict"


# SP-06/07 -- ENGINE PARITY. The fix changed TWO sites, applyGovernanceSandbox()
# and the VM inline block, and the comment above the former warns they must
# agree. Arms SP-01..SP-05 only exercise the default VM path, so a fix applied to
# one site and not the other would pass all of them.
cfg '{"sandbox_level":"standard"}' '{}'
r=$(run --tree-walk --sandbox-level elevated)
[ "$r" = refused ] \
  && ok "SP-06" "the escalation is blocked on the TREE-WALKER too" \
  || bad "SP-06" "the escalation is blocked on the TREE-WALKER too" \
         "got '$r' — the two sandbox-level sites have drifted"

cfg '{"sandbox_level":"elevated"}' '{}'
r=$(run --tree-walk)
[ "$r" = read ] \
  && ok "SP-07" "NEGATIVE CONTROL: tree-walker still honours a permissive config" \
  || bad "SP-07" "NEGATIVE CONTROL: tree-walker still honours a permissive config" \
         "got '$r' — SP-06 would pass for a tree-walker that blocks everything"

# SP-08 -- an unrecognised configured level applied NOTHING, silently. The CLI
# value is validated and rejected; the govern.json value never was. Fail-open and
# silent is the worst pair, so it is at least announced now. Pre-existing
# behaviour (the old predicate took the same branch), found while verifying the
# tighten-only change rather than looked for.
cfg '{"sandbox_level":"bogus"}' '{}'
out=$( cd "$W" && timeout 60 "$NAAB" "$W/t.naab" 2>&1 )
case "$out" in
    *"unknown security.sandbox_level"*)
        ok "SP-08" "an unknown configured sandbox_level is announced, not silent" ;;
    *)  bad "SP-08" "an unknown configured sandbox_level is announced, not silent" \
            "no warning emitted — a typo silently applies no sandbox at all" ;;
esac

# SP-09 -- NEGATIVE CONTROL for SP-08: a VALID level must warn about nothing, or
# the warning fires on every config and stops being read.
cfg '{"sandbox_level":"standard"}' '{}'
out=$( cd "$W" && timeout 60 "$NAAB" "$W/t.naab" 2>&1 )
case "$out" in
    *"unknown security.sandbox_level"*)
        bad "SP-09" "NEGATIVE CONTROL: a valid level warns about nothing" \
            "warned on a valid level — the check cries wolf" ;;
    *)  ok "SP-09" "NEGATIVE CONTROL: a valid level warns about nothing" ;;
esac

echo ""
echo -e "${CYAN}--------------------------------------------------------------${NC}"
echo -e "  Passed: ${GREEN}${PASS}${NC}   Failed: ${RED}${FAIL}${NC}"
[ "$FAIL" -gt 0 ] && { echo -e "${RED}FAILURES:${NC}${FAILURES}"; echo ""; exit 1; }
echo -e "  ${GREEN}ALL PASSED${NC}"; echo ""; exit 0
