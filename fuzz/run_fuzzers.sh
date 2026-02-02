#!/bin/bash
# Convenience script for running NAAb fuzzers
# Week 2, Task 2.1-2.2: Fuzzing Infrastructure

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build-fuzz"
FUZZ_DIR="${BUILD_DIR}/fuzz"

# Default fuzzing time (seconds)
FUZZ_TIME="${FUZZ_TIME:-60}"

# Check if fuzzers are built
if [ ! -d "$FUZZ_DIR" ]; then
    echo "Error: Fuzzers not built. Please run:"
    echo "  cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++"
    echo "  cmake --build build-fuzz"
    exit 1
fi

echo "========================================="
echo "NAAb Fuzzing Campaign"
echo "========================================="
echo "Fuzzing time per target: ${FUZZ_TIME}s"
echo "Build directory: ${BUILD_DIR}"
echo ""

# Function to run a fuzzer
run_fuzzer() {
    local name=$1
    local corpus=$2
    local binary="${FUZZ_DIR}/${name}"

    if [ ! -f "$binary" ]; then
        echo "⚠️  Skipping ${name} (not built)"
        return
    fi

    echo "Running ${name}..."
    echo "  Corpus: ${corpus}"

    "$binary" \
        -max_total_time="${FUZZ_TIME}" \
        -timeout=10 \
        -print_final_stats=1 \
        "${SCRIPT_DIR}/${corpus}" \
        2>&1 | tail -20

    echo "✓ ${name} complete"
    echo ""
}

# Run core language fuzzers
echo "📦 Core Language Fuzzers"
echo "-------------------------"
run_fuzzer "fuzz_lexer" "corpus/lexer"
run_fuzzer "fuzz_parser" "corpus/parser"
run_fuzzer "fuzz_interpreter" "corpus/interpreter"

# Run FFI boundary fuzzers
echo "🔌 FFI Boundary Fuzzers"
echo "------------------------"
run_fuzzer "fuzz_python_executor" "corpus/python"
run_fuzzer "fuzz_value_conversion" "corpus/values"
run_fuzzer "fuzz_json_marshal" "corpus/json"

echo "========================================="
echo "Fuzzing campaign complete!"
echo "========================================="
echo ""
echo "Results:"
echo "  • Check for crash-* files in current directory"
echo "  • Review fuzzer output above for coverage stats"
echo ""
echo "To run longer fuzzing campaign:"
echo "  FUZZ_TIME=3600 ./fuzz/run_fuzzers.sh"
echo ""
echo "To run individual fuzzer:"
echo "  ${FUZZ_DIR}/fuzz_parser -max_total_time=3600 ${SCRIPT_DIR}/corpus/parser/"
