#!/usr/bin/env bash
# test_error_msg_leaks.sh — Ensure error messages don't teach bypass techniques
#
# Governance/security error messages must NEVER contain:
#   - CLI flags that disable governance
#   - Specific sanitizer names/prefixes from config
#   - Sandbox escape flags
#   - Direct pointers to security config keys
#
# WHY: LLMs read error messages and use them to construct bypasses.
# An error that says "try --no-governance" is a one-step bypass recipe.
# An error that lists sanitizer prefixes enables identity-function laundering.
#
# This test greps inside STRING LITERALS only (text between quotes).

PASS=0
FAIL=0
LANG_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

# Files that contain governance/security error messages
SECURITY_FILES=(
    "src/runtime/governance_engine.cpp"
    "src/runtime/governance_checks.cpp"
    "src/runtime/governance_config.cpp"
    "src/runtime/governance_reports.cpp"
    "src/runtime/shell_executor.cpp"
    "src/runtime/persistent_process_executor.cpp"
    "src/runtime/trust_store.cpp"
    "src/interpreter/governance_taint.cpp"
    "src/interpreter/interpreter.cpp"
    "src/interpreter/call_dispatch.cpp"
    "src/stdlib/env_impl.cpp"
    "src/stdlib/file_impl.cpp"
    "src/stdlib/http_impl.cpp"
    "src/stdlib/process_impl.cpp"
    "src/vm/vm.cpp"
    "src/api/governance_c_api.cpp"
    "src/stdlib/agent_impl.cpp"
    "src/stdlib/codegen_impl.cpp"
    "src/interpreter/polyglot.cpp"
)

# Patterns that should NEVER appear in string literals within these files
# Format: "pattern|description"
BANNED_IN_STRINGS=(
    # CLI bypass flags
    '--no-governance|bypass flag leaked in error string'
    '--governance-override|bypass flag leaked in error string'
    '--sandbox-level|sandbox escape flag leaked in error string'
    '--allow-network|network escape flag leaked in error string'
    '--drift-baseline-save|baseline bypass flag leaked in error string'
    '--sign-governance|signing flag leaked in error string'
    '--sign-baseline|signing flag leaked in error string'
    '--keygen|key generation flag leaked in error string'
    # Config keys that teach weakening
    'taint_tracking.sanitizers|config key pointing LLM to sanitizer list'
    'taint_tracking.sources|config key pointing LLM to taint sources'
    'taint_tracking.sinks|config key pointing LLM to sink list'
    # Env var names for signing keys
    'NAAB_SIGNING_KEY|signing key env var leaked in error string'
    'NAAB_GOVERN_KEY|governance key env var leaked in error string'
    # Config weakening hints
    'Adjust.*govern.json|config weakening hint in error string'
    'adjust.*govern.json|config weakening hint in error string'
    'increase.*govern.json|config weakening hint in error string'
    'increase.*in govern|config weakening hint in error string'
    'increase.*max_output_size|config weakening hint in error string'
    'max_turns in govern|agent config key leaked in error string'
    'max_total_tokens in govern|agent config key leaked in error string'
    # Enforcement bypass hints
    'soft-mandatory|enforcement level bypass hint in error string'
    'soft.mandatory|enforcement level bypass hint in error string'
    # Tool execution internals
    's_registered_tools|internal tool map name leaked in error string'
    't_tool_agent_context|thread-local name leaked in error string'
    't_in_tool_execution|thread-local name leaked in error string'
    'tool_snapshot|internal structure name leaked in error string'
    # Codegen internals (match only inside string literals)
    't_codegen_nesting_depth|thread-local name leaked in error string'
    't_codegen_arg_tainted|thread-local name leaked in error string'
    'setCodegenArgTainted|internal function name leaked in error string'
    # Pulse internals
    'PulseVerdict|pulse enum type leaked in error string'
    'PULSE_DEGRADED|pulse BSD event constant leaked in error string'
    'PULSE_IMPAIRED|pulse BSD event constant leaked in error string'
    'computePulseVerdict|internal pulse method name leaked in error string'
    'consecutive_degraded|pulse hysteresis field leaked in error string'
    'advisory_count|pulse counter field leaked in error string'
    'pulse_.bsd_connected|internal pulse state leaked in error string'
    # Standing Lease internals
    'lease_granted_turn|standing lease tracker field leaked in error string'
    'lease_expires_turn|standing lease tracker field leaked in error string'
    # Advisory Escalation internals
    'AdvisoryEscalationConfig|advisory escalation config type leaked in error string'
    'weight_multiplier_|advisory escalation field leaked in error string'
    # Evidence Epoch internals
    'governance_epoch_|evidence epoch field leaked in error string'
    # Wall-clock lease internals
    'lease_granted_time|standing lease wall-clock field leaked in error string'
    # Advisory decay internals
    'decayAdvisoryHistory|advisory decay method leaked in error string'
    # BSD/CDD reconfigure internals
    'updateConfig|BSD/CDD internal method leaked in error string'
    # Telemetry pulse internals
    'telemetry_connected|pulse telemetry field leaked in error string'
)

