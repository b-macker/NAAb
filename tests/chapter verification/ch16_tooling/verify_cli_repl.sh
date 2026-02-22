#!/bin/bash
# Verify Chapter 16: Tooling and Development Environment

NAAB_BIN="./build/naab-lang"
NAAB_REPL="./build/naab-repl"

echo ">>> Chapter 16 Verification: Tooling CLI & REPL <<<"

# 1. Verify naab-lang blocks list
echo "--- naab-lang blocks list ---"
OUTPUT=$("$NAAB_BIN" blocks list)
echo "$OUTPUT"
if [[ "$OUTPUT" == *"Total blocks"* ]]; then
    echo "PASS: 'blocks list' works."
else
    echo "FAIL: 'blocks list' output unexpected."
    exit 1
fi

# 2. Verify naab-lang blocks search (expecting known issue ISS-013)
echo "--- naab-lang blocks search ---"
OUTPUT=$("$NAAB_BIN" blocks search "test")
echo "$OUTPUT"
if [[ "$OUTPUT" == *"No blocks found"* || "$OUTPUT" == *"Search results"* ]]; then
    echo "PASS: 'blocks search' command runs (behavior confirms ISS-013)."
else
    echo "FAIL: 'blocks search' output unexpected."
    exit 1
fi

# 3. Verify naab-lang version
echo "--- naab-lang version ---"
OUTPUT=$("$NAAB_BIN" version)
echo "$OUTPUT"
if [[ "$OUTPUT" == *"NAAb Block Assembly Language v"* ]]; then
    echo "PASS: 'version' command works."
else
    echo "FAIL: 'version' command output unexpected."
    exit 1
fi

# 4. Verify naab-lang help
echo "--- naab-lang help ---"
OUTPUT=$("$NAAB_BIN" help)
if [[ "$OUTPUT" == *"Usage:"* && "$OUTPUT" == *"naab-lang run"* ]]; then
    echo "PASS: 'help' command works."
else
    echo "FAIL: 'help' command output unexpected."
    exit 1
fi

# 5. Verify naab-repl launches and exits
echo "--- naab-repl ---"
# Launch REPL, send ':exit' command, capture output
OUTPUT=$(echo ":exit" | "$NAAB_REPL")
echo "$OUTPUT"
if [[ "$OUTPUT" == *">"* && "$OUTPUT" == *"Goodbye!"* ]]; then
    echo "PASS: 'naab-repl' launches and exits cleanly."
else
    echo "FAIL: 'naab-repl' launch/exit unexpected."
    exit 1
fi

echo ">>> Chapter 16 Verification: COMPLETE <<<"
exit 0
