#!/usr/bin/env bash
# ============================================================
# test_key_env_withheld.sh -- the evidence keys are withheld from the program (A33(b), A186)
#
# THE DEFECT (A33(b), recorded 2026-10-14, left as its own follow-up)
#
# The variables named by telemetry.tamper_evidence.hmac_key_env,
# audit.tamper_evidence.hmac_key_env, audit.provenance.signing_key_env and
# telemetry.webhook_auth_env hold the HMAC keys that seal the telemetry chain
# and the audit trail, the path of the provenance signing key, and the
# webhook's credential. The engine reads each when the config is loaded, and
# the governed program could read them too: env.get, a child process, embedded
# Python's os.environ. A program holding the chain key can append a correctly
# chained forged record of its own run. Only guidance's auth_env was withheld.
#
# A186, found building the fix: the loader kept only the VALUES of these
# variables, never their names, so there was nothing to withhold by name, and
# the reload ratchet's keyRef() compared values. It now compares both the name
# and the value: comparing the name alone would accept a key changed under the
# same name, which a program can do with env.set() before a reload re-reads it.
#
#   KE-00  CONTROL: with no setting naming it, the program reads the variable
#          (the refusal arms below are measuring a variable that reaches it)
#   KE-01  telemetry.tamper_evidence.hmac_key_env: env.get refused (exit 3,
#          blocked_read), value absent; an unnamed variable read in the same run
#   KE-02  audit.tamper_evidence.hmac_key_env: the same
#   KE-03  audit.provenance.signing_key_env: the same
#   KE-04  telemetry.webhook_auth_env: the same
#   KE-05  a child process (process.run printenv) does not receive the key;
#          it does receive the unnamed variable (same run)
#   KE-06  embedded Python's os.environ lacks the key and has the unnamed
#          variable; UNMEASURABLE where Python is not embedded
#   KE-07  the engine still seals the chain with the key: the chain verifies
#          with it and fails with a wrong key (the control that it was used)
#   KE-08  after an accepted mid-run reload (a tightening, "N change(s)
#          applied"), the key is still refused
#   KE-09  A186: a program that env.set()s the key variable cannot get a reload
#          with that value accepted -- the ratchet refuses the key change
#   KE-09c CONTROL: the same reload without env.set is accepted
#
# Kill table (each mutant applied alone, rebuilt, this suite run; observed
# 2026-11-21 on the default build):
#   M1 no push for telemetry.tamper_evidence.hmac_key_env -> KE-01 KE-05 KE-06 KE-08 red
#   M2 no push for audit.tamper_evidence.hmac_key_env     -> KE-02 red
#   M3 no push for audit.provenance.signing_key_env       -> KE-03 red
#   M4 no push for telemetry.webhook_auth_env              -> KE-04 red
#   M5 keyRef() compares the name only                      -> KE-09 red
#   M6 the loader does not keep the names (A186 reverted)   -> KE-01..KE-06 KE-08 red
# On master c1312132 (before the fix): KE-01..KE-06 and KE-08 red; KE-00, KE-07,
# KE-09 and KE-09c green (the value comparison already refused KE-09).
#
# Not covered: audit mode does not enforce blocked_read, and a subprocess
# language with shell on can read the process environment file (A32,
# CONTRA-013) -- the same limits as guidance's auth_env.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
source "$SCRIPT_DIR/../helpers/config_swap.sh"
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_keyenv_XXXXXX")
cleanup() { stop_swap_operators; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2${3:+ -- $3}"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

echo "=== test_key_env_withheld.sh ==="
if [ ! -x "$NAAB" ]; then echo "  UNMEASURABLE: naab-lang not built"; echo "Result: 0 passed, 0 failed, 1 unmeasurable"; exit 1; fi

# Values built from fragments, so neither appears whole in this file.
KEYVAL="kv-$(printf '%s' sealing)-$$"
OTHERVAL="ov-$(printf '%s' plain)-$$"
export EVK_A="$KEYVAL" EVK_OTHER="$OTHERVAL"

# cfg SETTING -> a govern.json naming EVK_A under one of the four settings
cfg() {
    local extra='"security": {"sandbox_level": "elevated"}'
    case "$1" in
        none)    echo "{\"mode\": \"enforce\", $extra}" ;;
        tel)     echo "{\"mode\": \"enforce\", $extra, \"telemetry\": {\"enabled\": true, \"output_file\": \"tele.jsonl\", \"tamper_evidence\": {\"enabled\": true, \"hmac_key_env\": \"EVK_A\"}}${2:-}}" ;;
        audit)   echo "{\"mode\": \"enforce\", $extra, \"audit\": {\"output_file\": \"audit.jsonl\", \"tamper_evidence\": {\"enabled\": true, \"hmac_key_env\": \"EVK_A\"}}}" ;;
        sign)    echo "{\"mode\": \"enforce\", $extra, \"audit\": {\"provenance\": {\"enabled\": true, \"signing_key_env\": \"EVK_A\"}}}" ;;
        webhook) echo "{\"mode\": \"enforce\", $extra, \"telemetry\": {\"enabled\": true, \"output_file\": \"tele.jsonl\", \"webhook_auth_env\": \"EVK_A\"}}" ;;
    esac
}

