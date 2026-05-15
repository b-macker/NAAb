#!/bin/bash
# Live integration test for Governance Under Survivability
# Requires: GK1 environment variable (Gemini API key)
# Tests: mid-run govern.json reload produces governance_notices in agent.send() response

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB_BIN="${SCRIPT_DIR}/../../build/naab-lang"
PASS=0
FAIL=0
TEST_DIR="${HOME}/.naab_reload_live_test_$$"
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

echo "=== NAAb Governance Reload Live Tests ==="
echo ""

# Find a working Gemini API key from GK1-GK6
WORKING_KEY=""
WORKING_KEY_NAME=""
for kname in GK1 GK2 GK3 GK4 GK5 GK6; do
    kval="${!kname}"
    [ -z "$kval" ] && continue
    PROBE=$(curl -s --max-time 10 \
        "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=${kval}" \
        -H 'Content-Type: application/json' \
        -d '{"contents":[{"parts":[{"text":"hi"}]}]}' 2>&1)
    if echo "$PROBE" | grep -q '"text"'; then
        WORKING_KEY="$kval"
        WORKING_KEY_NAME="$kname"
        break
    fi
done

if [ -z "$WORKING_KEY" ]; then
    echo "  SKIP: No working Gemini API key found (GK1-GK6 all invalid or quota exhausted)"
    echo "=== Results: 0/0 passed, 0 failed (skipped) ==="
    exit 0
fi
echo "  Using API key: $WORKING_KEY_NAME"
export NAAB_TEST_GK="$WORKING_KEY"

# --- Test 1: agent.send() returns governance_notices after mid-run tightening ---
echo "--- Live Reload: governance_notices in agent.send() ---"

# Create initial govern.json with agent config and loose limits
cat > "$TEST_DIR/govern.json" <<GOVEOF
{
  "version": "5.0",
  "mode": "enforce",
  "sandbox_level": "unrestricted",
  "limits": {
    "execution": {
      "loop_iterations": 1000
    },
    "data": {
      "output_size": 10000
    }
  },
  "capabilities": {
    "shell": {
      "enabled": true
    }
  },
  "agents": {
    "test_bot": {
      "provider": "gemini",
      "model": "gemini-2.5-flash",
      "api_key_env": "NAAB_TEST_GK",
      "max_tokens": 50,
      "max_turns": 5,
      "system_prompt": "Reply with exactly one word. Nothing else."
    }
  }
}
GOVEOF

# Create .naab test script that:
# 1. Sends first agent message (no reload expected)
# 2. Modifies govern.json via shell block (tightens limits)
# 3. Sends second agent message (reload + notices expected)
cat > "$TEST_DIR/reload_live.naab" <<'NAABEOF'
use json
use file
use agent

main {
    // First agent.send — no governance_notices expected
    let handle = agent.create("test_bot")
    let r1 = agent.send(handle, "hello")

    let has_notices_r1 = r1.get("governance_notices") != null
    print("FIRST_HAS_NOTICES=" + string(has_notices_r1))
    print("FIRST_CONTENT=" + r1.get("content"))

    // Tighten govern.json via shell block (lower limits, disable shell)
    let _ = <<shell
cat > govern.json << 'TIGHTEOF'
{
  "version": "5.0",
  "mode": "enforce",
  "sandbox_level": "unrestricted",
  "update_reason": "test tightening for CI",
  "limits": {
    "execution": {
      "loop_iterations": 100
    },
    "data": {
      "output_size": 5000
    }
  },
  "capabilities": {
    "shell": {
      "enabled": false
    }
  },
  "agents": {
    "test_bot": {
      "provider": "gemini",
      "model": "gemini-2.5-flash",
      "api_key_env": "NAAB_TEST_GK",
      "max_tokens": 50,
      "max_turns": 5,
      "system_prompt": "Reply with exactly one word. Nothing else."
    }
  }
}
TIGHTEOF
>>

    // Second agent.send — should detect mtime change and produce notices
    let r2 = agent.send(handle, "bye")

    let notices = r2.get("governance_notices")
    let has_notices_r2 = notices != null
    print("SECOND_HAS_NOTICES=" + string(has_notices_r2))
    print("SECOND_CONTENT=" + r2.get("content"))

    if has_notices_r2 {
        print("NOTICE_COUNT=" + string(len(notices)))
        for i in 0..len(notices) {
            print("NOTICE_" + string(i) + "=" + notices[i])
        }
    }
}
NAABEOF

