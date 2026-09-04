#!/usr/bin/env bash
# ============================================================
# test_bsd_preexec_evidence.sh — the pre-execution BSD path left no evidence
#
# There are two enforce sites for `behavioral_sequences.<name>`:
#
#   checkBehavioralSequence  writes BSD_MATCH, consumes risk budget
#   checkPreExecution        did NEITHER
#
# The BSD_MATCH write carries a comment saying it exists so ADVISORY matches
# stay visible -- enforce() returns "" at ADVISORY, so without the event a match
# is invisible. Its sibling defeated that for most real traffic: checkPreExecution
# handles agent.send, file.read/write, http.*, crypto encode, process.* and
# env.get/list, so it is the path most matches actually take. An 8-turn run whose
# pattern fired on EVERY turn produced one stderr line and ZERO BSD_MATCH events.
#
#   B9-01  the pre-execution path emits BSD_MATCH, labelled path=pre_execution
#   B9-02  the completed path still emits, labelled path=completed — without this
#          B9-01 passes just as well for a label slapped on every event
#   B9-03  agent.send's pre-check detail carries no __nonce material
#
# B9-03 is a security fix that had to land WITH the evidence fix, not after.
# agent.send's first argument is the HANDLE DICT, which contains __nonce -- the
# HMAC tying the handle to server-side state. It was already interpolated into
# the block message and so into stderr; emitting BSD_MATCH would have carried it
# into telemetry as well, so adding evidence without this would have widened a
# leak rather than closed one.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/bsd-preexec-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"
"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  BSD pre-execution enforcement left no evidence               |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

bsd_field() {  # $1=telemetry  $2=field -> distinct values, space separated
    python3 - "$1" "$2" <<'PYEOF'
import json, sys
vals=[]
for line in open(sys.argv[1]):
    if '"BSD_MATCH"' not in line: continue
    try: e=json.loads(line)
    except Exception: continue
    d=e.get("fields",e)
    if d.get("event_type")!="BSD_MATCH": continue
    v=d.get(sys.argv[2])
    if v is not None and v not in vals: vals.append(v)
print(" ".join(vals))
PYEOF
}

# ── B9-01: the pre-execution path. credential_harvesting is ENV_READ -> NET_CONNECT
# and http.* is pre-checked, so the completing step goes through checkPreExecution.
W="$TEST_TMP/pre"; mkdir -p "$W"
cat > "$W/govern.json" <<'GEOF'
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true } }
GEOF
(cd "$W" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
cat > "$W/cred.naab" <<'GEOF'
use env
use http
main { let k = env.get("MY_API_KEY") ?? "x" let r = http.get("http://127.0.0.1:9/x") print("DONE") }
GEOF
OUT_PRE=$(cd "$W" && timeout 60s "$NAAB" cred.naab 2>&1) || true
if echo "$OUT_PRE" | grep -q "INTEGRITY BLOCK"; then
    skip "B9-01" "config integrity blocked — arm cannot run"
    skip "B9-02" "config integrity blocked"
elif [ ! -f "$W/tele.jsonl" ]; then
    fail "B9-01" "no telemetry produced" "$(echo "$OUT_PRE" | tail -2)"
else
    PATHS="$(bsd_field "$W/tele.jsonl" path)"
    NAMES="$(bsd_field "$W/tele.jsonl" pattern_name)"
    if [ -z "$NAMES" ]; then
        fail "B9-01" "no BSD_MATCH at all — the pre-execution path is still silent" \
             "fired=[$(echo "$OUT_PRE" | grep -oE 'behavioral_sequences\.[a-z_]+' | sort -u | tr '\n' ' ')]"
    elif echo "$PATHS" | grep -q pre_execution; then
        pass "B9-01" "pre-execution match is recorded (pattern=$NAMES path=$PATHS)"
    else
        fail "B9-01" "BSD_MATCH exists but not from the pre-execution path" "paths=[$PATHS]"
    fi
fi

# ── B9-02: control — the completed path must still be labelled, and labelled
# DIFFERENTLY. Without this, B9-01 passes for a build that stamps
# path=pre_execution on every event including the ones that are not.
# The pre-check covers agent.send, file.read/write, http.*, crypto ENCODE,
# process.* and env.get/list. crypto.base64_DECODE emits a DECODE event and is
# NOT pre-checked, so a pattern ending there reaches checkBehavioralSequence via
# the VM's normal emit -- the completed path.
W2="$TEST_TMP/done"; mkdir -p "$W2"
cat > "$W2/govern.json" <<'GEOF'
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl" },
  "behavioral_sequences": { "enabled": true, "default_pattern_enforcement": "observe",
    "patterns": [ { "name": "decode_after_env", "level": "advisory", "max_gap": 5,
      "sequence": [ "env.get", "decode" ] } ] } }
GEOF
(cd "$W2" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true
cat > "$W2/dec.naab" <<'GEOF'
use env
use crypto
main { let k = env.get("MY_API_KEY") ?? "eA==" let d = crypto.base64_decode("eA==") print("DONE") }
GEOF
OUT_DONE=$(cd "$W2" && timeout 60s "$NAAB" dec.naab 2>&1) || true
if [ -f "$W2/tele.jsonl" ]; then
    P2="$(bsd_field "$W2/tele.jsonl" path)"
    if [ -z "$P2" ]; then
        fail "B9-02" "the completed-path fixture produced no BSD_MATCH" \
             "without a working completed arm, B9-01's label is unverified: out=$(echo "$OUT_DONE" | tail -1)"
    elif [ "$P2" = "completed" ]; then
        pass "B9-02" "control: the completed path is labelled distinctly (path=$P2)"
    else
        fail "B9-02" "completed path carries the wrong label" "paths=[$P2]"
    fi
else
    fail "B9-02" "no telemetry from the completed-path fixture"
fi

# ── B9-03: no nonce material anywhere in the evidence.
echo -e "${CYAN}--- B9-03: agent.send pre-check detail carries no nonce ---${NC}"
if grep -rq "__nonce" "$W/tele.jsonl" 2>/dev/null || echo "$OUT_PRE" | grep -q "__nonce"; then
    fail "B9-03" "nonce material reached the evidence" "handle __nonce must never enter telemetry or stderr"
else
    # Positive control: the guard is only meaningful if the detail interpolation
    # it constrains is actually live. cred.naab's env.get detail DOES carry its
    # argument, so an empty detail everywhere would make this vacuous.
    if grep -q "MY_API_KEY" "$W/tele.jsonl" 2>/dev/null; then
        pass "B9-03" "details are populated, and no __nonce appears in them"
    else
        fail "B9-03" "no argument detail present at all — the absence check is vacuous" \
             "expected MY_API_KEY in the recorded events"
    fi
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "  Passed: ${GREEN}$PASS_COUNT${NC}  Failed: ${RED}$FAIL_COUNT${NC}  Skipped: ${YELLOW}$SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then echo -e "${RED}FAILURES:${NC}$FAILURES"; exit 1; fi
echo -e "${GREEN}ALL PASSED${NC}"
exit 0
