#!/bin/bash
# V-PKG-001/V-PKG-002: Package manager security tests
# Shell injection prevention + integrity hash verification

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
PASS=0; FAIL=0; TOTAL=0

check() {
    TOTAL=$((TOTAL + 1))
    if [ "$1" = "0" ]; then
        PASS=$((PASS + 1))
        echo "  PASS: T$TOTAL - $2"
    else
        FAIL=$((FAIL + 1))
        echo "  FAIL: T$TOTAL - $2"
    fi
}

SRC="$SCRIPT_DIR/../../src"

echo "=== Package Manager Security Tests ==="
echo ""

echo "--- V-PKG-001: Shell injection prevention ---"

# T1: Source — no system() on POSIX path
if grep -A5 'Extract using tar' "$SRC/packages/package_manager.cpp" | grep -q 'system('; then
    check 1 "system() still used for tarball extraction"
else
    check 0 "fork/execvp used instead of system() for extraction"
fi

# T2: Source — parseSpec rejects metacharacters
grep -q 'Reject shell metacharacters' "$SRC/packages/package_manager.cpp"
check $? "parseSpec validates against shell metacharacters"

# T3: Runtime — single quote injection blocked
WORK_DIR=$(mktemp -d "${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab_pkg_XXXXXX")
cat > "$WORK_DIR/govern.json" << 'G'
{"version":"1.0.0","mode":"off"}
G
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" install "evil';echo pwned;'" 2>&1)
echo "$OUTPUT" | grep -q "Invalid package spec"
check $? "Single quote injection rejected"

# T4: Runtime — semicolon injection blocked
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" install "user/repo;rm -rf /" 2>&1)
echo "$OUTPUT" | grep -q "Invalid package spec"
check $? "Semicolon injection rejected"

# T5: Runtime — backtick injection blocked
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" install 'user/repo`id`' 2>&1)
echo "$OUTPUT" | grep -q "Invalid package spec"
check $? "Backtick injection rejected"

# T6: Runtime — pipe injection blocked
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" install "user/repo|cat /etc/passwd" 2>&1)
echo "$OUTPUT" | grep -q "Invalid package spec"
check $? "Pipe injection rejected"

rm -rf "$WORK_DIR"

echo ""
echo "--- V-PKG-002: Integrity hash verification ---"

# T7: Source — hash computed after download
grep -q 'computeSHA256.*tarball_path' "$SRC/packages/package_manager.cpp"
check $? "SHA-256 computed on downloaded tarball"

# T8: Source — integrity stored in lockfile
grep -q 'entry.integrity.*last_download_hash' "$SRC/packages/package_manager.cpp"
check $? "Integrity hash stored in lockfile entry"

# T9: Source — integrity verified on re-install
grep -q 'Integrity check failed' "$SRC/packages/package_manager.cpp"
check $? "Integrity verification on re-install exists"

echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="
exit $FAIL
