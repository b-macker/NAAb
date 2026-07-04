#!/bin/bash
# Invariant 7: Exact-Arithmetic Oracle
#
# Runs the naabfuzz oracle suite: fixed hand-audited arithmetic vectors,
# 150 deterministic generated programs (seed 42), and the metamorphic
# property suite — all executed on BOTH engines and compared against the
# Python exact-arithmetic oracle (int32 checked overflow + IEEE doubles).

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB_BIN="${NAAB_BIN:-$PROJECT_DIR/build/naab-lang}"

echo "--- Invariant 7: Exact-Arithmetic Oracle (VM + tree-walker) ---"

if [ ! -f "$NAAB_BIN" ]; then
    echo "Error: naab-lang binary not found at $NAAB_BIN"
    exit 1
fi

if ! command -v python3 > /dev/null 2>&1; then
    echo "SKIP: python3 not available"
    exit 0
fi

PYTHONPATH="$PROJECT_DIR/tools" python3 -m naabfuzz oracle \
    --naab "$NAAB_BIN" --vectors fixed --seed 42 --count 150 --prop-count 5
