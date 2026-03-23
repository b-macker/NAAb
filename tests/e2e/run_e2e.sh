#!/bin/bash
# ═══════════════════════════════════════════════════════════════
#  NAAb v0.6.0 A/B E2E Conformance Test Harness
#  Runs identical app under STRICT vs ADVISORY governance
#  Compares functional output for parity
# ═══════════════════════════════════════════════════════════════

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$PROJECT_DIR/build/naab-lang"
LOG="$SCRIPT_DIR/e2e_log.txt"

echo "═══════════════════════════════════════════════════════════" | tee "$LOG"
echo "  NAAb v0.6.0 A/B E2E Conformance Test" | tee -a "$LOG"
echo "  Date: $(date)" | tee -a "$LOG"
echo "  Binary: $NAAB" | tee -a "$LOG"
echo "═══════════════════════════════════════════════════════════" | tee -a "$LOG"
echo "" | tee -a "$LOG"

# Check binary exists
if [ ! -f "$NAAB" ]; then
    echo "ERROR: naab-lang binary not found at $NAAB" | tee -a "$LOG"
    exit 1
fi

# ─── Run App A (STRICT) ───
echo "[A] Running App A with STRICT governance..." | tee -a "$LOG"
APP_A_START=$(date +%s)
APP_A_OUT=$(timeout 120s "$NAAB" "$SCRIPT_DIR/app_a/main.naab" 2>&1)
APP_A_EXIT=$?
APP_A_END=$(date +%s)
APP_A_TIME=$((APP_A_END - APP_A_START))

echo "--- App A Output ---" >> "$LOG"
echo "$APP_A_OUT" >> "$LOG"
echo "--- App A Exit: $APP_A_EXIT (${APP_A_TIME}s) ---" >> "$LOG"
echo "" >> "$LOG"

if [ $APP_A_EXIT -ne 0 ]; then
    echo "  [A] CRASHED (exit $APP_A_EXIT)" | tee -a "$LOG"
else
    echo "  [A] Completed in ${APP_A_TIME}s" | tee -a "$LOG"
fi

# ─── Run App B (ADVISORY) ───
echo "[B] Running App B with ADVISORY governance..." | tee -a "$LOG"
APP_B_START=$(date +%s)
APP_B_OUT=$(timeout 120s "$NAAB" "$SCRIPT_DIR/app_b/main.naab" 2>&1)
APP_B_EXIT=$?
APP_B_END=$(date +%s)
APP_B_TIME=$((APP_B_END - APP_B_START))

echo "--- App B Output ---" >> "$LOG"
echo "$APP_B_OUT" >> "$LOG"
echo "--- App B Exit: $APP_B_EXIT (${APP_B_TIME}s) ---" >> "$LOG"
echo "" >> "$LOG"

if [ $APP_B_EXIT -ne 0 ]; then
    echo "  [B] CRASHED (exit $APP_B_EXIT)" | tee -a "$LOG"
else
    echo "  [B] Completed in ${APP_B_TIME}s" | tee -a "$LOG"
fi

# ─── Extract Results ───
echo "" | tee -a "$LOG"
echo "═══════════════════════════════════════════════════════════" | tee -a "$LOG"
echo "  RESULTS ANALYSIS" | tee -a "$LOG"
echo "═══════════════════════════════════════════════════════════" | tee -a "$LOG"

# Extract E2E RESULT lines
A_RESULT=$(echo "$APP_A_OUT" | grep "^E2E RESULT:")
B_RESULT=$(echo "$APP_B_OUT" | grep "^E2E RESULT:")

echo "  App A: $A_RESULT (exit $APP_A_EXIT)" | tee -a "$LOG"
echo "  App B: $B_RESULT (exit $APP_B_EXIT)" | tee -a "$LOG"

# Extract pass/fail counts
A_PASS=$(echo "$A_RESULT" | grep -oP '\d+(?=/)' | head -1)
A_TOTAL=$(echo "$A_RESULT" | grep -oP '/\K\d+' | head -1)
B_PASS=$(echo "$B_RESULT" | grep -oP '\d+(?=/)' | head -1)
B_TOTAL=$(echo "$B_RESULT" | grep -oP '/\K\d+' | head -1)

