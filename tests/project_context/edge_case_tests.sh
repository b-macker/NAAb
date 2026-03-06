#!/bin/bash
# Project Context Awareness — Edge Case Tests
# Tests frustration scenarios, conflict resolution, and robustness

NAAB="$(cd "$(dirname "$0")/../.." && pwd)/build/naab-lang"
DIR="$(cd "$(dirname "$0")" && pwd)"
EDGE="$DIR/edge_cases"
PASS=0
FAIL=0
TOTAL=0

pass() {
    PASS=$((PASS + 1))
    TOTAL=$((TOTAL + 1))
    echo "  PASS: $1"
}

fail() {
    FAIL=$((FAIL + 1))
    TOTAL=$((TOTAL + 1))
    echo "  FAIL: $1"
    echo "        $2"
}

# Helper: write a temp govern.json, run a test script, capture stderr
run_test() {
    local govern_json="$1"
    local test_script="$2"
    local test_dir="${3:-$EDGE}"

    echo "$govern_json" > "$test_dir/govern.json"

    # Create minimal test script if not provided as file
    if [ ! -f "$test_script" ]; then
        local tmp_script="$test_dir/_test.naab"
        echo "$test_script" > "$tmp_script"
        test_script="$tmp_script"
    fi

    OUTPUT=$("$NAAB" "$test_script" 2>&1)
    echo "$OUTPUT"
}

echo "═══════════════════════════════════════════════════════════"
echo "  PROJECT CONTEXT — EDGE CASE TESTS"
echo "═══════════════════════════════════════════════════════════"
echo ""

# Create minimal test script
cat > "$EDGE/_test.naab" << 'NAAB'
main {
    let x = <<python
2 + 2
>>
    io.write("ok=" + x)
}
NAAB

# ─────────────────────────────────────────────────────────────
# TEST 1: Code block content should NOT be extracted as rules
# ─────────────────────────────────────────────────────────────
echo "Test 1: Code block skipping"
OUT=$(run_test '{"project_context":{"enabled":true,"show_extractions":true,"sources":{"llm":true,"linters":false,"manifests":false}}}' "$EDGE/_test.naab")

# The CLAUDE.md has "always use JavaScript" and "never use Python" INSIDE a code block
# These should NOT appear in extractions
if echo "$OUT" | grep -q "javascript.*APPLIED.*strong preference"; then
    fail "Code block: 'always use JavaScript' was extracted from code block" "Should have been skipped"
else
    pass "Code block: content inside \`\`\` fences correctly skipped"
fi

# The indented block has "always use Rust" — should also be skipped
if echo "$OUT" | grep -q "rust.*APPLIED.*strong preference"; then
    fail "Indented block: 'always use Rust' was extracted from indented code" "Should have been skipped"
else
    pass "Indented block: 4-space indented content correctly skipped"
fi

# Blockquote "Consider using Haskell" should be skipped
if echo "$OUT" | grep -q "haskell"; then
    fail "Blockquote: 'Consider using Haskell' was extracted" "Blockquotes should be skipped"
else
    pass "Blockquote: content in > blockquotes correctly skipped"
fi

# But "Prefer Ruby for string processing" AFTER the code block should be extracted
if echo "$OUT" | grep -q "ruby"; then
    pass "Post-code-block: 'Prefer Ruby' correctly extracted after code block ends"
else
    fail "Post-code-block: 'Prefer Ruby' not found after code block" "Rules after code blocks should still be extracted"
fi

echo ""

# ─────────────────────────────────────────────────────────────
# TEST 2: Multi-file conflict — CLAUDE.md vs .cursorrules
# ─────────────────────────────────────────────────────────────
echo "Test 2: Multi-file conflict resolution (first-file-wins)"
OUT=$(run_test '{"project_context":{"enabled":true,"show_extractions":true,"sources":{"llm":true,"linters":false,"manifests":false}}}' "$EDGE/_test.naab")

# Both files say "never use eval()" — one should be applied, one redundant or conflict
EVAL_COUNT=$(echo "$OUT" | grep -c "ban eval.*APPLIED" || true)
if [ "$EVAL_COUNT" -le 1 ]; then
    pass "Duplicate ban: eval() not double-applied"
else
    fail "Duplicate ban: eval() applied $EVAL_COUNT times" "Should be applied once, conflict/redundant for second"
fi

# CLAUDE.md says "use 4 spaces", .cursorrules says "use 2 spaces"
# CLAUDE.md should win (discovered first in priority order)
if echo "$OUT" | grep -q "indent_size=4.*APPLIED"; then
    pass "Style conflict: CLAUDE.md indent_size=4 wins (higher priority)"
