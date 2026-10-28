#!/usr/bin/env bash
# Test: Telemetry webhook forwarding
# Verifies that governance telemetry events are forwarded to a webhook endpoint.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${1:-$PROJECT_DIR/build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

SIGNING_KEY="${HOME}/.naab/keys/signing.pem"
WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/naab_telfwd_XXXXXX")
trap 'rm -rf "$WORKDIR"; [ -n "${LISTENER_PID:-}" ] && kill "$LISTENER_PID" 2>/dev/null || true' EXIT

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS: $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL: $1"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP: $1"; }

sign_gov() {
    local dir="$1"
    if [ -f "$SIGNING_KEY" ]; then
        (cd "$dir" && NAAB_SIGNING_KEY="$SIGNING_KEY" "$NAAB" --sign-governance 2>/dev/null) || true
    fi
}

echo "=== Telemetry Forwarding Tests ==="

# --- Test 1: Config parsing ---
echo ""
echo "--- Test 1: Webhook config fields parsed correctly ---"

T1DIR="$WORKDIR/t1"
mkdir -p "$T1DIR"

cat > "$T1DIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": {
        "enabled": true,
        "output_file": "test_telemetry.jsonl",
        "webhook_url": "http://localhost:19876/ingest",
        "forward_batch_size": 5,
        "forward_timeout_ms": 3000,
        "forward_retry_count": 1,
        "forward_buffer_max": 500
    }
}
GOVEOF
sign_gov "$T1DIR"

# Simple script — should parse config without error
cat > "$T1DIR/test_config.naab" << 'NAABEOF'
main {
    let x = 42
    print(x)
}
NAABEOF

OUTPUT1=$(cd "$T1DIR" && timeout 10s "$NAAB" test_config.naab 2>/dev/null) || true
if grep <<<"$OUTPUT1" -q "42"; then
    ok "Webhook config fields parsed without error"
else
    fail "Webhook config broke execution"
    echo "    OUTPUT: $(echo "$OUTPUT1" | head -3)"
fi

# --- Test 2: Local telemetry still works with webhook configured ---
echo ""
echo "--- Test 2: Local JSONL file still written with webhook configured ---"

T2DIR="$WORKDIR/t2"
mkdir -p "$T2DIR"

cat > "$T2DIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "capabilities": { "shell": { "enabled": true } },
    "telemetry": {
        "enabled": true,
        "output_file": "telemetry.jsonl",
        "webhook_url": "http://127.0.0.1:19999/nonexistent",
        "forward_timeout_ms": 500,
        "forward_retry_count": 0,
        "tamper_evidence": {
            "enabled": true,
            "chain_genesis": "TEST-GENESIS"
        }
    }
}
GOVEOF
sign_gov "$T2DIR"

cat > "$T2DIR/test_local.naab" << 'NAABEOF'
main {
    let r = <<sh
    echo "hello"
    >>
    print(r)
}
NAABEOF

cd "$T2DIR" && timeout 15s "$NAAB" test_local.naab --governance-dashboard 2>/dev/null || true
if [ -f "$T2DIR/telemetry.jsonl" ]; then
    LINES=$(wc -l < "$T2DIR/telemetry.jsonl")
    if [ "$LINES" -gt 0 ]; then
        ok "Local JSONL file written ($LINES events) even with unreachable webhook"
    else
        fail "JSONL file exists but is empty"
    fi
else
    fail "Local JSONL file not created"
fi

# --- Test 3: Webhook auth env var resolution ---
echo ""
echo "--- Test 3: Webhook auth via env var ---"

T3DIR="$WORKDIR/t3"
mkdir -p "$T3DIR"

cat > "$T3DIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "telemetry": {
        "enabled": true,
        "output_file": "telemetry.jsonl",
        "webhook_url": "http://localhost:19876/ingest",
        "webhook_auth_env": "NAAB_TEST_SIEM_KEY"
    }
}
GOVEOF
sign_gov "$T3DIR"

cat > "$T3DIR/test_auth.naab" << 'NAABEOF'
main {
    print("auth_test_ok")
}
NAABEOF

NAAB_TEST_SIEM_KEY="test-api-key-12345" \
    timeout 10s "$NAAB" "$T3DIR/test_auth.naab" 2>/dev/null || true
