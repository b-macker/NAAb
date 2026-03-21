#!/bin/bash
# Meta-Test Layer 4: Coverage Verification
# Cross-references stdlib C++ source against test file usage

DIR="tests/robustness"
SRC="src/stdlib"

echo "═══════════════════════════════════════════════════════════"
echo "  Layer 4: Coverage Verification"
echo "═══════════════════════════════════════════════════════════"
echo ""

TOTAL_TESTED=0
TOTAL_AVAILABLE=0

# Helper: check which functions from a module are tested
check_module() {
    local module="$1"
    local src_file="$2"
    local test_file="$3"
    shift 3
    local expected_fns=("$@")

    local tested=0
    local available=${#expected_fns[@]}
    local missing=""

    for fn in "${expected_fns[@]}"; do
        if grep -q "${module}\.${fn}" "$test_file" 2>/dev/null; then
            tested=$((tested + 1))
        else
            # Check all test files as fallback (includes module.fn and .fn dot notation)
            if grep -rq "${module}\.${fn}\|\.${fn}(" "$DIR"/test_*.naab 2>/dev/null; then
                tested=$((tested + 1))
            else
                missing="$missing $fn"
            fi
        fi
    done

    TOTAL_TESTED=$((TOTAL_TESTED + tested))
    TOTAL_AVAILABLE=$((TOTAL_AVAILABLE + available))

    if [ "$tested" -eq "$available" ]; then
        printf "  %-10s %d/%d functions tested (FULL)\n" "$module:" "$tested" "$available"
    else
        printf "  %-10s %d/%d functions tested" "$module:" "$tested" "$available"
        echo " (MISSING:$missing)"
    fi
}

# ── Array Module ──
check_module "array" "$SRC/array_impl.cpp" "$DIR/test_stdlib_array.naab" \
    length push pop shift unshift first last \
    map_fn filter_fn reduce_fn find \
    slice reverse sort contains join

# ── String Module ──
check_module "string" "$SRC/string_impl.cpp" "$DIR/test_stdlib_string.naab" \
    length substring concat split join trim \
    upper lower replace contains starts_with ends_with \
    index_of repeat char_at reverse format pad_left pad_right

# ── Math Module ──
check_module "math" "$SRC/math_impl.cpp" "$DIR/test_stdlib_math_json.naab" \
    PI E pi e abs sqrt pow floor ceil round min max sin cos tan

# ── JSON Module ──
check_module "json" "$SRC/json_impl.cpp" "$DIR/test_stdlib_math_json.naab" \
    parse stringify parse_object parse_array is_valid pretty

# ── Regex Module ──
check_module "regex" "$SRC/regex_impl.cpp" "$DIR/test_stdlib_math_json.naab" \
    matches search find find_all replace replace_first split \
    groups find_groups escape is_valid compile_pattern

# ── Env Module ──
check_module "env" "$SRC/env_impl.cpp" "$DIR/test_stdlib_env_time.naab" \
    get set_var has delete_var get_all \
    load_dotenv parse_env_file get_int get_float get_bool get_args list

# ── Time Module ──
check_module "time" "$SRC/time_impl.cpp" "$DIR/test_stdlib_env_time.naab" \
    now now_millis sleep format_timestamp parse_datetime \
    year month day hour minute second weekday

echo ""

# ── Operator Coverage ──
echo "  --- Operator Coverage ---"
OP_FILE="$DIR/test_operators_matrix.naab"
OP_TESTED=0
OP_TOTAL=0

check_op() {
    local name="$1"
    local pattern="$2"
    OP_TOTAL=$((OP_TOTAL + 1))
    if grep -qP "$pattern" "$OP_FILE" 2>/dev/null; then
        OP_TESTED=$((OP_TESTED + 1))
    else
        echo "    MISSING operator: $name"
    fi
}

check_op "Add (+)"        ' \+ '
check_op "Sub (-)"        '[0-9] - [0-9]'
check_op "Mul (*)"        ' \* '
check_op "Div (/)"        ' / '
check_op "Mod (%)"        ' % '
check_op "Eq (==)"        '=='
check_op "Ne (!=)"        '!='
check_op "Lt (<)"         ' < '
check_op "Gt (>)"         ' > '
check_op "Le (<=)"        '<='
check_op "Ge (>=)"        '>='
check_op "And (&&)"       '&&'
check_op "Or (||)"        '\|\|'
check_op "NullCoalesce"   '\?\?'
check_op "Pipeline (|>)"  '\|>'
check_op "Not (!)"        '!true|!false|!null|!array|!ran|!env|!catch'
check_op "Negate (-x)"    ' -[0-9]|\(-[0-9]|= -[0-9]|let .* = -'

printf "  Operators: %d/%d covered\n" "$OP_TESTED" "$OP_TOTAL"

echo ""

# ── Structural Pattern Coverage ──
echo "  --- Pattern Coverage ---"
PAT_TESTED=0
PAT_TOTAL=0

check_pattern() {
    local name="$1"
    local pattern="$2"
    local file="$3"
    PAT_TOTAL=$((PAT_TOTAL + 1))
    if grep -qP "$pattern" "$file" 2>/dev/null; then
        PAT_TESTED=$((PAT_TESTED + 1))
    else
        echo "    MISSING pattern: $name in $(basename $file)"
    fi
}

check_pattern "lambda"    'fn\('            "$DIR/test_closures_scope.naab"
check_pattern "for-in"    'for .* in'       "$DIR/test_control_flow.naab"
check_pattern "while"     'while '          "$DIR/test_control_flow.naab"
check_pattern "break"     'break'           "$DIR/test_control_flow.naab"
check_pattern "continue"  'continue'        "$DIR/test_control_flow.naab"
check_pattern "try"       'try \{'          "$DIR/test_control_flow.naab"
check_pattern "catch"     'catch '          "$DIR/test_control_flow.naab"
check_pattern "match"     'match '          "$DIR/test_structs_enums.naab"
check_pattern "struct"    'struct '         "$DIR/test_structs_enums.naab"
check_pattern "enum"      'enum '           "$DIR/test_structs_enums.naab"
check_pattern "new"       'new '            "$DIR/test_structs_enums.naab"

printf "  Patterns: %d/%d present\n" "$PAT_TESTED" "$PAT_TOTAL"

echo ""

# ── Summary ──
COVERAGE_PCT=0
if [ "$TOTAL_AVAILABLE" -gt 0 ]; then
    COVERAGE_PCT=$((TOTAL_TESTED * 100 / TOTAL_AVAILABLE))
fi

echo "═══════════════════════════════════════════════════════════"
echo "  COVERAGE SUMMARY"
echo "═══════════════════════════════════════════════════════════"
echo "  Stdlib functions: $TOTAL_TESTED/$TOTAL_AVAILABLE ($COVERAGE_PCT%)"
echo "  Operators:        $OP_TESTED/$OP_TOTAL"
echo "  Patterns:         $PAT_TESTED/$PAT_TOTAL"
echo ""
echo "  NOTE: Missing functions may not be implemented yet"
echo "        or may require external dependencies"
echo "═══════════════════════════════════════════════════════════"
