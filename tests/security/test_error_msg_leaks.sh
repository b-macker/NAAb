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

# ---------------------------------------------------------------------------
# One first-stage grep per FILE, not per (file, pattern).
#
# This used to run a 10-process pipeline (grep + 9 x grep -v) for each of the
# 855 (file, pattern) pairs: 8,590 execve and 9,464 forks per run. On Linux that
# is ~5 s; under MSYS2, where fork() is emulated, it was 138 s on the Windows
# runner -- the slowest suite there -- for 0.06-0.3 s of actual grep work.
# See docs/findings/build-windows-ci.md (item 3).
#
# Same answers, by construction: every pattern goes into one `grep -f`, the
# exclusion filters run once over the union, and each pattern then picks its own
# lines in bash. The filters are line-wise, so filtering the union and then
# selecting is the same as selecting and then filtering. Checked byte-for-byte
# against the old loop on the clean tree and on a copy with planted leaks.
#
# CAUTION when adding a pattern: grep reads these as BASIC regexes and the
# per-pattern selection below uses bash's EXTENDED regexes. They agree for
# everything in the list today (literals, '.', '.*'); a pattern containing
# + ? | ( ) { } would mean different things to the two and needs care.
# ---------------------------------------------------------------------------
PATFILE="$(mktemp)"
CTRL_FILE="$(mktemp)"
trap 'rm -f "$PATFILE" "$CTRL_FILE"' EXIT
for entry in "${BANNED_IN_STRINGS[@]}"; do
    printf '%s\n' "\".*${entry%%|*}"
done > "$PATFILE"

# Lines of $1 that contain any banned pattern inside a string literal, minus
# comments, declarations, comparisons, unsetenv/setenv, blocklist array entries
# and enum-style string constants.
filtered_lines() {
    grep -n -f "$PATFILE" "$1" 2>/dev/null \
        | grep -v '^\s*//' \
        | grep -v 'static const char\*' \
        | grep -v 'unsetenv(' \
        | grep -v 'setenv(' \
        | grep -v '== "' \
        | grep -v '{".*,' \
        | grep -v 'blocked_env_vars' \
        | grep -v 'NAAB_INTERNAL_ENV_VARS' \
        | grep -v '^\s*[0-9]*:\s*"[A-Z_]*",'
}

# Sets MATCHES to the lines of $1 matching pattern $2 (as the old per-pattern
# grep did). A global rather than $(...), because a subshell per call would put
# back most of the forks this replaced.
select_matches() {
    MATCHES=""
    [ -n "$1" ] || return 0
    local re="\".*$2" line
    while IFS= read -r line; do
        [[ $line =~ $re ]] && MATCHES+="${MATCHES:+$'\n'}$line"
    done <<< "$1"
}

echo "=== Error Message Leak Check ==="
echo ""

# Positive control, through the same two functions. Without it a matcher that
# silently matched nothing -- an unreadable pattern file, a quoting slip in the
# selection -- would report every file clean. The second line is the negative
# half: an excluded shape must stay excluded.
printf '%s\n' \
    'throw std::runtime_error("planted: try --no-governance");' \
    'static const char* planted = "--keygen";' > "$CTRL_FILE"
ctrl_lines=$(filtered_lines "$CTRL_FILE")
select_matches "$ctrl_lines" '--no-governance'; ctrl_pos="$MATCHES"
select_matches "$ctrl_lines" '--keygen';        ctrl_neg="$MATCHES"
if [ -z "$ctrl_pos" ] || [ -n "$ctrl_neg" ]; then
    echo "  FAIL: matcher control -- planted literal flagged: $([ -n "$ctrl_pos" ] && echo yes || echo NO)," \
         "excluded line flagged: $([ -n "$ctrl_neg" ] && echo YES || echo no)"
    echo "        The checks below cannot be trusted; not running them."
    exit 1
fi
echo "  Control: a planted literal is flagged, an excluded declaration is not"
echo ""

for src in "${SECURITY_FILES[@]}"; do
    filepath="$LANG_DIR/$src"
    [ -f "$filepath" ] || continue

    filtered=$(filtered_lines "$filepath")

    for entry in "${BANNED_IN_STRINGS[@]}"; do
        pattern="${entry%%|*}"
        desc="${entry##*|}"

        select_matches "$filtered" "$pattern"
        if [ -n "$MATCHES" ]; then
            echo "  FAIL: $src — $desc"
            echo "        Pattern: $pattern"
            echo "$MATCHES" | head -3 | sed 's/^/        /'
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
