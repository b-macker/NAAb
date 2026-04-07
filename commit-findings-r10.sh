#!/usr/bin/env bash
# Commit V-ERR-001 fix: ErrorSanitizer keyword expansion + IP_ADDRESS pattern.
# Builds naab-lang, runs the new security test, then commits.

set -euo pipefail
cd "$(dirname "$0")"

echo "=== Building naab-lang ==="
cmake --build build --target naab-lang -j4

echo ""
echo "=== Making test script executable ==="
chmod +x tests/security/test_error_sanitizer_verr001.sh

echo ""
echo "=== Running V-ERR-001 test ==="
bash tests/security/test_error_sanitizer_verr001.sh

echo ""
echo "=== Committing ==="
git add \
    include/naab/error_sanitizer.h \
    src/runtime/error_sanitizer.cpp \
    tests/security/test_error_sanitizer_verr001.sh \
    commit-findings-r10.sh

git commit -m "$(cat <<'EOF'
fix(security): V-ERR-001 expand ErrorSanitizer keyword coverage

ErrorSanitizer::redactValues() used only 3 keywords (value/content/data),
leaking secrets under common names like token, apiKey, password, bearer,
auth, credential, access_token, refresh_token in PRODUCTION mode error msgs.

Changes:
- patterns::API_KEY: add camelCase variants (apiKey, authToken, bearerToken,
  secretKey); lower minimum match length from 16 → 8 chars
- patterns::QUOTED_VALUE: add token, key, secret, pass, password, auth,
  authorization, bearer, apikey, api_key, credential, access_token,
  refresh_token to keyword list
- sensitive_patterns_ vector: add patterns::IP_ADDRESS (was defined in header
  but never registered — IP addresses were not redacted at all)
- redactValues() inline value_pattern: sync keyword list with QUOTED_VALUE
  constant; add std::regex::icase for case-insensitive matching (Token/TOKEN)

New test: tests/security/test_error_sanitizer_verr001.sh
  Compiles a minimal C++ harness against project headers; 7 test cases covering
  token, apiKey, password, bearer, value (regression), counter (false positive),
  and IP address redaction. Skips gracefully if no C++ compiler available.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

echo ""
echo "Done. V-ERR-001 fix committed."
