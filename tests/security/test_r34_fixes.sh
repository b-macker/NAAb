#!/bin/bash
# Security R34 verification tests
# V-CONC-006 (deepCopy cycle detection), V-GOV-023 Part 2 (VM collection governance), V-DOS-015 (polyglot serialization cycle detection)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
PASS=0; FAIL=0; TOTAL=0

check() {
    TOTAL=$((TOTAL + 1))
    if [ "$1" = "0" ]; then
        PASS=$((PASS + 1))
        echo "  PASS: T$TOTAL - $2"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL: T$TOTAL - $2"
    fi
}

SRC="$SCRIPT_DIR/../../src"
INC="$SCRIPT_DIR/../../include"

echo "=== R34 Security Fixes ==="
echo ""
echo "--- V-CONC-006: deepCopy cycle detection ---"

# T1: deepCopy has visited parameter
grep -q 'deepCopy.*visited' "$INC/naab/naab_val.h"
check $? "deepCopy declaration has visited parameter"

# T2: deepCopy throws on circular reference (not silent return)
grep -q 'circular reference detected' "$SRC/interpreter/naab_val.cpp"
check $? "deepCopy throws descriptive error on cycle"

# T3: deepCopy uses visited set (insert + check)
grep -q 'visited->insert' "$SRC/interpreter/naab_val.cpp"
check $? "deepCopy uses visited set for cycle detection"

# T4: deepCopy depth overflow throws (not returns *this)
grep -v '//' "$SRC/interpreter/naab_val.cpp" | grep -A2 'depth > 64' | grep -q 'throw'
check $? "deepCopy throws on depth overflow (not silent return)"

echo ""
echo "--- V-GOV-023 Part 2: VM collection governance ---"

# T5: OP_LIST has checkArraySize
grep -A5 'VM_CASE(OP_LIST)' "$SRC/vm/vm.cpp" | grep -q 'checkArraySize'
check $? "OP_LIST has checkArraySize"

# T6: OP_DICT has checkArraySize
grep -A5 'VM_CASE(OP_DICT)' "$SRC/vm/vm.cpp" | grep -q 'checkArraySize'
check $? "OP_DICT has checkArraySize"

# T7: OP_STRUCT_NEW has checkArraySize
grep -A10 'VM_CASE(OP_STRUCT_NEW)' "$SRC/vm/vm.cpp" | grep -q 'checkArraySize'
check $? "OP_STRUCT_NEW has checkArraySize"

# T8: limits.h is included in vm.cpp
grep -q '#include "naab/limits.h"' "$SRC/vm/vm.cpp"
check $? "vm.cpp includes limits.h"

echo ""
echo "--- V-DOS-015: polyglot serialization cycle detection ---"

# T9: serializeValueForLanguage has visited parameter
grep -q 'serializeValueForLanguage.*visited' "$INC/naab/interpreter.h"
check $? "serializeValueForLanguage declaration has visited parameter"

# T10: polyglot.cpp has list cycle detection
grep -q 'list_ptr' "$SRC/interpreter/polyglot.cpp"
check $? "polyglot serialization tracks list pointers"

# T11: polyglot.cpp has dict cycle detection
grep -q 'dict_ptr' "$SRC/interpreter/polyglot.cpp"
check $? "polyglot serialization tracks dict pointers"

# T12: polyglot cycle error has helpful message
grep -q 'Break the cycle before passing data' "$SRC/interpreter/polyglot.cpp"
check $? "polyglot cycle error has guidance"

# T13: recursive calls pass visited parameter
COUNT=$(grep -c 'depth + 1, visited)' "$SRC/interpreter/polyglot.cpp")
if [ "$COUNT" -ge 10 ]; then
    check 0 "All recursive serialization calls pass visited ($COUNT calls)"
else
    check 1 "All recursive serialization calls pass visited (only $COUNT, expected 10+)"
fi

echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="
exit $FAIL
