#!/usr/bin/env bash
# ============================================================
# test_per_agent_signals.sh — Per-agent CDD signal overrides
#
# Per-agent "context_drift_signals" in the govern.json agents block
# overrides the global context_drift.signals toggles for that agent's
# handles, with ratchet enforcement (disabling an effectively enabled
# signal mid-run is a violation, both per-agent and globally).
#
# Group P: override suppresses a signal for one agent; a control agent
#          with the same global config still fires; env dict exposure
# Group R: mid-run reload ADDING a disable override -> ratchet rejection
#          + CONFIG_ADJUSTMENT (accepted=false, reason=ratchet)
# Group G: mid-run reload disabling the signal GLOBALLY -> ratchet rejection
# Group A: mid-run reload REMOVING a false override (tightening) accepted;
#          live handle picks up the re-enabled signal
# Group Q: agent.reset() preserves the override mask
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_per_agent_signals.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/peragentsig-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    teardown_isolated_trust
    rm -rf "$TEST_TMP"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"
export FAKE_KEY_PERAGENT_TEST="fake-key-per-agent-signals-test"

sign_govern() {
    (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

presign() {  # $1=json content
    local pdir="$TEST_TMP/.presign"
    mkdir -p "$pdir"
    printf '%s' "$1" > "$pdir/govern.json"
    sign_govern "$pdir"
    cat "$pdir/govern.json.sig" 2>/dev/null || echo ""
    rm -rf "$pdir"
}

start_stub() {  # $1=fixture $2=workdir
    STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
    python3 "$SCRIPT_DIR/../helpers/agent_stub.py" "$STUB_PORT" "$1" "$2" > "$2/stub.log" 2>&1 &
    STUB_PID=$!
    for _ in $(seq 1 50); do
        grep -q READY "$2/stub.log" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}
stop_stub() { [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; wait "$STUB_PID" 2>/dev/null; STUB_PID=""; }

# Off-mandate fixture: HTML-parser code with zero overlap with the
# calculator mandate — mandate_alignment fires unless overridden off.
write_offmandate_fixture() {  # $1=workdir
    cat > "$1/fixture.json" << 'EOF'
{"responses": [
  {"content": "class HtmlParser:\n    def parseNodes(self, markup):\n        tree = ElementTree(markup)\n        return tree.walkChildren()\n", "output_tokens": 50},
  {"content": "class HtmlParser:\n    def stripTags(self, markup):\n        cleaned = removeMarkup(markup)\n        return cleaned\n", "output_tokens": 45},
  {"content": "class HtmlParser:\n    def findLinks(self, document):\n        anchors = document.selectAnchors()\n        return anchors\n", "output_tokens": 45},
  {"content": "class HtmlParser:\n    def renderTemplate(self, layout):\n        widget = LayoutWidget(layout)\n        return widget.paint()\n", "output_tokens": 45}
]}
EOF
}

MANDATE="Build a Calculator class with add, subtract, multiply, divide methods and history tracking."

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Per-Agent CDD Signal Override Tests (context_drift_signals) |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# Group P: override suppresses signal; control agent still fires
# ============================================================
echo -e "${CYAN}--- Group P: per-agent suppression vs control ---${NC}"

WDIR="$TEST_TMP/group_p"; mkdir -p "$WDIR"
write_offmandate_fixture "$WDIR"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "P-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": { "enabled": true, "check_interval_turns": 1 },
    "agents": {
        "control": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_PERAGENT_TEST",
            "max_tokens": 1024, "max_turns": 20,
            "system_prompt": "$MANDATE"
        },
        "quiet": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_PERAGENT_TEST",
            "max_tokens": 1024, "max_turns": 20,
            "system_prompt": "$MANDATE",
            "context_drift_signals": { "mandate_alignment": false }
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let hc = agent.create("control")
    let hq = agent.create("quiet")
    let i = 0
    while i < 4 {
        let rc = agent.send(hc, "Continue the work")
        let rq = agent.send(hq, "Continue the work")
        i = i + 1
    }
    let ec = agent.environment(hc)
    let eq = agent.environment(hq)
    print("CONTROL_DRIFT=" + string(ec.get("state").get("mandate_drift_count")))
    print("QUIET_DRIFT=" + string(eq.get("state").get("mandate_drift_count")))
    let ov = eq.get("state").get("cdd_signal_overrides")
    if ov != null && ov.get("mandate_alignment") == false {
        print("OVERRIDE_EXPOSED=true")
    } else {
        print("OVERRIDE_EXPOSED=false")
    }
    let ovc = ec.get("state").get("cdd_signal_overrides")
    print("CONTROL_HAS_OVERRIDES=" + string(ovc != null))
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 90s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "CONTROL_DRIFT=0"; then
    fail "P-01" "control agent never fired mandate_alignment on off-mandate code" "$OUTPUT"
else
    if echo "$OUTPUT" | grep -q "CONTROL_DRIFT="; then
        pass "P-01" "control agent fires mandate_alignment (global config intact)"
    else
        fail "P-01" "no control drift output" "$OUTPUT"
    fi
fi
if echo "$OUTPUT" | grep -q "QUIET_DRIFT=0"; then
    pass "P-02" "per-agent override suppresses mandate_alignment for quiet agent"
else
    fail "P-02" "override did not suppress signal" "$OUTPUT"
fi
if echo "$OUTPUT" | grep -q "OVERRIDE_EXPOSED=true"; then
    pass "P-03" "cdd_signal_overrides exposed in agent environment state"
else
    fail "P-03" "override not visible in environment dict" "$OUTPUT"
fi
if echo "$OUTPUT" | grep -q "CONTROL_HAS_OVERRIDES=false"; then
    pass "P-04" "agents without overrides have no cdd_signal_overrides key"
else
    fail "P-04" "control agent unexpectedly has overrides" "$OUTPUT"
fi

# ============================================================
# Group R: reload ADDING a disable override -> ratchet rejection
# ============================================================
echo ""
echo -e "${CYAN}--- Group R: per-agent disable via reload rejected ---${NC}"

WDIR="$TEST_TMP/group_r"; mkdir -p "$WDIR"
write_offmandate_fixture "$WDIR"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "R-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "version": "5.0",
    "mode": "enforce",
    "sandbox_level": "unrestricted",
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": { "enabled": true, "check_interval_turns": 1 },
    "capabilities": { "shell": { "enabled": true } },
    "agents": {
        "worker": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_PERAGENT_TEST",
            "max_tokens": 1024, "max_turns": 20,
            "system_prompt": "$MANDATE"
        }
    }
}
GOVEOF

LOOSENED_JSON="{\"version\":\"5.0\",\"mode\":\"enforce\",\"sandbox_level\":\"unrestricted\",\"telemetry\":{\"enabled\":true,\"output_file\":\"telemetry.jsonl\"},\"behavioral_sequences\":{\"enabled\":true},\"context_drift\":{\"enabled\":true,\"check_interval_turns\":1},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"worker\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_PERAGENT_TEST\",\"max_tokens\":1024,\"max_turns\":20,\"system_prompt\":\"$MANDATE\",\"context_drift_signals\":{\"mandate_alignment\":false}}}}"
LOOSENED_SIG=$(presign "$LOOSENED_JSON")
export NAAB_PRESIGNED_JSON="$LOOSENED_JSON"
export NAAB_PRESIGNED_SIG="$LOOSENED_SIG"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("worker")
    let r1 = agent.send(h, "hello")
    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_PRESIGNED_JSON" > govern.json
printf '%s' "$NAAB_PRESIGNED_SIG" > govern.json.sig
>>
    try {
        let r2 = agent.send(h, "bye")
        print("R2_OK=true")
    } catch (e) {
        print("R2_OK=false")
    }
}
NAABEOF

