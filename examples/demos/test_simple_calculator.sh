#!/bin/bash

echo "--- Running Simple Calculator Tests ---"

# Define NAAB_LANG_PATH to find modules (though not strictly needed for this single file)
export NAAB_LANG_PATH=$(pwd)

NAAB_CMD="./build/naab-lang run simple_calculator.naab"

# Run the NAAb calculator program and capture its output
output=$($NAAB_CMD 2>&1)
echo "$output"

# --- Verification ---

# Test 1: Verify NAAb Addition
echo -e "\n--- Verifying NAAb Calculations ---"
if echo "$output" | grep -q "Addition: 10 + 5 = 15"; then echo "Test 1.1 PASSED: Addition correct"; else echo "Test 1.1 FAILED: Addition incorrect"; fi

# Test 2: Verify NAAb Subtraction
if echo "$output" | grep -q "Subtraction: 10 - 5 = 5"; then echo "Test 1.2 PASSED: Subtraction correct"; else echo "Test 1.2 FAILED: Subtraction incorrect"; fi

# Test 3: Verify NAAb Multiplication
if echo "$output" | grep -q "Multiplication: 10 \* 5 = 50"; then echo "Test 1.3 PASSED: Multiplication correct"; else echo "Test 1.3 FAILED: Multiplication incorrect"; fi

# Test 5: Verify NAAb Mixed Operations
if echo "$output" | grep -q "Mixed: (10 + 5) \* 10 / 5 = 30.000000"; then echo "Test 1.5 PASSED: Mixed operations correct"; else echo "Test 1.5 FAILED: Mixed operations incorrect"; fi

# Test 6: Verify Python Polyglot Addition
echo -e "\n--- Verifying Python Polyglot ---"
if echo "$output" | grep -q "Python Addition: 20 + 10 = 30"; then echo "Test 2.1 PASSED: Python Addition correct"; else echo "Test 2.1 FAILED: Python Addition incorrect"; fi

echo -e "\n--- Simple Calculator Tests Finished ---"
