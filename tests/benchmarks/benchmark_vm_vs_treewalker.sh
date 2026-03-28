#!/bin/bash
# Benchmark: VM vs Tree-Walker performance comparison
# Usage: bash tests/benchmarks/benchmark_vm_vs_treewalker.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB_BIN="$PROJECT_DIR/build/naab-lang"
BENCH_FILE="$SCRIPT_DIR/benchmark_interpreter.naab"

if [ ! -x "$NAAB_BIN" ]; then
    echo "Error: naab-lang not found at $NAAB_BIN"
    echo "Run: cd build && cmake .. && make naab-lang -j4"
    exit 1
fi

echo "═══════════════════════════════════════════════════════════"
echo "  NAAb VM vs Tree-Walker Benchmark"
echo "═══════════════════════════════════════════════════════════"
echo ""

echo "--- Tree-Walker ---"
TW_START=$(date +%s%N)
"$NAAB_BIN" --tree-walk "$BENCH_FILE" 2>&1
TW_END=$(date +%s%N)
TW_MS=$(( (TW_END - TW_START) / 1000000 ))
echo ""
echo "Tree-Walker total: ${TW_MS}ms"
echo ""

echo "--- Bytecode VM ---"
VM_START=$(date +%s%N)
"$NAAB_BIN" --vm "$BENCH_FILE" 2>&1
VM_END=$(date +%s%N)
VM_MS=$(( (VM_END - VM_START) / 1000000 ))
echo ""
echo "VM total: ${VM_MS}ms"
echo ""

echo "═══════════════════════════════════════════════════════════"
echo "  Results"
echo "═══════════════════════════════════════════════════════════"
echo "  Tree-Walker: ${TW_MS}ms"
echo "  Bytecode VM: ${VM_MS}ms"
if [ "$VM_MS" -gt 0 ] && [ "$TW_MS" -gt 0 ]; then
    # Calculate speedup as percentage
    if [ "$VM_MS" -lt "$TW_MS" ]; then
        SPEEDUP=$(( (TW_MS - VM_MS) * 100 / TW_MS ))
        echo "  VM is ${SPEEDUP}% faster"
    elif [ "$VM_MS" -gt "$TW_MS" ]; then
        SLOWDOWN=$(( (VM_MS - TW_MS) * 100 / TW_MS ))
        echo "  VM is ${SLOWDOWN}% slower"
    else
        echo "  Same speed"
    fi
fi
echo "═══════════════════════════════════════════════════════════"
