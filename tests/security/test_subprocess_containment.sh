#!/usr/bin/env bash
# test_subprocess_containment.sh — OS-level polyglot subprocess containment
# Verifies that sandbox restrictions (RLIMIT_NPROC, PATH, RLIMIT_FSIZE)
# are enforced inside child processes, preventing runtime bypass.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
SIGNING_KEY="${HOME}/.naab/keys/signing.pem"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TMPBASE="$_SYSTMP/test_containment_$$"
mkdir -p "$TMPBASE"

PASS=0; FAIL=0; SKIP=0

cleanup() { rm -rf "$TMPBASE"; }
trap cleanup EXIT

check() {
    local id="$1" desc="$2" expected="$3" actual="$4"
    if [ "$expected" = "$actual" ]; then
        echo "  PASS [$id] $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL [$id] $desc (expected=$expected actual=$actual)"
        FAIL=$((FAIL + 1))
    fi
}

check_contains() {
    local id="$1" desc="$2" file="$3" pattern="$4"
    if grep -q "$pattern" "$file" 2>/dev/null; then
        echo "  PASS [$id] $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL [$id] $desc (pattern '$pattern' not found in output)"
        FAIL=$((FAIL + 1))
    fi
}

check_not_contains() {
    local id="$1" desc="$2" file="$3" pattern="$4"
    if grep -q "$pattern" "$file" 2>/dev/null; then
        echo "  FAIL [$id] $desc (found '$pattern' in output)"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS [$id] $desc"
        PASS=$((PASS + 1))
    fi
}

# Create govern.json with specified sandbox level
setup_test() {
    local dir="$TMPBASE/$1"
    local sandbox_level="${2:-standard}"
    mkdir -p "$dir"
    cat > "$dir/govern.json" <<GOVEOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "Subprocess containment test — $sandbox_level",
  "languages": {
    "allowed": ["python", "shell", "go"]
  },
  "capabilities": {
    "network": { "enabled": false },
    "filesystem": { "mode": "read_write" },
    "shell": { "enabled": true }
  },
  "security": {
    "sandbox_level": "$sandbox_level"
  }
}
GOVEOF
    if [ -f "$SIGNING_KEY" ]; then
        (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
    fi
    echo "$dir"
}

echo "=== Subprocess Containment: OS-Level Restriction Tests ==="
echo ""

# Capability probe: T1, T2, and T7 all rely on RLIMIT_NPROC=0 blocking fork
# (apply_posix_containment). Root users and some container runtimes are not
# subject to RLIMIT_NPROC, so probe the mechanism directly: if a process can
# still spawn a subprocess after setting NPROC to 0, the kernel context does
# not enforce it and those tests would fail for environmental reasons.
NPROC_ENFORCED=1
if python3 -c "import resource,subprocess; resource.setrlimit(resource.RLIMIT_NPROC,(0,0)); subprocess.run(['true'],check=True)" 2>/dev/null; then
    NPROC_ENFORCED=0
    echo "  NOTE: RLIMIT_NPROC fork-blocking not enforced in this environment;"
    echo "        skipping the tests that depend on it (T1, T2, T7)."
    echo ""
fi

# ---------- T1: Python subprocess.run blocked by RLIMIT_NPROC=0 ----------
echo "--- T1: Python subprocess.run blocked (RLIMIT_NPROC=0, sandbox=standard) ---"
if [ "$NPROC_ENFORCED" -eq 0 ]; then
    echo "  SKIP [T1a] subprocess.run did not succeed (RLIMIT_NPROC not enforced)"
    echo "  SKIP [T1b] subprocess.run was blocked (RLIMIT_NPROC not enforced)"
    SKIP=$((SKIP + 2))
else
DIR=$(setup_test "t1" "standard")
cat > "$DIR/test.naab" <<'EOF'
main {
    let result = <<python
import subprocess
try:
    r = subprocess.run(["echo", "ESCAPED"], capture_output=True, text=True)
    print("BYPASS: " + r.stdout.strip())
except Exception as e:
    print("BLOCKED: " + str(e))
>>
    print(string(result))
}
EOF