# ─── A/B Comparison ───
echo "" | tee -a "$LOG"
echo "  --- A/B Functional Parity Check ---" | tee -a "$LOG"

# Extract phase lines and assertion results (strip governance-specific lines)
A_PHASES=$(echo "$APP_A_OUT" | grep -E "^\s+(Phase|P[0-9])" | head -20)
B_PHASES=$(echo "$APP_B_OUT" | grep -E "^\s+(Phase|P[0-9])" | head -20)

# Extract FAIL lines
A_FAILS=$(echo "$APP_A_OUT" | grep "FAIL:" || true)
B_FAILS=$(echo "$APP_B_OUT" | grep "FAIL:" || true)

# Compare functional output (everything except header line)
A_FUNC=$(echo "$APP_A_OUT" | grep -v "^=== E2E App" | grep -v "^\[governance\]")
B_FUNC=$(echo "$APP_B_OUT" | grep -v "^=== E2E App" | grep -v "^\[governance\]")

DIFF=$(diff <(echo "$A_FUNC") <(echo "$B_FUNC") 2>/dev/null || true)

if [ -z "$DIFF" ]; then
    echo "  A/B Diff: IDENTICAL (functional parity CONFIRMED)" | tee -a "$LOG"
    AB_PARITY="PASS"
else
    echo "  A/B Diff: DIVERGENT" | tee -a "$LOG"
    echo "$DIFF" | head -30 | tee -a "$LOG"
    AB_PARITY="FAIL"
fi

# ─── Failure Details ───
if [ -n "$A_FAILS" ]; then
    echo "" | tee -a "$LOG"
    echo "  App A Failures:" | tee -a "$LOG"
    echo "$A_FAILS" | tee -a "$LOG"
fi

if [ -n "$B_FAILS" ]; then
    echo "" | tee -a "$LOG"
    echo "  App B Failures:" | tee -a "$LOG"
    echo "$B_FAILS" | tee -a "$LOG"
fi

# ─── Error Logs ───
A_ERRORS=$(echo "$APP_A_OUT" | grep -A100 "^ERRORS:" || true)
B_ERRORS=$(echo "$APP_B_OUT" | grep -A100 "^ERRORS:" || true)

if [ -n "$A_ERRORS" ]; then
    echo "" >> "$LOG"
    echo "  App A Error Details:" >> "$LOG"
    echo "$A_ERRORS" >> "$LOG"
fi

if [ -n "$B_ERRORS" ]; then
    echo "" >> "$LOG"
    echo "  App B Error Details:" >> "$LOG"
    echo "$B_ERRORS" >> "$LOG"
fi

# ─── Final Summary ───
echo "" | tee -a "$LOG"
echo "═══════════════════════════════════════════════════════════" | tee -a "$LOG"
echo "  FINAL SUMMARY" | tee -a "$LOG"
echo "═══════════════════════════════════════════════════════════" | tee -a "$LOG"
echo "  App A (STRICT):   ${A_PASS:-?}/${A_TOTAL:-?} passed (exit $APP_A_EXIT, ${APP_A_TIME}s)" | tee -a "$LOG"
echo "  App B (ADVISORY): ${B_PASS:-?}/${B_TOTAL:-?} passed (exit $APP_B_EXIT, ${APP_B_TIME}s)" | tee -a "$LOG"
echo "  A/B Parity:       $AB_PARITY" | tee -a "$LOG"

# Overall verdict
if [ "$APP_A_EXIT" -eq 0 ] && [ "$APP_B_EXIT" -eq 0 ] && [ "$AB_PARITY" = "PASS" ] && [ "${A_PASS:-0}" = "${A_TOTAL:-1}" ] && [ "${B_PASS:-0}" = "${B_TOTAL:-1}" ]; then
    echo "  VERDICT:          ALL CHECKS PASSED" | tee -a "$LOG"
    EXIT=0
else
    echo "  VERDICT:          FAILURES DETECTED — see $LOG" | tee -a "$LOG"
    EXIT=1
fi

echo "═══════════════════════════════════════════════════════════" | tee -a "$LOG"
echo ""
echo "Full log: $LOG"

exit $EXIT
