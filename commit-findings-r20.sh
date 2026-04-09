#!/usr/bin/env bash
# Commit R20 security findings and fixes
set -euo pipefail

cd "$(dirname "$0")"

git add \
    src/interpreter/interpreter.cpp \
    src/interpreter/expressions.cpp \
    include/naab/stdlib.h \
    src/stdlib/stdlib.cpp \
    src/api/rest_api.cpp \
    include/naab/ffi_async_callback.h \
    src/runtime/ffi_async_callback.cpp \
    tests/security/test_r20_fixes.sh \
    security_refuted_findings.md

git commit -m "$(cat <<'EOF'
Security R20: fix nested container taint bypass, REST API serialization DoS, async taint loss

V-GOV-013: findRootIdentifier() recursive helper added to expressions.cpp and
interpreter.cpp so that subscript chains like arr[0]=s and arr[0].push(s)
correctly mark the root container (arr) as tainted. Previous dynamic_cast to
IdentifierExpr* silently dropped taint for any non-trivial LHS.

V-DOS-002: replace global stdout_capture_mutex_+rdbuf() redirect in rest_api.cpp
with thread_local std::ostream* tl_capture_stream in stdlib.cpp. Each REST worker
thread captures to its own ostringstream; concurrent requests no longer serialize.

V-GOV-015: AsyncCallbackResult gains is_tainted field; AsyncCallbackWrapper gains
TaintReporterFunc taint_reporter_ hook. After executeWithTimeout() succeeds,
taint_reporter_() is called (if set) and the result is propagated through
makeSuccess(), closing the governance taint gap across FFI async boundaries.

V-GOV-014 refuted — container literal taint already handled by BUG-U/BUG-V
branches in expressionContainsTaint() (ListExpr/DictExpr). Details in
security_refuted_findings.md.

Tests: 7/7 pass (tests/security/test_r20_fixes.sh)

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

echo "R20 committed: $(git rev-parse --short HEAD)"
