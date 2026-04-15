#!/bin/bash
# Fixes from Gemini naab-7 session (supply chain audit)
# Subcommand flag parsing, installAll reporting, unpinned dep warnings

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

echo "=== naab-7 Session Fixes ==="
echo ""

echo "--- Fix 1: Subcommand flag parsing ---"

# T1: Source — install handler skips flags
grep -A20 'command == "install"' "$SRC/cli/main.cpp" | grep -q 'rfind("--"'
check $? "Install handler skips -- flags"

# T2: Runtime — --verbose not treated as package name
WORK_DIR=$(mktemp -d "${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab7_XXXXXX")
cat > "$WORK_DIR/govern.json" << 'G'
{"version":"1.0.0","mode":"off"}
G
cat > "$WORK_DIR/naab.toml" << 'T'
[package]
name = "test"
version = "1.0.0"
T
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" install --verbose 2>&1)
if echo "$OUTPUT" | grep -q "naab-community/--verbose\|Invalid package spec.*--verbose"; then
    check 1 "--verbose treated as package name"
else
    check 0 "--verbose not treated as package name"
fi
rm -rf "$WORK_DIR"

echo ""
echo "--- Fix 2: installAll reporting ---"

# T3: Source — installAll prints summary
grep -q 'Install complete' "$SRC/packages/package_manager.cpp"
check $? "installAll prints Install complete summary"

# T4: Source — installAll handles string version deps
grep -q 'val.is_string()' "$SRC/packages/package_manager.cpp"
check $? "installAll handles string version deps"

echo ""
echo "--- Fix 3: Unpinned dependency warning ---"

# T5: Source — unpinned git warning exists
grep -q 'unpinned git URL' "$SRC/packages/package_manager.cpp"
check $? "Unpinned git URL warning exists"

# T6: Source — suggests commit/tag/ref fix
grep -q 'commit.*tag.*ref' "$SRC/packages/package_manager.cpp"
check $? "Warning suggests commit/tag/ref pinning"

echo ""
echo "--- Fix 4: Build scripts not executed (security) ---"

# T7: Source — no pre_install/post_install execution in package_manager
if grep -q 'pre_install\|post_install' "$SRC/packages/package_manager.cpp"; then
    check 1 "Package manager should NOT reference pre_install/post_install"
else
    check 0 "Package manager correctly ignores build scripts"
fi

echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="
exit $FAIL
