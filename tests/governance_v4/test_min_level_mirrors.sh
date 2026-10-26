#!/usr/bin/env bash
# ============================================================
# test_min_level_mirrors.sh -- no_secrets / no_pii are never warn-only, a
# "hard" level stays hard, and every config form of a code_quality check is
# seen by the reload ratchet and the extends merge
#
# WHY THIS EXISTS. F3 (29982dc6) meant "no_secrets and no_pii always minimum
# SOFT". It compared the raw enum (`level < SOFT`), whose declaration order is
# NONE, HARD, APPROVAL_REQUIRED, SOFT, ADVISORY, DETECT -- so it LOWERED hard
# to soft and never raised advisory, from the day it was written. Observed
# before the fix: "no_pii": "hard" ran as soft (PII passed with
# --governance-override, exit 0), and an advisory no_pii or no_secrets only
# warned.
#
# no_secrets, no_placeholders and no_hardcoded_results also store their switch
# and level TWICE (a legacy flat pair the checks read, and code_quality.<x>
# which the ratchet, the minimum-level rule and the function-body gate read).
# The shorthand ("no_secrets": "hard") set only the flat pair, and the extends
# merge inherited only the structured struct and the flat ENABLED flag. So:
# the minimum-level rule wrote a copy checkSecrets never reads; a signed
# reload from "no_secrets": "hard" to false or "advisory" was accepted and a
# secret then ran (exit 0) while the object form was refused; and a parent's
# hard no_placeholders / no_hardcoded_results reached an extends child at the
# flat default (soft / advisory). no_pii and no_hardcoded_results were not
# compared by the ratchet at all.
#
# Startup (each program puts a marker in its source or a polyglot block):
#   ML-00  CONTROL: with no no_pii key the PII program runs (fixture is live)
#   ML-01  no_pii advisory blocks (exit 3)                      [VM]
#   ML-01t the tree-walker is refused under enforce (Bundle 2), so it cannot run it
#   ML-02  no_pii {} (default level) blocks
#   ML-03  PRECISION: no_pii advisory is raised to SOFT, not HARD
#          (--governance-override lets it through)
#   ML-04  no_pii "hard" stays HARD (override does not let it through)
#   ML-05  no_pii true stays HARD
#   ML-06  PRECISION: no_pii detect is left at DETECT (exit 1) -- DETECT
#          blocks the content and has no override, so raising it is not a
#          tightening
#   MS-01  no_secrets "advisory" shorthand blocks -- the rule reaches the flat
#          copy checkSecrets reads
#   MS-02  no_secrets {level: advisory} blocks
#   MS-03  PRECISION: no_secrets advisory is SOFT (override passes)
#   MS-04  CONTROL: no_secrets hard is unchanged (override does not pass)
#   MC-01  PRECISION: no_placeholders "advisory" stays advisory -- the rule
#          covers no_secrets and no_pii only, and the reconcile raises nothing
#          that was not written
#   FB-01  the shorthand also turns on the NAAb function-body scan, which the
#          object form always had (a tightening side effect, pinned so the
#          documentation stays true)
#   Extends (each with the parent used directly as its control):
#   XE-01  a parent's hard no_placeholders is HARD in the child
#   XE-02  a parent's hard no_hardcoded_results is HARD in the child
#   XE-03  a parent's SHORTHAND "hard" no_placeholders is HARD in the child --
#          the merge copies the structured struct only, so this needs the
#          copies reconciled when the parent is parsed
#   XC-01  CONTROL: under child_wins a child that sets its own level in the
#          shorthand keeps it. The shorthand was never marked explicitly set,
#          so a reconcile alone handed the child its parent's hard level here
#          while the object form (XC-02) kept the child's. The first version
#          of this change had exactly that defect; an adversarial pass found
#          it, no failing test did
#   XC-02  CONTROL: the same in the object form (passes before and after)
# Model responses (loopback agent stub; the response carries an SSN):
#   MR-00  CONTROL: with no no_pii key the response reaches the script
#   MR-01  no_pii {} blocks the response (was: a warning, response returned)
#   MR-02  no_pii hard blocks it under --governance-override
# Reload (signed config, signed reload; the probe builds its marker at runtime
# and runs it through codegen, so the startup scan never sees it):
#   R-00   CONTROL: the swap harness reloads and a tightening is accepted
#   RS-00  CONTROL: the secret probe trips no_secrets without a reload
#   RS-01  no_secrets "hard" -> false is refused
#   RS-02  no_secrets "hard" -> "advisory" is refused
#   RS-03  no_secrets true -> key removed is refused
#   RS-04  CONTROL: no_secrets "advisory" -> "hard" is accepted and blocks
#          at HARD (read from the block label)
#   RP-01  no_pii hard -> removed is refused
#   RP-02  no_pii hard -> advisory is refused
#   RP-03  CONTROL: no_pii absent -> hard is accepted and enforced
#   RH-01  no_hardcoded_results hard -> removed is refused
#   RA-01..RA-03  the restrictions.* aliases are refused under the
#          code_quality name -- the evidence for ratchet_classification.txt.
#          The alias writes both copies, so RA-01/RA-02 pass before and after;
#          RA-03 failed before, because no_hardcoded_results was never compared
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../helpers/tree_walk_refusal.sh"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -8 | sed 's/^/       | /'; }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }
report() { echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

echo "=== no_secrets / no_pii minimum level, and the code_quality mirrors ==="
if [ ! -x "$NAAB" ]; then
    skip "ML-00" "naab-lang not built -- UNMEASURABLE, not a pass"; report; exit 0
fi

W="$(mktemp -d)"
SWAP_LOADED=0
cleanup() {
    [ -n "${STUB_PID:-}" ] && kill "$STUB_PID" 2>/dev/null
    [ "$SWAP_LOADED" = 1 ] && { stop_swap_operators; teardown_isolated_trust; }
    rm -rf "$W"
}
trap cleanup EXIT

# The secret is assembled here so this file never holds a whole key.
FAKE_KEY="AK""IA""ABCDEFGHIJ""KLMNOP"
P_PII_SRC='main {
    let s = "123-45-6789"
    print("RAN")
}'
P_SEC_SRC="main {
    let k = \"$FAKE_KEY\"
    print(\"RAN\")
}"
P_PH_SRC='main {
    let r = <<python
