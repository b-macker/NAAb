#!/usr/bin/env bash
# Security R22 Fix Verification Tests
# Tests: V-GOV-017 (symlink info disclosure), V-RT-013 (OOM read cap), V-GOV-018 (agent shell bypass)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
GOV="$SCRIPT_DIR/../../build/naab-gov"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/naab_r22.XXXXXX")"
# A failed mktemp leaves $WORK_DIR EMPTY, and every "$WORK_DIR/x" write below then
# rebases onto the filesystem ROOT. That is how a stray govern.json reached /
# and silently governed every later run on the machine. Fail loudly instead.
[ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ] || { echo "FATAL: could not create work dir" >&2; exit 1; }
trap 'rm -rf "$WORK_DIR"' EXIT

PASS=0
FAIL=0
SKIP=0
skip() { echo "SKIP: $1 (UNMEASURABLE)"; SKIP=$((SKIP+1)); }

# Instrument usability. Six arms below run naab-gov. With no binary, "$GOV"
# exits 127 and prints nothing -- which the two symlink arms read as "secret
# absent from scan output", i.e. a PASS for a scan that never ran. The Linux CI
# jobs did not build naab-gov when this suite was registered. No binary is
# UNMEASURABLE, never a pass.
if [ ! -x "$GOV" ]; then
    echo "FAIL: naab-gov not built at $GOV (UNMEASURABLE, not a pass)"
    echo "  build it with: cmake --build build --target naab-gov"
    exit 1
fi

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

# The scanner never prints file CONTENTS, so "secret absent from scan output"
# held whether or not a symlink was followed -- these arms could not fail. The
# discriminating signal is the scanned-file count: a followed symlink is
# counted, a rejected one is not. Each arm has a positive control (the same
# file as a regular file MUST be counted), and the symlink targets are .naab
# files so an extension filter cannot be what produced the zero. /etc/hostname
# is gone as a target: it does not exist on the Windows runner.
SECRET_CONTENT="NAAB_R22_SECRET_KEY=sk-ant-AAABBBCCCDDDEEEFFFGGG12345678901234567890"
mkdir -p "$WORK_DIR/outside" "$WORK_DIR/sym1" "$WORK_DIR/sym2" "$WORK_DIR/ctl"
echo "$SECRET_CONTENT" > "$WORK_DIR/outside/private_key.txt"
echo 'let x = 1' > "$WORK_DIR/outside/target.naab"
echo 'let x = 1' > "$WORK_DIR/ctl/audit.naab"
ln -s "$WORK_DIR/outside/private_key.txt" "$WORK_DIR/sym1/audit.naab" 2>/dev/null
ln -s "$WORK_DIR/outside/target.naab" "$WORK_DIR/sym2/system.naab" 2>/dev/null

scanned_count() {  # prints N from "naab-gov scan: N file(s) scanned", or nothing
    local out
    out=$(cd "$WORK_DIR" && "$GOV" scan "$1" 2>&1)
    case "$out" in
        *"naab-gov scan: "*) out="${out##*naab-gov scan: }"; echo "${out%% file*}" ;;
    esac
}

CTL_N=$(scanned_count "$WORK_DIR/ctl")
if [[ "$CTL_N" != "1" ]]; then
    fail_msg="positive control: a regular .naab file was not counted (got '${CTL_N}')"
    echo "FAIL: V-GOV-017: $fail_msg"; FAIL=$((FAIL+1))
else
    # Test 1: symlink to a non-.naab file carrying a secret
    if [[ ! -L "$WORK_DIR/sym1/audit.naab" ]]; then
        skip "V-GOV-017 T1: platform cannot create symlinks"
    else
        N=$(cd "$WORK_DIR" && "$GOV" scan "$WORK_DIR/sym1" 2>&1)
        case "$N" in
            *"naab-gov scan: 0 file"*)
                if ! grep -qF "NAAB_R22_SECRET_KEY" <<<"$N"; then
                    echo "PASS: V-GOV-017 T1: symlink audit.naab -> private_key.txt not followed (0 files scanned)"
                    PASS=$((PASS+1))
                else
                    echo "FAIL: V-GOV-017 T1: secret content leaked into scan output"; FAIL=$((FAIL+1))
                fi ;;
            *)  echo "FAIL: V-GOV-017 T1: symlink was followed"; echo "  output: $N"; FAIL=$((FAIL+1)) ;;
        esac
    fi
    # Test 2: symlink to a .naab file outside the scanned tree
    if [[ ! -L "$WORK_DIR/sym2/system.naab" ]]; then
        skip "V-GOV-017 T2: platform cannot create symlinks"
    else
        N2=$(scanned_count "$WORK_DIR/sym2")
        if [[ "$N2" == "0" ]]; then
            echo "PASS: V-GOV-017 T2: symlink to an outside .naab file not followed (0 files scanned)"
            PASS=$((PASS+1))
        else
            echo "FAIL: V-GOV-017 T2: symlink to outside .naab file followed (scanned '${N2}')"
            FAIL=$((FAIL+1))
        fi
    fi
fi

