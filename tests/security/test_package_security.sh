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
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/naab_pkg_XXXXXX")
# A failed mktemp leaves $WORK_DIR EMPTY, and every "$WORK_DIR/x" write below then
# rebases onto the filesystem ROOT. That is how a stray govern.json reached /
# and silently governed every later run on the machine. Fail loudly instead.
[ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ] || { echo "FATAL: could not create work dir" >&2; exit 1; }
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

# T8 REMOVED. It grepped for `entry.integrity.*last_download_hash` -- the
# PRE-FIX shape, where the lockfile pin was written from the member
# last_download_hash_ read AFTER the recursive transitive-dependency loop had
# overwritten it. That defect was fixed by handing the expected hash DOWN into
# downloadFromGitHub(), so the assertion began failing BECAUSE the bug was
# fixed, and chasing it green would mean reinstating the defect.
# The guarantee it meant to cover is now tested behaviourally, in a suite that
# actually runs: tests/package_manager/test_package_integrity.sh Group B
# (B-03 is the load-bearing case), registered at run-all-tests.sh:1509.

# T9: Source — integrity verified on re-install
grep -q 'Integrity check failed' "$SRC/packages/package_manager.cpp"
check $? "Integrity verification on re-install exists"

echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="
exit $FAIL
