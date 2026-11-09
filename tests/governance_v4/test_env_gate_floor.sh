#!/usr/bin/env bash
# test_env_gate_floor.sh -- an environment may switch a gate on, never off
# (A130, the owner's decision of 2026-11-09: "only raise govern.json's mode,
# never lower it", applied to the gates as to the mode in A95(b))
#
# An environment (`--env NAME`, or governance.default_env) lays its keys over
# govern.json. Three of them are gate switches -- quality_gate.enabled,
# governance_baseline.enabled, governance_baseline.fail_on_regression -- and
# applyEnvironmentTo() wrote each as `value == "true"`, so an environment's
# "false" turned off a gate the file turned on, and any other value ("no",
# "off") did the same. Observed on master 314bccfd: a quality gate that fails
# the run (exit 2) exited 0 under `--env dev` whose environment sets
# "quality_gate.enabled": "false" -- the template's own `development`
# environment did exactly that. Now an environment can only turn a gate ON; a
# "false" (or anything but "true") over a gate the file turned on is ignored,
# with a warning naming the environment and the key.
#
# Fixtures: a quality gate that always fails (total_violations >= 0), and a
# governance baseline whose recorded advisory count is -1, so any run is a
# regression. Each arm therefore measures one thing: is the gate still on.
#
#   EG-00   CONTROL the file's quality gate fails the run (exit 2)
#   EG-00b  CONTROL the file's baseline gate fails the run (exit 2)
#   EG-01   --env, "quality_gate.enabled": "false": still exit 2, warned
#   EG-02   governance.default_env, the same environment: still exit 2
#   EG-03   --env, "governance_baseline.enabled": "false": still exit 2, warned
#   EG-04   --env, "governance_baseline.fail_on_regression": "false": still exit 2
#   EG-05   --env, "quality_gate.enabled": "no" (not a boolean): still exit 2
#   EG-06c  CONTROL file gate off, --env "quality_gate.enabled": "true": exit 2
#   EG-07c  CONTROL file baseline off, --env turns it and fail_on_regression on: exit 2
#   EG-08c  CONTROL file gate off and no environment: exit 0 (the program runs)
#   EG-09c  CONTROL an environment at the file's own value: no warning
#
# Kill table (each mutant applied alone, rebuilt, this suite run; observed
# 2026-11-09):
#   the quality_gate floor removed (value == "true" again)     EG-01, EG-02, EG-05
#   the governance_baseline.enabled floor removed               EG-03
#   the fail_on_regression floor removed                        EG-04
#   an environment's gate keys ignored altogether               EG-06c, EG-07c
#   the warning removed                                         EG-01, EG-03
# Red first (observed, master 314bccfd): EG-01..EG-05 fail; the controls pass.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_envgate_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       -> /'; }

if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then echo "UNMEASURABLE: naab-lang not built"; exit 1; fi

PROG='main {
    print("PROGRAM_RAN")
}'
# cfg GATE BASELINE ENVS [EXTRA]: GATE/BASELINE are true|false
cfg() {
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "quality_gate": {"enabled": %s, "conditions": [{"metric": "total_violations", "operator": ">=", "threshold": 0}]},
 "governance_baseline": {"enabled": %s, "fail_on_regression": %s, "path": "baseline.json"},
 "environments": %s%s}\n' "$1" "$2" "$2" "$3" "${4:-}"
}
run() {
    local id="$1" c="$2"; shift 2
    local d="$W/$id"; mkdir -p "$d"
    printf '%s\n' "$c" > "$d/govern.json"
    printf '%s\n' "$PROG" > "$d/p.naab"
    printf '{"advisory_violations": -1, "timestamp": "fixture"}\n' > "$d/baseline.json"
    RC=0; OUT=$(cd "$d" && timeout 60s "$NAAB" p.naab "$@" 2>&1) || RC=$?
}
ran()    { grep -q -F "PROGRAM_RAN" <<<"$OUT"; }
gated()  { [ "$RC" -eq 2 ] && ran; }
warned() { grep -qiE "environment '$1'.*$2.*ignored" <<<"$OUT"; }

