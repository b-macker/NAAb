#!/usr/bin/env bash
# ============================================================
# test_stub_routing.sh — per-agent response queues in agent_stub.py
#
# WHY
#
# agent_stub.py had ONE global counter, so every agent in a scenario drew from
# one interleaved queue. A scenario cannot then script a failure sequence for
# one agent while another stays clean — which is exactly what living-script_v3
# needs in order to produce drift on a single handle and keep its siblings
# quiet. Routing gives each matched agent its own counter.
#
# EVERY GATE HAS A DEMONSTRATED FAILURE CASE
#
# Five degradations were applied to agent_stub.py one at a time and the suite
# re-run against each. Recorded here rather than in a commit message because
# the next person to touch this needs it, and a commit message is not where
# they will look:
#
#   D1  route matching disabled (`if False`)      -> SR-02 SR-03 SR-05 SR-06
#   D2  routed requests also bump global_idx      -> SR-02 SR-03
#   D3  a routes key shifts the global queue      -> SR-02 SR-03 SR-04
#   D4  last matching key wins, not first         -> SR-05
#   D5  wrap-around instead of last-repeats       -> SR-01 SR-02 SR-04
#
# Each gate is isolated by at least one row (SR-01/SR-02 by D5, SR-04 by D3,
# SR-05 by D4), so none of them is riding on a neighbour.
#
# SR-04 is worth stating plainly because the obvious version of it is
# worthless: comparing a routed fixture's output against a SEPARATE routeless
# fixture's output proves nothing, since two different fixtures produce
# different output whatever the stub does. What can actually go wrong is
# subtler — the mere PRESENCE of a "routes" key perturbing the unrouted path.
# SR-04 therefore runs a fixture that HAS a route table whose keys match
# nothing in any request body, and requires byte-identical output to the
# routeless baseline. That is the claim every other stub-backed suite depends
# on, and D3 shows it is a real claim rather than a restatement of SR-01.
#
# SR-03 is the other easily-missed half: the global index must NOT advance on
# routed requests, so the one unrouted request still draws G1. A stub that
# routed correctly but also consumed the global queue would pass SR-02 (D2).
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../helpers/gatelib.sh"
source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/stub-routing-$$"
trap 'stop_stub; rm -rf "$TEST_TMP"' EXIT
mkdir -p "$TEST_TMP"

echo "=== agent_stub.py per-agent routing ==="

gate_init "stub-routing"
gate_def SR-01 COMPAT   "routeless fixture keeps global ordering and last-repeats"
gate_def SR-02 ROUTING  "each route consumes its own queue"
gate_def SR-03 ROUTING  "routed requests do not advance the global index"
gate_def SR-04 CONTROL  "a route table matching nothing is byte-identical to no route table"
gate_def SR-05 ROUTING  "first matching key in fixture order wins"
gate_def SR-06 EVIDENCE "routes.log records one decision per request"