# If it ran without crash, the env var was resolved
OUTPUT3=$(NAAB_TEST_SIEM_KEY="test-api-key-12345" timeout 10s "$NAAB" "$T3DIR/test_auth.naab" 2>/dev/null) || true
if grep <<<"$OUTPUT3" -q "auth_test_ok"; then
    ok "Webhook auth env var resolved without error"
else
    fail "Auth env var resolution broke execution"
fi

# --- Test 4: Forwarding failure doesn't block execution ---
echo ""
echo "--- Test 4: Forwarding failure is non-blocking ---"

T4DIR="$WORKDIR/t4"
mkdir -p "$T4DIR"

cat > "$T4DIR/govern.json" << 'GOVEOF'
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "capabilities": { "shell": { "enabled": true } },
    "telemetry": {
        "enabled": true,
        "output_file": "telemetry.jsonl",
        "webhook_url": "http://192.0.2.1:1/unreachable",
        "forward_timeout_ms": 500,
        "forward_retry_count": 0,
        "forward_buffer_max": 10
    }
}
GOVEOF
sign_gov "$T4DIR"

cat > "$T4DIR/test_nonblock.naab" << 'NAABEOF'
main {
    let r = <<sh
    echo "nonblock_ok"
    >>
    print(r)
}
NAABEOF

START=$(date +%s)
OUTPUT4=$(cd "$T4DIR" && timeout 15s "$NAAB" test_nonblock.naab 2>/dev/null) || true
END=$(date +%s)
ELAPSED=$((END - START))

if grep <<<"$OUTPUT4" -q "nonblock_ok"; then
    if [ "$ELAPSED" -lt 10 ]; then
        ok "Execution completed in ${ELAPSED}s despite unreachable webhook (non-blocking)"
    else
        fail "Execution took ${ELAPSED}s — webhook may be blocking"
    fi
else
    fail "Execution failed with unreachable webhook"
fi

# --- F53: what actually reaches the webhook --------------------------------
#
# Tests 1-4 above never started a listener, so none of them could see that a
# CLI run forwarded nothing: every run ends in _exit() (even a clean one, as
# _exit(0)), _exit() runs no destructors, and the forwarder's drain lived in
# ~GovernanceEngine. Measured on the master binary: a clean run forwarded 0 of
# its 3 events, a HARD block 1 of 3 (the worker raced it), and RunStart, RunEnd,
# GovernanceCheckSummary and the end-of-run health warning were never enqueued
# at all. Every arm below compares what a loopback sink RECEIVED, in order,
# with the telemetry file.
#   FW-00   CONTROL: the sink records a POST made directly to it
#   FW-01   a clean run (exit 0): every file line arrives, RunStart and RunEnd included
#   FW-02   a HARD block (exit 3): every file line arrives, the violation and RunEnd included
#   FW-03   a run writing every chained event type: every line arrives, and the
#           forwarded stream verifies as a chain on its own
#   FW-03c  CONTROL: the verifier warns on the run's lines with the RunEnd cut off
#   FW-04   a webhook that never answers: the exit waits forward_shutdown_drain_ms
#           (1 s), not forward_timeout_ms (20 s), and says how many were lost
#   FW-04c  CONTROL: the stalled sink really received nothing
echo ""
echo "--- F53: forwarded events reach the webhook on every exit path ---"
SINK_HELPER="$PROJECT_DIR/tests/helpers/webhook_sink.py"
# Callers run start_sink inside $( ... ): a PID kept in a shell variable there
# dies with the subshell, so the sinks are tracked in a file the EXIT trap reads.
# The sink's own stdout must not be the substitution's, or $( ... ) waits for it.
SINK_PIDFILE="$WORKDIR/sinks.pids"
trap 'while read -r p; do kill "$p" 2>/dev/null; done < "$SINK_PIDFILE" 2>/dev/null; rm -rf "$WORKDIR"; true' EXIT

