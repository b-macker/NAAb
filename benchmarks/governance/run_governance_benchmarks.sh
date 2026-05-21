#!/usr/bin/env bash
# run_governance_benchmarks.sh — Build and run governance performance benchmarks
#
# Usage:
#   ./benchmarks/governance/run_governance_benchmarks.sh [build_dir]

set -e

LANG_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$LANG_DIR/build}"

if [ ! -f "$BUILD/bench_governance" ]; then
    echo "Building bench_governance..."
    cd "$BUILD"
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make bench_governance -j$(nproc 2>/dev/null || echo 2)
fi

echo ""
"$BUILD/bench_governance"
