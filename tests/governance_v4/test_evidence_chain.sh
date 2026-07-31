#!/usr/bin/env bash
# ============================================================
# test_evidence_chain.sh — Evidence Chain Hardening Tests
#
# Covers the file-anchored telemetry hash chain, run anchors, the
# upgraded verifier, per-decision CDD snapshots, transcript
# cross-references, and the telemetry evidence ratchet.
#
# Group A: chain continuity across runs + verifier detections
# Group B: per-decision CDD snapshots (telemetry.decision_snapshots)
# Group C: transcript entry_hash + TRANSCRIPT_REF cross-reference
# Group D: telemetry evidence ratchet (mid-run loosening rejected)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/evidence-chain-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

IS_WINDOWS=false
if [[ "$(uname -s)" == MINGW* ]] || [[ "$(uname -s)" == MSYS* ]] || [[ -n "${WINDIR:-}" ]]; then
    IS_WINDOWS=true
fi

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
export FAKE_KEY_EVIDENCE_TEST="fake-key-evidence-chain-test"

sign_govern() {
    (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
}

# Pre-sign JSON content out-of-band; echoes the signature.
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

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|   Evidence Chain: continuity / snapshots / transcript refs   |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# ============================================================
# Group A: chain continuity across runs + verifier detections
# ============================================================
echo -e "${CYAN}--- Group A: file-anchored chain + verifier ---${NC}"

WDIR="$TEST_TMP/group_a"; mkdir -p "$WDIR"
cat > "$WDIR/govern.json" << 'GOVEOF'
{
  "version": "5.0",
  "mode": "monitor",
  "telemetry": {
    "enabled": true,
    "output_file": "tele.jsonl",
    "tamper_evidence": { "enabled": true, "algorithm": "sha256" }
  }
}
GOVEOF
sign_govern "$WDIR"
cat > "$WDIR/t.naab" << 'NAABEOF'
main {
    print("hello")
}
NAABEOF

(cd "$WDIR" && timeout 30s "$NAAB" t.naab >/dev/null 2>&1)
if (cd "$WDIR" && "$NAAB" --verify-telemetry-chain tele.jsonl >/dev/null 2>&1); then
    pass "A-01" "single run: chain verifies clean"
else
    fail "A-01" "single-run chain failed verification"
fi

(cd "$WDIR" && timeout 30s "$NAAB" t.naab >/dev/null 2>&1)
(cd "$WDIR" && timeout 30s "$NAAB" t.naab >/dev/null 2>&1)
VOUT=$(cd "$WDIR" && "$NAAB" --verify-telemetry-chain tele.jsonl 2>&1); VEXIT=$?
if [ "$VEXIT" -eq 0 ] && ! echo "$VOUT" | grep -q "BREAK\|LEGACY RESTART"; then
    pass "A-02" "three sequential runs, shared file: continuous chain, no breaks"
else
    fail "A-02" "cross-run continuity broken" "$VOUT"
fi

# RunStart of run N+1 must carry run N's final hash as prev_hash
LINK_OK=$(python3 - "$WDIR/tele.jsonl" << 'PYEOF'
import json, sys
events = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
ok = True
prev_hash = None
for e in events:
    if e.get("event_type") == "RunStart" and prev_hash is not None:
        if e.get("prev_hash") != prev_hash:
            ok = False
    if "hash" in e:
        prev_hash = e["hash"]
print("OK" if ok else "MISMATCH")
PYEOF
)
if [ "$LINK_OK" = "OK" ]; then
    pass "A-03" "each RunStart anchors to the previous run's final hash"
else
    fail "A-03" "RunStart anchor mismatch"
fi

# RunEnd declares the exact per-run chained event count
COUNT_OK=$(python3 - "$WDIR/tele.jsonl" << 'PYEOF'
import json, sys
from collections import defaultdict
counts = defaultdict(int); declared = {}
for l in open(sys.argv[1]):
    e = json.loads(l)
    if "hash" not in e: continue
    counts[e.get("run_id")] += 1
    if e.get("event_type") == "RunEnd":
        declared[e.get("run_id")] = e.get("chained_events")
ok = declared and all(declared.get(r) == c for r, c in counts.items())
print("OK" if ok else "MISMATCH")
PYEOF
)
if [ "$COUNT_OK" = "OK" ]; then
    pass "A-04" "RunEnd chained_events matches observed per-run count"
else
    fail "A-04" "RunEnd count mismatch"
fi

# Delete a middle event → BREAK
sed '2d' "$WDIR/tele.jsonl" > "$WDIR/cut_ev.jsonl"
VOUT=$(cd "$WDIR" && "$NAAB" --verify-telemetry-chain cut_ev.jsonl 2>&1); VEXIT=$?
if [ "$VEXIT" -eq 1 ] && echo "$VOUT" | grep -q "BREAK"; then
    pass "A-05" "deleting a middle event is detected (BREAK, exit 1)"
else
    fail "A-05" "middle-event deletion not detected" "exit=$VEXIT"
fi

# Delete an entire interior run (lines 3-4 = run 2 of 3) → BREAK via anchor
sed '3,4d' "$WDIR/tele.jsonl" > "$WDIR/cut_run.jsonl"
VOUT=$(cd "$WDIR" && "$NAAB" --verify-telemetry-chain cut_run.jsonl 2>&1); VEXIT=$?
if [ "$VEXIT" -eq 1 ] && echo "$VOUT" | grep -q "BREAK"; then
    pass "A-06" "deleting an entire interior run is detected (BREAK, exit 1)"
else
    fail "A-06" "interior-run deletion not detected" "exit=$VEXIT"
fi

# Truncate the final run's tail (drop its RunEnd) → advisory warning, exit 0
sed '$d' "$WDIR/tele.jsonl" > "$WDIR/trunc.jsonl"
VOUT=$(cd "$WDIR" && "$NAAB" --verify-telemetry-chain trunc.jsonl 2>&1); VEXIT=$?
if [ "$VEXIT" -eq 0 ] && echo "$VOUT" | grep -q "WARNING.*no RunEnd"; then
    pass "A-07" "final-run tail truncation warns (crash-indistinguishable, exit 0)"
else
    fail "A-07" "final-run truncation handling wrong" "exit=$VEXIT $VOUT"
fi

# Mutate an event → TAMPER
sed '1s/"event_type":"RunStart"/"event_type":"RunStarX"/' "$WDIR/tele.jsonl" > "$WDIR/tamper.jsonl"
VOUT=$(cd "$WDIR" && "$NAAB" --verify-telemetry-chain tamper.jsonl 2>&1); VEXIT=$?
if [ "$VEXIT" -eq 1 ] && echo "$VOUT" | grep -q "TAMPER"; then
    pass "A-08" "mutating an event is detected (TAMPER, exit 1)"
else
    fail "A-08" "event mutation not detected" "exit=$VEXIT"
fi

# Legacy pre-continuity file (per-run genesis restarts) → warning, not break
python3 - "$WDIR/legacy.jsonl" << 'PYEOF'
import json, hashlib, sys
G = "NAAB-GOVERNANCE-GENESIS"
def mk(ev, prev):
    ev["prev_hash"] = prev
    h = hashlib.sha256(json.dumps(ev, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    ev["hash"] = h
    return ev, h
evs = []
e, h = mk({"run_id": "r1", "event_type": "GovernanceCheck", "n": 1}, G); evs.append(e)
e, h = mk({"run_id": "r1", "event_type": "GovernanceCheck", "n": 2}, h); evs.append(e)
e, h = mk({"run_id": "r2", "event_type": "GovernanceCheck", "n": 3}, G); evs.append(e)  # legacy restart
e, h = mk({"run_id": "r2", "event_type": "GovernanceCheck", "n": 4}, h); evs.append(e)
open(sys.argv[1], "w").write("\n".join(json.dumps(x, sort_keys=True, separators=(',', ':')) for x in evs) + "\n")
PYEOF
VOUT=$(cd "$WDIR" && "$NAAB" --verify-telemetry-chain legacy.jsonl 2>&1); VEXIT=$?
if [ "$VEXIT" -eq 0 ] && echo "$VOUT" | grep -q "LEGACY RESTART"; then
    pass "A-09" "legacy per-run genesis restart classified as warning, not break"
else
    fail "A-09" "legacy restart handling wrong" "exit=$VEXIT $VOUT"
fi

# ============================================================
# Group B: per-decision CDD snapshots
# ============================================================
echo -e "${CYAN}--- Group B: decision snapshots ---${NC}"

WDIR="$TEST_TMP/group_b"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "reconcile ledger quarterly totals balance done", "output_tokens": 20},
  {"content": "checked the ledger balance again totals fine", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "B-00" "stub failed to start"; STUB_PORT=0; }

if [ "$STUB_PORT" != "0" ]; then
cat > "$WDIR/govern.json" << GOVEOF
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "telemetry.jsonl",
                 "tamper_evidence": { "enabled": true },
                 "decision_snapshots": true },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1 },
  "circuit_breaker": { "enabled": true,
                       "output_admissibility": { "enabled": true, "threshold": 0.1, "action": "quarantine" } },
  "agents": {
    "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_EVIDENCE_TEST", "max_tokens": 100, "max_turns": 20 }
  }
}
GOVEOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
main {
    let h = agent.create("worker")
    let r1 = agent.send(h, "reconcile ledger quarterly totals balance")
    let r2 = agent.send(h, "check the ledger balance once more")
    print("DONE")
}
NAABEOF

OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

if echo "$OUTPUT" | grep -q "DONE"; then
    pass "B-01" "agent sends complete with decision_snapshots enabled"
else
    fail "B-01" "agent sends failed" "$(echo "$OUTPUT" | head -3)"
fi

SNAP_CHECK=$(python3 - "$WDIR/telemetry.jsonl" << 'PYEOF'
import json, sys
semantic = oa = 0
fields_ok = True
for l in open(sys.argv[1]):
    e = json.loads(l)
    snap = e.get("cdd_snapshot")
    if e.get("event_type") == "SEMANTIC_TURN" and isinstance(snap, dict):
        semantic += 1
        for f in ("coherence_score", "signal_baselines", "counts", "mandate_keywords"):
            if f not in snap: fields_ok = False
        if not isinstance(snap.get("mandate_keywords", {}).get("digest"), str): fields_ok = False
    if e.get("event_type") == "OUTPUT_ADMISSIBILITY_EVAL" and isinstance(snap, dict):
        oa += 1
print(f"{semantic} {oa} {'OK' if fields_ok else 'BAD'}")
PYEOF
)
read -r SEM_N OA_N FIELDS_OK <<< "$SNAP_CHECK"
if [ "${SEM_N:-0}" -ge 1 ] && [ "$FIELDS_OK" = "OK" ]; then
    pass "B-02" "SEMANTIC_TURN carries nested cdd_snapshot with decision fields ($SEM_N events)"