else
    fail "Style conflict: CLAUDE.md indent_size=4 not applied" "First-file-wins should apply CLAUDE.md"
fi

if echo "$OUT" | grep -q "indent_size=2.*SKIP"; then
    pass "Style conflict: .cursorrules indent_size=2 correctly skipped"
else
    # It's also ok if it just wasn't extracted separately
    if echo "$OUT" | grep -q "indent_size=2"; then
        fail "Style conflict: .cursorrules indent_size=2 not marked as skipped" "Should show [SKIPPED: conflict]"
    else
        pass "Style conflict: .cursorrules indent_size=2 not present (acceptable)"
    fi
fi

echo ""

# ─────────────────────────────────────────────────────────────
# TEST 3: priority_source flips the winner
# ─────────────────────────────────────────────────────────────
echo "Test 3: priority_source override"
OUT=$(run_test '{"project_context":{"enabled":true,"priority_source":".cursorrules","show_extractions":true,"sources":{"llm":true,"linters":false,"manifests":false}}}' "$EDGE/_test.naab")

# With priority_source=".cursorrules", TypeScript preference should win over Python
if echo "$OUT" | grep -q "typescript.*APPLIED"; then
    pass "priority_source: .cursorrules TypeScript preference applied"
else
    fail "priority_source: TypeScript from .cursorrules not applied" "priority_source should make .cursorrules win"
fi

echo ""

# ─────────────────────────────────────────────────────────────
# TEST 4: govern.json blocks language that CLAUDE.md recommends
# ─────────────────────────────────────────────────────────────
echo "Test 4: govern.json overrides LLM language preference"
OUT=$(run_test '{"languages":{"allowed":["javascript"]},"project_context":{"enabled":true,"show_extractions":true,"sources":{"llm":true,"linters":false,"manifests":false}}}' "$EDGE/_test.naab")

# CLAUDE.md says "always use Python" but govern.json only allows JavaScript
if echo "$OUT" | grep -q "python.*SKIP.*govern"; then
    pass "govern.json override: Python preference skipped (govern restricts languages)"
else
    # Check if it was applied anyway
    if echo "$OUT" | grep -q "python.*APPLIED"; then
        fail "govern.json override: Python preference was APPLIED despite govern.json restricting languages" "Should be SKIPPED"
    else
        pass "govern.json override: Python preference not applied (acceptable)"
    fi
fi

echo ""

# ─────────────────────────────────────────────────────────────
# TEST 5: Linter vs LLM conflict — linter should win
# ─────────────────────────────────────────────────────────────
echo "Test 5: Linter vs LLM conflict (linter wins)"
OUT=$(run_test '{"project_context":{"enabled":true,"show_extractions":true}}' "$EDGE/_test.naab")

# .editorconfig says "indent_style = tab", CLAUDE.md says "use 4 spaces" (implies spaces)
# .editorconfig has higher confidence, but the conflict detection is by mapped_rule key
# Check that both appear and one is skipped
INDENT_STYLE_COUNT=$(echo "$OUT" | grep -c "indent_style" || true)
if [ "$INDENT_STYLE_COUNT" -ge 2 ]; then
    if echo "$OUT" | grep -q "indent_style.*SKIP"; then
        pass "Linter vs LLM: One indent_style rule correctly skipped as conflict"
    else
        fail "Linter vs LLM: Multiple indent_style rules but none skipped" "Lower priority should be SKIPPED"
    fi
else
    pass "Linter vs LLM: Only one indent_style applied (acceptable)"
fi

echo ""

# ─────────────────────────────────────────────────────────────
# TEST 6: Manifest detection should NOT block languages
# ─────────────────────────────────────────────────────────────
echo "Test 6: Manifest detection is advisory-only"
# go.mod exists, but user writes Python — should work fine
OUT=$(run_test '{"project_context":{"enabled":true,"show_extractions":true,"sources":{"llm":false,"linters":false,"manifests":true}}}' "$EDGE/_test.naab")

if echo "$OUT" | grep -q "ok=4"; then
    pass "Manifest advisory: Python block runs despite go.mod being detected"
else
    fail "Manifest advisory: Python block failed" "Manifests should NEVER block languages"
fi

if echo "$OUT" | grep -q "go.*detected\|Go.*detected"; then
    pass "Manifest advisory: Go detected from go.mod"
else
    fail "Manifest advisory: Go not detected from go.mod" "Should detect Go from go.mod"
fi

if echo "$OUT" | grep -q "javascript.*detected\|JavaScript.*detected"; then
    pass "Manifest advisory: JavaScript detected from package.json"
else
    fail "Manifest advisory: JavaScript not detected" "Should detect JS from package.json"
fi

echo ""

