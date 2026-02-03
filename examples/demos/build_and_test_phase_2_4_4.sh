#!/bin/bash
# Build and test Phase 2.4.4 implementations

set -e  # Exit on error

echo "=== Building NAAb with Phase 2.4.4 changes ==="
cd /data/data/com.termux/files/home/.naab/language/build

echo "Step 1: Building naab-lang..."
make -j4 naab-lang

echo ""
echo "=== Build successful! ==="
echo ""

# Check if binary was updated
if [ -f "./naab-lang" ]; then
    echo "Binary size: $(du -h naab-lang | cut -f1)"
    echo "Last modified: $(stat -c '%y' naab-lang)"
    echo ""
fi

echo "=== Running Phase 2.4.4 Tests ==="
echo ""

# Test 1: Function Return Type Inference
echo "--- Test 1: Function Return Type Inference ---"
if [ -f "../examples/test_function_return_inference.naab" ]; then
    ./naab-lang ../examples/test_function_return_inference.naab
    echo ""
else
    echo "Test file not found: test_function_return_inference.naab"
fi

# Test 2: Generic Argument Inference
echo "--- Test 2: Generic Argument Inference ---"
if [ -f "../examples/test_generic_argument_inference.naab" ]; then
    ./naab-lang ../examples/test_generic_argument_inference.naab
    echo ""
else
    echo "Test file not found: test_generic_argument_inference.naab"
fi

echo "=== All tests completed ==="
echo ""
echo "Check the output above for:"
echo "  - [INFO] Inferred return type messages"
echo "  - [INFO] Inferred type argument messages"
echo "  - Test results and outputs"