(cd "$DIR" && "$NAAB" test.naab) >"$DIR/t1_out.log" 2>"$DIR/t1_err.log" && T1_EXIT=0 || T1_EXIT=$?
# subprocess.run should fail with EAGAIN/"Resource temporarily unavailable"/"Try again"
check_not_contains "T1a" "subprocess.run did not succeed" "$DIR/t1_out.log" "BYPASS"
check_contains "T1b" "subprocess.run was blocked" "$DIR/t1_out.log" "BLOCKED"
fi

echo ""

# ---------- T2: Python os.system blocked by PATH restriction ----------
echo "--- T2: Python os.system blocked (PATH restricted, sandbox=standard) ---"
if [ "$NPROC_ENFORCED" -eq 0 ]; then
    echo "  SKIP [T2a] os.system did not succeed (RLIMIT_NPROC not enforced)"
    SKIP=$((SKIP + 1))
else
DIR=$(setup_test "t2" "standard")
cat > "$DIR/test.naab" <<'EOF'
main {
    let result = <<python
import os
ret = os.system("ls /tmp 2>&1")
if ret == 0:
    print("BYPASS: ls succeeded")
else:
    print("BLOCKED: ls failed with exit " + str(ret))
>>
    print(string(result))
}
EOF

(cd "$DIR" && "$NAAB" test.naab) >"$DIR/t2_out.log" 2>"$DIR/t2_err.log" && T2_EXIT=0 || T2_EXIT=$?
# os.system calls fork() which should fail with RLIMIT_NPROC=0
check_not_contains "T2a" "os.system did not succeed" "$DIR/t2_out.log" "BYPASS"
fi

echo ""

# ---------- T3: Basic Python execution still works (no subprocess) ----------
echo "--- T3: Python basic execution works (no subprocess calls) ---"
DIR=$(setup_test "t3" "standard")
cat > "$DIR/test.naab" <<'EOF'
main {
    let result = <<python
import os
home = os.environ.get("HOME", "unknown")
print("HOME=" + home)
>>
    print(string(result))
}
EOF

(cd "$DIR" && "$NAAB" test.naab) >"$DIR/t3_out.log" 2>"$DIR/t3_err.log" && T3_EXIT=0 || T3_EXIT=$?
check "T3a" "Basic Python exits 0" "0" "$T3_EXIT"
check_contains "T3b" "Python can read env vars" "$DIR/t3_out.log" "HOME="

echo ""

# ---------- T4: Unrestricted sandbox allows subprocess ----------
echo "--- T4: Unrestricted sandbox allows subprocess.run ---"
DIR=$(setup_test "t4" "unrestricted")
cat > "$DIR/test.naab" <<'EOF'
main {
    let result = <<python
import subprocess
try:
    r = subprocess.run(["echo", "ALLOWED"], capture_output=True, text=True)
    print("OK: " + r.stdout.strip())
except Exception as e:
    print("FAIL: " + str(e))
>>
    print(string(result))
}
EOF

(cd "$DIR" && "$NAAB" test.naab) >"$DIR/t4_out.log" 2>"$DIR/t4_err.log" && T4_EXIT=0 || T4_EXIT=$?
check "T4a" "Unrestricted exits 0" "0" "$T4_EXIT"
check_contains "T4b" "subprocess.run works in unrestricted" "$DIR/t4_out.log" "OK: ALLOWED"

echo ""

# ---------- T5: Elevated sandbox allows subprocess (allow_fork=true) ----------
echo "--- T5: Elevated sandbox allows subprocess.run (allow_fork=true) ---"
DIR=$(setup_test "t5" "elevated")
cat > "$DIR/test.naab" <<'EOF'
main {
    let result = <<python
import subprocess
try:
    r = subprocess.run(["echo", "ELEVATED_OK"], capture_output=True, text=True)
    print("OK: " + r.stdout.strip())
except Exception as e:
    print("FAIL: " + str(e))
>>
    print(string(result))
}
EOF

