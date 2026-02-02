#!/bin/bash
# Run all FFI Async Callback tests

echo "=== Running FFI Async Callback Tests ==="
echo ""

PASSED=0
FAILED=0

run_test() {
    local test=$1
    printf "Running FFIAsyncCallbackTest.%s... " "$test"
    if ./naab_unit_tests --gtest_filter=FFIAsyncCallbackTest.$test > /dev/null 2>&1; then
        echo "✅ PASS"
        PASSED=$((PASSED + 1))
        return 0
    else
        echo "❌ FAIL"
        FAILED=$((FAILED + 1))
        return 1
    fi
}

# Run all tests
run_test "SimpleBlockingExecution"
run_test "SimpleAsyncExecution"
run_test "ExecutionTime"
run_test "ExceptionCaught"
run_test "MultipleExceptionTypes"
run_test "TimeoutTriggered"
run_test "NoTimeoutWhenFast"
run_test "ZeroTimeoutMeansNoLimit"
run_test "CancelBeforeExecution"
run_test "CancelDuringExecution"
run_test "GuardBasicExecution"
run_test "GuardCancellation"
run_test "PoolBasicSubmit"
run_test "PoolMultipleCallbacks"
run_test "PoolConcurrencyLimit"
run_test "PoolCancelAll"
run_test "ExecuteWithRetrySuccess"
run_test "ExecuteWithRetryFailure"
run_test "ExecuteParallelAll"
run_test "ExecuteRaceFirstWins"
run_test "ExecuteRaceTimeout"
run_test "ExecuteRaceEmpty"
run_test "ConcurrentExecutions"
run_test "PoolThreadSafety"

echo ""
echo "========================================="
echo "Results: $PASSED passed, $FAILED failed"

if [ $FAILED -gt 0 ]; then
    echo "❌ Some tests failed"
    exit 1
else
    echo ""
    echo "╔════════════════════════════════════════════════════════════╗"
    echo "║   ✅ Item 10 Day 4: Async Callback Framework COMPLETE! ✅  ║"
    echo "╚════════════════════════════════════════════════════════════╝"
    echo ""
    echo "🎉 All 24 async callback tests passed!"
    echo ""
    echo "Summary:"
    echo "  ✅ AsyncCallbackWrapper (thread-safe async execution)"
    echo "  ✅ AsyncCallbackGuard (RAII wrapper)"
    echo "  ✅ AsyncCallbackPool (concurrent callback management)"
    echo "  ✅ Helper functions (retry, parallel, race)"
    echo "  ✅ 24 comprehensive tests"
    echo ""
    echo "Next: Item 10 Day 5 - Polyglot Integration"
    exit 0
fi
