#!/bin/bash
# Meta-Test Layer 1+5: Static Integrity Audit + Runtime Count Verification
# Validates that robustness test files are structurally correct and produce expected results

NAAB_BIN="./build/naab-lang"
DIR="tests/robustness"
LAYER1_PASS=0
LAYER1_TOTAL=7
LAYER5_PASS=0
LAYER5_TOTAL=8

# Files to validate (only the 8 new exhaustive test files)
TEST_FILES=(
    "test_stdlib_array"
    "test_stdlib_string"
    "test_stdlib_math_json"
    "test_operators_matrix"
    "test_closures_scope"
    "test_control_flow"
    "test_structs_enums"
    "test_stdlib_env_time"
)

# Expected runtime summary lines (Layer 5 manifest)
declare -A EXPECTED_SUMMARY
EXPECTED_SUMMARY["test_stdlib_array"]="Stdlib Array: 40/40"
EXPECTED_SUMMARY["test_stdlib_string"]="Stdlib String: 50/50"
EXPECTED_SUMMARY["test_stdlib_math_json"]="Stdlib Math/JSON/Regex: 49/49"
EXPECTED_SUMMARY["test_operators_matrix"]="Operators Matrix: 102/102"
EXPECTED_SUMMARY["test_closures_scope"]="Closures/Scope: 43/43"
EXPECTED_SUMMARY["test_control_flow"]="Control Flow: 39/39"
EXPECTED_SUMMARY["test_structs_enums"]="Structs/Enums: 40/40"
EXPECTED_SUMMARY["test_stdlib_env_time"]="Stdlib Env/Time: 33/33"

# Expected assertion counts per file
declare -A EXPECTED_COUNT
EXPECTED_COUNT["test_stdlib_array"]=40
EXPECTED_COUNT["test_stdlib_string"]=50
EXPECTED_COUNT["test_stdlib_math_json"]=49
EXPECTED_COUNT["test_operators_matrix"]=102
EXPECTED_COUNT["test_closures_scope"]=43
EXPECTED_COUNT["test_control_flow"]=39
EXPECTED_COUNT["test_structs_enums"]=40
EXPECTED_COUNT["test_stdlib_env_time"]=33

echo "═══════════════════════════════════════════════════════════"
echo "  Layer 1: Static Integrity Audit"
echo "═══════════════════════════════════════════════════════════"
echo ""

# ─── Check 1.1: No Trivial Assertions ───
trivial_count=0
for name in "${TEST_FILES[@]}"; do
    file="$DIR/${name}.naab"
    # Look for trivially-true conditions before "passed = passed + 1"
    # Check: if true {, if 1 {, if !false {
    hits=$(grep -B1 "passed = passed + 1" "$file" | grep -cP '^\s*if\s+(true|1|!false)\s*\{')
    trivial_count=$((trivial_count + hits))
    if [ "$hits" -gt 0 ]; then
        echo "  WARNING: $file has $hits trivial assertion(s)"
        grep -n -B1 "passed = passed + 1" "$file" | grep -P '^\s*if\s+(true|1|!false)\s*\{'
    fi
done
if [ "$trivial_count" -eq 0 ]; then
    echo "  1.1 Trivial assertions:    0 found (PASS)"
    LAYER1_PASS=$((LAYER1_PASS + 1))
else
    echo "  1.1 Trivial assertions:    $trivial_count found (FAIL)"
fi