# Only match inside string literals that are likely error messages.
# Exclude: comments, variable declarations, comparisons, unsetenv/setenv,
# blocklist array entries, and enum-style string constants.
# Reads `grep -n` output on stdin. ONE copy, shared by the screening pass and
# the per-pattern loop below, so the two cannot drift apart.
# Known: the first filter cannot match -- every input line starts with "N:" --
# so comment lines are scanned as live (docs/governance-campaign-findings.md).
# Kept as is: a speed change must not move a verdict.
string_literal_filter() {
    grep -v '^\s*//' \
        | grep -v 'static const char\*' \
        | grep -v 'unsetenv(' \
        | grep -v 'setenv(' \
        | grep -v '== "' \
        | grep -v '{".*,' \
        | grep -v 'blocked_env_vars' \
        | grep -v 'NAAB_INTERNAL_ENV_VARS' \
        | grep -v '^\s*[0-9]*:\s*"[A-Z_]*",'
}

# SCREENING PASS -- speed only; it never decides a FAIL.
#
# The per-pattern loop is 45 patterns x 19 files, each a pipeline of ten greps:
# ~8,600 processes. fork+exec is cheap on Linux and ~15 ms under MSYS2, which is
# why this suite took ~7 s on build-linux and ~130 s on build-windows.
#
# `grep -e A -e B` selects a line iff A or B matches it, so one grep with all 45
# patterns selects exactly the UNION of the lines the 45 per-pattern greps
# select, numbered identically. string_literal_filter judges each line on its
# own, so the filtered union is empty iff every per-pattern pipeline's output
# would be empty. A file that screens clean therefore earns exactly the 45
# PASSes the loop would have given it. Anything else -- any surviving line, or
# a grep error (exit 2, e.g. a pattern that does not compile) -- runs the
# original per-pattern loop, which is the only thing that prints a FAIL. A
# screen that errored must never read as clean: the loop then reproduces the
# old behaviour exactly, including its per-pattern 2>/dev/null.
SCREEN_ARGS=()
for entry in "${BANNED_IN_STRINGS[@]}"; do
    SCREEN_ARGS+=(-e "\".*${entry%%|*}")
done

echo "=== Error Message Leak Check ==="
echo ""

for src in "${SECURITY_FILES[@]}"; do
    filepath="$LANG_DIR/$src"
    [ -f "$filepath" ] || continue

    screen=$(grep -n "${SCREEN_ARGS[@]}" "$filepath" 2>/dev/null)
    screen_rc=$?
    if [ "$screen_rc" -eq 1 ] || { [ "$screen_rc" -eq 0 ] &&
            [ -z "$(printf '%s\n' "$screen" | string_literal_filter)" ]; }; then
        PASS=$((PASS + ${#BANNED_IN_STRINGS[@]}))
        continue
    fi

    for entry in "${BANNED_IN_STRINGS[@]}"; do
        pattern="${entry%%|*}"
        desc="${entry##*|}"

        matches=$(grep -n "\".*${pattern}" "$filepath" 2>/dev/null | string_literal_filter)
        if [ -n "$matches" ]; then
            echo "  FAIL: $src — $desc"
            echo "        Pattern: $pattern"
            echo "$matches" | head -3 | sed 's/^/        /'
            FAIL=$((FAIL + 1))
        else
            PASS=$((PASS + 1))
        fi
    done
done

# Also check that no error message iterates over sanitizer config to build output
# (runtime checks that READ sanitizers are fine; PRINTING them to users is not)
for src in "${SECURITY_FILES[@]}"; do
    filepath="$LANG_DIR/$src"
    [ -f "$filepath" ] || continue

    # Look for patterns that iterate config values into error strings
    # e.g.: msg += "\n    - " + san;  (dumping sanitizer names into output)
    matches=$(grep -n 'msg.*+=.*\bsan\b\|msg.*+=.*sanitizers_\|msg.*+=.*\.sanitizers' "$filepath" 2>/dev/null | grep -v '^\s*//')
    if [ -n "$matches" ]; then
        echo "  FAIL: $src — error message string built from sanitizer config values"
        echo "$matches" | head -3 | sed 's/^/        /'
        FAIL=$((FAIL + 1))
    else
        PASS=$((PASS + 1))
    fi
done

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="

if [ $FAIL -gt 0 ]; then
    echo ""
    echo "ERROR: Security error messages contain bypass information."
    echo ""
    echo "Rules for governance/security error messages:"
    echo "  1. NEVER put --no-governance or --governance-override in error text"
    echo "  2. NEVER list or iterate sanitizer names/prefixes in error output"
    echo "  3. NEVER suggest --sandbox-level or --allow-network in errors"
    echo "  4. NEVER point to specific govern.json keys (taint_tracking.sanitizers etc.)"
    echo "  5. DO say 'identity functions are detected' (deters gaming)"
    echo "  6. DO say 'see govern.json' generically (legitimate users know where to look)"
    exit 1
fi

echo "  All security error messages are clean."
exit 0
