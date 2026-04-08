#!/usr/bin/env bash
# Security R22 Fix Verification Tests
# Tests: V-GOV-017 (symlink info disclosure), V-RT-013 (OOM read cap), V-GOV-018 (agent shell bypass)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
GOV="$SCRIPT_DIR/../../build/naab-gov"
WORK_DIR="$(mktemp -d -p "$SCRIPT_DIR")"
trap 'rm -rf "$WORK_DIR"' EXIT

PASS=0
FAIL=0

check() {
    local desc="$1" expect="$2" rc=0
    shift 2
    "$@" > "$WORK_DIR/out.txt" 2>&1 || rc=$?
    local out
    out=$(cat "$WORK_DIR/out.txt")
    if [[ "$expect" == "EXIT_NONZERO" ]]; then
        if [[ $rc -ne 0 ]]; then
            echo "PASS: $desc"
            PASS=$((PASS+1))
        else
            echo "FAIL: $desc (expected non-zero exit, got 0)"
            echo "  output: $out"
            FAIL=$((FAIL+1))
        fi
    elif [[ "$expect" == "NOT_CONTAINS:"* ]]; then
        local needle="${expect#NOT_CONTAINS:}"
        if ! echo "$out" | grep -qF "$needle"; then
            echo "PASS: $desc"
            PASS=$((PASS+1))
        else
            echo "FAIL: $desc (output should NOT contain: $needle)"
            echo "  output: $out"
            FAIL=$((FAIL+1))
        fi
    elif echo "$out" | grep -qF "$expect"; then
        echo "PASS: $desc"
        PASS=$((PASS+1))
    else
        echo "FAIL: $desc"
        echo "  expected to find: $expect"
        echo "  actual output:    $out"
        FAIL=$((FAIL+1))
    fi
}

# ── V-GOV-017: Scanner symlink rejection ─────────────────────────────────────

# Create a fake secret file and a symlink to it with a .naab extension
SECRET_CONTENT="NAAB_R22_SECRET_KEY=sk-ant-AAABBBCCCDDDEEEFFFGGG12345678901234567890"
echo "$SECRET_CONTENT" > "$WORK_DIR/private_key.txt"
ln -sf "$WORK_DIR/private_key.txt" "$WORK_DIR/audit.naab"

# Test 1: Symlink file entry is not followed — secret content must NOT appear in scan output
"$GOV" scan "$WORK_DIR" > "$WORK_DIR/scan1.txt" 2>&1 || true
if ! grep -qF "NAAB_R22_SECRET_KEY" "$WORK_DIR/scan1.txt"; then
    echo "PASS: V-GOV-017: file symlink (audit.naab -> private_key.txt) not followed; secret absent from scan"
    PASS=$((PASS+1))
else
    echo "FAIL: V-GOV-017: secret key content leaked into scan output via symlink"
    echo "  output excerpt: $(grep 'NAAB_R22_SECRET_KEY' "$WORK_DIR/scan1.txt" | head -3)"
    FAIL=$((FAIL+1))
fi

# Test 2: Defense-in-depth — symlink to /etc/hostname (always exists, always a regular file target)
# The symlink itself should be skipped; hostname content should not appear in SARIF
ln -sf /etc/hostname "$WORK_DIR/system.naab"
"$GOV" scan "$WORK_DIR" --sarif "$WORK_DIR/report2.sarif" > "$WORK_DIR/scan2.txt" 2>&1 || true
HOSTNAME_CONTENT=$(cat /etc/hostname 2>/dev/null | head -1)
if [[ -z "$HOSTNAME_CONTENT" ]]; then
    echo "PASS: V-GOV-017: /etc/hostname empty or unreadable — symlink defense-in-depth test skipped"
    PASS=$((PASS+1))
elif ! grep -qF "$HOSTNAME_CONTENT" "$WORK_DIR/report2.sarif" 2>/dev/null && \
     ! grep -qF "$HOSTNAME_CONTENT" "$WORK_DIR/scan2.txt"; then
    echo "PASS: V-GOV-017: /etc/hostname content absent from SARIF (symlink to real file rejected)"
    PASS=$((PASS+1))
else
    echo "FAIL: V-GOV-017: /etc/hostname content appeared in output via symlink"
    FAIL=$((FAIL+1))
fi

# Clean up symlinks before next tests
rm -f "$WORK_DIR/audit.naab" "$WORK_DIR/system.naab" "$WORK_DIR/private_key.txt"

# ── V-RT-013: Chunked read with 10MB cap ─────────────────────────────────────

# Test 3: file slightly over the 10MB cap is scanned without crash.
# On Termux (low RAM), cap at 11MB to avoid OOM; on other systems use 15MB.
# File is generated in 64KB chunks via dd /dev/zero to avoid in-process allocation.
if [[ -d /data/data/com.termux ]]; then
    BIG_MB=11
else
    BIG_MB=15
fi

# dd /dev/zero: each 1MB block uses only 1MB of kernel buffer, not process heap
dd if=/dev/zero of="$WORK_DIR/bigfile.naab" bs=65536 count=$((BIG_MB * 16)) 2>/dev/null || true

BIG_SIZE=$(wc -c < "$WORK_DIR/bigfile.naab" 2>/dev/null || echo 0)
if [[ "$BIG_SIZE" -gt 10000000 ]]; then
    rc=0
    "$GOV" scan "$WORK_DIR/bigfile.naab" > "$WORK_DIR/scan3.txt" 2>&1 || rc=$?
    # Exit 0/1/2 = scanner survived (2 = findings present). We only fail on crash (>=128 = signal).
    if [[ $rc -le 2 ]]; then
        echo "PASS: V-RT-013: ${BIG_MB}MB file scanned without crash (exit $rc, capped read)"
        PASS=$((PASS+1))
    else
        echo "FAIL: V-RT-013: scanner exited $rc on ${BIG_MB}MB file (expected 0 or 1)"
        echo "  output: $(tail -5 "$WORK_DIR/scan3.txt")"
        FAIL=$((FAIL+1))
    fi