else
    fail "B-02" "cdd_snapshot missing or malformed on SEMANTIC_TURN" "$SNAP_CHECK"
fi
if [ "${OA_N:-0}" -ge 1 ]; then
    pass "B-03" "OUTPUT_ADMISSIBILITY_EVAL carries cdd_snapshot ($OA_N events)"
else
    fail "B-03" "cdd_snapshot missing on OUTPUT_ADMISSIBILITY_EVAL"
fi

if (cd "$WDIR" && "$NAAB" --verify-telemetry-chain telemetry.jsonl >/dev/null 2>&1); then
    pass "B-04" "chain verifies with snapshots embedded in hashed payloads"
else
    fail "B-04" "chain broken by snapshot embedding"
fi
fi

# Default-off: no decision_snapshots key → no cdd_snapshot fields
WDIR="$TEST_TMP/group_b_off"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [{"content": "reconcile ledger totals", "output_tokens": 20}]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "B-05" "stub failed to start"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
cat > "$WDIR/govern.json" << GOVEOF
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "telemetry.jsonl" },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1 },
  "agents": {
    "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_EVIDENCE_TEST", "max_tokens": 100, "max_turns": 20 }
  }
}
GOVEOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
main {
    let h = agent.create("worker")
    let r1 = agent.send(h, "reconcile ledger totals")
    print("DONE")
}
NAABEOF
OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub
if grep -q '"cdd_snapshot"' "$WDIR/telemetry.jsonl" 2>/dev/null; then
    fail "B-05" "cdd_snapshot emitted despite decision_snapshots being off"
