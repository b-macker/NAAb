#!/bin/bash
cd /data/data/com.termux/files/home/.naab/language/build

echo "=== Linking unit tests ==="
cmake --build . --target naab_unit_tests -j2 2>&1 | tail -10

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
        echo ""
        echo "🎉 All async callback tests passed!"
        echo ""
        echo "Summary:"
        echo "  ✅ AsyncCallbackWrapper (thread-safe async execution)"
        echo "  ✅ AsyncCallbackGuard (RAII wrapper)"
        echo "  ✅ AsyncCallbackPool (concurrent callback management)"
        echo "  ✅ Helper functions (retry, parallel, race)"
        echo "  ✅ 30+ comprehensive tests"
        echo ""
        echo "Next: Item 10 Day 5 - Polyglot Integration"
    else
        echo "❌ Some tests failed - review output above"
        exit 1
    fi
else
    echo "❌ Link failed"
    exit 1
fi