READ_PROG='use env
main {
    print("OTHER=" + env.get("EVK_OTHER"))
    print("KEY=" + env.get("EVK_A"))
}'

# run ID CONFIG PROGRAM -> OUT, RC, D
run() {
    D="$TEST_TMP/$1"; rm -rf "$D"; mkdir -p "$D"
    printf '%s\n' "$2" > "$D/govern.json"
    printf '%s\n' "$3" > "$D/t.naab"
    RC=0; OUT=$(cd "$D" && timeout 60s "$NAAB" run t.naab 2>&1) || RC=$?
}

# ---- KE-00 control ----------------------------------------------------------
run ke00 "$(cfg none)" "$READ_PROG"
if [ "$RC" = 0 ] && grep -q "KEY=$KEYVAL" <<<"$OUT" && grep -q "OTHER=$OTHERVAL" <<<"$OUT"; then
    pass KE-00 "CONTROL: unnamed, the variable reaches the program"
else
    fail KE-00 "the variable does not reach the program; nothing below can be read" "rc=$RC $(head -3 <<<"$OUT" | tr '\n' ' ')"
    echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
fi

# ---- KE-01..04 one arm per setting -----------------------------------------
refused_here() {   # $1 id, $2 setting, $3 label
    run "$1" "$(cfg "$2")" "$READ_PROG"
    if [ "$RC" = 3 ] && grep -q "OTHER=$OTHERVAL" <<<"$OUT" && \
       grep -q 'blocked_read' <<<"$OUT" && ! grep -q "$KEYVAL" <<<"$OUT"; then
        pass "$1" "$3: env.get refused, value absent, unnamed variable still read"
    else
        fail "$1" "$3: the program read the key or the run did not reach it" "rc=$RC $(grep -E 'OTHER=|KEY=|rror' <<<"$OUT" | head -3 | tr '\n' ' ')"
    fi
}
refused_here KE-01 tel     "telemetry.tamper_evidence.hmac_key_env"
refused_here KE-02 audit   "audit.tamper_evidence.hmac_key_env"
refused_here KE-03 sign    "audit.provenance.signing_key_env"
refused_here KE-04 webhook "telemetry.webhook_auth_env"

# ---- KE-05 child process -----------------------------------------------------
run ke05 "$(cfg tel)" 'use process
main {
    let r = process.run("printenv", [])
    print("CHILD=" + r["stdout"])
}'
if [ "$RC" = 0 ] && grep -q "$OTHERVAL" <<<"$OUT" && ! grep -q "$KEYVAL" <<<"$OUT"; then
    pass KE-05 "a child process receives the unnamed variable and not the key"
