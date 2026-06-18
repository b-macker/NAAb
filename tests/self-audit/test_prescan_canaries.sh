#!/usr/bin/env bash
# test_prescan_canaries.sh — Prescan Canary Tests
#
# Validates the prescan itself: injects known-bad code, confirms the prescan
# detects it, then reverts. Prevents prescan bit-rot where FP filters grow
# until real bugs slip through.
#
# 10 canary tests (inject → run prescan check → assert finding → revert)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LANG_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
PRESCAN="$LANG_DIR/examples/self-audit/stage0-prescan.sh"
SRC="$LANG_DIR/src"
PASS=0; FAIL=0

# Safety: ensure we're in a git repo so we can revert
if ! cd "$LANG_DIR" || ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "SKIP: not in a git repo — cannot safely inject/revert canaries"
    echo "  test_prescan_canaries.sh: SKIPPED (no git)"
    exit 0
fi

# Verify no uncommitted changes to files we'll inject into
check_clean() {
    local file="$1"
    local relfile="${file#$LANG_DIR/}"
    if ! git diff --quiet -- "$relfile" 2>/dev/null; then
        echo "ERROR: $relfile has uncommitted changes — cannot inject canary"
        exit 1
    fi
}

inject() {
    local file="$1" line="$2" payload="$3"
    # GNU sed 4.9+: \n in i\ command text creates newlines (multi-line inject)
    sed -i "${line}i\\${payload}" "$file"
}

revert() {
    local file="$1"
    local relfile="${file#$LANG_DIR/}"
    git checkout -- "$relfile" 2>/dev/null
}

run_prescan() {
    bash "$PRESCAN" "$LANG_DIR" 2>/dev/null
}

count_findings() {
    local check="$1" output_file="$2"
    local n
    n=$(grep -c "\"check\":\"$check\"" "$output_file" 2>/dev/null) || true
    echo "${n:-0}"
}

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
PRESCAN_OUT="$_SYSTMP/canary_prescan_$$"
trap 'rm -f "$PRESCAN_OUT"' EXIT

echo "=== Prescan Canary Tests ==="
echo "  Prescan: $PRESCAN"
echo ""

# Get baseline findings count
run_prescan > "$PRESCAN_OUT"
BASELINE_P1=$(count_findings "P1" "$PRESCAN_OUT")
BASELINE_P2=$(count_findings "P2" "$PRESCAN_OUT")
BASELINE_L10=$(count_findings "L10" "$PRESCAN_OUT")
echo "  Baseline: P1=$BASELINE_P1, P2=$BASELINE_P2, L10=$BASELINE_L10"
echo ""

# Injection point for P1/L10: line 110 in interpreter.cpp
# Verified clean of all P1 Filter 2/3 patterns (GovernanceHardError, std::transform,
# block_loader_, etc.) in 30-line context, and no throw;/rethrow_exception in 20 lines below.
# Also clean of L10 filter patterns (semverGe, stoi, etc.) in 20-line context.
P1_INJECT_LINE=110
# Injection point for P2: line 50 in governance_config.cpp (standard)
P2_INJECT_LINE=50

# =====================================================================
# C1: P1 — Inject catch(std::exception) without GovernanceHardError guard
# Target: interpreter.cpp (must be in P1 target_files list)
# =====================================================================
echo "--- C1: P1 catch gap detection ---"
TARGET="$SRC/interpreter/interpreter.cpp"
check_clean "$TARGET"
inject "$TARGET" "$P1_INJECT_LINE" "void canary_p1_test() { try { int x = 1; } catch (const std::exception& e) { /* no rethrow */ } }"
run_prescan > "$PRESCAN_OUT"
revert "$TARGET"
NEW_P1=$(count_findings "P1" "$PRESCAN_OUT")
if [ "$NEW_P1" -gt "$BASELINE_P1" ]; then
    echo "  PASS [C1] P1 catch gap detected (baseline=$BASELINE_P1 injected=$NEW_P1)"
    PASS=$((PASS + 1))
else
    echo "  FAIL [C1] P1 catch gap NOT detected (baseline=$BASELINE_P1 injected=$NEW_P1)"
    FAIL=$((FAIL + 1))
fi

# =====================================================================
# C2: P1 FP filter — Inject catch with known-safe pattern nearby
# Target: interpreter.cpp (same file, with 'enqueue' FP pattern in context)
# =====================================================================
echo "--- C2: P1 FP filter (known-safe pattern) ---"
TARGET="$SRC/interpreter/interpreter.cpp"
check_clean "$TARGET"
# Inject the FP filter pattern on a preceding line, then the catch
inject "$TARGET" "$P1_INJECT_LINE" "void canary_fp_test() { /* enqueue callback */ try { int x = 1; } catch (const std::exception& e) { } }"
run_prescan > "$PRESCAN_OUT"
revert "$TARGET"
NEW_P1=$(count_findings "P1" "$PRESCAN_OUT")
if [ "$NEW_P1" -eq "$BASELINE_P1" ]; then
    echo "  PASS [C2] P1 FP filter correctly suppressed (baseline=$BASELINE_P1)"
    PASS=$((PASS + 1))
