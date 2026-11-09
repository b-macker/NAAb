#!/usr/bin/env bash
# test_env_mode_floor.sh -- an environment may raise the mode, never lower it
# (A95(b), the owner's decision of 2026-11-06)
#
# An environment (`--env NAME`, or governance.default_env) lays its keys over
# govern.json. Its `mode` used to replace the file's whatever the direction, and
# because whether governance is ACTIVE was decided from the file's own mode, a
# lower mode did one of two things (observed on master 169d6600, a blocked_read
# variable under an elevated sandbox):
#   - audit over an enforce file made the run an audit run: the blocked
#     variable was read, exit 0 -- govern.json said enforce;
#   - off over an enforce or audit file left the engine active with rules
#     saying off, and enforce() exempts only AUDIT, so the run BLOCKED -- under
#     an audit file, stricter than the file by accident.
# Now an environment's mode applies only when it is stricter than the file's
# (off < audit < enforce); a lower one is ignored with a warning naming it.
# The tree-walker refusal and the REPL gate judge "under enforce" through the
# same rule (modeUnderEnvironment), and a reload lays the environment over the
# new file the same way (applyEnvironmentTo).
#
#   EM-00   CONTROL enforce file alone: the read is refused (exit 3)
#   EM-00a  CONTROL audit file alone: the read runs, with an AUDIT line (exit 0)
#   EM-01   enforce file, --env (audit): still refused, and the warning names it
#   EM-02   enforce file, governance.default_env (audit): still refused
#   EM-03   audit file, --env (off): an audit run (exit 0), not a block
#   EM-04   enforce file, --env (off): refused (as before), with the warning
#   EM-05   enforce file, --env (audit), --tree-walk: refused before anything runs
#   EM-06   a reload whose environment lowers the mode is judged with that mode
#           ignored (naab-gov ratchet, --env): refused as enforce -> audit,
#           the file's own mode. Master refused it too, as enforce -> off: the
#           arm pins that the reload path applies the floor, not a new refusal
#   EM-06c  CONTROL the same reload with the environment unchanged is accepted
#   EM-07c  CONTROL off file, --env (enforce): refused -- raising still works
#   EM-08c  CONTROL audit file, --env (enforce): refused
#   EM-09c  CONTROL off file, --env (audit): an audit run
#   EM-10c  CONTROL an environment at the file's own mode: no warning
#
# Kill table (each mutant rebuilt, this suite run; observed 2026-11-08):
#   applyEnvironmentTo applies a lower mode again        EM-01, EM-02, EM-03, EM-04, EM-06
#   modeUnderEnvironment returns the environment's mode  EM-05
#   the warning removed                                   EM-01, EM-04
#   an environment's mode ignored altogether             EM-07c, EM-08c, EM-09c
# Red first (observed, master 169d6600): EM-01, EM-02, EM-03, EM-05 and EM-06
# fail (EM-06 on the wording: master named enforce -> off); EM-04's block
# holds but its warning is missing; the controls pass.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
GOV="${NAAB_GOV:-$(dirname "$NAAB")/naab-gov}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_envfloor_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then echo "UNMEASURABLE: naab-lang not built"; exit 1; fi

TOKEN="emf-$$-$RANDOM"
PROG='main {
    let v = env.get("NAAB_EMF_VAR")
    print("READ=" + string(v))
}'

# cfg MODE ENVS [EXTRA] -> a govern.json: NAAB_EMF_VAR blocked, the given
# environments, an elevated sandbox (so only the env policy decides).
cfg() {
    printf '{"version": "5.0", "mode": "%s", "security": {"sandbox_level": "elevated"},
 "capabilities": {"env_vars": {"blocked_read": ["NAAB_EMF_VAR"]}},
 "environments": %s%s}\n' "$1" "$2" "${3:-}"
}
# run ID CONFIG ARGS... -> OUT, RC
run() {
    local id="$1" c="$2"; shift 2
    local d="$W/$id"; mkdir -p "$d"
    printf '%s\n' "$c" > "$d/govern.json"
    printf '%s\n' "$PROG" > "$d/p.naab"
    RC=0; OUT=$(cd "$d" && NAAB_EMF_VAR="$TOKEN" timeout 60s "$NAAB" p.naab "$@" 2>&1) || RC=$?
}
has() { grep -q -F -- "$1" <<<"$OUT"; }
read_ran() { has "READ=$TOKEN"; }
blocked()  { [ "$RC" -eq 3 ] && ! read_ran && has "blocked_read"; }
audited()  { [ "$RC" -eq 0 ] && read_ran && has "AUDIT capabilities.env_vars.blocked_read"; }
warned()   { grep -qiE "environment '$1'.*mode $2.*ignored" <<<"$OUT"; }

ENVS='{"dev": {"mode": "audit"}, "off": {"mode": "off"}, "prod": {"mode": "enforce"}, "same": {"mode": "enforce"}}'

echo "=== an environment may raise the mode, never lower it (A95(b)) ==="