elif [ "$RC" = 0 ] && ! grep -q "$OTHERVAL" <<<"$OUT"; then
    unm KE-05 "the child's environment never showed the unnamed variable (printenv did not run?)"
else
    fail KE-05 "the key reached a child process" "rc=$RC $(grep -c "$KEYVAL" <<<"$OUT") key lines"
fi

# ---- KE-06 embedded Python ---------------------------------------------------
run ke06 "$(cfg tel)" 'main {
    let v = <<python
import os
str(os.environ.get("EVK_A")) + "|" + str(os.environ.get("EVK_OTHER"))
>>
    print("PY=" + v)
}'
if grep -q 'Python support not available' <<<"$OUT"; then
    unm KE-06 "Python is not embedded in this build"
elif [ "$RC" = 0 ] && grep -q "PY=None|$OTHERVAL" <<<"$OUT"; then
    pass KE-06 "embedded Python's os.environ lacks the key and has the unnamed variable"
else
    fail KE-06 "embedded Python saw the key, or the block did not run" "rc=$RC $(grep -E 'PY=|rror' <<<"$OUT" | head -2 | tr '\n' ' ')"
fi

# ---- KE-07 the engine still seals with the key ------------------------------
run ke07 "$(cfg tel)" 'main {
    print("RAN")
}'
V_OK=0; (cd "$D" && NAAB_TELEMETRY_HMAC_KEY="$KEYVAL" "$NAAB" --verify-telemetry-chain tele.jsonl >/dev/null 2>&1) || V_OK=$?
V_BAD=0; (cd "$D" && NAAB_TELEMETRY_HMAC_KEY="not-$KEYVAL" "$NAAB" --verify-telemetry-chain tele.jsonl >/dev/null 2>&1) || V_BAD=$?
if [ "$RC" = 0 ] && [ -s "$D/tele.jsonl" ] && [ "$V_OK" = 0 ] && [ "$V_BAD" != 0 ]; then
    pass KE-07 "the chain is sealed with the withheld key (verifies with it, not with another)"
else
    fail KE-07 "the engine lost the key it withholds" "rc=$RC verify=$V_OK wrong-key=$V_BAD"
fi

