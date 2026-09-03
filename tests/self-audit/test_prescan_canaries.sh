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

# Serialize concurrent runs — this test injects canaries into source files,
# so two instances racing will see each other's uncommitted changes.
LOCKFILE="${TMPDIR:-/tmp}/naab_prescan_canary.lock"
exec 9>"$LOCKFILE"
if ! flock -n 9; then
    echo "SKIP: another test_prescan_canaries.sh is running — waiting for lock"
    flock 9  # block until the other instance finishes
fi
PASS=0; FAIL=0

# Safety: ensure we're in a git repo so we can revert
if ! cd "$LANG_DIR" || ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "SKIP: not in a git repo — cannot safely inject/revert canaries"
    echo "  test_prescan_canaries.sh: SKIPPED (no git)"
    exit 0
fi

# Files this suite is permitted to `git checkout --`. Populated below, BEFORE
# the EXIT trap is armed, and only with files that were clean (or held nothing
# but our own leftovers) at startup.
#
# The trap used to revert both targets unconditionally. check_clean's `exit 1`
# on a dirty file therefore FIRED the trap, so the guard that detected a
# developer's uncommitted work was immediately followed by the cleanup that
# destroyed it. Refusing now happens before the trap exists, and the trap only
# ever touches files on this list.
CLEANUP_FILES=()

# A dirty target is one of two things, and they need opposite handling:
# leftovers from a previous run that was SIGKILLed (the EXIT trap does not run
# on SIGKILL, which is how container resets end long suites), or someone's
# actual work. Every payload this suite injects contains "canary", so a diff
# consisting only of added canary lines is ours to remove; anything else is
# not, and we refuse rather than guess.
classify_target() {
    local file="$1"
    local relfile="${file#$LANG_DIR/}"
    if git diff --quiet -- "$relfile" 2>/dev/null; then
        CLEANUP_FILES+=("$relfile")
        return 0
    fi
    local foreign
    foreign=$(git diff -- "$relfile" | grep -c '^[+-][^+-]' || true)
    local ours
    ours=$(git diff -- "$relfile" | grep -c '^[+-].*canary' || true)
    if [ "$foreign" -gt 0 ] && [ "$foreign" -eq "$ours" ]; then
        echo "  NOTE: $relfile holds canary leftovers from an interrupted run — reverting"
        if ! git checkout -- "$relfile"; then
            echo "ERROR: could not revert leftovers in $relfile"
            exit 1
        fi
        CLEANUP_FILES+=("$relfile")
        return 0
    fi
    echo "ERROR: $relfile has uncommitted changes that are not ours — refusing to run"
    echo "       This suite reverts the files it injects into, which would destroy them."
    echo "       Commit or stash them first."
    exit 1
}

# Belt-and-braces: re-asserted before each injection. classify_target already
# refused anything foreign, so reaching this means a canary leaked between
# tests rather than that a developer had work in flight.
check_clean() {
    local file="$1"
    local relfile="${file#$LANG_DIR/}"
    if ! git diff --quiet -- "$relfile" 2>/dev/null; then
        echo "ERROR: $relfile is dirty mid-suite — a previous canary did not revert"
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
    # No 2>/dev/null: a revert that fails leaves invalid C++ in the tree and
    # breaks the NEXT build with an error far from its cause. Silence here was
    # one of two candidate explanations for exactly that, observed 2026-08-30.
    if ! git checkout -- "$relfile"; then
        echo "  ERROR: revert FAILED for $relfile — working tree may hold a canary"
        FAIL=$((FAIL + 1))
    fi
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
# Classify the two injection targets before arming the trap, so a refusal
# happens while cleanup() does not yet exist and cannot revert anything.
classify_target "$SRC/interpreter/interpreter.cpp"
classify_target "$SRC/runtime/governance_config.cpp"

cleanup() {
    # Revert only files classify_target() cleared. Reverting unconditionally
    # meant any early exit -- including the dirty-file guard's own -- destroyed
    # whatever it had just refused to touch.
    cd "$LANG_DIR" 2>/dev/null || true
    local relfile
    for relfile in ${CLEANUP_FILES+"${CLEANUP_FILES[@]}"}; do
        git checkout -- "$relfile" 2>/dev/null || true
    done
    rm -f "$PRESCAN_OUT"
}
trap cleanup EXIT

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

# =====================================================================
# C11: every target is clean AFTER the last injection
# C7 checks the same property but runs before C8, C9 and C10 inject, so it is
# structurally incapable of catching a leftover from any of them -- which is
# how a run reported ALL PASSED while leaving an invalid function body inside
# ~GovernanceEngine(), breaking the next build (observed 2026-08-30).
# =====================================================================
echo "--- C11: working tree clean after all injections ---"
C11_DIRTY=""
for _relfile in ${CLEANUP_FILES+"${CLEANUP_FILES[@]}"}; do
    git diff --quiet -- "$_relfile" 2>/dev/null || C11_DIRTY="$C11_DIRTY $_relfile"
done
if [ -z "$C11_DIRTY" ]; then
    echo "  PASS [C11] all injected files reverted"
    PASS=$((PASS + 1))
else
    echo "  FAIL [C11] canary left in tree:$C11_DIRTY"
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