# TODO finish this
x = 1
>>
    print("RAN")
}'
P_HR_SRC='main {
    let r = <<python
x = 1  # stub
>>
    print("RAN")
}'

# startup NAME CODE_QUALITY_JSON PROGRAM [naab args...] -> OUT, RC
startup() {
    local name="$1" cq="$2" prog="$3"; shift 3
    local d="$W/$name"; mkdir -p "$d"
    printf '{ "version": "4.0", "mode": "enforce", "code_quality": %s }\n' "$cq" > "$d/govern.json"
    printf '%s\n' "$prog" > "$d/p.naab"
    OUT="$(cd "$d" && timeout 60 "$NAAB" "$@" p.naab 2>&1)"; RC=$?
}
ran() { grep -qx 'RAN' <<<"$OUT"; }
# blocked ID DESC RC_WANT   -- PASS iff the program did not run and exited RC_WANT
blocked() {
    if ran; then bad "$1" "$2 -- the program ran (rc=$RC)" "$OUT"
    elif [ "$RC" -ne "$3" ]; then bad "$1" "$2 -- exit $RC, expected $3" "$OUT"
    else ok "$1" "$2"; fi
}
# passed ID DESC  -- PASS iff the program ran and exited 0
passed() {
    if ran && [ "$RC" -eq 0 ]; then ok "$1" "$2"
    else bad "$1" "$2 -- expected the program to run (rc=$RC)" "$OUT"; fi
}

# ---- no_pii -----------------------------------------------------------------
startup ML00 '{ }' "$P_PII_SRC"
passed ML-00 "control: with no no_pii key the PII program runs"