else
    pass "B-05" "decision_snapshots is off by default (no cdd_snapshot fields)"
fi
fi

# ============================================================
# Group C: transcript entry_hash + TRANSCRIPT_REF cross-reference
# ============================================================
echo -e "${CYAN}--- Group C: transcript cross-reference ---${NC}"

WDIR="$TEST_TMP/group_c"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "reconcile ledger quarterly totals balance done", "output_tokens": 20},
  {"content": "checked the ledger balance again totals fine", "output_tokens": 20}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "C-00" "stub failed to start"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
cat > "$WDIR/govern.json" << GOVEOF
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "telemetry.jsonl",
                 "tamper_evidence": { "enabled": true },
                 "transcript": { "enabled": true, "output_file": "transcript.jsonl" } },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1 },
  "agents": {
    "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_EVIDENCE_TEST", "max_tokens": 100, "max_turns": 20 }
  }
}
GOVEOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
main {
    let h = agent.create("worker")
    let r1 = agent.send(h, "reconcile ledger quarterly totals balance")
    let r2 = agent.send(h, "check the ledger balance once more")
    print("DONE")
}
NAABEOF
OUTPUT=$(cd "$WDIR" && timeout 60s "$NAAB" test.naab 2>&1) || true
stop_stub

XREF=$(python3 - "$WDIR/telemetry.jsonl" "$WDIR/transcript.jsonl" << 'PYEOF'
import json, hashlib, sys
tele_hashes = set()
refs = 0
for l in open(sys.argv[1]):
    e = json.loads(l)
    if e.get("event_type") == "TRANSCRIPT_REF":
        refs += 1
        tele_hashes.add(e.get("entry_hash"))
ok = total = 0
for l in open(sys.argv[2]):
    e = json.loads(l); total += 1
    eh = e.pop("entry_hash", None)
    recomputed = hashlib.sha256(
        json.dumps(e, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    if eh == recomputed and eh in tele_hashes:
        ok += 1
print(f"{ok} {total} {refs}")
PYEOF
)
read -r XOK XTOTAL XREFS <<< "$XREF"
if [ "${XTOTAL:-0}" -ge 3 ] && [ "$XOK" = "$XTOTAL" ]; then
    pass "C-01" "all $XTOTAL transcript entries carry a recomputable entry_hash"
else
    fail "C-01" "transcript entry hashes wrong" "$XREF"
fi
if [ "${XREFS:-0}" -ge 3 ] && [ "$XOK" = "$XTOTAL" ]; then
    pass "C-02" "every transcript entry is committed into the chain via TRANSCRIPT_REF"
else
    fail "C-02" "TRANSCRIPT_REF cross-reference incomplete" "$XREF"
fi
if (cd "$WDIR" && "$NAAB" --verify-telemetry-chain telemetry.jsonl >/dev/null 2>&1); then
    pass "C-03" "chain verifies with transcript references"
else
    fail "C-03" "chain broken by transcript references"
fi
fi

# ============================================================
# Group D: telemetry evidence ratchet
# ============================================================
echo -e "${CYAN}--- Group D: evidence ratchet ---${NC}"

if $IS_WINDOWS; then
    skip "D-01" "mid-run file swap requires POSIX file semantics"
    skip "D-02" "mid-run file swap requires POSIX file semantics"
    skip "D-03" "mid-run file swap requires POSIX file semantics"
else
WDIR="$TEST_TMP/group_d"; mkdir -p "$WDIR"
cat > "$WDIR/fixture.json" << 'EOF'
{"responses": [
  {"content": "first stub reply", "output_tokens": 10},
  {"content": "second stub reply", "output_tokens": 10}
]}
EOF
start_stub "$WDIR/fixture.json" "$WDIR" || { skip "D-00" "stub failed to start"; STUB_PORT=0; }
if [ "$STUB_PORT" != "0" ]; then
# Loosened config: tamper_evidence and decision_snapshots switched off mid-run
LOOSE_JSON="{\"version\":\"5.0\",\"mode\":\"enforce\",\"security\":{\"sandbox_level\":\"elevated\"},\"telemetry\":{\"enabled\":true,\"output_file\":\"telemetry.jsonl\",\"tamper_evidence\":{\"enabled\":false},\"decision_snapshots\":false},\"behavioral_sequences\":{\"enabled\":true},\"context_drift\":{\"enabled\":true,\"level\":\"advisory\",\"check_interval_turns\":1},\"capabilities\":{\"shell\":{\"enabled\":true}},\"agents\":{\"worker\":{\"provider\":\"gemini\",\"model\":\"stub-model\",\"api_base\":\"http://127.0.0.1:$STUB_PORT\",\"api_key_env\":\"FAKE_KEY_EVIDENCE_TEST\",\"max_tokens\":100,\"max_turns\":20}}}"
LOOSE_SIG=$(presign "$LOOSE_JSON")
if [ -z "$LOOSE_SIG" ]; then
    skip "D-01" "failed to pre-sign loosened govern.json"
    stop_stub
else
export NAAB_LOOSE_JSON="$LOOSE_JSON"
export NAAB_LOOSE_SIG="$LOOSE_SIG"
cat > "$WDIR/govern.json" << GOVEOF
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "telemetry.jsonl",
                 "tamper_evidence": { "enabled": true },
                 "decision_snapshots": true },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true, "level": "advisory", "check_interval_turns": 1 },
  "capabilities": { "shell": { "enabled": true } },
  "agents": {
    "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "FAKE_KEY_EVIDENCE_TEST", "max_tokens": 100, "max_turns": 20 }
  }
}
GOVEOF
sign_govern "$WDIR"
cat > "$WDIR/test.naab" << 'NAABEOF'
use agent
main {
    let h = agent.create("worker")
    let r1 = agent.send(h, "hello")
    // Operator loosens evidence config mid-run (pre-signed out-of-band).
    // sleep first: reload detection is mtime-based with 1s granularity.
    let _ = <<shell
sleep 1.2
printf '%s' "$NAAB_LOOSE_JSON" > govern.json
printf '%s' "$NAAB_LOOSE_SIG" > govern.json.sig
>>
    let r2 = agent.send(h, "and again")
    print("DONE")
}
NAABEOF
OUTPUT=$(cd "$WDIR" && timeout 30s "$NAAB" test.naab 2>"$WDIR/stderr.txt") || true
stop_stub

