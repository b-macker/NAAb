#!/usr/bin/env bash
# ============================================================
# test_nonformation_proof.sh — Non-formation and its proof
#
# Answers one question end-to-end: can an inadmissible action be
# prevented from becoming operationally real, and can that prevention
# be proven afterward from preserved evidence?
#
# "Operationally real" is measured as an observable side effect on the
# filesystem, not as an exit code — a blocked run and a run that never
# started produce the same exit code, so absence alone proves nothing.
# Group A is the control that gives the absence meaning.
#
# Group A: control — the same action, admitted, DOES form its side effect
# Group B: blocked language — HARD block, side effect never forms
# Group C: proof of non-formation — signed RefusalAttestation
# Group D: proof is tamper-evident — mutation and deletion both detected
# Group E: capability gate (shell) — second boundary, same guarantee
#
# No API keys required.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/nonformation-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; TOTAL=0; FAILURES=""

pass() { PASS_COUNT=$((PASS_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); TOTAL=$((TOTAL + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/test-key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/test-key.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$TEST_TMP/test-key.pem"
SIGNING_KEY="$TEST_TMP/test-key.pem"

# Signs with the in-directory key copy for the same path-portability reason
# as write_govern below.
sign_govern() { (cd "$1" && NAAB_SIGNING_KEY="signing.pem" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

# jq is optional — fall back to grep on the raw JSONL when absent.
HAVE_JQ=false
command -v jq >/dev/null 2>&1 && HAVE_JQ=true

# The attestation record for the run's first refusal.
refusal_event() {  # $1 = telemetry file
    if [ "$HAVE_JQ" = true ]; then
        jq -c 'select(.event_type == "RefusalAttestation")' "$1" 2>/dev/null | head -1
    else
        grep '"event_type":"RefusalAttestation"' "$1" 2>/dev/null | head -1
    fi
}

# Emits a govern.json that blocks nothing except what $2 says, with
# attestation recording and the telemetry hash chain both on.
#
# Paths inside govern.json are RELATIVE and every run below cds into the
# test directory first. The binary is native on Windows while the harness
# runs under MSYS2, so an absolute POSIX path written into the config is
# not a path the binary can open — it silently fails to write telemetry,
# and the attestation disappears while the block itself still works.
write_govern() {  # $1=dir  $2=policy-fragment
    cp "$SIGNING_KEY" "$1/signing.pem"
    cat > "$1/govern.json" << EOF
{
    "mode": "enforce",
    "security": { "sandbox_level": "elevated" },
    $2
    "telemetry": {
        "enabled": true,
        "output_file": "telemetry.jsonl",
        "tamper_evidence": { "enabled": true, "algorithm": "sha256" }
    },
    "audit": {
        "provenance": {
            "enabled": true,
            "record_attestations": true,
            "sign_records": true,
            "signing_key": "signing.pem"
        }
    }
}
EOF
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|   Non-formation and its proof (inadmissible action gating)   |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"

# ============================================================
# Group A: control — the action, when admitted, forms its side effect
# ============================================================
echo ""
echo -e "${CYAN}--- Group A: control (admitted action DOES form) ---${NC}"

ADIR="$TEST_TMP/a"; mkdir -p "$ADIR"
write_govern "$ADIR" '"languages": { "allowed": ["python"] },'
sign_govern "$ADIR"
cat > "$ADIR/act.naab" << 'NAABEOF'
main {
    let r = <<python
open("side_effect.txt", "w").write("the action became real")
result = 1
>>
    print("COMPLETED")
}
NAABEOF

(cd "$ADIR" && timeout 60s "$NAAB" act.naab > out.txt 2>&1); RC_A=$?

if [ -f "$ADIR/side_effect.txt" ]; then
    pass "A-01" "control: admitted polyglot block produced its side effect"
    CONTROL_OK=true
else
    CONTROL_OK=false
    if grep -qi "not available\|executor disabled\|No such file" "$ADIR/out.txt" 2>/dev/null; then
        skip "A-01" "python executor unavailable — control cannot run"
    else
        fail "A-01" "control did not form a side effect (exit $RC_A)" "$(tail -3 "$ADIR/out.txt" 2>/dev/null)"
    fi
fi

# ============================================================
# Group B: the same action, made inadmissible, never becomes real
# ============================================================
echo ""
echo -e "${CYAN}--- Group B: blocked language (non-formation) ---${NC}"

BDIR="$TEST_TMP/b"; mkdir -p "$BDIR"
TELE_B="$BDIR/telemetry.jsonl"
write_govern "$BDIR" '"languages": { "blocked": ["python"] },'
sign_govern "$BDIR"
cp "$ADIR/act.naab" "$BDIR/act.naab"

(cd "$BDIR" && timeout 60s "$NAAB" act.naab > out.txt 2>&1); RC_B=$?

if [ "$RC_B" -eq 3 ]; then
    pass "B-01" "HARD governance block (exit 3)"
else
    fail "B-01" "expected exit 3, got $RC_B" "$(tail -3 "$BDIR/out.txt" 2>/dev/null)"
fi

if [ ! -f "$BDIR/side_effect.txt" ]; then
    if [ "$CONTROL_OK" = true ]; then
        pass "B-02" "side effect never formed (control proves it otherwise would)"
    else
        skip "B-02" "side effect absent, but control did not run — absence unproven"
    fi
else
    fail "B-02" "side effect file exists — the blocked action became real"
fi

if ! grep -q "COMPLETED" "$BDIR/out.txt" 2>/dev/null; then
    pass "B-03" "execution did not continue past the boundary"
else
    fail "B-03" "program ran to completion despite the HARD block"
fi

# ============================================================
# Group C: proof of non-formation
# ============================================================
echo ""
echo -e "${CYAN}--- Group C: signed proof the block happened ---${NC}"

ATT="$(refusal_event "$TELE_B")"

if [ -n "$ATT" ]; then
    pass "C-01" "RefusalAttestation written to telemetry"
else
    fail "C-01" "no RefusalAttestation in telemetry" "$(wc -l < "$TELE_B" 2>/dev/null) telemetry line(s)"
fi

if echo "$ATT" | grep -q '"execution_prevented":true'; then
    pass "C-02" "attestation asserts execution_prevented=true"
else
    fail "C-02" "execution_prevented not asserted" "${ATT:0:200}"
fi

if echo "$ATT" | grep -q '"rule_name":"languages.blocked"'; then
    pass "C-03" "attestation names the rule that refused"
else
    fail "C-03" "rule_name missing or wrong" "${ATT:0:200}"
fi

if echo "$ATT" | grep -q '"binding_status":"non-binding"'; then
    pass "C-04" "attestation declares itself non-binding evidence"
else
    fail "C-04" "binding_status missing" "${ATT:0:200}"
fi

if echo "$ATT" | grep -q '"signature":"' && echo "$ATT" | grep -q '"key_fingerprint":"'; then
    pass "C-05" "attestation is Ed25519-signed with a key fingerprint"
else
    fail "C-05" "attestation unsigned" "${ATT:0:200}"
fi

# ============================================================
# Group D: the proof cannot be quietly rewritten or removed
# ============================================================
echo ""
echo -e "${CYAN}--- Group D: proof is tamper-evident ---${NC}"

if (cd "$BDIR" && "$NAAB" --verify-telemetry-chain telemetry.jsonl) > "$TEST_TMP/verify_clean.txt" 2>&1; then
    pass "D-01" "hash chain over the refusal evidence verifies clean"
else
    fail "D-01" "chain verification failed on untouched evidence" "$(tail -3 "$TEST_TMP/verify_clean.txt")"
fi

# Mutation: flip the very claim the attestation makes.
MUT="$BDIR/tele_mutated.jsonl"
sed 's/"execution_prevented":true/"execution_prevented":false/' "$TELE_B" > "$MUT"
if ! cmp -s "$TELE_B" "$MUT"; then
    if (cd "$BDIR" && "$NAAB" --verify-telemetry-chain tele_mutated.jsonl) > "$TEST_TMP/verify_mut.txt" 2>&1; then
        fail "D-02" "mutated attestation still verified clean"
    else
        if grep -q "TAMPER" "$TEST_TMP/verify_mut.txt"; then
            pass "D-02" "flipping execution_prevented is detected as TAMPER"
        else
            pass "D-02" "mutated attestation rejected by the verifier"
        fi
    fi
else
    skip "D-02" "no execution_prevented field to mutate"
fi

# Deletion: remove the attestation line entirely.
DEL="$BDIR/tele_deleted.jsonl"
grep -v '"event_type":"RefusalAttestation"' "$TELE_B" > "$DEL" 2>/dev/null
if [ -s "$DEL" ] && ! cmp -s "$TELE_B" "$DEL"; then
    if (cd "$BDIR" && "$NAAB" --verify-telemetry-chain tele_deleted.jsonl) > "$TEST_TMP/verify_del.txt" 2>&1; then
        fail "D-03" "deleting the attestation left a clean-verifying chain"
    else
        pass "D-03" "deleting the attestation breaks the chain"
    fi
else
    skip "D-03" "attestation is the only chained event — deletion not separable"
fi

# ============================================================
# Group E: the guarantee is not specific to one boundary
# ============================================================
echo ""
echo -e "${CYAN}--- Group E: capability gate (shell) ---${NC}"

EDIR="$TEST_TMP/e"; mkdir -p "$EDIR"
TELE_E="$EDIR/telemetry.jsonl"
write_govern "$EDIR" '"capabilities": { "shell": { "enabled": false } },'
sign_govern "$EDIR"
cat > "$EDIR/act.naab" << 'NAABEOF'
main {
    let r = <<shell
touch shell_side_effect.txt
>>
    print("COMPLETED")
}
NAABEOF

(cd "$EDIR" && timeout 60s "$NAAB" act.naab > out.txt 2>&1); RC_E=$?

if [ "$RC_E" -eq 3 ]; then
    pass "E-01" "shell capability gate blocks HARD (exit 3)"
else
    fail "E-01" "expected exit 3, got $RC_E" "$(tail -3 "$EDIR/out.txt" 2>/dev/null)"
fi

if [ ! -f "$EDIR/shell_side_effect.txt" ]; then
    pass "E-02" "shell side effect never formed"
else
    fail "E-02" "shell block ran despite capabilities.shell=false"
fi

ATT_E="$(refusal_event "$TELE_E")"
if echo "$ATT_E" | grep -q '"execution_prevented":true'; then
    pass "E-03" "capability refusal is attested the same way"
else
    fail "E-03" "no attestation for the capability refusal" "${ATT_E:0:200}"
fi

# ============================================================
echo ""
echo -e "${CYAN}==============================================${NC}"
echo -e "  Total: $TOTAL  ${GREEN}Pass: $PASS_COUNT${NC}  ${RED}Fail: $FAIL_COUNT${NC}  ${YELLOW}Skip: $SKIP_COUNT${NC}"
[ -n "$FAILURES" ] && echo -e "${RED}Failures:$FAILURES${NC}"
echo -e "${CYAN}==============================================${NC}"
[ $FAIL_COUNT -eq 0 ]
