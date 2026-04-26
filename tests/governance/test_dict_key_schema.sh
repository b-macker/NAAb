#!/bin/bash
# Test: Dict key pre-flight scanner check
# Verifies that the scanner detects .get() calls with wrong keys against actual JSON files
set -e

NAAB_GOV="$(cd "$(dirname "$0")/../.." && pwd)/build/naab-gov"
WORK_DIR="$(mktemp -d)"
trap "rm -rf $WORK_DIR" EXIT

if [ ! -f "$NAAB_GOV" ]; then
    echo "  SKIP: naab-gov binary not found"
    exit 0
fi

PASS=0
TOTAL=0

# Create a test JSON data file
mkdir -p "$WORK_DIR/data"
cat > "$WORK_DIR/data/service.json" <<'EOF'
{
    "entries": [1, 2, 3],
    "service_name": "redis",
    "env_vars": {"PORT": "6379"}
}
EOF

cat > "$WORK_DIR/govern.json" <<'EOF'
{
    "mode": "enforce",
    "scanner": {
        "lang_naab": {
            "dict_key_schema_check": true
        }
    }
}
EOF

# --- T1: Wrong key detected by scanner ---
TOTAL=$((TOTAL+1))
cat > "$WORK_DIR/test1.naab" <<'EOF'
use json
use file
fn load() {
    let raw = json.parse(file.read("data/service.json"))
    let cache = raw.get("cache")
    return cache
}
main {
    let r = load()
    print(string(r))
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB_GOV" scan "$WORK_DIR/test1.naab" 2>&1) || true
if echo "$OUTPUT" | grep -qi "dict_key_schema_check\|did you mean\|not found in JSON\|not in JSON"; then
    echo "  PASS: T1: Scanner detects wrong key 'cache' (entries available)"
    PASS=$((PASS+1))
else
    echo "  FAIL: T1: Scanner should detect wrong key 'cache'"
    echo "  OUTPUT: $OUTPUT"
fi

# --- T2: Correct key should not be flagged ---
TOTAL=$((TOTAL+1))
cat > "$WORK_DIR/test2.naab" <<'EOF'
use json
use file
fn load() {
    let raw = json.parse(file.read("data/service.json"))
    let data = raw.get("entries")
    return data
}
main {
    let r = load()
    print(string(r))
}
EOF
OUTPUT=$(cd "$WORK_DIR" && "$NAAB_GOV" scan "$WORK_DIR/test2.naab" 2>&1) || true
if echo "$OUTPUT" | grep -q "dict_key_schema_check"; then
    echo "  FAIL: T2: Correct key 'entries' should not be flagged"
else
    echo "  PASS: T2: Correct key 'entries' not flagged"
    PASS=$((PASS+1))
fi

echo "  test_dict_key_schema.sh: $PASS/$TOTAL PASSED"
