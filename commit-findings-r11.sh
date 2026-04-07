#!/usr/bin/env bash
# Commit R11 fixes: V-API-001 (REST auth + body size), V-GOV-009 (CWD discovery), V-ERR-002 (ErrorSanitizer wired).
# Builds naab-lang + naab-gov, runs the security test, then commits.

set -euo pipefail
cd "$(dirname "$0")"

echo "=== Building naab-lang and naab-gov ==="
cmake --build build --target naab-lang naab-gov -j4

echo ""
echo "=== Making test script executable ==="
chmod +x tests/security/test_r11_fixes.sh

echo ""
echo "=== Running R11 security tests ==="
bash tests/security/test_r11_fixes.sh

echo ""
echo "=== Committing ==="
git add \
    CMakeLists.txt \
    include/naab/rest_api.h \
    src/api/rest_api.cpp \
    src/cli/main.cpp \
    src/cli/gov_main.cpp \
    tests/security/test_r11_fixes.sh \
    commit-findings-r11.sh

git commit -m "$(cat <<'EOF'
fix(security): R11 — V-API-001, V-GOV-009, V-ERR-002

V-API-001 (Critical): REST API unauthenticated RCE
- RestApiServer: add setApiKey() and setMaxBodySize() — wired via new
  --api-key <key> and --max-body <bytes> flags on 'naab-lang api'
- Auth: pre-routing handler checks Authorization: Bearer or X-API-Key;
  /health remains public; all other endpoints require the key when set
- Body cap: set_payload_max_length() applied at construction; default 1 MiB

V-GOV-009 (High): naab-gov config bypass via file-adjacent govern.json
- cmdLint: discover govern.json from fs::current_path() (CWD) by default
  instead of the scanned file's parent directory
- cmdScan: same fix — scanner.loadConfig() now receives CWD
- Add --config-from-file opt-in flag to restore source-relative discovery
  for monorepos where each file has its own govern.json

V-ERR-002 (Medium): ErrorSanitizer disconnected from error pipeline
- main.cpp: add #include error_sanitizer.h; wrap NaabError::formatError()
  and std::exception::what() in ErrorSanitizer::sanitize() before print;
  preserve raw_msg for Governance config error: exit-code detection
- rest_api.cpp: sanitize error messages in /execute execution error path
  and outer 500-handler before returning JSON to caller

New test: tests/security/test_r11_fixes.sh (3 groups, skips gracefully)

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

echo ""
echo "Done. R11 fixes committed."
