#!/bin/bash

echo "--- Running Word Counter Tests ---"

# Define NAAB_LANG_PATH to find modules
export NAAB_LANG_PATH=$(pwd)

NAAB_CMD="./build/naab-lang run word_counter.naab"

# --- Test Case 1: Count words in sample_text.txt ---
echo -e "\n--- Test 1: Counting words in sample_text.txt ---"
expected_count=22
output=$($NAAB_CMD sample_text.txt 2>&1)
echo "$output"

if echo "$output" | grep -q "Total word count: $expected_count"; then
    echo "Test 1 PASSED: Correct word count ($expected_count) found."
else
    echo "Test 1 FAILED: Expected word count $expected_count not found in output."
fi

# --- Test Case 2: Missing file path argument ---
echo -e "\n--- Test 2: Missing file path argument ---"
output=$($NAAB_CMD 2>&1)
echo "$output"

if echo "$output" | grep -q "Usage: naab run word_counter.naab <file_path>"; then
    echo "Test 2 PASSED: Usage message displayed for missing argument."
else
    echo "Test 2 FAILED: Usage message not displayed for missing argument."
fi

# --- Test Case 3: Non-existent file ---
echo -e "\n--- Test 3: Non-existent file ---"
output=$($NAAB_CMD non_existent_file.txt 2>&1)
echo "$output"

if echo "$output" | grep -q "Error: Could not read file 'non_existent_file.txt': No such file or directory"; then
    echo "Test 3 PASSED: Correct error message for non-existent file."
else
    echo "Test 3 FAILED: Expected error message for non-existent file not found."
fi

echo -e "\n--- Word Counter Tests Finished ---"
