#!/bin/bash

# Performance Benchmark: REST API Response Time
# Target: < 200ms average response time

API_BASE="${API_BASE:-http://localhost:8080}"
ITERATIONS=50

echo "========================================="
echo "  REST API Performance Benchmark"
echo "========================================="
echo ""
echo "API Base: $API_BASE"
echo "Iterations: $ITERATIONS"
echo ""

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Helper function to benchmark endpoint
benchmark_endpoint() {
    local name="$1"
    local method="$2"
    local endpoint="$3"
    local data="$4"
    local iterations="$5"

    echo "Benchmarking: $name"

    local total_time=0
    local success=0
    local failed=0

    for ((i=1; i<=iterations; i++)); do
        if [ "$method" = "GET" ]; then
            response=$(curl -s -w "\n%{time_total}" --connect-timeout 5 "$API_BASE$endpoint" 2>/dev/null)
        elif [ "$method" = "POST" ]; then
            response=$(curl -s -w "\n%{time_total}" --connect-timeout 5 \
                -X POST "$API_BASE$endpoint" \
                -H "Content-Type: application/json" \
                -d "$data" 2>/dev/null)
        fi

        # Extract time (last line)
        time_taken=$(echo "$response" | tail -n1)

        if [ -n "$time_taken" ]; then
            # Convert to milliseconds
            time_ms=$(echo "$time_taken * 1000" | bc)
            total_time=$(echo "$total_time + $time_ms" | bc)
            ((success++))
        else
            ((failed++))
        fi

        # Progress indicator
        if [ $((i % 10)) -eq 0 ]; then
            echo -n "."
        fi
    done

    echo ""

    if [ $success -gt 0 ]; then
        local avg_time=$(echo "scale=2; $total_time / $success" | bc)
        echo "  Completed: $success/$iterations"
        echo "  Average response time: ${avg_time}ms"

        if (( $(echo "$avg_time < 200" | bc -l) )); then
            echo -e "  Status: ${GREEN}PASS${NC} (< 200ms target)"
            return 0
        else
            echo -e "  Status: ${RED}FAIL${NC} (>= 200ms)"
            return 1
        fi
    else
        echo -e "  Status: ${RED}FAILED${NC} (no successful requests)"
        return 1
    fi
}

# Check if API server is running
echo "Checking API server..."
if ! curl -s --connect-timeout 2 "$API_BASE/health" > /dev/null 2>&1; then
    echo -e "${RED}ERROR: API server is not running${NC}"
    echo "Start the server with: ~/naab-lang api 8080"
    exit 1
fi
echo -e "${GREEN}API server is running${NC}"
echo ""

passed=0
failed=0

# Benchmark 1: Health check endpoint
echo "[1/5] Health Check Endpoint"
if benchmark_endpoint "GET /health" "GET" "/health" "" "$ITERATIONS"; then
    ((passed++))
else
    ((failed++))
fi
echo ""

# Benchmark 2: Simple code execution
echo "[2/5] Execute Simple Code"
code_simple='{"code": "print(\"Hello\")"}'
if benchmark_endpoint "POST /api/v1/execute (simple)" "POST" "/api/v1/execute" "$code_simple" "$ITERATIONS"; then
    ((passed++))
else
    ((failed++))
fi
echo ""

# Benchmark 3: Code with computation
echo "[3/5] Execute Code with Computation"
code_compute='{"code": "let sum = 0\nlet i = 0\nwhile (i < 100) { sum = sum + i\ni = i + 1 }\nprint(sum)"}'
if benchmark_endpoint "POST /api/v1/execute (compute)" "POST" "/api/v1/execute" "$code_compute" 25; then
    ((passed++))
else
    ((failed++))
fi
echo ""

# Benchmark 4: List blocks
echo "[4/5] List Blocks"
if benchmark_endpoint "GET /api/v1/blocks" "GET" "/api/v1/blocks?limit=10" "" "$ITERATIONS"; then
    ((passed++))
else
    ((failed++))
fi
echo ""

# Benchmark 5: Search blocks
echo "[5/5] Search Blocks"
if benchmark_endpoint "GET /api/v1/blocks/search" "GET" "/api/v1/blocks/search?query=email" "" "$ITERATIONS"; then
    ((passed++))
else
    ((failed++))
fi
echo ""

# Summary
echo "========================================="
echo "  Results"
echo "========================================="
echo "Benchmarks:"
echo -e "  Passed: ${GREEN}$passed${NC}"
echo -e "  Failed: ${RED}$failed${NC}"
echo ""

if [ $failed -eq 0 ]; then
    echo -e "${GREEN}✓ All API benchmarks passed (< 200ms target)${NC}"
    exit 0
else
    echo -e "${RED}✗ Some benchmarks exceeded 200ms target${NC}"
    exit 1
fi