startup ML01 '{ "no_pii": { "level": "advisory" } }' "$P_PII_SRC"
blocked ML-01 "no_pii advisory is raised to a block" 3
# ML-01t ran this on the tree-walker. Under enforce the tree-walker is now
# refused before anything runs (Bundle 2): the program cannot run there.
startup ML01t '{ "no_pii": { "level": "advisory" } }' "$P_PII_SRC" --tree-walk
if tw_refused "$RC" "$OUT" && ! ran; then ok ML-01t "the tree-walker is refused under enforce; the PII program did not run"
else bad ML-01t "the tree-walker should be refused under enforce (rc=$RC)" "$OUT"; fi
startup ML02 '{ "no_pii": { } }' "$P_PII_SRC"
blocked ML-02 "no_pii {} (default level) blocks" 3
startup ML03 '{ "no_pii": { "level": "advisory" } }' "$P_PII_SRC" --governance-override
passed ML-03 "precision: advisory is raised to SOFT, not HARD (override passes)"
startup ML04 '{ "no_pii": { "level": "hard" } }' "$P_PII_SRC" --governance-override
blocked ML-04 "no_pii hard stays HARD (override does not pass)" 3
startup ML05 '{ "no_pii": true }' "$P_PII_SRC" --governance-override
blocked ML-05 "no_pii true stays HARD (override does not pass)" 3
startup ML06 '{ "no_pii": { "level": "detect" } }' "$P_PII_SRC"
if ran; then bad ML-06 "no_pii detect -- the program ran" "$OUT"
elif [ "$RC" -eq 1 ] && grep -q 'DETECT' <<<"$OUT"; then ok ML-06 "precision: no_pii detect stays DETECT (exit 1)"
else bad ML-06 "no_pii detect -- expected DETECT, exit 1 (rc=$RC)" "$OUT"; fi

# ---- no_secrets ---------------------------------------------------------------
startup MS01 '{ "no_secrets": "advisory" }' "$P_SEC_SRC"
blocked MS-01 "no_secrets \"advisory\" shorthand is raised to a block" 3
startup MS02 '{ "no_secrets": { "level": "advisory" } }' "$P_SEC_SRC"
blocked MS-02 "no_secrets {level: advisory} is raised to a block" 3
startup MS03 '{ "no_secrets": "advisory" }' "$P_SEC_SRC" --governance-override
passed MS-03 "precision: no_secrets advisory is SOFT (override passes)"
startup MS04 '{ "no_secrets": { "level": "hard" } }' "$P_SEC_SRC" --governance-override
blocked MS-04 "control: no_secrets hard is unchanged" 3

# ---- siblings are not raised ------------------------------------------------------
startup MC01 '{ "no_placeholders": "advisory" }' "$P_PH_SRC"
if ran && [ "$RC" -eq 0 ] && grep -q 'ADVISORY' <<<"$OUT"; then
    ok MC-01 "precision: no_placeholders advisory stays advisory (fired, warned, ran)"
elif ran; then bad MC-01 "no_placeholders advisory -- ran, but the check did not fire" "$OUT"
else bad MC-01 "no_placeholders advisory was raised (rc=$RC)" "$OUT"; fi

# ---- the shorthand reaches the function-body scan ------------------------------
# checkNaabFunctionBody() gates its placeholder and secret scans on the
# structured copy, which the shorthand never set.
P_PH_FN='fn f() {
    // TODO finish this
    return 1
}
main {
    print("RAN")
}'
startup FB01 '{ "no_placeholders": "hard" }' "$P_PH_FN"
blocked FB-01 "shorthand no_placeholders \"hard\" scans a NAAb function body" 3

