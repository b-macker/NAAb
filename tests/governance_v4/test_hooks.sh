#!/usr/bin/env bash
# test_hooks.sh — Governance hooks: fireHook() wiring, variable substitution, timeout
# Tests: on_violation, on_override, on_complete, pre_check, post_check

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TMPBASE="$_SYSTMP/test_hooks_$$"
mkdir -p "$TMPBASE"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
"$NAAB" --keygen "$TMPBASE/test-key.pem" 2>/dev/null
"$NAAB" --trust-key "$TMPBASE/test-key.pem.pub" 2>/dev/null
SIGNING_KEY="$TMPBASE/test-key.pem"

PASS=0; FAIL=0; SKIP=0

cleanup() { teardown_isolated_trust; rm -rf "$TMPBASE"; }
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

check_grep() {
    local id="$1" desc="$2" pattern="$3" text="$4"
    if echo "$text" | grep -q "$pattern" 2>/dev/null; then
        echo "  PASS [$id] $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL [$id] $desc (pattern '$pattern' not found)"
        FAIL=$((FAIL + 1))
    fi
}

check_not_grep() {
    local id="$1" desc="$2" pattern="$3" text="$4"
    if echo "$text" | grep -q "$pattern" 2>/dev/null; then
        echo "  FAIL [$id] $desc (found '$pattern' but shouldn't)"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS [$id] $desc"
        PASS=$((PASS + 1))
    fi
}

