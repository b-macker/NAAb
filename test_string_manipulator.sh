#!/bin/bash

echo "--- Running String Manipulator Tests ---"

# Define NAAB_LANG_PATH to find modules (though not strictly needed for this single file)
export NAAB_LANG_PATH=$(pwd)

NAAB_CMD="./build/naab-lang run string_manipulator.naab"

# Run the NAAb string manipulator program and capture its output
output=$($NAAB_CMD 2>&1)
echo "$output"

# --- Verification ---

# Test 1: Verify Original String
echo -e "\n--- Verifying String Manipulations ---"
if echo "$output" | grep -q "Original String: 'Hello, NAAb Language!'"; then echo "Test 1.1 PASSED: Original string correct"; else echo "Test 1.1 FAILED: Original string incorrect"; fi

# Test 2: Verify Upper Case
if echo "$output" | grep -q "Upper Case:      'HELLO, NAAB LANGUAGE!'"; then echo "Test 1.2 PASSED: Upper case correct"; else echo "Test 1.2 FAILED: Upper case incorrect"; fi

# Test 3: Verify Lower Case
if echo "$output" | grep -q "Lower Case:      'hello, naab language!'"; then echo "Test 1.3 PASSED: Lower case correct"; else echo "Test 1.3 FAILED: Lower case incorrect"; fi

# Test 4: Verify Concatenation
# Concatenated:    'Concatenated String!'
# Note: io.write adds spaces between arguments, so " " is "  " in output
if echo "$output" | grep -q "Concatenated:    'Concatenated  String!'"; then echo "Test 1.4 PASSED: Concatenation correct"; else echo "Test 1.4 FAILED: Concatenation incorrect"; fi

# Test 5: Verify Substring
if echo "$output" | grep -q "Substring (7,10):'NAA'"; then echo "Test 1.5 PASSED: Substring correct"; else echo "Test 1.5 FAILED: Substring incorrect"; fi

# Test 6: Verify Length
if echo "$output" | grep -q "Length:          21"; then echo "Test 1.6 PASSED: Length correct"; else echo "Test 1.6 FAILED: Length incorrect"; fi

# Test 7: Verify Starts With
if echo "$output" | grep -q "Starts with 'Hello': true"; then echo "Test 1.7 PASSED: Starts With correct"; else echo "Test 1.7 FAILED: Starts With incorrect"; fi

# Test 8: Verify Ends With
if echo "$output" | grep -q "Ends with 'Language!': true"; then echo "Test 1.8 PASSED: Ends With correct"; else echo "Test 1.8 FAILED: Ends With incorrect"; fi


echo -e "\n--- String Manipulator Tests Finished ---"