# ---- KE-08 / KE-09: mid-run reloads (signed configs, external operator) ----
"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
sign_dir() { (cd "$1" && "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

# reload_run ID BASE_EXTRA NEW_EXTRA PRE_BODY POST_BODY: two signed configs
# (cfg tel + BASE_EXTRA, then cfg tel + NEW_EXTRA). PRE_BODY runs before the
# swap; the program then asks the external operator for it through the
# helper's marker files and waits; POST_BODY's first env call reloads the
# config (checkEnvVarRead/Write call reloadIfChanged() first). No polyglot
# block is involved, on purpose: an embedded Python block puts back the
# environment it started with when it ends (os.environ is a snapshot), which
# undid a PRE_BODY env.set() in the first version of this arm -- and env.set
# itself reloads BEFORE it writes, so the key must be set before the swap.
reload_run() {
    D="$TEST_TMP/$1"; rm -rf "$D"; mkdir -p "$D/new"
    printf '%s\n' "$(cfg tel "$2")" > "$D/govern.json"; sign_dir "$D"
    printf '%s\n' "$(cfg tel "$3")" > "$D/new/govern.json"; sign_dir "$D/new"
    cat > "$D/t.naab" <<EOF
use env
use file
use time
main {
$4
    time.sleep(1.2)
    file.write(".swap_req", "$D/new/govern.json.sig\t$D/govern.json.sig\n$D/new/govern.json\t$D/govern.json\n")
    file.write(".swap_req.ready", "")
    let waited = 0
    while file.exists(".swap_done") == false && waited < 600 {
        time.sleep(0.05)
        waited = waited + 1
    }
    print("SWAP=" + string(file.exists(".swap_done")))
$5
}
EOF
    start_swap_operator "$D"
    RC=0; OUT=$(cd "$D" && timeout 90s "$NAAB" run t.naab 2>&1) || RC=$?
    stop_swap_operators
}

FSW=', "capabilities": {"filesystem": {"mode": "write"}}'
reload_run ke08 "$FSW" ', "capabilities": {"filesystem": {"mode": "write"}, "env_vars": {"blocked_read": ["EVK_OTHER"]}}' '' \
'    print("KEY=" + env.get("EVK_A"))'
# The key is read first: a refused read is HARD and ends the run, so whatever
# is read first is the only thing this arm can observe. That the reload was
# accepted is read from the engine's own line (a change applied).
if grep -q 'SWAP=true' <<<"$OUT" && grep -qE 'reloaded mid-run: [1-9][0-9]* change' <<<"$OUT" && \
   [ "$RC" = 3 ] && grep -q 'blocked_read' <<<"$OUT" && ! grep -q "$KEYVAL" <<<"$OUT"; then
    pass KE-08 "after an accepted reload (a change applied) the key is still refused"
else
    fail KE-08 "the key was read after the reload, or the reload was not accepted" "rc=$RC $(grep -E 'SWAP=|reload|KEY=' <<<"$OUT" | head -4 | tr '\n' ' ')"
fi

reload_run ke09 "$FSW" "$FSW" '    env.set("EVK_A", "changed-by-the-program")' \
'    print("HAS=" + string(env.has("EVK_OTHER")))
    print("END")'
K9_OK=0; (cd "$D" && NAAB_TELEMETRY_HMAC_KEY="$KEYVAL" "$NAAB" --verify-telemetry-chain tele.jsonl >/dev/null 2>&1) || K9_OK=$?
if grep -q 'SWAP=true' <<<"$OUT" && grep -qi 'reload rejected' <<<"$OUT" && \
   grep -q "key changed mid-run" <<<"$OUT" && [ "$K9_OK" = 0 ]; then
    pass KE-09 "a key the program set with env.set is refused by the reload ratchet; the chain keeps the operator's key"
else
    fail KE-09 "a reload took a key value the program wrote" "rc=$RC verify-with-original=$K9_OK $(grep -iE 'SWAP=|reload|key changed' <<<"$OUT" | head -3 | tr '\n' ' ')"
fi

reload_run ke09c "$FSW" "$FSW" '' \
'    print("HAS=" + string(env.has("EVK_OTHER")))
    print("END")'
if grep -q 'SWAP=true' <<<"$OUT" && grep -q 'reloaded mid-run' <<<"$OUT" && grep -q '^END' <<<"$OUT"; then
    pass KE-09c "CONTROL: the same reload without env.set is accepted"
else
    fail KE-09c "the unchanged reload was not accepted, so KE-09 proves nothing" "rc=$RC $(grep -iE 'SWAP=|reload|rror' <<<"$OUT" | head -3 | tr '\n' ' ')"
fi

echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
fi

# ---- KE-01..04 one arm per setting -----------------------------------------
refused_here() {   # $1 id, $2 setting, $3 label
    run "$1" "$(cfg "$2")" "$READ_PROG"
    if [ "$RC" = 3 ] && grep -q "OTHER=$OTHERVAL" <<<"$OUT" && \
       grep -q 'blocked_read' <<<"$OUT" && ! grep -q "$KEYVAL" <<<"$OUT"; then
        pass "$1" "$3: env.get refused, value absent, unnamed variable still read"
    else
        fail "$1" "$3: the program read the key or the run did not reach it" "rc=$RC $(grep -E 'OTHER=|KEY=|rror' <<<"$OUT" | head -3 | tr '\n' ' ')"
    fi
}
refused_here KE-01 tel     "telemetry.tamper_evidence.hmac_key_env"
refused_here KE-02 audit   "audit.tamper_evidence.hmac_key_env"
refused_here KE-03 sign    "audit.provenance.signing_key_env"
refused_here KE-04 webhook "telemetry.webhook_auth_env"

# ---- KE-05 child process -----------------------------------------------------
run ke05 "$(cfg tel)" 'use process
main {
    let r = process.run("printenv", [])
    print("CHILD=" + r["stdout"])
}'
if [ "$RC" = 0 ] && grep -q "$OTHERVAL" <<<"$OUT" && ! grep -q "$KEYVAL" <<<"$OUT"; then
    pass KE-05 "a child process receives the unnamed variable and not the key"
elif [ "$RC" = 0 ] && ! grep -q "$OTHERVAL" <<<"$OUT"; then
    unm KE-05 "the child's environment never showed the unnamed variable (printenv did not run?)"
else
    fail KE-05 "the key reached a child process" "rc=$RC $(grep -c "$KEYVAL" <<<"$OUT") key lines"
fi

# ---- KE-06 embedded Python ---------------------------------------------------
run ke06 "$(cfg tel)" 'main {
    let v = <<python
import os
str(os.environ.get("EVK_A")) + "|" + str(os.environ.get("EVK_OTHER"))
>>
    print("PY=" + v)
}'
if grep -q 'Python support not available' <<<"$OUT"; then
    unm KE-06 "Python is not embedded in this build"
elif [ "$RC" = 0 ] && grep -q "PY=None|$OTHERVAL" <<<"$OUT"; then
    pass KE-06 "embedded Python's os.environ lacks the key and has the unnamed variable"
else
    fail KE-06 "embedded Python saw the key, or the block did not run" "rc=$RC $(grep -E 'PY=|rror' <<<"$OUT" | head -2 | tr '\n' ' ')"
fi

# ---- KE-07 the engine still seals with the key ------------------------------
run ke07 "$(cfg tel)" 'main {
    print("RAN")
}'
V_OK=0; (cd "$D" && NAAB_TELEMETRY_HMAC_KEY="$KEYVAL" "$NAAB" --verify-telemetry-chain tele.jsonl >/dev/null 2>&1) || V_OK=$?
V_BAD=0; (cd "$D" && NAAB_TELEMETRY_HMAC_KEY="not-$KEYVAL" "$NAAB" --verify-telemetry-chain tele.jsonl >/dev/null 2>&1) || V_BAD=$?
if [ "$RC" = 0 ] && [ -s "$D/tele.jsonl" ] && [ "$V_OK" = 0 ] && [ "$V_BAD" != 0 ]; then
    pass KE-07 "the chain is sealed with the withheld key (verifies with it, not with another)"