# ------------------------------------------------------------------
# A minimal client: POST a body, print the model's text on one line.
# ------------------------------------------------------------------
cat > "$TEST_TMP/post.py" << 'PYEOF'
import json, sys, urllib.request
port = sys.argv[1]
out = []
for body in sys.argv[2:]:
    req = urllib.request.Request("http://127.0.0.1:%s/v1" % port,
                                 data=body.encode("utf-8"),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        d = json.load(r)
    parts = d["candidates"][0]["content"]["parts"]
    out.append(parts[0].get("text", "") or "-")
print(" ".join(out))
PYEOF

# The bodies. Tokens stand in for what the v3 harness plants in each agent's
# system_prompt; the seventh carries neither.
BODY_A='{"contents":[{"parts":[{"text":"TOKEN_ALPHA work"}]}]}'
BODY_B='{"contents":[{"parts":[{"text":"TOKEN_BRAVO work"}]}]}'
BODY_N='{"contents":[{"parts":[{"text":"no token here"}]}]}'
SEQ=("$BODY_A" "$BODY_B" "$BODY_A" "$BODY_B" "$BODY_A" "$BODY_B" "$BODY_N")

# ------------------------------------------------------------------
# Fixtures
# ------------------------------------------------------------------
cat > "$TEST_TMP/routed.json" << 'EOF'
{
  "routes": {
    "TOKEN_ALPHA": {"responses": [{"content": "A1"}, {"content": "A2"}, {"content": "A3"}]},
    "TOKEN_BRAVO": {"responses": [{"content": "B1"}, {"content": "B2"}]}
  },
  "responses": [{"content": "G1"}, {"content": "G2"}]
}
EOF

cat > "$TEST_TMP/flat.json" << 'EOF'
{"responses": [{"content": "G1"}, {"content": "G2"}, {"content": "G3"}, {"content": "G4"}]}
EOF

# Same global queue as flat.json, plus a route table whose keys appear in no
# request body. The 30 suites that never heard of routing depend on this being
# indistinguishable from flat.json.
cat > "$TEST_TMP/unmatched.json" << 'EOF'
{
  "routes": {
    "TOKEN_ZULU":   {"responses": [{"content": "Z1"}]},
    "TOKEN_YANKEE": {"responses": [{"content": "Y1"}]}
  },
  "responses": [{"content": "G1"}, {"content": "G2"}, {"content": "G3"}, {"content": "G4"}]
}
EOF

# Both tokens in one route table, ALPHA declared first; the request carries
# BRAVO's token before ALPHA's, so a body-order match would pick BRAVO.
cat > "$TEST_TMP/ambiguous.json" << 'EOF'
{
  "routes": {
    "TOKEN_ALPHA": {"responses": [{"content": "FIRST"}]},
    "TOKEN_BRAVO": {"responses": [{"content": "SECOND"}]}
  }
}
EOF

run_seq() {  # $1=fixture  $2=state subdir ; echoes the response line
    local fx="$1" dir="$TEST_TMP/$2"
    mkdir -p "$dir"
    start_stub "$fx" "$dir" || return 1
    python3 "$TEST_TMP/post.py" "$STUB_PORT" "${SEQ[@]}" 2>"$dir/client.err"
    local rc=$?
    stop_stub
    return $rc
}

# ------------------------------------------------------------------
# SR-01 / SR-04 — the routeless baseline, which is also the control
# ------------------------------------------------------------------
FLAT_OUT="$(run_seq "$TEST_TMP/flat.json" flat)"
FLAT_RC=$?
FLAT_EXPECT="G1 G2 G3 G4 G4 G4 G4"

if [ $FLAT_RC -ne 0 ]; then
    skip SR-01 "routeless fixture keeps global ordering" "stub or client failed"
else
    if [ "$FLAT_OUT" = "$FLAT_EXPECT" ]; then
        pass SR-01 "routeless fixture keeps global ordering and last-repeats"
    else
        fail SR-01 "routeless fixture changed behaviour" \
             "expected '$FLAT_EXPECT', got '$FLAT_OUT'"
    fi
fi

# SR-04 — an unmatched route table must not perturb the unrouted path.
UNM_OUT="$(run_seq "$TEST_TMP/unmatched.json" unmatched)"
UNM_RC=$?
if [ $FLAT_RC -ne 0 ] || [ $UNM_RC -ne 0 ]; then
    skip SR-04 "unmatched route table is inert" "stub or client failed"
elif [ "$UNM_OUT" = "$FLAT_OUT" ] && [ "$FLAT_OUT" = "$FLAT_EXPECT" ]; then
    pass SR-04 "a route table matching nothing is byte-identical to no route table"
else
    fail SR-04 "an unmatched route table changed the unrouted path" \
         "flat='$FLAT_OUT' unmatched='$UNM_OUT' (both should be '$FLAT_EXPECT')"
fi

# ------------------------------------------------------------------
# SR-02 / SR-03 — routing
# ------------------------------------------------------------------
ROUTED_OUT="$(run_seq "$TEST_TMP/routed.json" routed)"
ROUTED_RC=$?
# A: A1 A2 A3 (three sends)   B: B1 B2 B2 (last repeats)   unrouted: G1
ROUTED_EXPECT="A1 B1 A2 B2 A3 B2 G1"

if [ $ROUTED_RC -ne 0 ]; then
    skip SR-02 "each route consumes its own queue" "stub or client failed"
    skip SR-03 "routed requests do not advance the global index" "stub or client failed"
else
    if [ "$ROUTED_OUT" = "$ROUTED_EXPECT" ]; then
        pass SR-02 "each route consumes its own queue"
    else
        fail SR-02 "route queues interleaved or mis-ordered" \
             "expected '$ROUTED_EXPECT', got '$ROUTED_OUT'"
    fi

    # The seventh request is the only unrouted one. G1 means the global index
    # sat still through six routed requests; G2 or a default means it did not.
    LAST="${ROUTED_OUT##* }"
    if [ "$LAST" = "G1" ]; then
        pass SR-03 "routed requests do not advance the global index"
    else
        fail SR-03 "global index advanced on routed requests" \
             "unrouted request got '$LAST', expected the first global response G1"
    fi
fi

# ------------------------------------------------------------------
# SR-05 — determinism when a body carries two tokens
# ------------------------------------------------------------------
AMB_DIR="$TEST_TMP/amb"; mkdir -p "$AMB_DIR"
if start_stub "$TEST_TMP/ambiguous.json" "$AMB_DIR"; then
    BOTH='{"contents":[{"parts":[{"text":"TOKEN_BRAVO then TOKEN_ALPHA"}]}]}'
    AMB_OUT="$(python3 "$TEST_TMP/post.py" "$STUB_PORT" "$BOTH" 2>/dev/null)"
    stop_stub
    if [ "$AMB_OUT" = "FIRST" ]; then
        pass SR-05 "first matching key in fixture order wins"
    else
        fail SR-05 "ambiguous body did not resolve to the first declared route" \
             "expected 'FIRST', got '$AMB_OUT'"
    fi
else
    skip SR-05 "first matching key wins" "stub failed to start"
fi

# ------------------------------------------------------------------
# SR-06 — the routing decision is recorded, because mis-routing is silent
# ------------------------------------------------------------------
RLOG="$TEST_TMP/routed/routes.log"
if [ $ROUTED_RC -ne 0 ]; then
    skip SR-06 "routes.log records one decision per request" "routed run failed"
elif [ ! -f "$RLOG" ]; then
    fail SR-06 "routes.log was not written" "expected $RLOG"
else
    LINES=$(wc -l < "$RLOG" | tr -d ' ')
    NALPHA=$(count_matches 'TOKEN_ALPHA' "$RLOG")
    NBRAVO=$(count_matches 'TOKEN_BRAVO' "$RLOG")
    NNONE=$(count_matches ' -$' "$RLOG")
    if [ "$LINES" -eq 7 ] && [ "$NALPHA" -eq 3 ] && [ "$NBRAVO" -eq 3 ] && [ "$NNONE" -eq 1 ]; then
        pass SR-06 "routes.log records one decision per request"
    else
        fail SR-06 "routes.log does not account for every request" \
             "lines=$LINES alpha=$NALPHA bravo=$NBRAVO unrouted=$NNONE (want 7/3/3/1)"
    fi
fi

gate_print_summary
gate_exit