sign_govern "$WDIR"
OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>"$WDIR/stderr.txt") || true
stop_stub

if grep -q "ratchet violation" "$WDIR/stderr.txt" 2>/dev/null && grep -q "context_drift_signals" "$WDIR/stderr.txt" 2>/dev/null; then
    pass "R-01" "adding a per-agent disable override mid-run rejected as ratchet violation"
else
    fail "R-01" "per-agent disable not rejected" "$(grep -i 'reload\|ratchet' "$WDIR/stderr.txt" 2>/dev/null | head -3)"
fi

REJ_LINE=$(grep '"event_type":"CONFIG_ADJUSTMENT"' "$WDIR/telemetry.jsonl" 2>/dev/null | tail -1)
if echo "$REJ_LINE" | grep -q '"accepted":"false"' && echo "$REJ_LINE" | grep -q '"reason":"ratchet"'; then
    pass "R-02" "rejected reload emits CONFIG_ADJUSTMENT (accepted=false, reason=ratchet)"
else
    fail "R-02" "no rejected CONFIG_ADJUSTMENT event" "$REJ_LINE"
fi

# ============================================================
# Group G: reload disabling the signal GLOBALLY -> ratchet rejection
# ============================================================
echo ""
echo -e "${CYAN}--- Group G: global signal disable via reload rejected ---${NC}"