if grep -q "ratchet violation" "$WDIR/stderr.txt" 2>/dev/null && \
   grep -q "tamper_evidence" "$WDIR/stderr.txt" 2>/dev/null; then
    pass "D-01" "mid-run tamper_evidence disable rejected as ratchet violation"
else
    fail "D-01" "loosened evidence config not rejected" "$(grep -i "reload\|ratchet" "$WDIR/stderr.txt" 2>/dev/null | head -3)"
fi
if grep -q "decision_snapshots" "$WDIR/stderr.txt" 2>/dev/null; then
    pass "D-02" "mid-run decision_snapshots disable reported in violations"
else
    fail "D-02" "decision_snapshots loosening not reported" "$(grep -i "ratchet" "$WDIR/stderr.txt" 2>/dev/null | head -3)"
fi
# The rejected reload must leave the chain intact and verifiable
if (cd "$WDIR" && "$NAAB" --verify-telemetry-chain telemetry.jsonl >/dev/null 2>&1); then
    pass "D-03" "chain remains verifiable after rejected reload"
else
    fail "D-03" "chain broken after rejected reload"
fi
fi
fi
fi

# ============================================================
# Group E: end-of-run health warnings participate in the chain
#
# emitEndOfRunHealthWarnings() writes chained events from its own code path
# rather than through writeAgentTelemetry(). It used to seed prev_hash from
# the in-memory hash and never increment the run's chained-event counter, so
# an untouched file failed verification with "RunEnd declares N but M
# observed" — the verifier reporting tampering that had not happened.
# ============================================================
echo -e "${CYAN}--- Group E: health-warning chain participation ---${NC}"

