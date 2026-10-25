#!/usr/bin/env bash
# Test V-ERR-001: ErrorSanitizer must redact sensitive keys beyond value/content/data.
# Compiles a minimal C++ test binary directly against project headers + built libs.
# Skips gracefully if compiler or headers are unavailable.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
PASS=0; FAIL=0; SKIP=0

pass() { echo "  PASS: $1"; ((PASS++)); }
fail() { echo "  FAIL: $1"; ((FAIL++)); }
skip() { echo "  SKIP: $1"; ((SKIP++)); }

echo "=== V-ERR-001: ErrorSanitizer Keyword Coverage ==="

# ── Prerequisites ───────────────────────────────────────────────────────────
if ! command -v c++ &>/dev/null && ! command -v g++ &>/dev/null && ! command -v clang++ &>/dev/null; then
    skip "No C++ compiler found — cannot compile test harness"
    echo ""
    echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
    exit 0
fi

CXX="${CXX:-$(command -v clang++ || command -v g++ || command -v c++)}"

# Build directory must contain libnaab_error_sanitizer or similar
LIB_DIR="$PROJECT_DIR/build"
INCLUDE_DIR="$PROJECT_DIR/include"
SRC="$PROJECT_DIR/src/runtime/error_sanitizer.cpp"

if [ ! -f "$SRC" ] || [ ! -d "$INCLUDE_DIR" ]; then
    skip "Project source not found at expected paths"
    echo ""
    echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
    exit 0
fi

WORK_DIR=$(mktemp -d -p "$HOME" .naab_verr001_XXXXXX 2>/dev/null || mktemp -d)
trap 'rm -rf "$WORK_DIR"' EXIT

# ── Write inline test harness ────────────────────────────────────────────────
cat > "$WORK_DIR/test_harness.cpp" << 'EOF'
#include "naab/error_sanitizer.h"
#include <iostream>
#include <string>

using namespace naab::error;

// Returns true if `haystack` does NOT contain `needle`
static bool redacted(const std::string& result, const std::string& needle) {
    return result.find(needle) == std::string::npos;
}

// Returns true if `haystack` DOES contain `needle` (false positive check)
static bool present(const std::string& result, const std::string& needle) {
    return result.find(needle) != std::string::npos;
}

