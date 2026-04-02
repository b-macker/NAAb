#!/bin/bash
# Split governance.cpp (7485 lines) into 5 focused files.
# Run from project root: bash tools/split_governance.sh
#
# Output files (compiled by naab_security in CMakeLists.txt):
#   src/runtime/governance_engine.cpp   - Core: globals, init, enforce, recordPass
#   src/runtime/governance_config.cpp   - Config: loadFromJson, applyEnvironment
#   src/runtime/governance_checks.cpp   - Checks: all check* methods + static helpers
#   src/runtime/governance_reports.cpp  - Reports + audit + hooks + baselines + plugins

set -e

SRC="src/runtime/governance.cpp"
if [ ! -f "$SRC" ]; then
    echo "Error: $SRC not found. Run from project root."
    exit 1
fi

TOTAL=$(wc -l < "$SRC")
echo "governance.cpp: $TOTAL lines"

# --- Boundaries (verified by source inspection) ---
# governance_engine.cpp: lines 1-30 (headers/globals) + 1728-2717 (core engine)
# governance_config.cpp: lines 31-1727 (loadFromJson + related config parsing)
# governance_checks.cpp: lines 2718-5602 (static helpers + all check*() methods)
# governance_reports.cpp: lines 5603-7485 (audit trail + hooks + reports + audit + baselines + plugins)

ENGINE_HEADER_END=30
CONFIG_END=1727
ENGINE_CORE_START=1728
ENGINE_CORE_END=2717
CHECKS_START=2718
CHECKS_END=5602
REPORTS_START=5603

COMMON_HEADERS=$(cat << 'HDRS'
#include "naab/governance.h"
#include "naab/language_registry.h"
#include "naab/interpreter.h"
#include "naab/analyzer/task_pattern_detector.h"
#include "naab/analyzer/syntactic_analyzer.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <regex>
#include <chrono>
#include <functional>
#include <sys/file.h>
#include <fmt/core.h>
HDRS
)

echo ""
echo "Creating governance_engine.cpp (core engine)..."
{
    # Original headers and global flags (lines 1-30)
    sed -n "1,${ENGINE_HEADER_END}p" "$SRC"
    echo ""
    echo "// --- Core Engine Implementation (extracted from governance.cpp) ---"
    echo ""
    # Core engine functions (lines 1728-2717)
    sed -n "${ENGINE_CORE_START},${ENGINE_CORE_END}p" "$SRC"
} > src/runtime/governance_engine.cpp
echo "  -> $(wc -l < src/runtime/governance_engine.cpp) lines"

echo "Creating governance_config.cpp (loadFromJson)..."
{
    echo "// governance_config.cpp — GovernanceEngine configuration loading"
    echo "// Extracted from governance.cpp lines ${ENGINE_HEADER_END+1}-${CONFIG_END}"
    echo ""
    echo "$COMMON_HEADERS"
    echo ""
    sed -n "$((ENGINE_HEADER_END + 1)),${CONFIG_END}p" "$SRC"
} > src/runtime/governance_config.cpp
echo "  -> $(wc -l < src/runtime/governance_config.cpp) lines"

echo "Creating governance_checks.cpp (check functions)..."
{
    echo "// governance_checks.cpp — GovernanceEngine check implementations"
    echo "// Extracted from governance.cpp lines ${CHECKS_START}-${CHECKS_END}"
    echo ""
    echo "$COMMON_HEADERS"
    echo ""
    sed -n "${CHECKS_START},${CHECKS_END}p" "$SRC"
} > src/runtime/governance_checks.cpp
echo "  -> $(wc -l < src/runtime/governance_checks.cpp) lines"

echo "Creating governance_reports.cpp (reports, audit, plugins)..."
{
    echo "// governance_reports.cpp — GovernanceEngine report generation, audit, plugins"
    echo "// Extracted from governance.cpp lines ${REPORTS_START}-${TOTAL}"
    echo ""
    echo "$COMMON_HEADERS"
    echo ""
    sed -n "${REPORTS_START},\$p" "$SRC"
} > src/runtime/governance_reports.cpp
echo "  -> $(wc -l < src/runtime/governance_reports.cpp) lines"

echo ""
echo "Replacing governance.cpp with stub..."
cat > "$SRC" << 'STUB'
// governance.cpp — SPLIT INTO MULTIPLE FILES
// This file is intentionally empty after the split.
// See:
//   governance_engine.cpp   - Core engine (init, enforce, recordPass)
//   governance_config.cpp   - Config loading (loadFromJson)
//   governance_checks.cpp   - Check functions (checkFileAccess, etc.)
//   governance_reports.cpp  - Reports, audit trail, hooks, plugins
STUB

echo ""
echo "Done! Verify by building:"
echo "  cd build && cmake .. && make naab-lang -j4"
echo ""
echo "Then run full tests:"
echo "  cd .. && bash run-all-tests.sh"