# ---- extends ------------------------------------------------------------------
# xe ID DESC PARENT_CQ PROGRAM [naab args...]
xe() {
    local id="$1" desc="$2" pcq="$3" prog="$4"; shift 4
    local d="$W/$id"; mkdir -p "$d/direct" "$d/child"
    printf '{ "version": "4.0", "mode": "enforce", "code_quality": %s }\n' "$pcq" > "$d/parent.json"
    cp "$d/parent.json" "$d/direct/govern.json"
    printf '{ "version": "4.0", "mode": "enforce", "extends": "../parent.json" }\n' > "$d/child/govern.json"
    printf '%s\n' "$prog" > "$d/direct/p.naab"; cp "$d/direct/p.naab" "$d/child/p.naab"
    OUT="$(cd "$d/direct" && timeout 60 "$NAAB" "$@" p.naab 2>&1)"; RC=$?
    if ran || [ "$RC" -ne 3 ]; then
        skip "$id" "control failed: the parent used directly does not block (rc=$RC) -- UNMEASURABLE"; return
    fi
    OUT="$(cd "$d/child" && timeout 60 "$NAAB" "$@" p.naab 2>&1)"; RC=$?
    blocked "$id" "$desc" 3
}
xe XE-01 "a parent's hard no_placeholders is HARD in the child" \
    '{ "no_placeholders": { "level": "hard" } }' "$P_PH_SRC" --governance-override
xe XE-02 "a parent's hard no_hardcoded_results is HARD in the child" \
    '{ "no_hardcoded_results": { "level": "hard" } }' "$P_HR_SRC"
# The merge copies the parent's structured struct only, so a parent written in
# the shorthand hands nothing down unless its copies were reconciled at parse.
xe XE-03 "a parent's shorthand \"hard\" no_placeholders is HARD in the child" \
    '{ "no_placeholders": "hard" }' "$P_PH_SRC" --governance-override

# xc ID DESC PARENT_CQ CHILD_CQ -- under child_wins (the default) a child that
# sets its own level keeps it. PASS iff the child's advisory fired and the
# program ran.
xc() {
    local id="$1" desc="$2" pcq="$3" ccq="$4"
    local d="$W/$id"; mkdir -p "$d/child"
    printf '{ "version": "4.0", "mode": "enforce", "code_quality": %s }\n' "$pcq" > "$d/parent.json"
    printf '{ "version": "4.0", "mode": "enforce", "extends": "../parent.json", "code_quality": %s }\n' "$ccq" > "$d/child/govern.json"
    printf '%s\n' "$P_PH_SRC" > "$d/child/p.naab"
    OUT="$(cd "$d/child" && timeout 60 "$NAAB" p.naab 2>&1)"; RC=$?
    if ran && [ "$RC" -eq 0 ] && grep -q 'ADVISORY' <<<"$OUT"; then ok "$id" "$desc"
    else bad "$id" "$desc -- the parent's level replaced the child's (rc=$RC)" "$OUT"; fi
}
xc XC-01 "control: a child's own shorthand level wins over its parent (child_wins)" \
    '{ "no_placeholders": { "level": "hard" } }' '{ "no_placeholders": "advisory" }'
xc XC-02 "control: a child's own object-form level wins over its parent (child_wins)" \
    '{ "no_placeholders": { "level": "hard" } }' '{ "no_placeholders": { "level": "advisory" } }'

# ---- model responses ----------------------------------------------------------
# The same rule on the agent path: agentSend() scans every response with
# checkPii(), which reads the level the rule set.
# mr ID CODE_QUALITY_JSON [naab args...] -> OUT, RC (or returns 1: stub down)
mr() {
    local id="$1" cq="$2"; shift 2
    local d="$W/$id"; mkdir -p "$d"
    python3 - "$d/fixture.json" <<'PYEOF'
import json, sys
json.dump({"responses": [
    {"content": "Customer record: ssn 123-45-6789, balance reconciled.", "output_tokens": 30},
]}, open(sys.argv[1], "w"))
PYEOF
    STUB_PID=""; start_stub "$d/fixture.json" "$d" || return 1
    printf '{ "version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "code_quality": %s,
  "agents": { "clerk": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:%s", "api_key_env": "FAKE_KEY_MLM",
      "max_tokens": 100, "max_turns": 3, "retry": { "max_attempts": 1, "backoff_ms": 0 } } } }\n' \
        "$cq" "$STUB_PORT" > "$d/govern.json"
    printf 'use agent\nmain {\n    let h = agent.create("clerk")\n    let r = agent.send(h, "summarise the record")\n    print("GOT=" + r["content"])\n}\n' > "$d/t.naab"
    OUT="$(cd "$d" && timeout 60 "$NAAB" "$@" t.naab 2>&1)"; RC=$?
    stop_stub
    return 0
}
if ! command -v python3 >/dev/null 2>&1; then
    skip "MR-00" "python3 missing -- MR arms UNMEASURABLE"
