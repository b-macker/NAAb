#!/bin/bash

# Navigate to the application directory
cd "$(dirname "$0")"

echo "--- Running DPRE Tests ---"

# Define NAAB_LANG_PATH to find modules
export NAAB_LANG_PATH=$(pwd)

NAAB_CMD="../build/naab-lang run engine.naab"

# --- Test Case 1: CSV Input, Console Output, Verbose ---
echo -e "\n--- Test 1: CSV Input, Console Output (Verbose) ---"
$NAAB_CMD --input sample_data/sample.csv --output-format console --verbose \
    | tee results/test1_output.txt
if grep -q "DPRE: Processing pipeline completed successfully." results/test1_output.txt; then
    echo "Test 1 PASSED"
else
    echo "Test 1 FAILED"
fi

# --- Test Case 2: CSV Input, JSON File Output ---
echo -e "\n--- Test 2: CSV Input, JSON File Output ---"
mkdir -p results
$NAAB_CMD --input sample_data/sample.csv --output-format json --output-file results/report_csv.json \
    | tee results/test2_output.txt
if grep -q "DPRE: Report generated and outputted successfully." results/test2_output.txt && [ -f results/report_csv.json ]; then
    echo "Test 2 PASSED"
else
    echo "Test 2 FAILED"
fi

# --- Test Case 3: JSON Input, Console Output ---
echo -e "\n--- Test 3: JSON Input, Console Output ---"
$NAAB_CMD --input sample_data/sample.json --output-format console \
    | tee results/test3_output.txt
if grep -q "DPRE: Processing pipeline completed successfully." results/test3_output.txt; then
    echo "Test 3 PASSED"
else
    echo "Test 3 FAILED"
fi

# --- Test Case 4: Missing Input Argument ---
echo -e "\n--- Test 4: Missing Input Argument ---"
$NAAB_CMD --output-format console 2>&1 \
    | tee results/test4_output.txt
if grep -q "Input file must be specified using --input." results/test4_output.txt; then
    echo "Test 4 PASSED"
else
    echo "Test 4 FAILED"
fi

# --- Test Case 5: Invalid Output Format ---
echo -e "\n--- Test 5: Invalid Output Format ---"
$NAAB_CMD --input sample_data/sample.csv --output-format invalid 2>&1 \
    | tee results/test5_output.txt
if grep -q "Invalid output format. Must be 'console', 'json', or 'csv'." results/test5_output.txt; then
    echo "Test 5 PASSED"
else
    echo "Test 5 FAILED"
fi

# Clean up generated files for subsequent runs
echo -e "\n--- Cleaning up results ---"
rm -f results/report_csv.json
rm -f results/*.txt

echo "--- DPRE Tests Finished ---"
