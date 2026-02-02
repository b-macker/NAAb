#!/bin/bash
# Build script for Item 10 Day 4: Async Callback Framework

cd /data/data/com.termux/files/home/.naab/language

echo "=== Building naab_security library with async callbacks ==="
cmake --build build --target naab_security -j2

if [ $? -eq 0 ]; then
    echo "✅ naab_security library built successfully"
else
    echo "❌ Build failed"
    exit 1
fi

echo ""
echo "=== Building unit tests ==="
cmake --build build --target naab_unit_tests -j2

if [ $? -eq 0 ]; then
    echo "✅ Unit tests built successfully"
else
    echo "❌ Test build failed"
    exit 1
fi

echo ""
echo "=== Running FFI async callback tests ==="
cd build
./naab_unit_tests --gtest_filter=FFIAsyncCallback*

if [ $? -eq 0 ]; then
    echo ""
    echo "╔════════════════════════════════════════════════════════════╗"
    echo "║   ✅ Item 10 Day 4: Async Callback Framework COMPLETE! ✅  ║"
    echo "╚════════════════════════════════════════════════════════════╝"
else
    echo "❌ Tests failed"
    exit 1
fi