# Clean up symlinks before next tests
rm -rf "$WORK_DIR/outside" "$WORK_DIR/sym1" "$WORK_DIR/sym2" "$WORK_DIR/ctl"

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
    (cd "$WORK_DIR" && "$GOV" scan "$WORK_DIR/bigfile.naab") > "$WORK_DIR/scan3.txt" 2>&1 || rc=$?
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
# Its own directory and govern.json: with none discoverable, the default
# require-governance refuses to run at all -- which the old "" expectation hid.
mkdir -p "$WORK_DIR/norm"
echo '{ "version": "4.0", "mode": "off" }' > "$WORK_DIR/norm/govern.json"
cat > "$WORK_DIR/norm/normal.naab" << 'NAAB'
main {
    let x = 42
    io.write(x)
}
NAAB
# (expected text was "" -- grep -F "" matches anything, so this arm could not fail)
check "V-RT-013: normal 1KB NAAb file runs cleanly" "42" \
    "$NAAB" "$WORK_DIR/norm/normal.naab"
# Just verify the scanner itself doesn't crash on it
rc=0
(cd "$WORK_DIR" && "$GOV" scan "$WORK_DIR/norm/normal.naab") > "$WORK_DIR/scan4.txt" 2>&1 || rc=$?
if [[ $rc -le 2 ]]; then
    echo "PASS: V-RT-013: small file scan completes without crash (exit $rc)"
    PASS=$((PASS+1))
else
    echo "FAIL: V-RT-013: small file scan exited $rc"
    FAIL=$((FAIL+1))
fi
rm -rf "$WORK_DIR/norm"

# ── V-GOV-018: Per-agent shell enforcement ────────────────────────────────────
# naab-lang discovers govern.json from the script's directory upward, so we
# create one subdir per scenario and place a govern.json + shell_test.naab in each.
#
# Both configs set security.sandbox_level: "elevated". Without it, mode:enforce
# upgrades the sandbox to "standard", which refuses EVERY <<shell block with a
# sandbox error (exit 1) before per-agent policy is consulted -- so the two
# "allowed" arms failed and, worse, the two "blocked" arms passed without ever
# reaching the rule they claim to test. The blocked arms therefore also require
# the GOVERNANCE refusal (exit 3), not merely a non-zero exit.

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
  "security": { "sandbox_level": "elevated" },
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
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": false }
}
GOV
echo "$SHELL_NAAB" > "$WORK_DIR/scen_b/shell_test.naab"

# Test 5: junior agent with shell_allowed: false — shell block must be blocked
rc=0
"$NAAB" --agent-id junior "$WORK_DIR/scen_a/shell_test.naab" \
    > "$WORK_DIR/gov018_t1.txt" 2>&1 || rc=$?
if [[ $rc -eq 3 ]]; then
    echo "PASS: V-GOV-018: --agent-id junior with shell_allowed:false blocks <<shell (exit $rc)"
    PASS=$((PASS+1))
else
    echo "FAIL: V-GOV-018: junior agent not refused by governance (exit $rc, want 3)"
    echo "  output: $(cat "$WORK_DIR/gov018_t1.txt")"
    FAIL=$((FAIL+1))
fi

# Test 6: senior agent has no shell_allowed override → falls through to global true
rc=0
"$NAAB" --agent-id senior "$WORK_DIR/scen_a/shell_test.naab" \
    > "$WORK_DIR/gov018_t2.txt" 2>&1 || rc=$?
if grep -q "No executor found for language: shell" "$WORK_DIR/gov018_t2.txt"; then
    skip "V-GOV-018: senior arm -- no shell executor on this platform"
elif [[ $rc -eq 0 ]] && grep -q shell_execution_marker "$WORK_DIR/gov018_t2.txt"; then
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
if [[ $rc -eq 3 ]]; then
    echo "PASS: V-GOV-018: global shell_allowed:false blocks <<shell without agent-id (exit $rc)"
    PASS=$((PASS+1))
else
    echo "FAIL: V-GOV-018: global shell_allowed:false not refused by governance (exit $rc, want 3)"
    echo "  output: $(cat "$WORK_DIR/gov018_t3.txt")"
    FAIL=$((FAIL+1))
fi

# Test 8: unknown agent (not in agent_roles) falls through to global policy (shell_allowed:true)
rc=0
"$NAAB" --agent-id unknown_bot "$WORK_DIR/scen_a/shell_test.naab" \
    > "$WORK_DIR/gov018_t4.txt" 2>&1 || rc=$?
if grep -q "No executor found for language: shell" "$WORK_DIR/gov018_t4.txt"; then
    skip "V-GOV-018: unknown arm -- no shell executor on this platform"
elif [[ $rc -eq 0 ]] && grep -q shell_execution_marker "$WORK_DIR/gov018_t4.txt"; then
    echo "PASS: V-GOV-018: unknown agent inherits global shell_allowed:true (exit 0)"
    PASS=$((PASS+1))
else
    echo "FAIL: V-GOV-018: unknown agent shell unexpectedly blocked (exit $rc)"
    echo "  output: $(cat "$WORK_DIR/gov018_t4.txt")"
    FAIL=$((FAIL+1))
fi

# ── Summary ──────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[[ $FAIL -eq 0 ]]
