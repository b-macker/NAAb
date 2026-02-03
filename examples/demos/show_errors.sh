#!/bin/bash
cd /data/data/com.termux/files/home/.naab/language/build

echo "=== Building unit tests (showing all errors) ==="
cmake --build . --target naab_unit_tests -j1 2>&1 | grep -E "(error:|Building.*ffi_async_callback_test|Linking)" | head -30

echo ""
echo "=== Checking if test file was compiled ==="
find . -name "ffi_async_callback_test.cpp.o" 2>/dev/null

if [ -f "CMakeFiles/naab_unit_tests.dir/tests/unit/ffi_async_callback_test.cpp.o" ]; then
    echo "✅ Test object file exists"
else
    echo "❌ Test object file NOT created - compilation failed"
    echo ""
    echo "=== Last 50 lines of build output ==="
    cmake --build . --target naab_unit_tests 2>&1 | tail -50
fi
