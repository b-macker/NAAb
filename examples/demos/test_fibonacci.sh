#!/bin/bash

echo "--- Running Fibonacci Tests ---"

# Define NAAB_LANG_PATH to find modules
export NAAB_LANG_PATH=$(pwd)

NAAB_CMD="./build/naab-lang run fibonacci.naab"

# Run the NAAb Fibonacci program and capture its output
output=$($NAAB_CMD 2>&1)
echo "$output"

# --- Verification ---

# Test 1: Verify NAAb calculations
echo -e "\n--- Verifying NAAb Calculations ---"
if echo "$output" | grep -q "NAAb F(0) = 0"; then echo "Test 1.1 PASSED: F(0) correct"; else echo "Test 1.1 FAILED: F(0) incorrect"; fi
if echo "$output" | grep -q "NAAb F(1) = 1"; then echo "Test 1.2 PASSED: F(1) correct"; else echo "Test 1.2 FAILED: F(1) incorrect"; fi
if echo "$output" | grep -q "NAAb F(5) = 5"; then echo "Test 1.3 PASSED: F(5) correct"; else echo "Test 1.3 FAILED: F(5) incorrect"; fi
if echo "$output" | grep -q "NAAb F(10) = 55"; then echo "Test 1.4 PASSED: F(10) correct"; else echo "Test 1.4 FAILED: F(10) incorrect"; fi
if echo "$output" | grep -q "NAAb F(15) = 610"; then echo "Test 1.5 PASSED: F(15) correct"; else echo "Test 1.5 FAILED: F(15) incorrect"; fi

# Test 2: Verify Python Polyglot Comparison
echo -e "\n--- Verifying Python Polyglot ---"
if echo "$output" | grep -q "Python F(10) = 55"; then echo "Test 2.1 PASSED: Python F(10) correct"; else echo "Test 2.1 FAILED: Python F(10) incorrect"; fi
if echo "$output" | grep -q "Comparison (NAAb vs Python): NAAb F(10) = 55, Python F(10) = 55"; then echo "Test 2.2 PASSED: Comparison output correct"; else echo "Test 2.2 FAILED: Comparison output incorrect"; fi

# Test 3: Verify Error Handling for negative input
echo -e "\n--- Verifying Error Handling ---"
if echo "$output" | grep -q "Error: Input must be a non-negative integer for Fibonacci calculation."; then
    echo "Test 3.1 PASSED: Error message for negative input found."
else
    echo "Test 3.1 FAILED: Error message for negative input not found."
fi
if ! echo "$output" | grep -q "This line should not be reached."; then
    echo "Test 3.2 PASSED: Program terminated before 'not reached' line."
else
    echo "Test 3.2 FAILED: Program reached 'not reached' line."
fi

echo -e "\n--- Fibonacci Tests Finished ---"