else
    source "$REPO/tests/helpers/stub_launch.sh"
    export FAKE_KEY_MLM="fake-key-min-level"
    if ! mr MR00 '{ }'; then
        skip "MR-00" "agent stub did not start -- MR arms UNMEASURABLE"
    elif ! grep -q 'GOT=.*123-45-6789' <<<"$OUT" || [ "$RC" -ne 0 ]; then
        skip "MR-00" "control failed: the stub response did not reach the script (rc=$RC) -- MR arms UNMEASURABLE"
    else
        ok "MR-00" "control: with no no_pii key the response reaches the script"
        mr MR01 '{ "no_pii": { } }'
        if grep -q 'GOT=' <<<"$OUT"; then bad MR-01 "no_pii {} on a response -- the response was returned (rc=$RC)" "$OUT"
        elif [ "$RC" -ne 3 ]; then bad MR-01 "no_pii {} on a response -- exit $RC, expected 3" "$OUT"
        else ok MR-01 "no_pii {} blocks a PII response (was: warning, response returned)"; fi
        mr MR02 '{ "no_pii": { "level": "hard" } }' --governance-override
        if grep -q 'GOT=' <<<"$OUT"; then bad MR-02 "no_pii hard on a response -- returned under override (rc=$RC)" "$OUT"
        elif [ "$RC" -ne 3 ]; then bad MR-02 "no_pii hard on a response -- exit $RC, expected 3" "$OUT"
        else ok MR-02 "no_pii hard blocks a PII response even under --governance-override"; fi
    fi
fi

# ---- reload -------------------------------------------------------------------
if ! command -v python3 >/dev/null 2>&1; then
    skip "R-00" "python3 missing -- every reload arm is UNMEASURABLE"; report; [ $FAIL -eq 0 ]; exit
fi
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
source "$REPO/tests/helpers/config_swap.sh"
SWAP_LOADED=1
"$NAAB" --keygen "$W/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$W/k.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$W/k.pem"
sign_dir() { (cd "$1" && "$NAAB" --sign-governance >/dev/null 2>&1); }
HDR='"version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" }, "codegen": { "enabled": true }'

# reload_run NAME START NEXT PROBE [naab args...] -> OUT, RC
#   START / NEXT: top-level JSON members after the header. The program swaps
#   the config through an external operator, runs a <<sql>> block (the reload
#   is checked before every polyglot block), then runs PROBE.
reload_run() {
    local name="$1" start="$2" next="$3" probe="$4"; shift 4
    local d="$W/$name" n="$W/$name.next"
    mkdir -p "$d" "$n"
    printf '{ %s, %s }\n' "$HDR" "$start" > "$d/govern.json"
    printf '{ %s, %s }\n' "$HDR" "$next" > "$n/govern.json"
    sign_dir "$d"; sign_dir "$n"
    if [ ! -f "$d/govern.json.sig" ] || [ ! -f "$n/govern.json.sig" ]; then OUT="__NOSIGN__"; RC=99; return; fi
    cat > "$d/r.naab" <<EOF
use codegen
main {
    let s = <<python
import os, time
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
time.sleep(1)
_swap("$n/govern.json.sig", "$d/govern.json.sig")
_swap("$n/govern.json", "$d/govern.json")
"swapped"
>>
    print(s)
    let trig = <<sql
SELECT 1 AS x
>>
    print("TRIGGERED")
$probe
    print("REACHED_END")
}
EOF
    start_swap_operator "$d"
    OUT="$(cd "$d" && timeout 120 "$NAAB" "$@" r.naab 2>&1)"; RC=$?
}
swapped() { case "$OUT" in *swapped*) return 0 ;; *) return 1 ;; esac; }

