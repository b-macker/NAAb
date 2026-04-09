#!/usr/bin/env bash
# Commit R22 security findings and fixes
set -euo pipefail

cd "$(dirname "$0")"

git add \
    src/scanner/scanner.cpp \
    include/naab/governance.h \
    src/runtime/governance_config.cpp \
    src/runtime/governance_reports.cpp \
    tests/security/test_r22_fixes.sh \
    security_refuted_findings.md

git commit -m "$(cat <<'EOF'
Security R22: scanner symlink guard, 10MB read cap, per-agent shell enforcement

V-GOV-017: collectFiles now uses symlink_status().type() == file_type::regular
instead of is_regular_file() so file-level symlinks are rejected without resolving
them. scanFile adds a lstat() guard on POSIX before std::ifstream open as
defense-in-depth; S_ISLNK on the lstat result skips any symlink that passed
the collectFiles gate. sys/stat.h and unistd.h added under #ifndef _WIN32.

V-RT-013: scanFile replaces unbounded istreambuf_iterator read with a chunked
65KB-buffer loop capped at MAX_SCAN_FILE_BYTES (10MB). TOCTOU-swapped files
and /dev/zero are truncated after 10MB instead of causing std::bad_alloc.
Partial content is still useful for pattern matching.

V-GOV-018: AgentRoleConfig gains shell_allowed + shell_allowed_set fields.
governance_config.cpp parses "shell_allowed" from agent_roles JSON entries.
applyAgentRole() in governance_reports.cpp applies role.shell_allowed to
rules_.shell_allowed when shell_allowed_set=true; checkShellAllowed() inherits
the restriction without change. Agent log line now reports shell: allowed/blocked.

V-GOV-016 refuted: max_files=200 cap terminates collectFiles before any OOM or
OS path exhaustion; added detailed entry to security_refuted_findings.md.

Tests: 8/8 pass (tests/security/test_r22_fixes.sh)

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

echo "R22 committed: $(git rev-parse --short HEAD)"
