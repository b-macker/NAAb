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

python3 "$SCRIPT_DIR/diff_runner.py" \
    --naab "$NAAB" \
    --corpus "$SCRIPT_DIR/corpus.list" \
    --root "$ROOT" \
    "$@"
