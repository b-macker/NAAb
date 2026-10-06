#!/usr/bin/env bash
# run_differential.sh — Differential harness v2 (VM vs tree-walker)
#
# Runs every corpus program on both engines and compares normalized
# stdout + exit codes; error cases compare error CATEGORY only. Known
# divergences (divergences.json) are reported as KNOWN, non-failing.
#
# Complements tests/vm/test_vm_treewalker_diff.sh (governance decision
# parity) — this suite covers output/semantics parity.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$ROOT/build/naab-lang}"

if [ ! -x "$NAAB" ]; then
    echo "Error: naab-lang binary not found at $NAAB"
    echo "Run 'cd build && make naab-lang' first"
    exit 1
fi

echo "=== Differential Harness v2: VM vs Tree-Walker ==="

# Known-answer probe. Parity alone cannot fail when both engines produce
# nothing: two dead engines "agree" on every corpus program. Each engine must
# first print the answer to a program whose output is not in doubt.
PROBE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/naab-diff-probe.XXXXXX")"
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

python3 "$SCRIPT_DIR/diff_runner.py" \
    --naab "$NAAB" \
    --corpus "$SCRIPT_DIR/corpus.list" \
    --root "$ROOT" \
    "$@"
