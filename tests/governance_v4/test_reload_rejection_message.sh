#!/usr/bin/env bash
# ============================================================
# test_reload_rejection_message.sh — a rejected mid-run reload says what it does
#
# Signature verification prints its reason as "INTEGRITY BLOCK", which is right
# at startup: a config that fails verification ends the run (exit 3). A mid-run
# reload shares the same verifier, but there a failure only KEEPS the config
# already loaded, and the run continues. The reload path printed the startup
# wording anyway, beside its own "Reload rejected" line, so a run that went on
# to exit 0 under "Governance: PASS" showed an INTEGRITY BLOCK in its stderr
# (repo-sentinel round 7b). And reload is retried before every governed call
# until the file changes again: its own line was printed once per mtime, but the
# verifier's line was printed on EVERY retry.
#
# RR-01  the run continues after the rejected reload (exit 0, reaches the end)
# RR-02  one "Reload rejected" line, carrying the verifier's reason
# RR-03  no INTEGRITY BLOCK line, however many retries
# RR-04  control: the SAME tampered config at startup is still an INTEGRITY
#        BLOCK with exit 3 — without it, RR-03 passes for a change that
#        silenced verification everywhere
# RR-05  control: a validly re-signed change is still accepted mid-run, so the
#        reload path is live and RR-02/03 are not passing on a reload that
#        never ran
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="${_SYSTMP}/reload-msg-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  A rejected mid-run reload is not an INTEGRITY BLOCK         |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

IS_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;; esac
[ -n "${WINDIR:-}" ] && IS_WINDOWS=1
if [ "$IS_WINDOWS" -eq 1 ]; then
    for id in RR-01 RR-02 RR-03 RR-04 RR-05; do
        skip "$id" "mid-run file swap requires POSIX file semantics"
    done
    echo ""
    echo "  Total: 5 | Pass: 0 | Fail: 0 | Skip: 5"
    exit 0
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "${TEST_TMP:?}"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP/run" "$TEST_TMP/tight" "$TEST_TMP/startup"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
sign_dir() { (cd "$1" && "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

# $1=path $2=max_turns  (a lower max_turns is a tightening, so a valid
# re-signed copy is accepted by the ratchet)
mkcfg() {
    python3 - "$1" "$2" << 'PY'
import json, sys
p, turns = sys.argv[1], int(sys.argv[2])
cfg = {"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
       "languages": {"allowed": ["python"]},
       "capabilities": {"env_vars": {"read": True}},
       "agents": {"a": {"provider": "gemini", "model": "m", "api_key_env": "FK",
                        "system_prompt": "s", "max_turns": turns}}}
json.dump(cfg, open(p, "w"), indent=1)
PY
}

# The swap copies the new govern.json (and, for RR-05, its .sig) into place,
# then five env reads each run reloadIfChanged().
mkscript() {  # $1=copy_sig(true|false)
    local sig_line=""
    [ "$1" = "true" ] && sig_line='shutil.copy("'"$TEST_TMP"'/tight/govern.json.sig", "'"$TEST_TMP"'/run/govern.json.sig")'
    cat > "$TEST_TMP/run/t.naab" << EOF
use env
main {
    let r1 = <<python
import time, shutil
time.sleep(1)
shutil.copy("$TEST_TMP/tight/govern.json", "$TEST_TMP/run/govern.json")
$sig_line
print("swapped")
>>
    print(r1)
    let i = 0
    while i < 5 {
        let v = env.get("HOME")
        i = i + 1
    }
    print("REACHED_END")
}
EOF
}

# ---- Rejected reload: the new content is NOT re-signed ----
mkcfg "$TEST_TMP/run/govern.json" 10; sign_dir "$TEST_TMP/run"
mkcfg "$TEST_TMP/tight/govern.json" 5   # unsigned: its old .sig no longer matches
mkscript false
OUT=$(cd "$TEST_TMP/run" && FK=x timeout 90s "$NAAB" t.naab 2>&1); RC=$?

case "$OUT" in
    *REACHED_END*) ended=1 ;;
    *) ended=0 ;;
esac
if [ "$RC" -eq 0 ] && [ "$ended" -eq 1 ]; then
    pass "RR-01" "the run continues after a rejected reload (exit 0)"
else
    fail "RR-01" "run did not continue" "rc=$RC ended=$ended"
fi

n_rej=$(printf '%s\n' "$OUT" | grep -c 'Reload rejected: signature verification failed')
has_reason=0
case "$OUT" in *"Reload rejected: signature verification failed - "*"does not match any trusted key"*) has_reason=1 ;; esac
if [ "$n_rej" -eq 1 ] && [ "$has_reason" -eq 1 ]; then
    pass "RR-02" "one 'Reload rejected' line, carrying the verifier's reason"
else
    fail "RR-02" "expected exactly one rejection line with the reason" "count=$n_rej reason=$has_reason"
fi

n_blk=$(printf '%s\n' "$OUT" | grep -c 'INTEGRITY BLOCK')
if [ "$n_blk" -eq 0 ]; then
    pass "RR-03" "no INTEGRITY BLOCK line for a reload that keeps the current config"
else
    fail "RR-03" "INTEGRITY BLOCK printed during a run that continued" "count=$n_blk"
fi

# ---- RR-04 control: the same tampered config at startup ----
cp "$TEST_TMP/tight/govern.json" "$TEST_TMP/startup/govern.json"
cp "$TEST_TMP/run/govern.json.sig" "$TEST_TMP/startup/govern.json.sig"
printf 'main { print("SHOULD_NOT_RUN") }\n' > "$TEST_TMP/startup/s.naab"
SOUT=$(cd "$TEST_TMP/startup" && timeout 60s "$NAAB" s.naab 2>&1); SRC=$?
case "$SOUT" in
    *"INTEGRITY BLOCK"*) sblk=1 ;;
    *) sblk=0 ;;
esac
if [ "$SRC" -eq 3 ] && [ "$sblk" -eq 1 ]; then
    pass "RR-04" "control: the same config at startup is still an INTEGRITY BLOCK (exit 3)"
else
    fail "RR-04" "startup verification changed" "rc=$SRC block=$sblk"
fi

# ---- RR-05 control: a validly re-signed change is accepted mid-run ----
mkcfg "$TEST_TMP/run/govern.json" 10; sign_dir "$TEST_TMP/run"
mkcfg "$TEST_TMP/tight/govern.json" 5; sign_dir "$TEST_TMP/tight"
mkscript true
VOUT=$(cd "$TEST_TMP/run" && FK=x timeout 90s "$NAAB" t.naab 2>&1); VRC=$?
case "$VOUT" in
    *"reloaded mid-run"*) vok=1 ;;
    *) vok=0 ;;
esac
if [ "$VRC" -eq 0 ] && [ "$vok" -eq 1 ]; then
    pass "RR-05" "control: a validly re-signed change is still reloaded mid-run"
else
    fail "RR-05" "valid reload was not accepted" "rc=$VRC reloaded=$vok"
fi

echo ""
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}Failures:${FAILURES}${NC}"
    exit 1
fi
exit 0
