#!/usr/bin/env bash
# ============================================================
# test_run_identity.sh — RunStart carries config_fingerprint + mandate_digest
#
# Telemetry could not say which config produced a run. report.py reads
# src/govern.json, which need not be the file that ran, and the prose-arm
# verification had to be settled by asking a human which arm had executed.
#
# WHAT IS HASHED, AND WHY NOT THE OBVIOUS THING
#
# The obvious implementation hashes the resolved GovernanceRules. That would be
# WRONG: GovernanceRules holds ~50 unordered containers and has no canonical
# serializer, so the same config could hash differently between two runs — and
# RI-02 below, the test that gives the fingerprint its meaning, would flake
# instead of failing honestly. File CONTENT is hashed instead, which is also
# path-independent (RI-02 runs the same config from a different directory).
#
# Digests only, never the prompts: system prompts are operator content and
# telemetry is forwarded to webhooks and SIEMs.
#
# BOTH DIRECTIONS ARE REQUIRED
#   RI-01  different configs -> different fingerprints. Alone, a random value passes.
#   RI-02  same config twice -> identical fingerprints. Alone, a constant passes.
# Neither test is meaningful without the other.
#
# RI-03 is what proves the two fields are not redundant: a config that changes a
# NON-agent setting must move config_fingerprint while leaving mandate_digest
# alone. Without it, mandate_digest could just be a second copy of the config
# hash and every other assertion would still pass.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/runid-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

if [ ! -x "$NAAB" ]; then
    skip "RI-00" "build/naab-lang not found"
    echo "  Results: 0 passed, 0 failed, 1 skipped"
    exit 0
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Run identity: which config actually produced this run?       |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

# $1 = dir, $2 = system prompt, $3 = max_tokens
mk() {
    mkdir -p "$TEST_TMP/$1"
    cat > "$TEST_TMP/$1/govern.json" <<GOVEOF
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "t.jsonl" },
  "agents": { "w": { "provider": "gemini", "model": "m", "api_key_env": "K",
      "max_tokens": $3, "max_turns": 5, "system_prompt": "$2" } } }
GOVEOF
    echo 'main { print("ok") }' > "$TEST_TMP/$1/t.naab"
    (cd "$TEST_TMP/$1" && "$NAAB" t.naab > out.txt 2>&1)
}

field() {
    python3 -c "
import json,sys
try:
    for l in open('$TEST_TMP/$1/t.jsonl'):
        r=json.loads(l)
        if r.get('event_type')=='RunStart':
            print(r.get('$2') or 'MISSING'); sys.exit()
except Exception: pass
print('NOFILE')
"
}

mk arm_a "Report on the inventory service."  50
mk arm_b "Write a poem about the sea."       50
mk arm_a2 "Report on the inventory service." 50   # identical content, different dir
mk arm_c "Report on the inventory service."  999  # same prompt, different setting

A_CFG=$(field arm_a config_fingerprint);  A_MAN=$(field arm_a mandate_digest)
B_CFG=$(field arm_b config_fingerprint);  B_MAN=$(field arm_b mandate_digest)
A2_CFG=$(field arm_a2 config_fingerprint); A2_MAN=$(field arm_a2 mandate_digest)
C_CFG=$(field arm_c config_fingerprint);  C_MAN=$(field arm_c mandate_digest)

# --- RI-00b: the fields exist at all --------------------------------------
if [ "$A_CFG" = "NOFILE" ] || [ "$A_CFG" = "MISSING" ]; then
    fail "RI-00b" "RunStart carries no config_fingerprint" "got: $A_CFG"
else
    pass "RI-00b" "RunStart carries config_fingerprint ($A_CFG)"
fi

# --- RI-01: different configs differ --------------------------------------
if [ "$A_CFG" != "$B_CFG" ]; then
    pass "RI-01" "different configs → different fingerprints"
else
    fail "RI-01" "two different configs share a fingerprint" "$A_CFG == $B_CFG"
fi

# --- RI-02: same config twice matches (and is path-independent) -----------
if [ "$A_CFG" = "$A2_CFG" ] && [ "$A_CFG" != "NOFILE" ]; then
    pass "RI-02" "same config from another directory → identical fingerprint"
else
    fail "RI-02" "identical config produced different fingerprints" \
         "$A_CFG vs $A2_CFG — non-deterministic or path-dependent"
fi

# --- RI-03: the two fields are not redundant ------------------------------
if [ "$C_CFG" != "$A_CFG" ] && [ "$C_MAN" = "$A_MAN" ]; then
    pass "RI-03" "non-agent change moves config only, mandate unchanged"
elif [ "$C_CFG" = "$A_CFG" ]; then
    fail "RI-03" "config_fingerprint ignored a non-agent setting change" \
         "max_tokens 50 → 999 left the fingerprint at $C_CFG"
else
    fail "RI-03" "mandate_digest moved when no prompt changed" \
         "$A_MAN → $C_MAN; it is tracking the file, not the mandates"
fi

# --- RI-04: mandate digest reacts to a prompt change ----------------------
if [ "$A_MAN" != "$B_MAN" ]; then
    pass "RI-04" "different system prompts → different mandate digests"
else
    fail "RI-04" "mandate_digest identical across different prompts" "$A_MAN"
fi

# --- RI-05: prompts must not appear in telemetry --------------------------
if grep -q "poem about the sea" "$TEST_TMP/arm_b/t.jsonl" 2>/dev/null; then
    fail "RI-05" "system prompt text leaked into telemetry" \
         "digests exist so forwarded telemetry carries no operator content"
else
    pass "RI-05" "prompt text absent from telemetry (digest only)"
fi

echo ""
echo -e "${CYAN}==============================================================${NC}"
echo -e "  Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC}"
echo -e "${CYAN}==============================================================${NC}"
[ "$FAIL_COUNT" -eq 0 ] || { echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; }
exit 0