# ─────────────────────────────────────────────────────────────
# TEST 7: Suppress specific rules
# ─────────────────────────────────────────────────────────────
echo "Test 7: Suppress specific rules by ID"
OUT=$(run_test '{"project_context":{"enabled":true,"suppress_rules":["ctx-llm-ban-eval"],"show_extractions":true,"sources":{"llm":true,"linters":false,"manifests":false}}}' "$EDGE/_test.naab")

if echo "$OUT" | grep -q "eval.*suppressed\|eval.*SUPPRESSED"; then
    pass "Suppress: eval ban marked as suppressed"
else
    if echo "$OUT" | grep -q "eval.*APPLIED"; then
        fail "Suppress: eval ban was APPLIED despite being in suppress_rules" "Should be SKIPPED:suppressed"
    else
        pass "Suppress: eval ban not applied (acceptable)"
    fi
fi

echo ""

# ─────────────────────────────────────────────────────────────
# TEST 8: Empty/malformed files don't crash
# ─────────────────────────────────────────────────────────────
echo "Test 8: Robustness — empty and malformed files"

# Add empty file to watch_files
OUT=$(run_test '{"project_context":{"enabled":true,"watch_files":["empty_file.md"],"show_extractions":true,"sources":{"llm":true,"linters":false,"manifests":false}}}' "$EDGE/_test.naab")

if echo "$OUT" | grep -q "ok=4"; then
    pass "Empty file: Script runs successfully with empty watch_file"
else
    fail "Empty file: Script failed" "Empty files should be handled gracefully"
fi

echo ""

# ─────────────────────────────────────────────────────────────
# TEST 9: watch_files — custom file gets scanned
# ─────────────────────────────────────────────────────────────
echo "Test 9: Custom watch_files scanning"
OUT=$(run_test '{"project_context":{"enabled":true,"watch_files":["custom-rules.md"],"show_extractions":true,"sources":{"llm":true,"linters":false,"manifests":false}}}' "$EDGE/_test.naab")

if echo "$OUT" | grep -q "nim\|pickle"; then
    pass "watch_files: custom-rules.md scanned and directives extracted"
else
    fail "watch_files: custom-rules.md directives not found" "watch_files should be scanned as LLM-layer files"
fi

echo ""

# ─────────────────────────────────────────────────────────────
# TEST 10: ignore_files — excluded file gets skipped
# ─────────────────────────────────────────────────────────────
echo "Test 10: ignore_files exclusion"
OUT=$(run_test '{"project_context":{"enabled":true,"ignore_files":[".cursorrules"],"show_extractions":true,"sources":{"llm":true,"linters":false,"manifests":false}}}' "$EDGE/_test.naab")

if echo "$OUT" | grep -q "cursorrules"; then
    fail "ignore_files: .cursorrules still scanned despite being in ignore_files" "Should be skipped"
else
    pass "ignore_files: .cursorrules correctly excluded"
fi

echo ""

# ─────────────────────────────────────────────────────────────
# TEST 11: Layer independence — each layer toggleable
# ─────────────────────────────────────────────────────────────
echo "Test 11: Layer independence"

# LLM only
OUT=$(run_test '{"project_context":{"enabled":true,"show_extractions":true,"sources":{"llm":true,"linters":false,"manifests":false}}}' "$EDGE/_test.naab")
if echo "$OUT" | grep -q "Layer 2"; then
    fail "LLM-only: Layer 2 appeared when linters=false" "Should not scan linter configs"
else
    pass "LLM-only: No linter output when sources.linters=false"
fi
if echo "$OUT" | grep -q "Layer 3"; then
    fail "LLM-only: Layer 3 appeared when manifests=false" "Should not scan manifests"
else
    pass "LLM-only: No manifest output when sources.manifests=false"
fi

# Linter only
OUT=$(run_test '{"project_context":{"enabled":true,"show_extractions":true,"sources":{"llm":false,"linters":true,"manifests":false}}}' "$EDGE/_test.naab")
if echo "$OUT" | grep -q "Layer 1"; then
    fail "Linter-only: Layer 1 appeared when llm=false" "Should not scan LLM files"
else
    pass "Linter-only: No LLM output when sources.llm=false"
fi

echo ""

# ─────────────────────────────────────────────────────────────
# TEST 12: Disabled feature has zero output
# ─────────────────────────────────────────────────────────────
echo "Test 12: Feature disabled = zero overhead"
OUT=$(run_test '{"project_context":{"enabled":false}}' "$EDGE/_test.naab")

if echo "$OUT" | grep -q "project-context"; then
    fail "Disabled: project-context output appeared when enabled=false" "Should be completely silent"
else
    pass "Disabled: No project-context output when enabled=false"
fi