# refused ID DESC KEYTEXT HELD -- PASS iff the reload was refused naming
# KEYTEXT, the probe did not run (CG_RAN absent), and HELD (the check's own
# message) appears.
refused() {
    local id="$1" desc="$2" key="$3" held="$4"
    if [ "$OUT" = "__NOSIGN__" ]; then skip "$id" "signing unavailable -- UNMEASURABLE"; return; fi
    if ! swapped; then skip "$id" "config swap did not happen -- UNMEASURABLE"; return; fi
    local rej=0
    case "$OUT" in *"Reload rejected: ratchet violation"*"$key"*) rej=1 ;; esac
    if [ $rej -eq 0 ]; then bad "$id" "$desc -- reload NOT refused for '$key'" "$OUT"
    elif grep -qx 'CG_RAN' <<<"$OUT"; then bad "$id" "$desc -- refused, but the probe ran" "$OUT"
    elif ! grep -q "$held" <<<"$OUT"; then bad "$id" "$desc -- refused, but the check did not fire on the probe" "$OUT"
    else ok "$id" "$desc"; fi
}
# accepted ID DESC HELD -- PASS iff no reload was refused, the probe did not
# run, and HELD appears (the tightened policy is in force).
accepted() {
    local id="$1" desc="$2" held="$3"
    if [ "$OUT" = "__NOSIGN__" ]; then skip "$id" "signing unavailable -- UNMEASURABLE"; return; fi
    if ! swapped; then skip "$id" "config swap did not happen -- UNMEASURABLE"; return; fi
    case "$OUT" in *"Reload rejected"*) bad "$id" "$desc -- the reload was refused" "$OUT"; return ;; esac
    if grep -qx 'CG_RAN' <<<"$OUT"; then bad "$id" "$desc -- accepted, but the probe ran" "$OUT"
    elif ! grep -q "$held" <<<"$OUT"; then bad "$id" "$desc -- expected '$held'" "$OUT"
    else ok "$id" "$desc"; fi
}

# Probes: the marker is built at runtime and run through codegen.
P_SEC='    let k = "AK" + "IA" + "ABCDEFGHIJ" + "KLMNOP"
    let r = codegen.run("python", "k = \"" + k + "\"")
    print("CG_RAN")'
P_PII='    let d = "123" + "-45-" + "6789"
    let r = codegen.run("python", "x = \"ssn " + d + "\"")
    print("CG_RAN")'
P_PH='    let m = "TO" + "DO"
    let r = codegen.run("python", "# " + m + ": finish this\nx = 1")
    print("CG_RAN")'
P_HR='    let m = "st" + "ub"
    let r = codegen.run("python", "x = 1  # " + m)
    print("CG_RAN")'
CQ() { printf '"code_quality": %s' "$1"; }
RS() { printf '"restrictions": %s' "$1"; }
M_SEC='AWS Access Key ID'
M_PII='PII detected'
M_PH='TODO'
M_HR='Hardcoded result'

reload_run R00 "$(CQ '{ }')" "\"limits\": { \"timeout\": { \"global\": 600 } }, $(CQ '{ }')" '    print("NOP")'
if [ "$OUT" = "__NOSIGN__" ]; then skip "R-00" "signing unavailable -- UNMEASURABLE"
elif ! swapped; then skip "R-00" "config swap did not happen -- every reload arm is UNMEASURABLE"
else
    case "$OUT" in
        *"Reload rejected"*) bad "R-00" "control: an unrelated tightening reload was refused" "$OUT" ;;
        *REACHED_END*) ok "R-00" "control: the swap harness reloads, and a tightening reload is accepted" ;;
        *) bad "R-00" "control: program did not finish" "$OUT" ;;
    esac