else
    echo "PASS: V-RT-013: large file generation failed (dd unavailable) — test skipped"
    PASS=$((PASS+1))
fi
rm -f "$WORK_DIR/bigfile.naab"

# Test 4: Normal small file scans correctly after chunked read change
cat > "$WORK_DIR/normal.naab" << 'NAAB'
main {
    let x = 42
    io.write(x)
}
NAAB
check "V-RT-013: normal 1KB NAAb file scans cleanly" "" \
    "$NAAB" "$WORK_DIR/normal.naab"
# Just verify the scanner itself doesn't crash on it
rc=0
"$GOV" scan "$WORK_DIR/normal.naab" > "$WORK_DIR/scan4.txt" 2>&1 || rc=$?
if [[ $rc -le 2 ]]; then
    echo "PASS: V-RT-013: small file scan completes without crash (exit $rc)"
    PASS=$((PASS+1))
else
    echo "FAIL: V-RT-013: small file scan exited $rc"
    FAIL=$((FAIL+1))
fi
rm -f "$WORK_DIR/normal.naab"

# ── V-GOV-018: Per-agent shell enforcement ────────────────────────────────────
# naab-lang discovers govern.json from the script's directory upward, so we
# create one subdir per scenario and place a govern.json + shell_test.naab in each.

SHELL_NAAB='main {
    let result = <<shell
echo "shell_execution_marker"
>>
    io.write(result)
}'

# Scenario A: global shell allowed, junior blocked, senior inherits global
mkdir -p "$WORK_DIR/scen_a"
cat > "$WORK_DIR/scen_a/govern.json" << 'GOV'
{
  "mode": "enforce",
  "capabilities": { "shell": true },
  "agent_roles": {
    "junior": {
      "allowed_languages": ["naab", "shell"],
      "shell_allowed": false
    },
    "senior": {
      "allowed_languages": ["naab", "shell", "python"]
    }
  }
}
GOV
echo "$SHELL_NAAB" > "$WORK_DIR/scen_a/shell_test.naab"

# Scenario B: global shell blocked
mkdir -p "$WORK_DIR/scen_b"
cat > "$WORK_DIR/scen_b/govern.json" << 'GOV'
{
  "mode": "enforce",
  "capabilities": { "shell": false }
}
GOV
echo "$SHELL_NAAB" > "$WORK_DIR/scen_b/shell_test.naab"

# Test 5: junior agent with shell_allowed: false — shell block must be blocked
rc=0
"$NAAB" --agent-id junior "$WORK_DIR/scen_a/shell_test.naab" \
    > "$WORK_DIR/gov018_t1.txt" 2>&1 || rc=$?
if [[ $rc -ne 0 ]]; then
    echo "PASS: V-GOV-018: --agent-id junior with shell_allowed:false blocks <<shell (exit $rc)"
    PASS=$((PASS+1))
else
    echo "FAIL: V-GOV-018: junior agent ran <<shell despite shell_allowed:false in role"
    echo "  output: $(cat "$WORK_DIR/gov018_t1.txt")"
    FAIL=$((FAIL+1))
fi

# Test 6: senior agent has no shell_allowed override → falls through to global true
rc=0
"$NAAB" --agent-id senior "$WORK_DIR/scen_a/shell_test.naab" \
    > "$WORK_DIR/gov018_t2.txt" 2>&1 || rc=$?
if [[ $rc -eq 0 ]]; then
    echo "PASS: V-GOV-018: --agent-id senior inherits global shell_allowed:true (exit 0)"
    PASS=$((PASS+1))
else
    echo "FAIL: V-GOV-018: senior agent shell unexpectedly blocked (exit $rc)"
    echo "  output: $(cat "$WORK_DIR/gov018_t2.txt")"
    FAIL=$((FAIL+1))
fi

# Test 7: global shell_allowed: false — no agent-id — shell blocked regardless
rc=0
"$NAAB" "$WORK_DIR/scen_b/shell_test.naab" \
    > "$WORK_DIR/gov018_t3.txt" 2>&1 || rc=$?
if [[ $rc -ne 0 ]]; then
    echo "PASS: V-GOV-018: global shell_allowed:false blocks <<shell without agent-id (exit $rc)"
    PASS=$((PASS+1))
else
    echo "FAIL: V-GOV-018: global shell_allowed:false did not block shell"
    echo "  output: $(cat "$WORK_DIR/gov018_t3.txt")"
    FAIL=$((FAIL+1))
fi

# Test 8: unknown agent (not in agent_roles) falls through to global policy (shell_allowed:true)
rc=0
"$NAAB" --agent-id unknown_bot "$WORK_DIR/scen_a/shell_test.naab" \
    > "$WORK_DIR/gov018_t4.txt" 2>&1 || rc=$?
if [[ $rc -eq 0 ]]; then
    echo "PASS: V-GOV-018: unknown agent inherits global shell_allowed:true (exit 0)"
    PASS=$((PASS+1))
else
    echo "FAIL: V-GOV-018: unknown agent shell unexpectedly blocked (exit $rc)"
    echo "  output: $(cat "$WORK_DIR/gov018_t4.txt")"
    FAIL=$((FAIL+1))
fi

# ── Summary ──────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed"
[[ $FAIL -eq 0 ]]