# ─── Check 1.2: Total/Passed Count Balance ───
balance_ok=0
balance_total=${#TEST_FILES[@]}
for name in "${TEST_FILES[@]}"; do
    file="$DIR/${name}.naab"
    total_count=$(grep -c "total = total + 1" "$file")
    passed_count=$(grep -c "passed = passed + 1" "$file")
    expected=${EXPECTED_COUNT[$name]}
    if [ "$total_count" -ne "$passed_count" ]; then
        echo "  FAIL: $name total=$total_count passed=$passed_count MISMATCH"
    elif [ "$total_count" -ne "$expected" ]; then
        echo "  FAIL: $name count=$total_count expected=$expected DRIFT"
    else
        balance_ok=$((balance_ok + 1))
    fi
done
if [ "$balance_ok" -eq "$balance_total" ]; then
    echo "  1.2 Total/passed balance:  $balance_ok/$balance_total files balanced (PASS)"
    LAYER1_PASS=$((LAYER1_PASS + 1))
else
    echo "  1.2 Total/passed balance:  $balance_ok/$balance_total files balanced (FAIL)"
fi

# ─── Check 1.3: Every Test Function Has Assertions ───
empty_fns=0
for name in "${TEST_FILES[@]}"; do
    file="$DIR/${name}.naab"
    # Extract test function names
    fn_names=$(grep -oP 'fn (test_\w+)' "$file" | awk '{print $2}')
    for fn in $fn_names; do
        # Use awk to extract function body and count assertions
        count=$(awk "/fn ${fn}\\(/{found=1} found{print} found && /^}/{found=0}" "$file" | grep -c "total = total + 1")
        if [ "$count" -eq 0 ]; then
            echo "  EMPTY: $name::$fn has 0 assertions"
            empty_fns=$((empty_fns + 1))
        fi
    done
done
if [ "$empty_fns" -eq 0 ]; then
    echo "  1.3 Empty test functions:  0 found (PASS)"
    LAYER1_PASS=$((LAYER1_PASS + 1))
else
    echo "  1.3 Empty test functions:  $empty_fns found (FAIL)"
fi

# ─── Check 1.4: Every Test Function Returns [passed, total] ───
missing_returns=0
for name in "${TEST_FILES[@]}"; do
    file="$DIR/${name}.naab"
    fn_names=$(grep -oP 'fn (test_\w+)' "$file" | awk '{print $2}')
    for fn in $fn_names; do
        has_return=$(awk "/fn ${fn}\\(/{found=1} found{print} found && /^}/{found=0}" "$file" | grep -c "return \[passed, total\]")
        if [ "$has_return" -eq 0 ]; then
            echo "  MISSING RETURN: $name::$fn"
            missing_returns=$((missing_returns + 1))
        fi
    done
done
if [ "$missing_returns" -eq 0 ]; then
    echo "  1.4 Missing returns:       0 found (PASS)"
    LAYER1_PASS=$((LAYER1_PASS + 1))
else
    echo "  1.4 Missing returns:       $missing_returns found (FAIL)"
fi

# ─── Check 1.5: Every Test Function Is Called in Main ───
uncalled=0
for name in "${TEST_FILES[@]}"; do
    file="$DIR/${name}.naab"
    fn_names=$(grep -oP 'fn (test_\w+)' "$file" | awk '{print $2}')
    # Extract main block
    main_block=$(awk '/^main \{/{found=1} found{print} found && /^\}/{found=0}' "$file")
    for fn in $fn_names; do
        if ! echo "$main_block" | grep -q "$fn()"; then
            echo "  UNCALLED: $name::$fn not called in main"
            uncalled=$((uncalled + 1))
        fi
    done
done
if [ "$uncalled" -eq 0 ]; then
    echo "  1.5 Uncalled functions:    0 found (PASS)"
    LAYER1_PASS=$((LAYER1_PASS + 1))
else
    echo "  1.5 Uncalled functions:    $uncalled found (FAIL)"
fi

# ─── Check 1.6: Summary Line Exists ───
missing_summary=0
for name in "${TEST_FILES[@]}"; do
    file="$DIR/${name}.naab"
    has_summary=$(grep -c 'print.*total_passed.*total_tests' "$file")
    if [ "$has_summary" -eq 0 ]; then
        echo "  MISSING SUMMARY: $name"
        missing_summary=$((missing_summary + 1))
    fi
done
if [ "$missing_summary" -eq 0 ]; then
    echo "  1.6 Summary lines:         ${#TEST_FILES[@]}/${#TEST_FILES[@]} present (PASS)"
    LAYER1_PASS=$((LAYER1_PASS + 1))
else
    echo "  1.6 Summary lines:         $((${#TEST_FILES[@]} - missing_summary))/${#TEST_FILES[@]} present (FAIL)"
fi

# ─── Check 1.7: No Duplicate Test Names ───
dup_count=0
for name in "${TEST_FILES[@]}"; do
    file="$DIR/${name}.naab"
    dups=$(grep -oP 'fn (test_\w+)' "$file" | sort | uniq -d)
    if [ -n "$dups" ]; then
        echo "  DUPLICATE in $name: $dups"
        dup_count=$((dup_count + 1))
    fi
done
if [ "$dup_count" -eq 0 ]; then
    echo "  1.7 Duplicate names:       0 found (PASS)"
    LAYER1_PASS=$((LAYER1_PASS + 1))
else
    echo "  1.7 Duplicate names:       $dup_count found (FAIL)"
fi

echo ""
echo "  Static Audit: $LAYER1_PASS/$LAYER1_TOTAL checks PASSED"

# ═══════════════════════════════════════════════════════════
echo ""
echo "═══════════════════════════════════════════════════════════"
echo "  Layer 5: Runtime Count Verification"
echo "═══════════════════════════════════════════════════════════"
echo ""

for name in "${TEST_FILES[@]}"; do
    file="$DIR/${name}.naab"
    expected="${EXPECTED_SUMMARY[$name]}"

    output=$(timeout 60s "$NAAB_BIN" "$file" 2>&1)
    exit_code=$?

    if [ $exit_code -ne 0 ]; then
        echo "  $name: CRASH (exit $exit_code) (FAIL)"
        continue
    fi

    # Extract summary line
    summary=$(echo "$output" | grep -E "^[A-Z].*: [0-9]+/[0-9]+" | tail -1)

    if [ -z "$summary" ]; then
        echo "  $name: NO SUMMARY OUTPUT (FAIL)"
        continue
    fi

    if [ "$summary" = "$expected" ]; then
        printf "  %-25s %-40s MATCH\n" "$name:" "\"$summary\""
        LAYER5_PASS=$((LAYER5_PASS + 1))
    else
        printf "  %-25s MISMATCH\n" "$name:"
        echo "    Expected: \"$expected\""
        echo "    Got:      \"$summary\""
    fi

    # Also check sub-function lines: verify pass==total for each
    subfails=$(echo "$output" | grep -oP 'T\d+\.\d+ \w+: (\d+)/(\d+)' | while IFS=: read -r label counts; do
        pass=$(echo "$counts" | grep -oP '^\s*(\d+)/' | tr -d '/ ')
        total=$(echo "$counts" | grep -oP '/(\d+)' | tr -d '/')
        if [ "$pass" != "$total" ]; then
            echo "    SUB-FAIL: $label: $counts"
        fi
    done)
    if [ -n "$subfails" ]; then
        echo "$subfails"
    fi
done

echo ""
echo "  Runtime Verification: $LAYER5_PASS/$LAYER5_TOTAL files match manifest"

# ═══════════════════════════════════════════════════════════
echo ""
echo "═══════════════════════════════════════════════════════════"
TOTAL_PASS=$((LAYER1_PASS + LAYER5_PASS))
TOTAL_CHECKS=$((LAYER1_TOTAL + LAYER5_TOTAL))
echo "  INTEGRITY SUMMARY: $TOTAL_PASS/$TOTAL_CHECKS checks passed"
if [ "$TOTAL_PASS" -eq "$TOTAL_CHECKS" ]; then
    echo "  ALL INTEGRITY CHECKS PASSED"
else
    echo "  $((TOTAL_CHECKS - TOTAL_PASS)) FAILURES"
fi
echo "═══════════════════════════════════════════════════════════"