fi

# RS-00: start == next, so the reload changes nothing; the probe must trip.
reload_run RS00 "$(CQ '{ "no_secrets": "hard" }')" "$(CQ '{ "no_secrets": "hard" }')" "$P_SEC"
if [ "$OUT" = "__NOSIGN__" ]; then skip "RS-00" "signing unavailable -- UNMEASURABLE"
elif grep -qx 'CG_RAN' <<<"$OUT"; then bad "RS-00" "control: the secret probe ran under no_secrets hard" "$OUT"
elif grep -q "$M_SEC" <<<"$OUT" && [ "$RC" -eq 3 ]; then ok "RS-00" "control: the secret probe trips no_secrets (exit 3)"
else bad "RS-00" "control: the probe did not trip no_secrets (rc=$RC)" "$OUT"; fi

reload_run RS01 "$(CQ '{ "no_secrets": "hard" }')" "$(CQ '{ "no_secrets": false }')" "$P_SEC"
refused RS-01 "no_secrets \"hard\" -> false" "code_quality.no_secrets.enabled" "$M_SEC"
reload_run RS02 "$(CQ '{ "no_secrets": "hard" }')" "$(CQ '{ "no_secrets": "advisory" }')" "$P_SEC"
refused RS-02 "no_secrets \"hard\" -> \"advisory\"" "code_quality.no_secrets.level" "$M_SEC"
reload_run RS03 "$(CQ '{ "no_secrets": true }')" "$(CQ '{ }')" "$P_SEC"
refused RS-03 "no_secrets true -> key removed" "code_quality.no_secrets.enabled" "$M_SEC"
# A signed config refuses --governance-override at startup, so the level is
# read from the block's own label rather than from an override.
reload_run RS04 "$(CQ '{ "no_secrets": "advisory" }')" "$(CQ '{ "no_secrets": "hard" }')" "$P_SEC"
accepted RS-04 "control: no_secrets \"advisory\" -> \"hard\" is accepted and HARD" "$M_SEC \[HARD"

reload_run RP01 "$(CQ '{ "no_pii": { "level": "hard" } }')" "$(CQ '{ }')" "$P_PII"
refused RP-01 "no_pii hard -> removed" "code_quality.no_pii.enabled" "$M_PII"
reload_run RP02 "$(CQ '{ "no_pii": { "level": "hard" } }')" "$(CQ '{ "no_pii": { "level": "advisory" } }')" "$P_PII"
refused RP-02 "no_pii hard -> advisory" "code_quality.no_pii.level" "$M_PII"
reload_run RP03 "$(CQ '{ }')" "$(CQ '{ "no_pii": { "level": "hard" } }')" "$P_PII"
accepted RP-03 "control: no_pii absent -> hard is accepted and enforced" "$M_PII"

reload_run RH01 "$(CQ '{ "no_hardcoded_results": { "level": "hard" } }')" "$(CQ '{ }')" "$P_HR"
refused RH-01 "no_hardcoded_results hard -> removed" "code_quality.no_hardcoded_results.enabled" "$M_HR"

reload_run RA01 "$(RS '{ "no_secrets": "hard" }')" "$(RS '{ "no_secrets": false }')" "$P_SEC"
refused RA-01 "restrictions.no_secrets hard -> false" "code_quality.no_secrets.enabled" "$M_SEC"
reload_run RA02 "$(RS '{ "no_placeholders": "hard" }')" "$(RS '{ "no_placeholders": "advisory" }')" "$P_PH"
refused RA-02 "restrictions.no_placeholders hard -> advisory" "code_quality.no_placeholders.level" "$M_PH"
reload_run RA03 "$(RS '{ "no_hardcoded_results": "hard" }')" "$(RS '{ "no_hardcoded_results": false }')" "$P_HR"
refused RA-03 "restrictions.no_hardcoded_results hard -> false" "code_quality.no_hardcoded_results.enabled" "$M_HR"

report
[ $FAIL -eq 0 ] || exit 1
exit 0
