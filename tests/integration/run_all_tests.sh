#!/bin/bash

# Master Integration Test Runner
# Runs all integration test suites

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "========================================="
echo "  NAAb Complete Integration Test Suite"
echo "========================================="
echo ""

total_passed=0
total_failed=0
suites_passed=0
suites_failed=0

# Color codes
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Suite 1: Integration Tests (multi-file, pipelines, composition)
echo -e "${BLUE}[1/3] Running Integration Tests...${NC}"
echo ""
if "$SCRIPT_DIR/run_integration_tests.sh"; then
    ((suites_passed++))
else
    ((suites_failed++))
fi
echo ""

# Suite 2: CLI Tests
echo -e "${BLUE}[2/3] Running CLI Tests...${NC}"
echo ""
if "$SCRIPT_DIR/test_cli.sh"; then
    ((suites_passed++))
else
    ((suites_failed++))
fi
echo ""

# Suite 3: API Tests (optional - requires server running)
echo -e "${BLUE}[3/3] Running API Tests...${NC}"
echo ""
echo "Note: API tests require the server to be running:"
echo "  ~/naab-lang api 8080"
echo ""
read -p "Run API tests? (y/N) " -n 1 -r
echo ""
if [[ $REPLY =~ ^[Yy]$ ]]; then
    if "$SCRIPT_DIR/test_api.sh"; then
        ((suites_passed++))
    else
        ((suites_failed++))
    fi
else
    echo "Skipping API tests"
fi
echo ""

# Summary
echo "========================================="
echo "  Final Results"
echo "========================================="
echo "Test Suites:"
echo -e "  Passed: ${GREEN}$suites_passed${NC}"
echo -e "  Failed: ${RED}$suites_failed${NC}"
echo ""

if [ $suites_failed -eq 0 ]; then
    echo -e "${GREEN}✓✓✓ All test suites passed! ✓✓✓${NC}"
    exit 0
else
    echo -e "${RED}✗ Some test suites failed${NC}"
    exit 1
fi
