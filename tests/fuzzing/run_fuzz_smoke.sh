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

# Self-tests first (fast, no binary needed)
PYTHONPATH=tools python3 -m naabfuzz selftest > /dev/null 2>&1 || {
    echo "FAIL: naabfuzz oracle self-tests failed"
    exit 1
}

rc=0
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