QOFF='{"dev": {"quality_gate.enabled": "false"}}'
echo "=== an environment may switch a gate on, never off (A130) ==="

run eg00 "$(cfg true false '{}')"
if gated; then pass EG-00 "CONTROL: the file's quality gate fails the run"
else fail EG-00 "the fixture's quality gate did not fail the run (rc=$RC)" "$OUT"
     echo ""; echo "PASS=$PASS FAIL=$FAIL"; exit 1; fi

run eg00b "$(cfg false true '{}')"
if gated && grep -q "Baseline REGRESSION" <<<"$OUT"; then pass EG-00b "CONTROL: the file's baseline gate fails the run"
else fail EG-00b "the fixture's baseline gate did not fail the run (rc=$RC)" "$OUT"
     echo ""; echo "PASS=$PASS FAIL=$FAIL"; exit 1; fi

run eg01 "$(cfg true false "$QOFF")" --env dev
if gated && warned dev quality_gate.enabled; then pass EG-01 "--env (quality_gate.enabled false): still gated, and warned"
else fail EG-01 "an environment switched the quality gate off, or no warning (rc=$RC)" "$OUT"; fi

run eg02 "$(cfg true false "$QOFF" ', "governance": {"default_env": "dev"}')"
if gated; then pass EG-02 "default_env (quality_gate.enabled false): still gated"
else fail EG-02 "default_env switched the quality gate off (rc=$RC)" "$OUT"; fi

run eg03 "$(cfg false true '{"dev": {"governance_baseline.enabled": "false"}}')" --env dev
if gated && warned dev governance_baseline.enabled; then pass EG-03 "--env (governance_baseline.enabled false): still gated, and warned"
else fail EG-03 "an environment switched the baseline gate off, or no warning (rc=$RC)" "$OUT"; fi

run eg04 "$(cfg false true '{"dev": {"governance_baseline.fail_on_regression": "false"}}')" --env dev
if gated; then pass EG-04 "--env (fail_on_regression false): still gated"
else fail EG-04 "an environment switched fail_on_regression off (rc=$RC)" "$OUT"; fi

run eg05 "$(cfg true false '{"dev": {"quality_gate.enabled": "no"}}')" --env dev
if gated; then pass EG-05 "--env (quality_gate.enabled \"no\"): still gated"
else fail EG-05 "a non-boolean value switched the quality gate off (rc=$RC)" "$OUT"; fi

run eg06 "$(cfg false false '{"prod": {"quality_gate.enabled": "true"}}')" --env prod
if gated; then pass EG-06c "CONTROL: file gate off, --env turns it on: gated"
else fail EG-06c "an environment could not turn the quality gate on (rc=$RC)" "$OUT"; fi

run eg07 "$(cfg false false '{"prod": {"governance_baseline.enabled": "true", "governance_baseline.fail_on_regression": "true"}}')" --env prod
if gated; then pass EG-07c "CONTROL: file baseline off, --env turns it on: gated"
else fail EG-07c "an environment could not turn the baseline gate on (rc=$RC)" "$OUT"; fi

run eg08 "$(cfg false false "$QOFF")"
if [ "$RC" -eq 0 ] && ran; then pass EG-08c "CONTROL: gates off and no environment: the program runs, exit 0"
else fail EG-08c "the ungated fixture did not exit 0 (rc=$RC)" "$OUT"; fi

run eg09 "$(cfg true false '{"same": {"quality_gate.enabled": "true"}}')" --env same
if gated && ! grep -qi "ignored" <<<"$OUT"; then pass EG-09c "CONTROL: an environment at the file's value: no warning"
else fail EG-09c "an environment at the file's value warned, or the gate did not hold (rc=$RC)" "$OUT"; fi

echo ""
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