# Run from the test directory so govern.json is discovered
cd "$TEST_DIR"
"$NAAB_BIN" "$TEST_DIR/reload_live.naab" \
    --governance-dashboard \
    > "$TEST_DIR/live_stdout.txt" 2> "$TEST_DIR/live_stderr.txt"
LIVE_EXIT=$?

# Debug output
echo "  [debug] exit=$LIVE_EXIT"
echo "  [debug] stdout:"
sed 's/^/    /' "$TEST_DIR/live_stdout.txt"
echo "  [debug] stderr (last 10 lines):"
tail -10 "$TEST_DIR/live_stderr.txt" | sed 's/^/    /'

check "Script completes successfully" \
    "[ '$LIVE_EXIT' -eq 0 ]"

check "First agent.send has no governance_notices" \
    "grep -q 'FIRST_HAS_NOTICES=false' '$TEST_DIR/live_stdout.txt'"

check "First agent.send got a response" \
    "grep -q 'FIRST_CONTENT=' '$TEST_DIR/live_stdout.txt'"

check "Second agent.send has governance_notices" \
    "grep -q 'SECOND_HAS_NOTICES=true' '$TEST_DIR/live_stdout.txt'"

check "Notices include tightened limits" \
    "grep -q 'tightened\|revoked' '$TEST_DIR/live_stdout.txt'"

check "Notices include update_reason" \
    "grep -q 'test tightening for CI' '$TEST_DIR/live_stdout.txt'"

check "Dashboard shows reload count" \
    "grep -q 'Reloads:' '$TEST_DIR/live_stderr.txt'"

check "Stderr shows config reloaded message" \
    "grep -q 'Config reloaded mid-run' '$TEST_DIR/live_stderr.txt'"

# --- Test 2: Ratchet rejects loosening mid-run ---
echo ""
echo "--- Live Ratchet Rejection ---"

# Start with tight config
cat > "$TEST_DIR/govern.json" <<GOVEOF
{
  "version": "5.0",
  "mode": "enforce",
  "sandbox_level": "unrestricted",
  "limits": {
    "execution": {
      "loop_iterations": 50
    }
  },
  "capabilities": {
    "shell": {
      "enabled": true
    }
  },
  "agents": {
    "test_bot": {
      "provider": "gemini",
      "model": "gemini-2.5-flash",
      "api_key_env": "NAAB_TEST_GK",
      "max_tokens": 50,
      "max_turns": 5,
      "system_prompt": "Reply with exactly one word."
    }
  }
}
GOVEOF

# Script that tries to loosen limits
cat > "$TEST_DIR/reload_loosen.naab" <<'NAABEOF'
use json
use agent

main {
    let handle = agent.create("test_bot")
    let r1 = agent.send(handle, "hi")
    print("FIRST_OK=true")

    // Try to LOOSEN govern.json (increase loop_iterations)
    let _ = <<shell
cat > govern.json << 'LOOSEEOF'
{
  "version": "5.0",
  "mode": "enforce",
  "sandbox_level": "unrestricted",
  "limits": {
    "execution": {
      "loop_iterations": 9999
    }
  },
  "capabilities": {
    "shell": {
      "enabled": true
    }
  },
  "agents": {
    "test_bot": {
      "provider": "gemini",
      "model": "gemini-2.5-flash",
      "api_key_env": "NAAB_TEST_GK",
      "max_tokens": 50,
      "max_turns": 5,
      "system_prompt": "Reply with exactly one word."
    }
  }
}
LOOSEEOF
>>

    // Second send — ratchet should reject, no notices
    let r2 = agent.send(handle, "bye")
    let has_notices = r2.get("governance_notices") != null
    print("LOOSEN_HAS_NOTICES=" + string(has_notices))
}
NAABEOF

cd "$TEST_DIR"
"$NAAB_BIN" "$TEST_DIR/reload_loosen.naab" \
    > "$TEST_DIR/loosen_stdout.txt" 2> "$TEST_DIR/loosen_stderr.txt" || true

echo "  [debug] loosen stdout:"
sed 's/^/    /' "$TEST_DIR/loosen_stdout.txt"
echo "  [debug] loosen stderr (last 10 lines):"
tail -10 "$TEST_DIR/loosen_stderr.txt" | sed 's/^/    /'

check "Loosening attempt rejected in stderr" \
    "grep -q 'ratchet violation\|Reload rejected' '$TEST_DIR/loosen_stderr.txt'"

check "Loosening does not produce governance_notices" \
    "grep -q 'LOOSEN_HAS_NOTICES=false' '$TEST_DIR/loosen_stdout.txt'"

# --- Results ---
echo ""
echo "=== Results: $PASS/$((PASS + FAIL)) passed, $FAIL failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
