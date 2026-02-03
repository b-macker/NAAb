#!/bin/bash
cd /data/data/com.termux/files/home/.naab/language/build

echo "=== Rebuilding naab_security ==="
cmake --build . --target naab_security -j2 2>&1 | tail -20

echo ""
echo "=== Building naab_unit_tests ==="
cmake --build . --target naab_unit_tests -j2 2>&1 | tail -30

if [ $? -eq 0 ]; then
    echo ""
    echo "✅ Build successful! Running tests..."
    echo ""
    ./naab_unit_tests --gtest_filter=FFIAsyncCallback*
else
    echo "❌ Build failed"
    exit 1
fi