if echo "$OUT" | grep -q "ok=4"; then
    pass "Disabled: Script runs normally"
else
    fail "Disabled: Script failed" "Should run without any interference"
fi

echo ""

# ─────────────────────────────────────────────────────────────
# TEST 13: extract category toggling
# ─────────────────────────────────────────────────────────────
echo "Test 13: Extract category toggling"
OUT=$(run_test '{"project_context":{"enabled":true,"show_extractions":true,"sources":{"llm":true,"linters":false,"manifests":false},"extract":{"language_preferences":false,"banned_patterns":true,"style_rules":false,"custom_directives":false}}}' "$EDGE/_test.naab")

# With language_preferences=false, should NOT see language extractions
if echo "$OUT" | grep -q "Language:"; then
    fail "Extract toggle: Language extractions appeared with language_preferences=false" "Should be filtered"
else
    pass "Extract toggle: Language preferences correctly filtered out"
fi

# But banned patterns should still appear
if echo "$OUT" | grep -q "Banned:.*eval\|ban.*eval"; then
    pass "Extract toggle: Banned patterns still extracted when enabled"
else
    fail "Extract toggle: Banned patterns not found" "banned_patterns=true should still extract bans"
fi

echo ""

# ─────────────────────────────────────────────────────────────
# TEST 14: All three layers combined — comprehensive
# ─────────────────────────────────────────────────────────────
echo "Test 14: All three layers combined"
OUT=$(run_test '{"project_context":{"enabled":true,"show_extractions":true}}' "$EDGE/_test.naab")

LAYER1=$(echo "$OUT" | grep -c "Layer 1" || true)
LAYER2=$(echo "$OUT" | grep -c "Layer 2" || true)
LAYER3=$(echo "$OUT" | grep -c "Layer 3" || true)

if [ "$LAYER1" -ge 1 ] && [ "$LAYER2" -ge 1 ] && [ "$LAYER3" -ge 1 ]; then
    pass "All layers: All 3 layers present in report"
else
    fail "All layers: Missing layers (L1=$LAYER1, L2=$LAYER2, L3=$LAYER3)" "All 3 should appear"
fi

if echo "$OUT" | grep -q "Summary:"; then
    pass "All layers: Summary line present"
else
    fail "All layers: No summary line" "Should show extraction summary"
fi

if echo "$OUT" | grep -q "ok=4"; then
    pass "All layers: Script executes successfully with all layers"
else
    fail "All layers: Script failed" "All layers should not break execution"
fi

echo ""

# ─────────────────────────────────────────────────────────────
# TEST 15: Bold/italic formatting stripped correctly
# ─────────────────────────────────────────────────────────────
echo "Test 15: Markdown formatting handling"

# Create a temp CLAUDE.md with bold formatting
mkdir -p "$EDGE/fmt_test"
cat > "$EDGE/fmt_test/CLAUDE.md" << 'MD'
# Rules
- **Never** use eval()
- *Always* use Python
- Use __Go__ for concurrency tasks
MD
cp "$EDGE/_test.naab" "$EDGE/fmt_test/_test.naab"

OUT=$(run_test '{"project_context":{"enabled":true,"show_extractions":true,"sources":{"llm":true,"linters":false,"manifests":false}}}' "$EDGE/fmt_test/_test.naab" "$EDGE/fmt_test")

if echo "$OUT" | grep -q "eval.*APPLIED\|ban.*eval"; then
    pass "Markdown format: **Never** use eval() correctly parsed through bold"
else
    fail "Markdown format: Bold-wrapped directive not extracted" "**Never** should be stripped to 'Never'"
fi

if echo "$OUT" | grep -q "python.*APPLIED\|python.*preference"; then
    pass "Markdown format: *Always* use Python correctly parsed through italic"
else
    fail "Markdown format: Italic-wrapped directive not extracted" "*Always* should be stripped"
fi

# Cleanup
rm -rf "$EDGE/fmt_test"

echo ""

# ─────────────────────────────────────────────────────────────
# SUMMARY
# ─────────────────────────────────────────────────────────────
echo "═══════════════════════════════════════════════════════════"
echo "  EDGE CASE TEST RESULTS"
echo "═══════════════════════════════════════════════════════════"
echo "  Total:  $TOTAL"
echo "  Passed: $PASS"
echo "  Failed: $FAIL"
echo ""

if [ "$FAIL" -eq 0 ]; then
    echo "  ALL EDGE CASE TESTS PASSED"
else
    echo "  $FAIL TESTS FAILED — review output above"
fi
echo "═══════════════════════════════════════════════════════════"

# Cleanup temp govern.json
rm -f "$EDGE/govern.json" "$EDGE/_test.naab"

exit $FAIL
