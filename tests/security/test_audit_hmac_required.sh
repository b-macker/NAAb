#!/usr/bin/env bash
# ============================================================
# test_audit_hmac_required.sh -- F31: the artefact could switch off its own check
#
# verifyIntegrity() gated the HMAC on `!entry.signature.empty()` and silently
# bailed when the signature carried no ':'. Both are properties of the ARTEFACT
# UNDER AUDIT, so a log could disable its own verification.
#
# WHY THAT IS TOTAL, not partial. toCanonicalString() -- what `hash` covers --
# EXCLUDES the signature, and every other field is keyless. A forger with no key
# can edit `details`, recompute `hash`, repair the next entry's `prev_hash`, and
# every chain check passes. The keyed signature is the only thing binding the log
# to the operator, so deleting it is the whole attack. Before the fix the tool
# printed "The log chain is intact and has not been tampered with" over an entry
# rewritten from "admin granted shell access to agent alpha" to "routine health
# check".
#
# Group A  the forgery must be caught (signature deleted, chain repaired)
# Group B  POSITIVE CONTROL -- the same edit with a signature KEPT must fail too.
#          Without it, Group A passes for a build that rejects everything, and
#          "VALID" in the old build could not be told from a broken fixture.
# Group C  NEGATIVE CONTROLS, both directions of over-fixing:
#            C-01 a clean signed log must still verify (not fail-everything)
#            C-02 chain-only mode (no key) must still work on an unsigned log --
#                 `hmac_key` is OUR config and stays a legitimate guard; only the
#                 artefact's own fields were removed from the condition
#          C-03 supplying a key against an unsigned log must FAIL: asking for
#               HMAC verification of something unsigned cannot return "verified".
#
# Fixtures are built here rather than committed: the canonical string and the
# JSON shape are the thing under test, so a stale committed fixture would drift
# away from the code and start passing for the wrong reason.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
VERIFY="${VERIFY:-$REPO/build/naab-verify-audit}"
PASS=0; FAIL=0
ok()  { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); }

# Instrument usability: the tool did not COMPILE on master (no <filesystem>
# include, and no CI workflow builds this target). "no binary" must report
# UNMEASURABLE, never a pass -- an absent verifier would otherwise look like
# a clean run.
if [ ! -x "$VERIFY" ]; then
    echo "  FAIL [AH-00] naab-verify-audit not built at $VERIFY (UNMEASURABLE, not a pass)"
    echo "         build it with: cmake --build build --target naab-verify-audit"
    exit 1
fi

W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
cd "$W"
KEY="test-hmac-secret-key"

python3 - "$KEY" <<'PY'
import hashlib, hmac, sys
key = sys.argv[1].encode()
sha = lambda s: hashlib.sha256(s.encode()).hexdigest()
mac = lambda h: hmac.new(key, h.encode(), hashlib.sha256).hexdigest()
canon = lambda q, t, p, e, d: f"{q}|{t}|{p}|{e}|{d}"
def mk(q, t, p, e, d):
    h = sha(canon(q, t, p, e, d))
    return {"sequence": q, "timestamp": t, "prev_hash": p, "event": e,
            "details": d, "hash": h, "signature": "hmac-sha256:" + mac(h)}
def ln(x):
    return ('{"sequence":%d,"timestamp":"%s","prev_hash":"%s","event":"%s",'
            '"details":"%s","hash":"%s","signature":"%s"}' % (
            x["sequence"], x["timestamp"], x["prev_hash"], x["event"],
            x["details"], x["hash"], x["signature"]))
G = "0" * 64
e = [mk(0, "2026-01-01T00:00:00Z", G, "GENESIS", "audit log start")]
e.append(mk(1, "2026-01-01T00:00:01Z", e[0]["hash"], "POLICY_CHANGE",
            "admin granted shell access to agent alpha"))
e.append(mk(2, "2026-01-01T00:00:02Z", e[1]["hash"], "AGENT_ACTION",
            "alpha executed deploy"))
open("clean.jsonl", "w").write("\n".join(ln(x) for x in e) + "\n")

# Forgery: rewrite the policy entry, recompute its hash (NO key needed),
# repair the successor's prev_hash, then delete the signature.
f = [dict(x) for x in e]
f[1]["details"] = "routine health check"
f[1]["hash"] = sha(canon(1, f[1]["timestamp"], f[1]["prev_hash"], f[1]["event"], f[1]["details"]))
f[1]["signature"] = ""
f[2]["prev_hash"] = f[1]["hash"]
f[2]["hash"] = sha(canon(2, f[2]["timestamp"], f[2]["prev_hash"], f[2]["event"], f[2]["details"]))
f[2]["signature"] = "hmac-sha256:" + mac(f[2]["hash"])
open("forged_nosig.jsonl", "w").write("\n".join(ln(x) for x in f) + "\n")

# Same forgery, signature left in place (stale -- a forger cannot recompute it).
c = [dict(x) for x in f]
c[1]["signature"] = e[1]["signature"]
open("forged_withsig.jsonl", "w").write("\n".join(ln(x) for x in c) + "\n")

# Same forgery, signature present but malformed (no ':') -- the second skip path.
m = [dict(x) for x in f]
m[1]["signature"] = "deadbeef"
open("forged_badsig.jsonl", "w").write("\n".join(ln(x) for x in m) + "\n")

# An unsigned-but-honest log, for the chain-only controls.
u = [dict(x) for x in e]
for x in u:
    x["signature"] = ""
open("unsigned.jsonl", "w").write("\n".join(ln(x) for x in u) + "\n")
PY

rc_of() { "$VERIFY" "$1" ${2:+--hmac-key "$2"} >/dev/null 2>&1; echo $?; }

echo "=== Group A: the forgery must be caught ==="
r="$(rc_of forged_nosig.jsonl "$KEY")"
if [ "$r" -eq 2 ]; then
    ok "A-01" "tampered entry with its signature DELETED is reported TAMPERED"
else
    bad "A-01" "forgery certified as valid (exit $r) -- the log disabled its own check"
fi
r="$(rc_of forged_badsig.jsonl "$KEY")"
if [ "$r" -eq 2 ]; then
    ok "A-02" "tampered entry with a MALFORMED signature is reported TAMPERED"
else
    bad "A-02" "malformed signature silently skipped the HMAC check (exit $r)"
fi

echo "=== Group B: POSITIVE CONTROL -- the check itself works ==="
r="$(rc_of forged_withsig.jsonl "$KEY")"
if [ "$r" -eq 2 ]; then
    ok "B-01" "tampered entry with signature KEPT is reported TAMPERED"
else
    bad "B-01" "HMAC check is not firing at all (exit $r) -- Group A proves nothing"
fi

echo "=== Group C: NEGATIVE CONTROLS -- the fix must not be fail-everything ==="
r="$(rc_of clean.jsonl "$KEY")"
if [ "$r" -eq 0 ]; then
    ok "C-01" "an untampered signed log still verifies"
else
    bad "C-01" "over-broad: a clean signed log now fails (exit $r)"
fi
r="$(rc_of unsigned.jsonl)"
if [ "$r" -eq 0 ]; then
    ok "C-02" "chain-only mode (no key) still verifies an unsigned log"
else
    bad "C-02" "over-broad: no-key chain-only verification broke (exit $r)"
fi
r="$(rc_of unsigned.jsonl "$KEY")"
if [ "$r" -eq 2 ]; then
    ok "C-03" "a key against an unsigned log refuses to certify"
else
    bad "C-03" "supplying a key to an unsigned log returned 'verified' (exit $r)"
fi

echo ""
echo "audit hmac required: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
