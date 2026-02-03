#!/bin/bash
cd /data/data/com.termux/files/home/.naab/language/build

echo "Building unit tests..."
cmake --build . --target naab_unit_tests -j2 2>&1 | tail -15

if [ $? -eq 0 ]; then
    echo ""
    echo "✅ Build successful! Running tests..."
    echo ""
    ./naab_unit_tests --gtest_filter=FFIAsyncCallback*
else
    echo "❌ Build failed"
fi