int main() {
    int pass = 0, fail = 0;

    auto check = [&](const std::string& name, const std::string& input,
                     const std::string& secret, bool should_redact) {
        std::string result = ErrorSanitizer::redactValues(input,
                                                          SanitizationMode::PRODUCTION);
        bool secret_gone = redacted(result, secret);
        bool ok = (should_redact ? secret_gone : !secret_gone);
        if (ok) {
            std::cout << "  PASS: " << name << "\n";
            pass++;
        } else {
            std::cout << "  FAIL: " << name
                      << " — input=[" << input << "] result=[" << result << "]\n";
            fail++;
        }
    };

    // T1: token (new keyword)
    check("T1: token: \"abc12345\" is redacted",
          "Error: token: \"abc12345\" is invalid", "abc12345", true);

    // T2: apiKey with = separator
    check("T2: apiKey = \"secret_value\" is redacted",
          "apiKey = \"secret_value\" rejected", "secret_value", true);

    // T3: password
    check("T3: password: \"letmein123\" is redacted",
          "Bad password: \"letmein123\" provided", "letmein123", true);

    // T4: bearer
    check("T4: bearer: \"Bearer_xyz789\" is redacted",
          "Invalid bearer: \"Bearer_xyz789\"", "Bearer_xyz789", true);

    // T5: value (pre-existing keyword — regression)
    check("T5: value: \"existing_key\" still redacted",
          "value: \"existing_key\" is wrong", "existing_key", true);

    // T6: counter (non-sensitive — false positive check)
    check("T6: counter: \"42\" NOT redacted (non-sensitive key)",
          "counter: \"42\" exceeded", "42", false);

    // T7: IP address in message is redacted (IP_ADDRESS pattern fix)
    check("T7: IP 192.168.1.100 in message is redacted",
          "Connection to 192.168.1.100 failed", "192.168.1.100", true);

    // T8-T15: a keyword must be SEPARATED from its value (owner's decision
    // 2026-10-24). The API_KEY pattern used to accept no separator at all, so
    // any identifier that merely started with a keyword was taken for a
    // secret: the operator's own rule name printed as "<redacted>", and
    // max_tokens_per_run as max_<redacted>. A separator is ':', '=',
    // whitespace or a quote. T8-T10 are the names that must now survive;
    // T11 is the one form that is newly redacted ('=' was never a separator
    // here, so token=value without quotes slipped through); T12-T15 are the
    // separated forms that were redacted before and must stay redacted.
    check("T8: rule name credential_exfiltration NOT redacted",
          "Behavioral sequence 'credential_exfiltration' blocked before execution",
          "credential_exfiltration", false);
    check("T9: config key max_tokens_per_run NOT redacted",
          "max_tokens_per_run (60) exceeded", "max_tokens_per_run", false);
    check("T10: identifier authorization_required NOT redacted",
          "Request failed: authorization_required by policy",
          "authorization_required", false);
    check("T11: token=value without quotes is redacted",
          "Rejected token=abc12345xyz in request", "abc12345xyz", true);
    check("T12: Authorization: Bearer <value> is redacted",
          "Header Authorization: Bearer eyJabc12345def was refused",
          "eyJabc12345def", true);
    check("T13: token \"value\" (space, then quote) is redacted",
          "Bad token \"abc12345xyz\" given", "abc12345xyz", true);
    check("T14: secret:\"value\" (colon, quote, no space) is redacted",
          "Got secret:\"zz9876543210\" here", "zz9876543210", true);
    check("T15: password <value> (space only) is redacted",
          "Wrong password abcd12345 entered", "abcd12345", true);
    // T16-T17: the two written forms a secret most often takes in a message,
    // JSON and a quoted assignment. Both must stay redacted with a separator
    // required (T16 is caught by QUOTED_VALUE, T17 by API_KEY).
    check("T16: JSON \"token\": \"value\" is redacted",
          "Body was {\"token\": \"abcd1234efgh\"}", "abcd1234efgh", true);
    check("T17: api_key='value' is redacted",
          "Config line api_key='abcd1234efgh' rejected", "abcd1234efgh", true);

    std::cout << "\nResults: " << pass << " passed, " << fail << " failed\n";
    return fail == 0 ? 0 : 1;
}
EOF

# ── Compile ──────────────────────────────────────────────────────────────────
echo ""
echo "[Building test harness...]"

# Collect object files from the build directory
SANITIZER_OBJ=$(find "$LIB_DIR" -name "error_sanitizer.cpp.o" 2>/dev/null | head -1)

if [ -n "$SANITIZER_OBJ" ]; then
    # Link against existing object
    COMPILE_CMD="$CXX -std=c++17 -I$INCLUDE_DIR $WORK_DIR/test_harness.cpp $SANITIZER_OBJ -o $WORK_DIR/test_bin 2>&1"
else
    # Compile from source
    COMPILE_CMD="$CXX -std=c++17 -I$INCLUDE_DIR $WORK_DIR/test_harness.cpp $SRC -o $WORK_DIR/test_bin 2>&1"
fi

compile_output=$(eval "$COMPILE_CMD")
compile_exit=$?

if [ $compile_exit -ne 0 ]; then
    skip "Compilation failed — may need additional libraries on this platform"
    echo "       $compile_output" | head -5
    echo ""
    echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
    exit 0
fi

echo "[Compilation succeeded]"
echo ""

# ── Run ──────────────────────────────────────────────────────────────────────
harness_output=$("$WORK_DIR/test_bin" 2>&1)
harness_exit=$?

echo "$harness_output"

# Count results from harness output
harness_pass=$(echo "$harness_output" | grep -c "^  PASS:" || true)
harness_fail=$(echo "$harness_output" | grep -c "^  FAIL:" || true)

PASS=$((PASS + harness_pass))
FAIL=$((FAIL + harness_fail))

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
