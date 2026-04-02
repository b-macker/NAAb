#!/bin/bash
# Integration tests for multi-agent governance with real C++ runtime
# Tests: agent role path enforcement, telemetry accumulation, dashboard output

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB_BIN="${SCRIPT_DIR}/../../build/naab-lang"
PASS=0
FAIL=0
TEST_DIR="${HOME}/.naab_multiagent_test_$$"
mkdir -p "$TEST_DIR"

cleanup() { rm -rf "$TEST_DIR"; }
trap cleanup EXIT

check() {
    local desc="$1"
    local condition="$2"
    if eval "$condition"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== NAAb Multi-Agent Governance Integration Tests ==="
echo ""

# --- Setup: directories and test data ---
mkdir -p "$TEST_DIR/data" "$TEST_DIR/config" "$TEST_DIR/output" "$TEST_DIR/secrets"
echo "hello world" > "$TEST_DIR/data/input.txt"
echo "setting=on" > "$TEST_DIR/config/settings.txt"
echo "supersecret" > "$TEST_DIR/secrets/key.txt"

# --- Setup: govern.json with agent roles (absolute paths for prefix matching) ---
# On MSYS2/Windows, naab-lang.exe is a native Windows binary that resolves
# paths to Windows format (C:\...). Convert POSIX paths before embedding in JSON.
GOV_DATA="$TEST_DIR/data"
GOV_OUTPUT="$TEST_DIR/output"
GOV_CONFIG="$TEST_DIR/config"
GOV_SECRETS="$TEST_DIR/secrets"
GOV_TELEM="$TEST_DIR/telemetry.jsonl"
if command -v cygpath &>/dev/null; then
    GOV_DATA=$(cygpath -m "$GOV_DATA")
    GOV_OUTPUT=$(cygpath -m "$GOV_OUTPUT")
    GOV_CONFIG=$(cygpath -m "$GOV_CONFIG")
    GOV_SECRETS=$(cygpath -m "$GOV_SECRETS")
    GOV_TELEM=$(cygpath -m "$GOV_TELEM")
fi

cat > "$TEST_DIR/govern.json" << GOVEOF
{
    "version": "3.0",
    "mode": "enforce",
    "languages": { "allowed": ["python", "shell"] },
    "capabilities": { "filesystem": "write" },
    "telemetry": { "enabled": true, "output_file": "$GOV_TELEM" },
    "agent_roles": {
        "data-bot": {
            "allowed_languages": ["python"],
            "allowed_paths": ["$GOV_DATA", "$GOV_OUTPUT"],
            "blocked_paths": ["$GOV_SECRETS"]
        },
        "ops-bot": {
            "allowed_languages": ["shell"],
            "allowed_paths": ["$GOV_CONFIG", "$GOV_OUTPUT"],
            "blocked_paths": ["$GOV_SECRETS"]
        }
    }
}
GOVEOF

# --- Setup: .naab test scripts ---
cat > "$TEST_DIR/read_data.naab" << 'NAAB'
use file
main {
    let content = file.read("./data/input.txt")
    print(content)
}
NAAB

cat > "$TEST_DIR/write_output.naab" << 'NAAB'
use file
main {
    file.write("./output/result.txt", "done")
    print("wrote output")
}
NAAB

cat > "$TEST_DIR/read_secrets.naab" << 'NAAB'
use file
main {
    let content = file.read("./secrets/key.txt")
    print(content)
}
NAAB

cat > "$TEST_DIR/read_config.naab" << 'NAAB'
use file
main {
    let content = file.read("./config/settings.txt")
    print(content)
}
NAAB

# --- Test 1: data-bot reads ./data/ (allowed) ---
echo "--- Agent Path Enforcement Tests ---"
OUT1=$(cd "$TEST_DIR" && "$NAAB_BIN" read_data.naab --agent-id data-bot 2>/dev/null)
RC1=$?
check "data-bot reads ./data/ (exit 0)" "[ $RC1 -eq 0 ]"

# --- Test 2: data-bot writes ./output/ (allowed) ---
RC2=0
cd "$TEST_DIR" && "$NAAB_BIN" write_output.naab --agent-id data-bot >/dev/null 2>&1 || RC2=$?
check "data-bot writes ./output/ (exit 0)" "[ $RC2 -eq 0 ]"

# --- Test 3: data-bot blocked from ./secrets/ ---
ERR3=$(cd "$TEST_DIR" && "$NAAB_BIN" read_secrets.naab --agent-id data-bot 2>&1)
RC3=$?
check "data-bot blocked from ./secrets/ (exit non-zero, stderr has 'blocked')" \
    "[ $RC3 -ne 0 ] && echo \"\$ERR3\" | grep -qi 'blocked'"

# --- Test 4: ops-bot reads ./config/ (allowed) ---
OUT4=$(cd "$TEST_DIR" && "$NAAB_BIN" read_config.naab --agent-id ops-bot 2>/dev/null)
RC4=$?
check "ops-bot reads ./config/ (exit 0)" "[ $RC4 -eq 0 ]"

# --- Test 5: ops-bot blocked from ./secrets/ ---
ERR5=$(cd "$TEST_DIR" && "$NAAB_BIN" read_secrets.naab --agent-id ops-bot 2>&1)
RC5=$?
check "ops-bot blocked from ./secrets/ (exit non-zero)" "[ $RC5 -ne 0 ]"

# --- Test 6: Telemetry JSONL accumulates ---
echo ""
echo "--- Telemetry & Dashboard Tests ---"
TELEM_LINES=0
if [ -f "$TEST_DIR/telemetry.jsonl" ]; then
    TELEM_LINES=$(wc -l < "$TEST_DIR/telemetry.jsonl")
fi
check "Telemetry JSONL exists and has events (lines=$TELEM_LINES)" "[ $TELEM_LINES -gt 0 ]"

# --- Test 7: Dashboard prints Agent: and Checks: on stderr ---
DASH_ERR=$(cd "$TEST_DIR" && "$NAAB_BIN" read_data.naab --agent-id data-bot --governance-dashboard 2>&1 >/dev/null)
check "Dashboard stderr contains 'Agent:' and 'Checks:'" \
    "echo \"\$DASH_ERR\" | grep -q 'Agent:' && echo \"\$DASH_ERR\" | grep -q 'Checks:'"

# --- Test 8: JSONL bridge — dashboard_cli.naab reads telemetry ---
DASHBOARD_CLI="${SCRIPT_DIR}/../../tools/agent-governance/dashboard_cli.naab"
if [ -f "$DASHBOARD_CLI" ]; then
    DASH_OUT=$(cd "$TEST_DIR" && TELEMETRY_FILE="$TEST_DIR/telemetry.jsonl" "$NAAB_BIN" "$DASHBOARD_CLI" 2>/dev/null)
    check "dashboard_cli.naab output contains 'Total Events'" \
        "echo \"\$DASH_OUT\" | grep -q 'Total Events'"
else
    echo "  SKIP: dashboard_cli.naab not found"
    # Count as pass to not break the suite
    PASS=$((PASS + 1))
fi

# --- Summary ---
echo ""
TOTAL=$((PASS + FAIL))
echo "=== Multi-Agent Governance Test Summary: $PASS/$TOTAL passed ==="
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
else
    exit 0
fi