WDIR="$TEST_TMP/group_g"; mkdir -p "$WDIR"
write_offmandate_fixture "$WDIR"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "G-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "version": "5.0",
    "mode": "enforce",
    "sandbox_level": "unrestricted",
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": { "enabled": true, "check_interval_turns": 1 },
    "capabilities": { "shell": { "enabled": true } },
    "agents": {
        "worker": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_PERAGENT_TEST",
            "max_tokens": 1024, "max_turns": 20,
            "system_prompt": "$MANDATE"
        }
    }
}
GOVEOF

GLOBAL_JSON="{\"version\":\"5.0\",\"mode\":\"enforce\",\"sandbox_level\":\"unrestricted\",\"telemetry\":{\"enabled\":true,\"output_file\":\"telemetry.jsonl\"},\"behavioral_sequences\":{\"enabled\":true},\"context_drift\":{\"enabled\":true,\"check_interval_turns\":1,\"signals\":{\"mandate_alignment\":false}},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"worker\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_PERAGENT_TEST\",\"max_tokens\":1024,\"max_turns\":20,\"system_prompt\":\"$MANDATE\"}}}"
GLOBAL_SIG=$(presign "$GLOBAL_JSON")
export NAAB_PRESIGNED_JSON="$GLOBAL_JSON"
export NAAB_PRESIGNED_SIG="$GLOBAL_SIG"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("worker")
    let r1 = agent.send(h, "hello")
    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_PRESIGNED_JSON" > govern.json
printf '%s' "$NAAB_PRESIGNED_SIG" > govern.json.sig
>>
    try {
        let r2 = agent.send(h, "bye")
        print("R2_OK=true")
    } catch (e) {
        print("R2_OK=false")
    }
}
NAABEOF

sign_govern "$WDIR"
OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>"$WDIR/stderr.txt") || true
stop_stub

if grep -q "ratchet violation" "$WDIR/stderr.txt" 2>/dev/null && grep -q "context_drift.signals.mandate_alignment" "$WDIR/stderr.txt" 2>/dev/null; then
    pass "G-01" "disabling context_drift.signals.mandate_alignment mid-run rejected"
else
    fail "G-01" "global signal disable not rejected" "$(grep -i 'reload\|ratchet' "$WDIR/stderr.txt" 2>/dev/null | head -3)"
fi

# ============================================================
# Group A: removing a false override (tightening) accepted + live effect
# ============================================================
echo ""
echo -e "${CYAN}--- Group A: tightening reload accepted, live handle updated ---${NC}"

WDIR="$TEST_TMP/group_a"; mkdir -p "$WDIR"
write_offmandate_fixture "$WDIR"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "A-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "version": "5.0",
    "mode": "enforce",
    "sandbox_level": "unrestricted",
    "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": { "enabled": true, "check_interval_turns": 1 },
    "capabilities": { "shell": { "enabled": true } },
    "agents": {
        "worker": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_PERAGENT_TEST",
            "max_tokens": 1024, "max_turns": 20,
            "system_prompt": "$MANDATE",
            "context_drift_signals": { "mandate_alignment": false }
        }
    }
}
GOVEOF

TIGHTENED_JSON="{\"version\":\"5.0\",\"mode\":\"enforce\",\"sandbox_level\":\"unrestricted\",\"telemetry\":{\"enabled\":true,\"output_file\":\"telemetry.jsonl\"},\"behavioral_sequences\":{\"enabled\":true},\"context_drift\":{\"enabled\":true,\"check_interval_turns\":1},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"worker\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_PERAGENT_TEST\",\"max_tokens\":1024,\"max_turns\":20,\"system_prompt\":\"$MANDATE\"}}}"
TIGHTENED_SIG=$(presign "$TIGHTENED_JSON")
export NAAB_PRESIGNED_JSON="$TIGHTENED_JSON"
export NAAB_PRESIGNED_SIG="$TIGHTENED_SIG"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("worker")
    let r1 = agent.send(h, "hello")
    r1 = agent.send(h, "again")
    let e1 = agent.environment(h)
    print("DRIFT_BEFORE=" + string(e1.get("state").get("mandate_drift_count")))

    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_PRESIGNED_JSON" > govern.json