(cd "$DIR" && "$NAAB" test.naab) >"$DIR/t5_out.log" 2>"$DIR/t5_err.log" && T5_EXIT=0 || T5_EXIT=$?
check "T5a" "Elevated exits 0" "0" "$T5_EXIT"
check_contains "T5b" "subprocess.run works in elevated" "$DIR/t5_out.log" "OK: ELEVATED_OK"

echo ""

# ---------- T6: Shell block blocked by restricted sandbox ----------
echo "--- T6: Shell block denied by restricted sandbox (allow_exec=false) ---"
DIR=$(setup_test "t6" "restricted")
cat > "$DIR/test.naab" <<'EOF'
main {
    let result = <<shell
echo "SHELL_OK"
>>
    print(string(result))
}
EOF

(cd "$DIR" && "$NAAB" test.naab) >"$DIR/t6_out.log" 2>"$DIR/t6_err.log" && T6_EXIT=0 || T6_EXIT=$?
# Restricted sandbox blocks shell execution entirely (allow_exec=false)
check "T6a" "Shell block denied by restricted sandbox" "1" "$T6_EXIT"
check_not_contains "T6b" "Shell block did not execute" "$DIR/t6_out.log" "SHELL_OK"

echo ""

# ---------- T7: Runtime-constructed command blocked ----------
echo "--- T7: Runtime-constructed dangerous command blocked (chr() bypass) ---"
if [ "$NPROC_ENFORCED" -eq 0 ]; then
    echo "  SKIP [T7a] chr() subprocess bypass blocked (RLIMIT_NPROC not enforced)"
    echo "  SKIP [T7b] chr() subprocess was blocked (RLIMIT_NPROC not enforced)"
    SKIP=$((SKIP + 2))
else
DIR=$(setup_test "t7" "standard")
cat > "$DIR/test.naab" <<'EOF'
main {
    let result = <<python
import subprocess
# Build "echo" from character codes — bypasses text scanning
cmd = chr(101) + chr(99) + chr(104) + chr(111)
try:
    r = subprocess.run([cmd, "CHAR_BYPASS"], capture_output=True, text=True)
    print("BYPASS: " + r.stdout.strip())
except Exception as e:
    print("BLOCKED: " + str(e))
>>
    print(string(result))
}
EOF

(cd "$DIR" && "$NAAB" test.naab) >"$DIR/t7_out.log" 2>"$DIR/t7_err.log" && T7_EXIT=0 || T7_EXIT=$?
check_not_contains "T7a" "chr() subprocess bypass blocked" "$DIR/t7_out.log" "BYPASS"
check_contains "T7b" "chr() subprocess was blocked" "$DIR/t7_out.log" "BLOCKED"
fi

echo ""

# ---------- T8: process.run() contained ----------
echo "--- T8: process.run() contained (stdlib, sandbox=elevated) ---"
DIR=$(setup_test "t8" "elevated")
cat > "$DIR/test.naab" <<'EOF'
use process

main {
    let result = process.run("echo", ["PROCESS_OK"])
    print("exit: " + string(result.exit_code))
    print("out: " + string(result.stdout))
}
EOF

(cd "$DIR" && "$NAAB" test.naab) >"$DIR/t8_out.log" 2>"$DIR/t8_err.log" && T8_EXIT=0 || T8_EXIT=$?
# process.run uses execute_subprocess_with_pipes with containment
# elevated sandbox allows fork/exec, so echo should work
check "T8a" "process.run exits 0" "0" "$T8_EXIT"
check_contains "T8b" "process.run output captured" "$DIR/t8_out.log" "PROCESS_OK"

echo ""

# ---------- Summary ----------
TOTAL=$((PASS + FAIL + SKIP))
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed, $SKIP skipped ==="
if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
