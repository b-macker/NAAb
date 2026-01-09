#!/bin/bash

# API Integration Tests for NAAb REST API
# Tests all REST API endpoints

API_BASE="${API_BASE:-http://localhost:8080}"
TIMEOUT=5

echo "======================================="
echo "  NAAb REST API Integration Tests"
echo "======================================="
echo ""
echo "API Base: $API_BASE"
echo "Timeout: ${TIMEOUT}s"
echo ""

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0;33m' # No Color

passed=0
failed=0

# Helper function to test endpoint
test_endpoint() {
    local name="$1"
    local method="$2"
    local endpoint="$3"
    local data="$4"
    local expected_status="$5"

    echo -n "Test: $name ... "

    if [ "$method" = "GET" ]; then
        response=$(curl -s -w "\n%{http_code}" --connect-timeout $TIMEOUT "$API_BASE$endpoint" 2>/dev/null)
    elif [ "$method" = "POST" ]; then
        response=$(curl -s -w "\n%{http_code}" --connect-timeout $TIMEOUT \
            -X POST "$API_BASE$endpoint" \
            -H "Content-Type: application/json" \
            -d "$data" 2>/dev/null)
    fi

    # Extract status code (last line)
    status=$(echo "$response" | tail -n1)
    body=$(echo "$response" | head -n-1)

    if [ "$status" = "$expected_status" ]; then
        echo -e "${GREEN}PASS${NC} (HTTP $status)"
        ((passed++))
        return 0
    else
        echo -e "${RED}FAIL${NC} (Expected HTTP $expected_status, got $status)"
        ((failed++))
        return 1
    fi
}

# Test 1: Health check
test_endpoint "Health check" "GET" "/health" "" "200"

# Test 2: Execute simple code
code_simple='{"code": "print(\"Hello, API!\")"}'
test_endpoint "Execute simple code" "POST" "/api/v1/execute" "$code_simple" "200"

# Test 3: Execute code with error
code_error='{"code": "throw \"Test error\""}'
test_endpoint "Execute code with error" "POST" "/api/v1/execute" "$code_error" "200"

# Test 4: List blocks
test_endpoint "List blocks" "GET" "/api/v1/blocks?limit=10" "" "200"

# Test 5: Search blocks
test_endpoint "Search blocks" "GET" "/api/v1/blocks/search?query=email" "" "200"

# Test 6: Get statistics
test_endpoint "Get statistics" "GET" "/api/v1/stats" "" "200"

# Test 7: Malformed JSON
test_endpoint "Malformed JSON" "POST" "/api/v1/execute" "not json" "400"

# Test 8: Missing required field
code_missing='{"invalid": "field"}'
test_endpoint "Missing required field" "POST" "/api/v1/execute" "$code_missing" "400"

# Test 9: Execute code with variables
code_vars='{"code": "let x = 10\nlet y = 20\nprint(x + y)"}'
test_endpoint "Execute with variables" "POST" "/api/v1/execute" "$code_vars" "200"

# Test 10: Execute code with functions
code_funcs='{"code": "function add(a, b) { return a + b }\nprint(add(5, 3))"}'
test_endpoint "Execute with functions" "POST" "/api/v1/execute" "$code_funcs" "200"

echo ""
echo "======================================="
echo "  Results"
echo "======================================="
echo "Total:  $((passed + failed))"
echo -e "Passed: ${GREEN}$passed${NC}"
echo -e "Failed: ${RED}$failed${NC}"
echo ""

if [ $failed -eq 0 ]; then
    echo -e "${GREEN}✓ All API tests passed!${NC}"
    exit 0
else
    echo -e "${RED}✗ Some tests failed${NC}"
    echo ""
    echo "Note: Make sure the API server is running:"
    echo "  ~/naab-lang api 8080"
    exit 1
fi