sign_govern() {
    local dir="$1"
    (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
}

echo "=== Governance Hooks ==="
echo ""

# ═══════════════════════════════════════════════════════════
# A1: Empty hook commands = no-op (no side effects)
# ═══════════════════════════════════════════════════════════
echo "--- A1: Empty hook commands = no-op ---"
DIR="$TMPBASE/a1"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<'EOF'
{
  "version": "5.0",
  "mode": "enforce",
  "description": "empty hooks test",
  "security": { "sandbox_level": "elevated" },
  "hooks": {
    "on_violation": { "command": "", "args": [] },
    "on_complete": { "command": "", "args": [] },
    "pre_check": { "command": "", "args": [] },
    "post_check": { "command": "", "args": [] }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
  print("hello")
}
EOF
OUTPUT=$("$NAAB" "$DIR/test.naab" 2>/dev/null) || true
check "A1" "Empty hooks don't crash" "hello" "$OUTPUT"

# ═══════════════════════════════════════════════════════════
# A2: Hook section parsed from govern.json correctly
# ═══════════════════════════════════════════════════════════
echo "--- A2: Hook section parsed ---"
DIR="$TMPBASE/a2"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "hook parse test",
  "security": { "sandbox_level": "elevated" },
  "hooks": {
    "on_complete": {
      "command": "/bin/sh",
      "args": ["-c", "echo HOOK_COMPLETE > $DIR/hook_marker.txt"],
      "timeout": 3
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
  print("parsed")
}
EOF
"$NAAB" "$DIR/test.naab" >/dev/null 2>&1 || true
# Give hook a moment to complete (fire-and-forget subprocess)
sleep 1
if [ -f "$DIR/hook_marker.txt" ]; then
    CONTENT=$(cat "$DIR/hook_marker.txt" | tr -d '[:space:]')
    check "A2" "on_complete hook fires and writes marker" "HOOK_COMPLETE" "$CONTENT"
else
    echo "  FAIL [A2] on_complete hook fires and writes marker (marker file not found)"
    FAIL=$((FAIL + 1))
fi

# ═══════════════════════════════════════════════════════════
# B1: HARD violation fires on_violation hook
# ═══════════════════════════════════════════════════════════
echo "--- B1: HARD violation fires on_violation ---"
DIR="$TMPBASE/b1"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "HARD violation hook test",
  "security": { "sandbox_level": "elevated" },
  "languages": { "allowed": ["python"], "require_explicit": true },
  "hooks": {
    "on_violation": {
      "command": "/bin/sh",
      "args": ["-c", "echo VIOLATION_FIRED > $DIR/violation_marker.txt"],
      "timeout": 3
    }
  }
}
EOF
sign_govern "$DIR"
# This program uses no language blocks, but require_explicit triggers a HARD block
# if any polyglot block isn't in the allowed list. Instead, use a rule that fires.
# Actually, require_explicit only fires on polyglot blocks. Let's use shell which
# isn't in allowed languages.
cat > "$DIR/test.naab" <<'EOF'
main {
  <<shell
  echo "hi"
  >>
}
EOF
"$NAAB" "$DIR/test.naab" >/dev/null 2>&1 || true
sleep 1
if [ -f "$DIR/violation_marker.txt" ]; then
    CONTENT=$(cat "$DIR/violation_marker.txt" | tr -d '[:space:]')
    check "B1" "HARD violation fires on_violation hook" "VIOLATION_FIRED" "$CONTENT"
else
    echo "  FAIL [B1] HARD violation fires on_violation hook (marker file not found)"
    FAIL=$((FAIL + 1))
fi

# ═══════════════════════════════════════════════════════════
# B2: on_violation hook receives ${level} variable
# ═══════════════════════════════════════════════════════════
echo "--- B2: Hook receives level variable ---"
DIR="$TMPBASE/b2"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "level variable test",
  "security": { "sandbox_level": "elevated" },
  "languages": { "allowed": ["python"], "require_explicit": true },
  "hooks": {
    "on_violation": {
      "command": "/bin/sh",
      "args": ["-c", "echo \${level} > $DIR/level_marker.txt"],
      "timeout": 3
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
  <<shell
  echo "trigger"
  >>
}
EOF
"$NAAB" "$DIR/test.naab" >/dev/null 2>&1 || true
sleep 1
if [ -f "$DIR/level_marker.txt" ]; then
    CONTENT=$(cat "$DIR/level_marker.txt" | tr -d '[:space:]')
    check "B2" "on_violation receives level variable" "hard" "$CONTENT"
else
    echo "  FAIL [B2] on_violation receives level variable (marker file not found)"
    FAIL=$((FAIL + 1))
fi

# ═══════════════════════════════════════════════════════════
# B3: Hook receives correct ${rule_name} variable
# ═══════════════════════════════════════════════════════════
echo "--- B3: Hook receives rule_name variable ---"
DIR="$TMPBASE/b3"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "variable substitution test",
  "security": { "sandbox_level": "elevated" },
  "languages": { "allowed": ["python"], "require_explicit": true },
  "hooks": {
    "on_violation": {
      "command": "/bin/sh",
      "args": ["-c", "echo \${rule_name} > $DIR/rule_marker.txt"],
      "timeout": 3
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
  <<shell
  echo "trigger"
  >>
}
EOF
"$NAAB" "$DIR/test.naab" >/dev/null 2>&1 || true
sleep 1
if [ -f "$DIR/rule_marker.txt" ]; then
    CONTENT=$(cat "$DIR/rule_marker.txt" | tr -d '[:space:]')
    # The rule name should be non-empty and meaningful
    if [ -n "$CONTENT" ] && [ "$CONTENT" != '${rule_name}' ]; then
        echo "  PASS [B3] Hook receives rule_name variable (got: $CONTENT)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL [B3] Hook receives rule_name variable (got empty or unsubstituted: $CONTENT)"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL [B3] Hook receives rule_name variable (marker file not found)"
    FAIL=$((FAIL + 1))
fi

# ═══════════════════════════════════════════════════════════
# B4: Hook failure doesn't mask governance (exit code still 3)
# ═══════════════════════════════════════════════════════════
echo "--- B4: Hook failure doesn't mask governance ---"
DIR="$TMPBASE/b4"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "hook failure test",
  "security": { "sandbox_level": "elevated" },
  "languages": { "allowed": ["python"], "require_explicit": true },
  "hooks": {
    "on_violation": {
      "command": "/nonexistent/binary/that/does/not/exist",
      "args": [],
      "timeout": 3
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
  <<shell
  echo "trigger"
  >>
}
EOF
RC=0
"$NAAB" "$DIR/test.naab" >/dev/null 2>&1 || RC=$?
check "B4" "Hook failure doesn't mask governance (exit 3)" "3" "$RC"

# ═══════════════════════════════════════════════════════════
# B5: Hook timeout works (sleep killed, governance proceeds)
# ═══════════════════════════════════════════════════════════
echo "--- B5: Hook timeout ---"
DIR="$TMPBASE/b5"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "hook timeout test",
  "security": { "sandbox_level": "elevated" },
  "languages": { "allowed": ["python"], "require_explicit": true },
  "hooks": {
    "on_violation": {
      "command": "/bin/sh",
      "args": ["-c", "sleep 60"],
      "timeout": 1
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
  <<shell
  echo "trigger"
  >>
}
EOF
START=$(date +%s)
RC=0
STDERR=$("$NAAB" "$DIR/test.naab" 2>"$DIR/b5_stderr.txt" >/dev/null) || RC=$?
END=$(date +%s)
ELAPSED=$((END - START))
# Should complete in < 10 seconds (timeout is 1s + overhead), not 60
if [ "$ELAPSED" -lt 10 ]; then
    echo "  PASS [B5] Hook timeout works (completed in ${ELAPSED}s)"
    PASS=$((PASS + 1))
else
    echo "  FAIL [B5] Hook timeout works (took ${ELAPSED}s, expected < 10)"
    FAIL=$((FAIL + 1))
fi
B5_STDERR=$(cat "$DIR/b5_stderr.txt" 2>/dev/null || echo "")
check_grep "B5b" "Hook timeout diagnostic in stderr" "Hook killed\|Hook exited" "$B5_STDERR"

# ═══════════════════════════════════════════════════════════
# C1: on_violation receives ${category} variable
# ═══════════════════════════════════════════════════════════
echo "--- C1: Hook receives category variable ---"
DIR="$TMPBASE/c1"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "category variable test",
  "security": { "sandbox_level": "elevated" },
  "languages": { "allowed": ["python"], "require_explicit": true },
  "hooks": {
    "on_violation": {
      "command": "/bin/sh",
      "args": ["-c", "echo \${category} > $DIR/cat_marker.txt"],
      "timeout": 3
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
  <<shell
  echo "trigger"
  >>
}
EOF
"$NAAB" "$DIR/test.naab" >/dev/null 2>&1 || true
sleep 1
if [ -f "$DIR/cat_marker.txt" ]; then
    CONTENT=$(cat "$DIR/cat_marker.txt" | tr -d '[:space:]')
    # Category should be non-empty
    if [ -n "$CONTENT" ]; then
        echo "  PASS [C1] Hook receives category variable (got: $CONTENT)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL [C1] Hook receives category variable (empty)"
        FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL [C1] Hook receives category variable (marker file not found)"
    FAIL=$((FAIL + 1))
fi

# ═══════════════════════════════════════════════════════════
# D1: on_complete fires after clean execution
# ═══════════════════════════════════════════════════════════
echo "--- D1: on_complete fires after clean execution ---"
DIR="$TMPBASE/d1"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "on_complete test",
  "security": { "sandbox_level": "elevated" },
  "hooks": {
    "on_complete": {
      "command": "/bin/sh",
      "args": ["-c", "echo COMPLETE > $DIR/complete_marker.txt"],
      "timeout": 3
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
  print("done")
}
EOF
"$NAAB" "$DIR/test.naab" >/dev/null 2>&1 || true
sleep 1
if [ -f "$DIR/complete_marker.txt" ]; then
    CONTENT=$(cat "$DIR/complete_marker.txt" | tr -d '[:space:]')
    check "D1" "on_complete fires after clean execution" "COMPLETE" "$CONTENT"
else
    echo "  FAIL [D1] on_complete fires after clean execution (marker file not found)"
    FAIL=$((FAIL + 1))
fi

# ═══════════════════════════════════════════════════════════
# D3: on_complete does NOT fire on HARD block
# ═══════════════════════════════════════════════════════════
echo "--- D3: on_complete does NOT fire on HARD block ---"
DIR="$TMPBASE/d3"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "on_complete HARD block test",
  "security": { "sandbox_level": "elevated" },
  "languages": { "allowed": ["python"], "require_explicit": true },
  "hooks": {
    "on_complete": {
      "command": "/bin/sh",
      "args": ["-c", "echo SHOULD_NOT_APPEAR > $DIR/no_complete_marker.txt"],
      "timeout": 3
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
  <<shell
  echo "trigger HARD"
  >>
}
EOF
"$NAAB" "$DIR/test.naab" >/dev/null 2>&1 || true
sleep 1
if [ -f "$DIR/no_complete_marker.txt" ]; then
    echo "  FAIL [D3] on_complete should NOT fire on HARD block (marker file exists)"
    FAIL=$((FAIL + 1))
else
    echo "  PASS [D3] on_complete does NOT fire on HARD block"
    PASS=$((PASS + 1))
fi

# ═══════════════════════════════════════════════════════════
# E1: pre_check fires (marker file written)
# ═══════════════════════════════════════════════════════════
echo "--- E1: pre_check fires ---"
DIR="$TMPBASE/e1"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "pre_check test",
  "security": { "sandbox_level": "elevated" },
  "hooks": {
    "pre_check": {
      "command": "/bin/sh",
      "args": ["-c", "echo PRE > $DIR/pre_marker.txt"],
      "timeout": 3
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
  print("pre_check test")
}
EOF
"$NAAB" "$DIR/test.naab" >/dev/null 2>&1 || true
sleep 1
if [ -f "$DIR/pre_marker.txt" ]; then
    CONTENT=$(cat "$DIR/pre_marker.txt" | tr -d '[:space:]')
    check "E1" "pre_check fires" "PRE" "$CONTENT"
else
    echo "  FAIL [E1] pre_check fires (marker file not found)"
    FAIL=$((FAIL + 1))
fi

# ═══════════════════════════════════════════════════════════
# E2: post_check fires (marker file written)
# ═══════════════════════════════════════════════════════════
echo "--- E2: post_check fires ---"
DIR="$TMPBASE/e2"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "post_check test",
  "security": { "sandbox_level": "elevated" },
  "hooks": {
    "post_check": {
      "command": "/bin/sh",
      "args": ["-c", "echo POST > $DIR/post_marker.txt"],
      "timeout": 3
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
  print("post_check test")
}
EOF
"$NAAB" "$DIR/test.naab" >/dev/null 2>&1 || true
sleep 1
if [ -f "$DIR/post_marker.txt" ]; then
    CONTENT=$(cat "$DIR/post_marker.txt" | tr -d '[:space:]')
    check "E2" "post_check fires" "POST" "$CONTENT"
else
    echo "  FAIL [E2] post_check fires (marker file not found)"
    FAIL=$((FAIL + 1))
fi

# ═══════════════════════════════════════════════════════════
# E3: pre_check fires before post_check (timestamp ordering)
# ═══════════════════════════════════════════════════════════
echo "--- E3: pre before post ordering ---"
DIR="$TMPBASE/e3"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "hook ordering test",
  "security": { "sandbox_level": "elevated" },
  "hooks": {
    "pre_check": {
      "command": "/bin/sh",
      "args": ["-c", "echo PRE >> $DIR/order.txt"],
      "timeout": 3
    },
    "post_check": {
      "command": "/bin/sh",
      "args": ["-c", "echo POST >> $DIR/order.txt"],
      "timeout": 3
    },
    "on_complete": {
      "command": "/bin/sh",
      "args": ["-c", "echo COMPLETE >> $DIR/order.txt"],
      "timeout": 3
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
  print("ordering")
}
EOF
"$NAAB" "$DIR/test.naab" >/dev/null 2>&1 || true
sleep 2
if [ -f "$DIR/order.txt" ]; then
    FIRST=$(head -1 "$DIR/order.txt" | tr -d '[:space:]')
    SECOND=$(sed -n '2p' "$DIR/order.txt" | tr -d '[:space:]')
    THIRD=$(sed -n '3p' "$DIR/order.txt" | tr -d '[:space:]')
    check "E3a" "pre_check fires first" "PRE" "$FIRST"
    check "E3b" "post_check fires second" "POST" "$SECOND"
    check "E3c" "on_complete fires third" "COMPLETE" "$THIRD"
else
    echo "  FAIL [E3a] Hook ordering (order.txt not found)"
    echo "  FAIL [E3b] Hook ordering (order.txt not found)"
    echo "  FAIL [E3c] Hook ordering (order.txt not found)"
    FAIL=$((FAIL + 3))
fi

# ═══════════════════════════════════════════════════════════
# F1: ${rule_name} substituted correctly in args
# ═══════════════════════════════════════════════════════════
echo "--- F1: Variable substitution ---"
# Already tested in B3, count it here
echo "  (covered by B3)"

# ═══════════════════════════════════════════════════════════
# F2: Args with quotes/spaces handled safely (no shell injection)
# ═══════════════════════════════════════════════════════════
echo "--- F2: Shell injection safety ---"
DIR="$TMPBASE/f2"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "injection safety test",
  "security": { "sandbox_level": "elevated" },
  "hooks": {
    "on_complete": {
      "command": "/bin/sh",
      "args": ["-c", "echo SAFE > $DIR/safe_marker.txt"],
      "timeout": 3
    }
  },
  "description": "test with \"; rm -rf / ; echo \" in description"
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
  print("safe")
}
EOF
"$NAAB" "$DIR/test.naab" >/dev/null 2>&1 || true
sleep 1
if [ -f "$DIR/safe_marker.txt" ]; then
    CONTENT=$(cat "$DIR/safe_marker.txt" | tr -d '[:space:]')
    check "F2" "No shell injection from malicious config" "SAFE" "$CONTENT"
else
    echo "  FAIL [F2] No shell injection (marker file not found)"
    FAIL=$((FAIL + 1))
fi

# ═══════════════════════════════════════════════════════════
# F3: Multiple hooks can be configured simultaneously
# ═══════════════════════════════════════════════════════════
echo "--- F3: Multiple hooks configured ---"
DIR="$TMPBASE/f3"
mkdir -p "$DIR"
cat > "$DIR/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "description": "multi-hook test",
  "security": { "sandbox_level": "elevated" },
  "hooks": {
    "pre_check": {
      "command": "/bin/sh",
      "args": ["-c", "echo PRE > $DIR/multi_pre.txt"],
      "timeout": 3
    },
    "post_check": {
      "command": "/bin/sh",
      "args": ["-c", "echo POST > $DIR/multi_post.txt"],
      "timeout": 3
    },
    "on_complete": {
      "command": "/bin/sh",
      "args": ["-c", "echo DONE > $DIR/multi_done.txt"],
      "timeout": 3
    }
  }
}
EOF
sign_govern "$DIR"
cat > "$DIR/test.naab" <<'EOF'
main {
  print("multi")
}
EOF
"$NAAB" "$DIR/test.naab" >/dev/null 2>&1 || true
sleep 2
PRE_EXISTS=0; POST_EXISTS=0; DONE_EXISTS=0
[ -f "$DIR/multi_pre.txt" ] && PRE_EXISTS=1
[ -f "$DIR/multi_post.txt" ] && POST_EXISTS=1
[ -f "$DIR/multi_done.txt" ] && DONE_EXISTS=1
TOTAL=$((PRE_EXISTS + POST_EXISTS + DONE_EXISTS))
check "F3" "All 3 hooks fire simultaneously" "3" "$TOTAL"

# ═══════════════════════════════════════════════════════════
echo ""
echo "================================================"
echo "  PASS: $PASS  FAIL: $FAIL  SKIP: $SKIP  TOTAL: $((PASS + FAIL + SKIP))"
if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo "  Some tests failed."
fi
echo "================================================"

exit "$FAIL"