else
    fail KE-07 "the engine lost the key it withholds" "rc=$RC verify=$V_OK wrong-key=$V_BAD"
fi

# ---- KE-08 / KE-09: mid-run reloads (signed configs, external operator) ----
"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
sign_dir() { (cd "$1" && "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

# reload_run ID NEW_EXTRA MID_BODY POST_BODY: base = cfg tel; the new config
# adds NEW_EXTRA to it. A <<python>> block asks the operator for the swap;
# MID_BODY then runs; a <<sh>> block triggers the reload (both engines reload
# before every polyglot block); POST_BODY runs after it. The reload is
# triggered from a SHELL block on purpose: an embedded Python block puts back
# the environment it started with when it ends (os.environ is a snapshot), so
# a MID_BODY env.set() followed by a Python trigger is undone before the
# reload reads it -- that measured Python, not the ratchet.
reload_run() {
    D="$TEST_TMP/$1"; rm -rf "$D"; mkdir -p "$D/new"
    printf '%s\n' "$(cfg tel)" > "$D/govern.json"; sign_dir "$D"
    printf '%s\n' "$(cfg tel "$2")" > "$D/new/govern.json"; sign_dir "$D/new"
    cat > "$D/t.naab" <<EOF
use env
main {
    let s = <<python
import time, os
def _swap(src, dst):
    if os.path.exists(".swap_done"):
        os.remove(".swap_done")
    with open(".swap_req", "w") as f:
        f.write(src + "\t" + dst + "\n")
    open(".swap_req.ready", "w").close()
    for _ in range(1200):
        if os.path.exists(".swap_done"):
            return
        time.sleep(0.05)
    raise RuntimeError("config swap operator did not respond")
time.sleep(1.2)
_swap("$D/new/govern.json.sig", "$D/govern.json.sig")
_swap("$D/new/govern.json", "$D/govern.json")
"swapped"
>>
    print("SWAP=" + s)
$3
    let t = <<sh
echo after
>>
    print("RELOAD_POINT=" + string(t))
$4
}
EOF
    start_swap_operator "$D"
    RC=0; OUT=$(cd "$D" && timeout 90s "$NAAB" run t.naab 2>&1) || RC=$?
    stop_swap_operators
}

if grep -q 'Python support not available' <<<"$(cd "$TEST_TMP/ke06" && "$NAAB" run t.naab 2>&1)"; then
    unm KE-08 "the swap runs from embedded Python, which this build lacks"
    unm KE-09 "as KE-08"; unm KE-09c "as KE-08"
else
    reload_run ke08 ', "capabilities": {"env_vars": {"blocked_read": ["EVK_OTHER"]}}' '' \
'    let a = "unset"
    try { a = env.get("EVK_OTHER") } catch (e) { a = "REFUSED" }
    print("OTHER_AFTER=" + a)
    print("KEY=" + env.get("EVK_A"))'
    if grep -q 'SWAP=swapped' <<<"$OUT" && grep -q 'reloaded mid-run' <<<"$OUT" && \
       ! grep -q "OTHER_AFTER=$OTHERVAL" <<<"$OUT" && [ "$RC" = 3 ] && ! grep -q "$KEYVAL" <<<"$OUT"; then
        pass KE-08 "after an accepted reload the key is still refused and the tightening applies"
    else
        fail KE-08 "the key or the reload did not behave as required" "rc=$RC $(grep -E 'SWAP=|reload|OTHER_AFTER=|KEY=' <<<"$OUT" | head -4 | tr '\n' ' ')"
    fi

    reload_run ke09 '' '    env.set("EVK_A", "changed-by-the-program")' '    print("END")'
    K9_OK=0; (cd "$D" && NAAB_TELEMETRY_HMAC_KEY="$KEYVAL" "$NAAB" --verify-telemetry-chain tele.jsonl >/dev/null 2>&1) || K9_OK=$?
    if grep -q 'SWAP=swapped' <<<"$OUT" && grep -qi 'reload rejected' <<<"$OUT" && \
       grep -q "key changed mid-run" <<<"$OUT" && [ "$K9_OK" = 0 ]; then
        pass KE-09 "a key the program set with env.set is refused by the reload ratchet; the chain keeps the operator's key"
    else
        fail KE-09 "a reload took a key value the program wrote" "rc=$RC verify-with-original=$K9_OK $(grep -iE 'SWAP=|reload|key changed' <<<"$OUT" | head -3 | tr '\n' ' ')"
    fi

    reload_run ke09c '' '' '    print("END")'
    if grep -q 'SWAP=swapped' <<<"$OUT" && grep -q 'reloaded mid-run' <<<"$OUT" && grep -q '^END' <<<"$OUT"; then
        pass KE-09c "CONTROL: the same reload without env.set is accepted"
    else
        fail KE-09c "the unchanged reload was not accepted, so KE-09 proves nothing" "rc=$RC $(grep -iE 'SWAP=|reload|rror' <<<"$OUT" | head -3 | tr '\n' ' ')"
    fi
fi

echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
