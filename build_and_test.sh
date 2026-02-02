#!/bin/bash
cd /data/data/com.termux/files/home/.naab/language/build

echo "=== Building unit tests ==="
cmake --build . --target naab_unit_tests -j2

if [ $? -eq 0 ]; then
    echo ""
    echo "✅ Build successful!"
    echo ""
    echo "=== Running FFI Async Callback Tests ==="
    ./naab_unit_tests --gtest_filter=FFIAsyncCallback*

    TEST_RESULT=$?

    if [ $TEST_RESULT -eq 0 ]; then
        echo ""
        echo "╔════════════════════════════════════════════════════════════╗"
        echo "║   ✅ Item 10 Day 4: Async Callback Framework COMPLETE! ✅  ║"
        echo "╚════════════════════════════════════════════════════════════╝"
    else
        echo "❌ Some tests failed"
        exit 1
    fi
else
    echo "❌ Build failed"
    exit 1
fi