else
    echo "  FAIL [C2] P1 FP filter did not suppress (baseline=$BASELINE_P1 injected=$NEW_P1)"
    FAIL=$((FAIL + 1))
fi

# =====================================================================
# C3: P2 — Inject unguarded .get<std::string>()
# =====================================================================
echo "--- C3: P2 unguarded .get detection ---"
TARGET="$SRC/runtime/governance_config.cpp"
check_clean "$TARGET"
inject "$TARGET" "$P2_INJECT_LINE" "void canary_p2_test(nlohmann::json j) { auto s = j.get<std::string>(); }"
run_prescan > "$PRESCAN_OUT"
revert "$TARGET"
NEW_P2=$(count_findings "P2" "$PRESCAN_OUT")
if [ "$NEW_P2" -gt "$BASELINE_P2" ]; then
    echo "  PASS [C3] P2 unguarded .get detected (baseline=$BASELINE_P2 injected=$NEW_P2)"
    PASS=$((PASS + 1))
else
    echo "  FAIL [C3] P2 unguarded .get NOT detected (baseline=$BASELINE_P2 injected=$NEW_P2)"
    FAIL=$((FAIL + 1))
fi

# =====================================================================
# C4: P2 FP filter — Inject .get<> after .is_string() guard
# =====================================================================
echo "--- C4: P2 FP filter (guarded .get) ---"
TARGET="$SRC/runtime/governance_config.cpp"
check_clean "$TARGET"
inject "$TARGET" "$P2_INJECT_LINE" "void canary_p2_fp(nlohmann::json j) { if (j.is_string()) { auto s = j.get<std::string>(); } }"
run_prescan > "$PRESCAN_OUT"
revert "$TARGET"
NEW_P2=$(count_findings "P2" "$PRESCAN_OUT")
if [ "$NEW_P2" -eq "$BASELINE_P2" ]; then
    echo "  PASS [C4] P2 FP filter correctly suppressed guarded .get"
    PASS=$((PASS + 1))
else
    echo "  FAIL [C4] P2 FP filter didn't suppress guarded .get (baseline=$BASELINE_P2 injected=$NEW_P2)"
    FAIL=$((FAIL + 1))
fi

# =====================================================================
# C5: L10 — Inject silent catch(...)
# L10 checks next 10 lines after catch for body content. Multi-line
# injection with comment padding ensures body strips to empty.
# =====================================================================
echo "--- C5: L10 silent catch detection ---"
TARGET="$SRC/interpreter/interpreter.cpp"
check_clean "$TARGET"
# Inject multi-line: catch on one line, then 10 padding lines (} + 9 comments)
# that strip to empty under L10's comment/whitespace/brace removal.
# L10 checks lineno+1 to lineno+10 (10 lines); all must strip to empty.
inject "$TARGET" "$P1_INJECT_LINE" "} catch (...) {\n}\n// canary_l10_pad_1\n// canary_l10_pad_2\n// canary_l10_pad_3\n// canary_l10_pad_4\n// canary_l10_pad_5\n// canary_l10_pad_6\n// canary_l10_pad_7\n// canary_l10_pad_8\n// canary_l10_pad_9"
run_prescan > "$PRESCAN_OUT"
revert "$TARGET"
NEW_L10=$(count_findings "L10" "$PRESCAN_OUT")
if [ "$NEW_L10" -gt "$BASELINE_L10" ]; then
    echo "  PASS [C5] L10 silent catch detected (baseline=$BASELINE_L10 injected=$NEW_L10)"
    PASS=$((PASS + 1))
else
    echo "  FAIL [C5] L10 silent catch NOT detected (baseline=$BASELINE_L10 injected=$NEW_L10)"
    FAIL=$((FAIL + 1))
fi

# =====================================================================
# C6: L10 FP filter — Inject catch with [packages] Warning nearby
# =====================================================================
echo "--- C6: L10 FP filter (known-safe pattern) ---"
TARGET="$SRC/interpreter/interpreter.cpp"
check_clean "$TARGET"
# Inject [packages] Warning within 20-line context above the catch
inject "$TARGET" "$P1_INJECT_LINE" "// [packages] Warning: canary test\n} catch (...) {\n}\n// canary_l10_fp_1\n// canary_l10_fp_2\n// canary_l10_fp_3\n// canary_l10_fp_4\n// canary_l10_fp_5\n// canary_l10_fp_6\n// canary_l10_fp_7\n// canary_l10_fp_8\n// canary_l10_fp_9"
run_prescan > "$PRESCAN_OUT"
revert "$TARGET"
NEW_L10=$(count_findings "L10" "$PRESCAN_OUT")
if [ "$NEW_L10" -eq "$BASELINE_L10" ]; then
    echo "  PASS [C6] L10 FP filter correctly suppressed"
    PASS=$((PASS + 1))
else
    echo "  FAIL [C6] L10 FP filter didn't suppress (baseline=$BASELINE_L10 injected=$NEW_L10)"
    FAIL=$((FAIL + 1))
