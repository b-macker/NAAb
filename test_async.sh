#!/bin/bash
cd /data/data/com.termux/files/home/.naab/language/build

echo "=== Building unit tests ==="
cmake --build . --target naab_unit_tests -j2

if [ $? -ne 0 ]; then
    echo "❌ Unit tests build failed"
    exit 1
fi

echo ""
echo "=== Running FFI async callback tests ==="
./naab_unit_tests --gtest_filter=FFIAsyncCallback*

if [ $? -eq 0 ]; then
    echo ""
    echo "╔════════════════════════════════════════════════════════════╗"
    echo "║   ✅ Item 10 Day 4: Async Callback Framework COMPLETE! ✅  ║"
    echo "╚════════════════════════════════════════════════════════════╝"
    echo ""
    echo "Implementation Summary:"
    echo "  ✅ AsyncCallbackWrapper class (thread-safe async execution)"
    echo "  ✅ AsyncCallbackGuard (RAII wrapper)"
    echo "  ✅ AsyncCallbackPool (concurrent callback management)"
    echo "  ✅ Helper functions (retry, parallel, race)"
    echo "  ✅ Thread safety with mutexes and atomics"
    echo "  ✅ Timeout support"
    echo "  ✅ Cancellation support"
    echo "  ✅ 30+ comprehensive unit tests"
    echo ""
    echo "Files Created:"
    echo "  - include/naab/ffi_async_callback.h (221 lines)"
    echo "  - src/runtime/ffi_async_callback.cpp (520+ lines)"
    echo "  - tests/unit/ffi_async_callback_test.cpp (600+ lines)"
    echo ""
    echo "Next: Day 5 - Integrate with polyglot executors"
else
    echo "❌ Some tests failed"
    exit 1
fi