WDIR="$TEST_TMP/group_e"; mkdir -p "$WDIR"
# CDD and BSD enabled with no agent activity ⇒ both inert-instrumentation
# warnings fire at end of run, which is what puts events on this path.
cat > "$WDIR/govern.json" << 'GOVEOF'
{
  "version": "5.0",
  "mode": "monitor",
  "telemetry": {
    "enabled": true,
    "output_file": "tele.jsonl",
    "tamper_evidence": { "enabled": true, "algorithm": "sha256" }
  },
  "governance_health": { "enabled": true },
  "behavioral_sequences": { "enabled": true },
  "context_drift": { "enabled": true }
}
GOVEOF
sign_govern "$WDIR"
cat > "$WDIR/t.naab" << 'NAABEOF'
main {
    print("hello")
}
NAABEOF

for _ in 1 2 3; do
    (cd "$WDIR" && timeout 30s "$NAAB" t.naab >/dev/null 2>&1)
done

# Control: without warnings actually firing, E-02..E-04 would be vacuous.
WARN_N=$(grep -c "GOVERNANCE_HEALTH_WARNING" "$WDIR/tele.jsonl" 2>/dev/null || echo 0)
if [ "$WARN_N" -ge 3 ]; then
    pass "E-01" "end-of-run health warnings fired ($WARN_N events on this path)"
else
    fail "E-01" "no end-of-run health warnings — E-02..E-04 would be vacuous" "count=$WARN_N"
fi

VOUT=$(cd "$WDIR" && "$NAAB" --verify-telemetry-chain tele.jsonl 2>&1); VEXIT=$?
if [ "$VEXIT" -eq 0 ] && ! echo "$VOUT" | grep -q "BREAK\|LEGACY RESTART"; then
    pass "E-02" "runs emitting health warnings verify clean on a shared file"
else
    fail "E-02" "health warnings break chain verification" "exit=$VEXIT $VOUT"
fi

COUNT_OK=$(python3 - "$WDIR/tele.jsonl" << 'PYEOF'
import json, sys
from collections import defaultdict
counts = defaultdict(int); declared = {}
for l in open(sys.argv[1]):
    if not l.strip(): continue
    e = json.loads(l)
    if "hash" not in e: continue
    counts[e.get("run_id")] += 1
    if e.get("event_type") == "RunEnd":
        declared[e.get("run_id")] = e.get("chained_events")
bad = [(r, declared.get(r), c) for r, c in counts.items() if declared.get(r) != c]
print("OK" if declared and not bad else "MISMATCH " + repr(bad))
PYEOF
)
if [ "$COUNT_OK" = "OK" ]; then
    pass "E-03" "health warnings counted in RunEnd chained_events"
else
    fail "E-03" "health warnings written without incrementing the run counter" "$COUNT_OK"
fi

# The RunStart anchor is emitted lazily by chainPrevLocked(); a writer that
# bypasses it leaves the anchor stranded behind the events it should anchor.
ORDER_OK=$(python3 - "$WDIR/tele.jsonl" << 'PYEOF'
import json, sys
seen = set(); ok = True
for l in open(sys.argv[1]):
    if not l.strip(): continue
    e = json.loads(l)
    if "hash" not in e: continue
    rid = e.get("run_id")
    if e.get("event_type") == "RunStart":
        if rid in seen: ok = False   # events preceded this run's anchor
    seen.add(rid)
print("OK" if ok else "STRANDED")
PYEOF
)
if [ "$ORDER_OK" = "OK" ]; then
    pass "E-04" "RunStart anchor precedes every chained event of its run"
else
    fail "E-04" "chained events written before the run's RunStart anchor"
fi

# ============================================================
# Summary
# ============================================================
echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}Failures:${NC}$FAILURES"
    exit 1
fi
exit 0