fi

# =====================================================================
# C7: Revert verification — file is restored after canary
# =====================================================================
echo "--- C7: Revert verification ---"
TARGET="$SRC/interpreter/interpreter.cpp"
check_clean "$TARGET"
HASH_BEFORE=$(git hash-object "$TARGET")
inject "$TARGET" "$P1_INJECT_LINE" "// CANARY_REVERT_TEST"
revert "$TARGET"
HASH_AFTER=$(git hash-object "$TARGET")
if [ "$HASH_BEFORE" = "$HASH_AFTER" ]; then
    echo "  PASS [C7] File correctly reverted after injection"
    PASS=$((PASS + 1))
else
    echo "  FAIL [C7] File NOT correctly reverted (hash mismatch)"
    FAIL=$((FAIL + 1))
fi

# =====================================================================
# C8: P2 with .get<int>() detection
# =====================================================================
echo "--- C8: P2 unguarded .get<int>() ---"
TARGET="$SRC/runtime/governance_config.cpp"
check_clean "$TARGET"
inject "$TARGET" "$P2_INJECT_LINE" "void canary_p2_int(nlohmann::json j) { int x = j.get<int>(); }"
run_prescan > "$PRESCAN_OUT"
revert "$TARGET"
NEW_P2=$(count_findings "P2" "$PRESCAN_OUT")
if [ "$NEW_P2" -gt "$BASELINE_P2" ]; then
    echo "  PASS [C8] P2 unguarded .get<int> detected"
    PASS=$((PASS + 1))
else
    echo "  FAIL [C8] P2 unguarded .get<int> NOT detected"
    FAIL=$((FAIL + 1))
fi

# =====================================================================
# C9: P2 with .get<bool>() detection
# =====================================================================
echo "--- C9: P2 unguarded .get<bool>() ---"
TARGET="$SRC/runtime/governance_config.cpp"
check_clean "$TARGET"
inject "$TARGET" "$P2_INJECT_LINE" "void canary_p2_bool(nlohmann::json j) { bool b = j.get<bool>(); }"
run_prescan > "$PRESCAN_OUT"
revert "$TARGET"
NEW_P2=$(count_findings "P2" "$PRESCAN_OUT")
if [ "$NEW_P2" -gt "$BASELINE_P2" ]; then
    echo "  PASS [C9] P2 unguarded .get<bool> detected"
    PASS=$((PASS + 1))
else
    echo "  FAIL [C9] P2 unguarded .get<bool> NOT detected"
    FAIL=$((FAIL + 1))
fi

# =====================================================================
# C10: Full prescan — inject P1+P2+L10 simultaneously
# =====================================================================
echo "--- C10: Combined P1+P2+L10 injection ---"
T_INT="$SRC/interpreter/interpreter.cpp"
T_CFG="$SRC/runtime/governance_config.cpp"
check_clean "$T_INT"
check_clean "$T_CFG"
# P1: catch gap in interpreter.cpp (in P1 target list)
inject "$T_INT" "$P1_INJECT_LINE" "void canary_combined_p1() { try { int x = 1; } catch (const std::exception& e) { } }"
# L10: multi-line silent catch in interpreter.cpp (separate injection below P1)
L10_LINE=$((P1_INJECT_LINE + 1))
inject "$T_INT" "$L10_LINE" "} catch (...) {\n}\n// canary_combined_l10_1\n// canary_combined_l10_2\n// canary_combined_l10_3\n// canary_combined_l10_4\n// canary_combined_l10_5\n// canary_combined_l10_6\n// canary_combined_l10_7\n// canary_combined_l10_8\n// canary_combined_l10_9"
# P2: unguarded .get in governance_config.cpp
inject "$T_CFG" "$P2_INJECT_LINE" "void canary_combined_p2(nlohmann::json j) { auto s = j.get<std::string>(); }"
run_prescan > "$PRESCAN_OUT"
revert "$T_INT"
revert "$T_CFG"
NEW_P1=$(count_findings "P1" "$PRESCAN_OUT")
NEW_P2=$(count_findings "P2" "$PRESCAN_OUT")
NEW_L10=$(count_findings "L10" "$PRESCAN_OUT")
if [ "$NEW_P1" -gt "$BASELINE_P1" ] && [ "$NEW_P2" -gt "$BASELINE_P2" ] && [ "$NEW_L10" -gt "$BASELINE_L10" ]; then
    echo "  PASS [C10] Combined injection: all 3 checks detected"
    PASS=$((PASS + 1))
else
    echo "  FAIL [C10] Combined injection: P1=$NEW_P1(was $BASELINE_P1) P2=$NEW_P2(was $BASELINE_P2) L10=$NEW_L10(was $BASELINE_L10)"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "=== Canary Results: $PASS passed, $FAIL failed (of $((PASS + FAIL)) tests) ==="

if [ $FAIL -gt 0 ]; then
    echo ""
    echo "FAILED: Prescan canary detection failures."
    exit 1
fi

echo "  All canaries detected and reverted successfully."
exit 0