run em00 "$(cfg enforce "$ENVS")"
if blocked; then pass EM-00 "CONTROL: the enforce file alone refuses the read"
else fail EM-00 "the enforce file alone did not refuse the read (rc=$RC)" "$OUT"
     echo ""; echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$UNM"; exit 1; fi

run em00a "$(cfg audit "$ENVS")"
if audited; then pass EM-00a "CONTROL: the audit file alone runs the read with an AUDIT line"
else fail EM-00a "the audit file alone did not run as audit (rc=$RC)" "$OUT"; fi

run em01 "$(cfg enforce "$ENVS")" --env dev
if blocked && warned dev audit; then pass EM-01 "enforce file, --env (audit): still refused, and the warning names the environment"
else fail EM-01 "an audit environment lowered an enforce file, or no warning (rc=$RC)" "$OUT"; fi

run em02 "$(cfg enforce "$ENVS" ', "governance": {"default_env": "dev"}')"
if blocked; then pass EM-02 "enforce file, default_env (audit): still refused"
else fail EM-02 "default_env (audit) lowered an enforce file (rc=$RC)" "$OUT"; fi

run em03 "$(cfg audit "$ENVS")" --env off
if audited && warned off off; then pass EM-03 "audit file, --env (off): an audit run, not a block"
else fail EM-03 "an off environment over an audit file did not give an audit run (rc=$RC)" "$OUT"; fi

run em04 "$(cfg enforce "$ENVS")" --env off
if blocked && warned off off; then pass EM-04 "enforce file, --env (off): refused, with the warning"
else fail EM-04 "enforce file with an off environment: not refused, or no warning (rc=$RC)" "$OUT"; fi

run em05 "$(cfg enforce "$ENVS")" --env dev --tree-walk
if [ "$RC" -eq 4 ] && ! read_ran && grep -qi 'tree-walk' <<<"$OUT"; then
    pass EM-05 "enforce file, --env (audit), --tree-walk: refused before anything runs"
else fail EM-05 "an audit environment let the tree-walker run under an enforce file (rc=$RC)" "$OUT"; fi

# EM-06: the reload half. OLD is an audit file whose environment raises it to
# enforce; NEW keeps the file at audit and lowers the environment to off. The
# enforced mode would go enforce -> audit, which the ratchet must refuse.
if [ -x "$GOV" ] || [ -x "$GOV.exe" ]; then
    d="$W/em06"; mkdir -p "$d"
    cfg audit '{"x": {"mode": "enforce"}}' > "$d/old.json"
    cfg audit '{"x": {"mode": "off"}}' > "$d/new.json"
    RC=0; OUT=$(cd "$d" && timeout 60s "$GOV" ratchet old.json new.json --env x 2>&1) || RC=$?
    if [ "$RC" -eq 1 ] && grep -qiE 'mode.*enforce.*audit|enforced enforce -> audit' <<<"$OUT"; then
        pass EM-06 "a reload whose environment lowers the mode is refused (enforced enforce -> audit)"
    else fail EM-06 "a reload lowering the environment's mode was not refused (rc=$RC)" "$OUT"; fi
    # Control for EM-06: the same pair with the environment staying at
    # enforce is accepted, so EM-06 is not the ratchet refusing every reload.
    cfg audit '{"x": {"mode": "enforce"}}' > "$d/same.json"
    RC=0; OUT=$(cd "$d" && timeout 60s "$GOV" ratchet old.json same.json --env x 2>&1) || RC=$?
    if [ "$RC" -eq 0 ]; then pass EM-06c "CONTROL: the same reload with the environment unchanged is accepted"
    else fail EM-06c "an unchanged reload was refused (rc=$RC)" "$OUT"; fi
else
    unm EM-06 "naab-gov not built"; unm EM-06c "naab-gov not built"
fi

run em07 "$(cfg off "$ENVS")" --env prod
if blocked; then pass EM-07c "CONTROL: off file, --env (enforce): refused -- an environment still raises"
else fail EM-07c "an enforce environment did not raise an off file (rc=$RC)" "$OUT"; fi

run em08 "$(cfg audit "$ENVS")" --env prod
if blocked; then pass EM-08c "CONTROL: audit file, --env (enforce): refused"
else fail EM-08c "an enforce environment did not raise an audit file (rc=$RC)" "$OUT"; fi

run em09 "$(cfg off "$ENVS")" --env dev
if audited; then pass EM-09c "CONTROL: off file, --env (audit): an audit run"
else fail EM-09c "an audit environment did not raise an off file to audit (rc=$RC)" "$OUT"; fi

run em10 "$(cfg enforce "$ENVS")" --env same
if blocked && ! grep -qi 'ignored' <<<"$OUT"; then pass EM-10c "CONTROL: an environment at the file's own mode gives no warning"
else fail EM-10c "an environment at the file's mode warned, or the read ran (rc=$RC)" "$OUT"; fi

echo ""
echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$UNM"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
