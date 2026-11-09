#!/usr/bin/env bash
# test_audit_verify_engine.sh -- naab-verify-audit verifies the audit trail the
# engine writes (F-27; Bundle 17)
#
# GovernanceEngine::logAuditEvent() writes audit.output_file as JSON lines
# chained prev_hash -> hash (hash = SHA-256 of the entry without "hash", or
# HMAC when audit.tamper_evidence has a key). naab-verify-audit was written
# (f666e6e5, February) for TamperEvidenceLogger's format -- "sequence" numbers,
# a 64-zero genesis -- before the engine's writer existed (5d08322c, April), and
# the two were never reconciled: observed on master 314bccfd, an untouched
# engine audit log read "Status: TAMPERED", exit 2, every entry flagged. A
# verifier that flags everything cannot tell tampering from none. It now
# recognises the engine's format and checks each hash and each link.
#
#   AV-00   CONTROL the engine writes a chained audit trail (3 entries)
#   AV-01   the untouched trail verifies: VALID, exit 0
#   AV-02   an entry's message edited: TAMPERED, exit 2, the line named
#   AV-03   an interior entry deleted: TAMPERED, exit 2 (the link breaks)
#   AV-04   two entries swapped: TAMPERED, exit 2
#   AV-05   a trail with tamper evidence off: NOT VERIFIABLE, exit 3 -- never VALID
#
# Kill table (each mutant applied alone, rebuilt, this suite run; observed
# 2026-11-09):
#   the engine format not recognised (master's tool)    AV-01, AV-02, AV-03, AV-05
#   the hash not recomputed                             AV-02
#   the prev_hash link not checked                      AV-03, AV-04
# Red first (observed, master 314bccfd's naab-verify-audit): AV-01, AV-02,
# AV-03 and AV-05 fail; AV-04 passes there only because master flags
# everything (it names no line and no link, so AV-02/03 fail on wording).
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
VERIFY="${NAAB_VERIFY_AUDIT:-$(dirname "$NAAB")/naab-verify-audit}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_auditv_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       -> /'; }

if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then echo "UNMEASURABLE: naab-lang not built"; exit 1; fi
if [ ! -x "$VERIFY" ] && [ ! -x "$VERIFY.exe" ]; then echo "UNMEASURABLE: naab-verify-audit not built"; exit 1; fi

PROG='main {
    let a = <<javascript
1 + 1
>>
    let b = <<javascript
2 + 2
>>
    let c = <<javascript
3 + 3
>>
    print("RAN=" + string(a + b + c))
}'
trail() {   # trail DIR TAMPER(true|false)
    mkdir -p "$1"
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "audit": {"level": "detailed", "output_file": "audit.jsonl", "tamper_evidence": {"enabled": %s},
           "log_events": {"polyglot_executed": true}}}\n' "$2" > "$1/govern.json"
    printf '%s\n' "$PROG" > "$1/p.naab"
    (cd "$1" && timeout 60s "$NAAB" p.naab >/dev/null 2>&1)
}
verify_rc() { RC=0; OUT=$(timeout 60s "$VERIFY" "$1" 2>&1) || RC=$?; OUT=$(sed 's/\x1b\[[0-9;]*m//g' <<<"$OUT"); }

echo "=== naab-verify-audit verifies the engine's audit trail ==="

trail "$W/t" true
n=$(grep -c . "$W/t/audit.jsonl" 2>/dev/null || echo 0)
if [ "$n" -ge 3 ] && grep -q '"prev_hash"' "$W/t/audit.jsonl"; then pass AV-00 "CONTROL: the engine wrote a chained trail ($n entries)"
else fail AV-00 "the engine wrote no chained trail ($n entries)" "$(cat "$W/t/audit.jsonl" 2>/dev/null)"
     echo ""; echo "PASS=$PASS FAIL=$FAIL"; exit 1; fi

verify_rc "$W/t/audit.jsonl"
if [ "$RC" -eq 0 ] && grep -q "Status: *VALID" <<<"$OUT"; then pass AV-01 "the untouched trail verifies (exit 0)"
else fail AV-01 "an untouched engine trail did not verify (rc=$RC)" "$OUT"; fi

cp "$W/t/audit.jsonl" "$W/edited.jsonl"
python3 - "$W/edited.jsonl" <<'EOF'
import sys
p = sys.argv[1]; ls = open(p).read().splitlines()
ls[1] = ls[1].replace('"message":"', '"message":"X', 1)
open(p, "w").write("\n".join(ls) + "\n")
EOF
verify_rc "$W/edited.jsonl"
if [ "$RC" -eq 2 ] && grep -q "line 2: its hash does not match" <<<"$OUT"; then pass AV-02 "an edited entry is caught, and its line named"
else fail AV-02 "an edited entry was not caught (rc=$RC)" "$OUT"; fi

sed '2d' "$W/t/audit.jsonl" > "$W/deleted.jsonl"
verify_rc "$W/deleted.jsonl"
if [ "$RC" -eq 2 ] && grep -q "prev_hash does not link" <<<"$OUT"; then pass AV-03 "a deleted interior entry is caught"
else fail AV-03 "a deleted entry was not caught (rc=$RC)" "$OUT"; fi

python3 - "$W/t/audit.jsonl" "$W/swapped.jsonl" <<'EOF'
import sys
ls = open(sys.argv[1]).read().splitlines()
ls[1], ls[2] = ls[2], ls[1]
open(sys.argv[2], "w").write("\n".join(ls) + "\n")
EOF
verify_rc "$W/swapped.jsonl"
if [ "$RC" -eq 2 ] && grep -q "TAMPERED" <<<"$OUT"; then pass AV-04 "swapped entries are caught"
else fail AV-04 "swapped entries were not caught (rc=$RC)" "$OUT"; fi

trail "$W/off" false
verify_rc "$W/off/audit.jsonl"
if [ "$RC" -eq 3 ] && grep -q "NOT VERIFIABLE" <<<"$OUT" && ! grep -q "Status: *VALID" <<<"$OUT"; then
    pass AV-05 "a trail with no hash chain is not verifiable, never VALID (exit 3)"
else fail AV-05 "a chainless trail was not reported as unverifiable (rc=$RC)" "$OUT"; fi

echo ""
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
