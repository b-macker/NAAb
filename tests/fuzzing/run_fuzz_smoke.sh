#!/usr/bin/env bash
# run_fuzz_smoke.sh — deterministic PR-CI fuzz run (<= 90s)
#
# Runs seeds 1..300 plus every regression seed (seeds that produced a true
# finding in the past) through the naabfuzz pipeline: generate program ->
# run both engines -> compare against the exact-arithmetic oracle -> triage.
# Fails on any new severity-1 signature not listed in known_findings.txt.
#
# Nightly deep fuzzing lives in .github/workflows/fuzz-nightly.yml.
# Repro a finding locally: PYTHONPATH=tools python3 -m naabfuzz repro --seed N

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$ROOT/build/naab-lang}"

if [ ! -x "$NAAB" ]; then
    echo "Error: naab-lang binary not found at $NAAB"
    exit 1
fi

if ! command -v python3 > /dev/null 2>&1; then
    echo "SKIP: python3 not available"
    exit 0
fi

echo "=== Fuzz Smoke: grammar fuzzer, seeds 1..300 + regression seeds ==="

cd "$ROOT"

# Known-answer probe. Parity alone cannot fail when both engines produce
# nothing: two dead engines "agree" on every corpus program. Each engine must
# first print the answer to a program whose output is not in doubt.
PROBE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/naab-fuzz-probe.XXXXXX")"
source "$ROOT/tests/helpers/trust_setup.sh"
setup_isolated_trust   # the probe's unsigned govern.json must not meet a populated trust store
trap 'rm -rf "$PROBE_DIR"; teardown_isolated_trust' EXIT
echo '{ "version": "4.0", "mode": "off" }' > "$PROBE_DIR/govern.json"
printf 'main {\n    let xs = [3, 4, 5]\n    let t = 0\n    for x in xs {\n        t = t + x * x\n    }\n    print("probe:" + string(t))\n}\n' > "$PROBE_DIR/probe.naab"
for engine in "" "--tree-walk"; do
    probe_out=$("$NAAB" $engine "$PROBE_DIR/probe.naab" 2>/dev/null || true)
    if ! printf '%s\n' "$probe_out" | grep -qx "probe:50"; then
        echo "FAIL: known-answer probe (${engine:-vm}) printed '${probe_out}', expected 'probe:50'"
        echo "      -- the engines are not running programs, so parity below would be vacuous"
        exit 1
    fi
done
echo "Known-answer probe: both engines print probe:50"

# Self-tests first (fast, no binary needed)
PYTHONPATH=tools python3 -m naabfuzz selftest > /dev/null 2>&1 || {
    echo "FAIL: naabfuzz oracle self-tests failed"
    exit 1
}

rc=0

# Parser precedence parity: fully-parenthesized emission of the same AST
# must produce identical output
PYTHONPATH=tools python3 -m naabfuzz paren-check \
    --naab "$NAAB" --seed 1 --count 40 || rc=1

PYTHONPATH=tools python3 -m naabfuzz fuzz \
    --naab "$NAAB" \
    --seeds 1..300 \
    --known-findings "$SCRIPT_DIR/known_findings.txt" || rc=1

# Regression seeds: every past true finding stays covered
if grep -qv '^\s*#' "$SCRIPT_DIR/regression_seeds.txt" 2>/dev/null; then
    while read -r seed; do
        case "$seed" in ''|\#*) continue ;; esac
        PYTHONPATH=tools python3 -m naabfuzz fuzz \
            --naab "$NAAB" \
            --seeds "$seed..$seed" \
            --known-findings "$SCRIPT_DIR/known_findings.txt" || rc=1
    done < "$SCRIPT_DIR/regression_seeds.txt"
fi

if [ "$rc" -eq 0 ]; then
    echo "=== Fuzz smoke: PASS ==="
else
    echo "=== Fuzz smoke: FAIL (new severity-1 signature) ==="
fi
exit "$rc"