printf '%s' "$NAAB_PRESIGNED_SIG" > govern.json.sig
>>
    let r2 = agent.send(h, "more")
    r2 = agent.send(h, "and more")
    let e2 = agent.environment(h)
    print("DRIFT_AFTER=" + string(e2.get("state").get("mandate_drift_count")))
    let ov = e2.get("state").get("cdd_signal_overrides")
    print("OVERRIDES_AFTER=" + string(ov != null))
}
NAABEOF

sign_govern "$WDIR"
OUTPUT=$(cd "$WDIR" && timeout 90s "$NAAB" test.naab 2>"$WDIR/stderr.txt") || true
stop_stub

if echo "$OUTPUT" | grep -q "DRIFT_BEFORE=0"; then
    pass "A-01" "override suppresses mandate_alignment before reload"
else
    fail "A-01" "signal fired despite override" "$OUTPUT"
fi
if echo "$OUTPUT" | grep -q "DRIFT_AFTER=0"; then
    fail "A-02" "re-enabled signal did not fire on live handle after tightening reload" "$OUTPUT $(grep -i 'reload\|ratchet' "$WDIR/stderr.txt" | head -3)"
else
    if echo "$OUTPUT" | grep -q "DRIFT_AFTER="; then
        pass "A-02" "tightening reload accepted; live handle picks up re-enabled signal"
    else
        fail "A-02" "no post-reload drift output" "$OUTPUT"
    fi
fi
if echo "$OUTPUT" | grep -q "OVERRIDES_AFTER=false"; then
    pass "A-03" "override mask cleared on live handle after override removal"
else
    fail "A-03" "stale override mask after reload" "$OUTPUT"
fi

# ============================================================
# Group Q: agent.reset() preserves override mask
# ============================================================
echo ""
echo -e "${CYAN}--- Group Q: agent.reset preserves overrides ---${NC}"

WDIR="$TEST_TMP/group_q"; mkdir -p "$WDIR"
write_offmandate_fixture "$WDIR"
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "Q-00" "stub failed to start"; exit 1; }

cat > "$WDIR/govern.json" << GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "behavioral_sequences": { "enabled": true },
    "context_drift": { "enabled": true, "check_interval_turns": 1 },
    "agents": {
        "quiet": {
            "provider": "gemini", "model": "stub-model",
            "api_base": "http://127.0.0.1:$STUB_PORT",
            "api_key_env": "FAKE_KEY_PERAGENT_TEST",
            "max_tokens": 1024, "max_turns": 20,
            "system_prompt": "$MANDATE",
            "context_drift_signals": { "mandate_alignment": false }
        }
    }
}
GOVEOF
sign_govern "$WDIR"

cat > "$WDIR/test.naab" << 'NAABEOF'
use agent

main {
    let h = agent.create("quiet")
    let r = agent.send(h, "hello")
    r = agent.send(h, "again")
    agent.reset(h)
    r = agent.send(h, "post reset")
    r = agent.send(h, "post reset two")
    let env = agent.environment(h)
    let st = env.get("state")
    print("DRIFT=" + string(st.get("mandate_drift_count")))
    let ov = st.get("cdd_signal_overrides")
    if ov != null && ov.get("mandate_alignment") == false {
        print("OVERRIDE_KEPT=true")
    } else {
        print("OVERRIDE_KEPT=false")
    }
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 90s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "DRIFT=0" && echo "$OUTPUT" | grep -q "OVERRIDE_KEPT=true"; then
    pass "Q-01" "agent.reset preserves the per-agent override mask"
else
    fail "Q-01" "override lost across agent.reset" "$OUTPUT"
fi

# ============================================================
# Summary
# ============================================================
echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "  ${GREEN}PASS: $PASS_COUNT${NC}  ${RED}FAIL: $FAIL_COUNT${NC}  ${YELLOW}SKIP: $SKIP_COUNT${NC}  TOTAL: $TOTAL"
if [ $FAIL_COUNT -gt 0 ]; then
    echo -e "${RED}Failures:${NC}$FAILURES"
    echo "test_per_agent_signals.sh: FAILED"
    exit 1
fi
echo "test_per_agent_signals.sh: ALL PASSED"
exit 0