# start_sink DIR [--hang]: starts a sink writing DIR/recv.jsonl; prints its port
start_sink() {
    local dir="$1"; shift
    (cd "$dir" && rm -f port recv.jsonl && : > recv.jsonl && exec python3 "$SINK_HELPER" port recv.jsonl "$@") \
        >/dev/null 2>&1 &
    echo "$!" >> "$SINK_PIDFILE"
    local i
    for i in $(seq 1 50); do [ -s "$dir/port" ] && break; sleep 0.1; done
    cat "$dir/port" 2>/dev/null || true
}
# fwd_config DIR PORT EXTRA_TELEMETRY_KEYS EXTRA_TOP_KEYS
fwd_config() {
    cat > "$1/govern.json" <<GOVEOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    "code_quality": { "no_placeholders": { "enabled": true, "level": "${5:-hard}" } }$4,
    "telemetry": {
        "enabled": true,
        "output_file": "tel.jsonl",
        "webhook_url": "http://127.0.0.1:$2/ingest",
        "forward_batch_size": 50,
        "forward_retry_count": 0,$3
        "tamper_evidence": { "enabled": true, "chain_genesis": "FW-GENESIS" }
    }
}
GOVEOF
    sign_gov "$1"
}
# The file's lines, each re-serialized the way the sink writes them (sorted
# keys, compact, ASCII), so the two can be compared byte for byte. Bytes on
# stdin, not a path: no path crosses into a native python3.
normalize() { python3 -c 'import json,sys
for l in sys.stdin:
    l = l.strip()
    if l: print(json.dumps(json.loads(l), separators=(",", ":"), sort_keys=True))'; }
types_of() { python3 -c 'import json,sys
print(" ".join(json.loads(l).get("event_type","?") for l in sys.stdin if l.strip()))'; }
# run_fw DIR PROG: runs PROG there with no proxy in the way; sets FW_RC, FW_ERR
run_fw() {
    FW_RC=0
    FW_ERR=$(cd "$1" && env -u http_proxy -u HTTP_PROXY -u all_proxy -u ALL_PROXY \
        timeout 60s "$NAAB" run "$2" 2>&1 >/dev/null) || FW_RC=$?
    sleep 1   # the sink writes each batch as it lands; nothing is in flight after the exit
}
same_stream() {   # same_stream DIR: 0 when the sink received exactly the file's lines, in order
    [ -s "$1/tel.jsonl" ] && [ -s "$1/recv.jsonl" ] &&
        [ "$(normalize < "$1/tel.jsonl")" = "$(cat "$1/recv.jsonl")" ]
}
show_streams() {
    echo "    file:      $(types_of < "$1/tel.jsonl" 2>/dev/null)"
    echo "    forwarded: $(types_of < "$1/recv.jsonl" 2>/dev/null)"
}

FW0="$WORKDIR/fw0"; mkdir -p "$FW0"
P0=$(start_sink "$FW0")
python3 -c 'import sys, urllib.request
r = urllib.request.Request("http://127.0.0.1:%s/x" % sys.argv[1], data=b"[{\"event_type\":\"PROBE\"}]",
                           headers={"Content-Type": "application/json"})
urllib.request.build_opener(urllib.request.ProxyHandler({})).open(r, timeout=5).read()' "$P0" 2>/dev/null || true
if [ -n "$P0" ] && grep -q '"PROBE"' "$FW0/recv.jsonl" 2>/dev/null; then
    ok "FW-00 the sink records a direct POST"
    SINK_OK=1
else
    skip "FW-00 UNMEASURABLE: the loopback sink did not start or record a POST (port='$P0')"
    SINK_OK=0
fi

if [ "$SINK_OK" = 1 ]; then
    FW1="$WORKDIR/fw1"; mkdir -p "$FW1"
    fwd_config "$FW1" "$(start_sink "$FW1")" "" ""
    printf 'main {\n    print("fw1")\n}\n' > "$FW1/clean.naab"
    run_fw "$FW1" clean.naab
    if [ "$FW_RC" -eq 0 ] && same_stream "$FW1" && grep -q '"RunStart"' "$FW1/recv.jsonl" \
       && grep -q '"RunEnd"' "$FW1/recv.jsonl"; then
        ok "FW-01 a clean run (exit 0) forwards every telemetry line, RunStart and RunEnd included"
    else
        fail "FW-01 a clean run should forward every telemetry line (rc=$FW_RC)"; show_streams "$FW1"
    fi

    FW2="$WORKDIR/fw2"; mkdir -p "$FW2"
    fwd_config "$FW2" "$(start_sink "$FW2")" "" ""
    printf 'fn f() {\n    // TODO: finish\n    return 1\n}\nmain {\n    print(f())\n}\n' > "$FW2/block.naab"
    run_fw "$FW2" block.naab
    if [ "$FW_RC" -eq 3 ] && same_stream "$FW2" && grep -q '"RuleViolation"' "$FW2/recv.jsonl" \
       && grep -q '"RunEnd"' "$FW2/recv.jsonl"; then
        ok "FW-02 a HARD block (exit 3) forwards every telemetry line, the violation and RunEnd included"
    else
        fail "FW-02 a HARD block should forward every telemetry line (rc=$FW_RC, expected 3)"; show_streams "$FW2"
    fi

    FW3="$WORKDIR/fw3"; mkdir -p "$FW3"
    fwd_config "$FW3" "$(start_sink "$FW3")" '
        "deduplicate_checks": true,' ',
    "context_drift": { "enabled": true },
    "governance_health": { "enabled": true },
    "scoring": { "enabled": true }' advisory
    printf 'fn f() {\n    // TODO: a\n    return 1\n}\nmain {\n    let i = 0\n    while i < 3 {\n        let r = <<python\n1 + 1\n>>\n        i = i + 1\n    }\n    print(f())\n}\n' > "$FW3/rich.naab"
    run_fw "$FW3" rich.naab
    FT3=$(types_of < "$FW3/tel.jsonl" 2>/dev/null || true)
    if ! grep -q GovernanceCheckSummary <<<"$FT3" || ! grep -q GOVERNANCE_HEALTH_WARNING <<<"$FT3" \
       || ! grep -q ScoringSnapshot <<<"$FT3"; then
        skip "FW-03 UNMEASURABLE: the run did not write the summary, health and scoring events (rc=$FW_RC; file: $FT3)"
    else
        VER3=$("$NAAB" --verify-telemetry-chain "$FW3/recv.jsonl" 2>&1) || true
        if same_stream "$FW3" && grep -q 'no breaks (0 warning(s)' <<<"$VER3"; then
            ok "FW-03 every chained event type is forwarded, and the forwarded stream verifies on its own"
        else
            fail "FW-03 the forwarded stream should equal the file and verify as a chain"; show_streams "$FW3"
            echo "    verify: $VER3"
        fi
        # From the FILE, so the control does not depend on what was forwarded.
        normalize < "$FW3/tel.jsonl" | grep -v '"RunEnd"' > "$FW3/cut.jsonl" || true
        VER3C=$("$NAAB" --verify-telemetry-chain "$FW3/cut.jsonl" 2>&1) || true
        if grep -q 'has no RunEnd' <<<"$VER3C"; then
            ok "FW-03c CONTROL: the verifier warns when the run's lines lack their RunEnd"
        else
            fail "FW-03c the verifier should warn on a stream with no RunEnd"; echo "    verify: $VER3C"
        fi
    fi

    FW4="$WORKDIR/fw4"; mkdir -p "$FW4"
    fwd_config "$FW4" "$(start_sink "$FW4" --hang)" '
        "forward_shutdown_drain_ms": 1000,
        "forward_timeout_ms": 20000,' ""
    printf 'main {\n    print("fw4")\n}\n' > "$FW4/stall.naab"
    T0=$(date +%s%N)
    run_fw "$FW4" stall.naab
    T1=$(date +%s%N)
    MS4=$(( (T1 - T0) / 1000000 - 1000 ))   # minus run_fw's own 1 s settle
    if [ "$FW_RC" -eq 0 ] && [ "$MS4" -lt 4000 ] && grep -q 'Shutdown drain timeout: [0-9]* events not forwarded' <<<"$FW_ERR"; then
        ok "FW-04 a webhook that never answers holds the exit ${MS4} ms (drain 1000 ms, POST timeout 20000 ms) and the loss is reported"
    else
        fail "FW-04 the exit should wait about forward_shutdown_drain_ms and report the loss (rc=$FW_RC, ${MS4} ms)"
        echo "    stderr: $(grep -i telemetry <<<"$FW_ERR")"
    fi
    if [ ! -s "$FW4/recv.jsonl" ]; then
        ok "FW-04c CONTROL: the stalled sink received nothing, so FW-04 measured a stall"
    else
        fail "FW-04c the stalled sink should have received nothing"
    fi
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $SKIP skipped ==="
[ "$FAIL" -eq 0 ]
