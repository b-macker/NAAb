#!/usr/bin/env bash
# ============================================================
# test_readme_claims.sh -- do README.md and docs/language-tour.md still
# describe the binary?
#
# The README is what a visitor reads first, and nothing checked it. Reviewed
# against the build on 2026-10-24, it had drifted in every section: the hero
# output was not the engine's text (and the rule name in the real text was
# printed as "<redacted>"), the CI commands wrote a JSON report to a file named
# "sarif", four keys of the config example were read by nothing, the signing
# commands failed as written, the pipeline example errored on both engines,
# optional chaining gave the wrong value on the VM, and the stdlib table listed
# functions that do not exist. Each was fixed; this suite is what keeps them
# fixed. It reads the documents themselves -- code blocks, commands, tables
# and the sentences that make a claim -- so a claim edited is checked as
# edited. The README was then restructured (2026-10-25): the hero became a
# shipped program (examples/quickstart/), and the language tour and stdlib
# table moved to docs/language-tour.md, which this suite reads too.
#
# Arms (each an observation of the binary, never of a document alone):
#   RC-00  the binary runs a program at all (without it every arm is vacuous)
#   RC-01  Quick Start: no govern.json refuses (exit 4, names `naab-lang init`);
#          after `naab-lang init`, the README's hello.naab prints its greeting
#   RC-02  the hero: examples/quickstart/ is refused with exit 3 and no request
#          reaches a stub model, and every line of the README's hero output is
#          the engine's own text; RC-02c the same program without the patterns
#          sends; RC-02d the README's copies of the program and its govern.json
#          are the shipped files; RC-02e the README's "run it" command, as
#          written, is refused the same way
#   RC-03  the second pattern (config_harvest) refuses a config read followed
#          by a send; RC-03c reading another file still sends
#   RC-04  every ```json block: valid, every key read by the loader
#          (tools/template_coverage.py --unread, screened), and loads with no
#          "has no effect" / "not a setting" warning
#   RC-05  the CI commands write the report each one names; naab-gov lint tells
#          a clean file from one with a placeholder
#   RC-06  the signing commands run as written; a signed config runs, an edited
#          one is an INTEGRITY BLOCK
#   RC-07  every ```naab block runs (exit 0, no Error:), and every value its
#          print-line comments promise (`print(x)  // "B"`) is printed
#   RC-08  every function in the stdlib table exists in its module
#   RC-09  each "Supported languages" list is the binary's language set
#          (aliases folded), and any language count matches it
#   RC-10  every module count stated is the table's, and the table names the
#          modules stdlib.cpp registers (screened)
#   RC-11  the demo script passes (every scene blocked for its own reason)
#   RC-12  the four enforcement levels do what "How It Works" says
#   RC-13  the program can neither read nor overwrite govern.json
#   RC-14  shell disabled refuses a separate-process language
#   RC-15  CONTRA-013 is printed for a config with path rules
#   RC-16  the MCP server lists exactly the tools the README names
#   RC-17  the error text the tour quotes is the engine's
#   RC-18  the "16 built-in patterns" and "23 signals" counts (screened: read
#          from the source)
#   RC-19  Go and PHP blocks run without `package main` / `<?php`
#   RC-20  live guidance: a violation shows the section named for its rule,
#          and the program may not write a source
#   RC-21  scanning agent replies for secrets is off by default and ends the
#          run when on
#
# Outcomes are PASS, FAIL or UNMEASURABLE. UNMEASURABLE is for a missing
# prerequisite (no embedded Python, no network, no compiler, the agent stub on
# Windows), and it says which; it is never used for a claim that did not hold.
# A document that stops making a claim an arm checks is a FAIL too ("no longer
# says ..."): the arm must then be updated, not left checking nothing.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
NAAB_GOV="${NAAB_GOV:-$REPO/build/naab-gov}"
# Overrides exist for the mutation checks of this suite.
README="${README_UNDER_TEST:-$REPO/README.md}"
TOUR="${TOUR_UNDER_TEST:-$REPO/docs/language-tour.md}"
QS="${QUICKSTART_UNDER_TEST:-$REPO/examples/quickstart}"

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '       -> %s\n' "$3"; }
unm()  { UNM=$((UNM + 1));   echo "  UNMEASURABLE [$1] $2"; }

IS_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;; esac
if [ -n "${WINDIR:-}" ]; then IS_WINDOWS=1; fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/readme-claims-XXXXXX")"
# Every arm writes an unsigned govern.json, which is an INTEGRITY BLOCK when
# the trust store holds any key, so the suite gets an empty store of its own
# instead of whatever ~/.naab/trusted-keys holds. RC-06 trusts a key and uses
# a second store, so that key never reaches the arms after it.
# shellcheck source=../helpers/trust_setup.sh
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "$WORK"' EXIT

echo ""
echo "=== README and language tour claims vs the binary ==="

for f in "$README" "$TOUR" "$QS/agent_task.naab" "$QS/govern.json"; do
    if [ ! -f "$f" ]; then
        fail "SETUP" "not found: $f"
        echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
    fi
done

# ---- Document access, in the shell's own path vocabulary (no native python
# opens a path here: under MSYS2 it could not). CR is dropped so a CRLF
# checkout reads the same as an LF one.
doc() { tr -d '\r' < "$1"; }
# doc_block FILE LANG N -> the Nth fenced block whose info string is LANG
# ("" = a bare ``` fence). Every fence toggles, so a closing fence is never
# mistaken for an opening one.
doc_block() {
    doc "$1" | awk -v lang="$2" -v want="$3" '
        /^```/ { if (open) { open = 0; cur = 0; next }
                 open = 1; tag = substr($0, 4)
                 if (tag == lang) { n++; if (n == want) cur = 1 }
                 next }
        cur { print }'
}
doc_count() {
    doc "$1" | awk -v lang="$2" '
        /^```/ { if (open) { open = 0; next } open = 1; if (substr($0, 4) == lang) n++; next }
        END { print n + 0 }'
}
# The block of LANG containing a fixed string; prints its index (0 = none).
doc_find() {
    local f="$1" lang="$2" needle="$3" n i
    n=$(doc_count "$f" "$lang")
    for i in $(seq 1 "$n"); do
        if grep -qF -- "$needle" <<<"$(doc_block "$f" "$lang" "$i")"; then echo "$i"; return; fi
    done
    echo 0
}
readme() { doc "$README"; }
readme_block() { doc_block "$README" "$@"; }
readme_find() { doc_find "$README" "$@"; }
# readme_says ARM TEXT: the README still makes the claim ARM checks. When it
# does not, the arm FAILs and is skipped: a document that dropped a claim must
# take its arm with it, not leave it checking nothing.
readme_says() {
    if grep -qF -- "$2" <<<"$(readme)"; then return 0; fi
    fail "$1" "README no longer says: $2 (update or remove this arm)"
    return 1
}

newdir() { local d="$WORK/$1"; rm -rf "$d"; mkdir -p "$d"; echo "$d"; }
run_in() { local d="$1"; shift; (cd "$d" && "$@" </dev/null 2>&1); }
lines() { tr -d '\r' <<<"$1"; }

# A config under which document code blocks can run: the languages they use,
# the environment readable, the network allowed for the http example.
OPEN_CFG='{ "version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
  "languages": {"allowed": ["python", "javascript", "sql", "shell"]},
  "capabilities": {"network": {"enabled": true}, "shell": {"enabled": true}} }'
AUDIT_CFG='{"version": "5.0", "mode": "audit"}'
# The warnings that mean a setting in a config does nothing (the init suite's
# Test 10 uses the same list).
DEAD_SETTING_RE='has no effect|unknown enforcement level|not a setting the engine reads|not enforced|no check enforces'
PY_MISSING_RE='Python support not available|Python executor disabled|python3: not found|No module named'

# ---- RC-00: the instrument ---------------------------------------------------
D=$(newdir rc00)
echo "$AUDIT_CFG" > "$D/govern.json"
printf 'main {\n    print("RC00_MARKER_" + string(6 * 7))\n}\n' > "$D/m.naab"
OUT=$(run_in "$D" "$NAAB" m.naab)
if grep -qx 'RC00_MARKER_42' <<<"$(lines "$OUT")"; then
    pass "RC-00" "the binary runs a program and prints its result"
else
    fail "RC-00" "the binary did not run a trivial program" "$(head -3 <<<"$OUT")"
    echo ""; echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
    exit 1
fi

# ---- RC-01: Quick Start ------------------------------------------------------
HELLO_IDX=$(readme_find naab "// hello.naab")
if [ "$HELLO_IDX" -eq 0 ]; then
    fail "RC-01" "README has no hello.naab block (the Quick Start changed shape)"
else
    D=$(newdir rc01a)
    readme_block naab "$HELLO_IDX" > "$D/hello.naab"
    OUT=$(run_in "$D" "$NAAB" hello.naab); RC=$?
    if [ "$RC" -eq 4 ] && grep -qF 'naab-lang init' <<<"$OUT"; then
        pass "RC-01a" "with no govern.json the run is refused (exit 4) and names naab-lang init"
    else
        fail "RC-01a" "no govern.json: expected exit 4 naming 'naab-lang init'" "rc=$RC $(head -2 <<<"$OUT")"
    fi
    D=$(newdir rc01b)
    run_in "$D" "$NAAB" init >/dev/null
    readme_block naab "$HELLO_IDX" > "$D/hello.naab"
    OUT=$(run_in "$D" "$NAAB" hello.naab); RC=$?
    if [ ! -f "$D/govern.json" ]; then
        fail "RC-01b" "naab-lang init wrote no govern.json"
    elif [ "$RC" -eq 0 ] && grep -qx 'Hello, World!' <<<"$(lines "$OUT")"; then
        pass "RC-01b" "after naab-lang init, the README's hello.naab prints Hello, World!"
    else
        fail "RC-01b" "hello.naab after init did not print Hello, World!" "rc=$RC $(grep -m2 -E 'Error|Hello' <<<"$OUT")"
    fi
fi

# ---- RC-02 / RC-03: the quickstart, against a stub model ---------------------
HARVEST_PROG='use file
use agent
main {
    let cfg = file.read("__FILE__")
    let h = agent.create("assistant")
    let r = agent.send(h, cfg)
    print("SENT")
}'
# RC-02d: the README shows the shipped files, not a copy that can drift.
PROG_IDX=$(readme_find naab 'agent.create("assistant")')
CFG_IDX=$(readme_find json '"credential_exfiltration"')
if [ "$PROG_IDX" -eq 0 ] || [ "$CFG_IDX" -eq 0 ]; then
    fail "RC-02d" "README no longer shows the quickstart program and its govern.json"
else
    if cmp -s <(readme_block naab "$PROG_IDX") <(doc "$QS/agent_task.naab"); then
        pass "RC-02d" "the README's program is examples/quickstart/agent_task.naab, byte for byte"
    else
        fail "RC-02d" "the README's program differs from examples/quickstart/agent_task.naab" \
             "$(diff <(readme_block naab "$PROG_IDX") <(doc "$QS/agent_task.naab") | head -4 | tr '\n' ' ')"
    fi
    SAME=$(python3 -c '
import json, sys
a, b = sys.stdin.read().split("\n=====\n")
print("same" if json.loads(a) == json.loads(b) else "different")' <<<"$(readme_block json "$CFG_IDX")
=====
$(doc "$QS/govern.json")" 2>&1)
    if [ "$SAME" = "same" ]; then
        pass "RC-02d" "the README's govern.json is examples/quickstart/govern.json"
    else
        fail "RC-02d" "the README's govern.json differs from examples/quickstart/govern.json" "$SAME"
    fi
fi

# $1=arm dir  $2=port  $3=1 to keep the behavioral_sequences, 0 to drop them.
# The shipped config with its one agent pointed at the stub.
quickstart_config() {
    python3 -c '
import json, sys
cfg = json.load(sys.stdin)
if sys.argv[2] == "0":
    cfg.pop("behavioral_sequences", None)
agent = cfg["agents"]["assistant"]
agent["api_base"] = "http://127.0.0.1:" + sys.argv[1]
agent["api_key_env"] = "README_CLAIMS_FAKE_KEY"
agent["model"] = "stub-model"
sys.stdout.write(json.dumps(cfg, indent=1))
' "$2" "$3" < "$QS/govern.json" > "$1/govern.json"
}

# $1=arm id  $2=program (empty: the shipped one)  $3=keep patterns (1/0)
# -> sets ARM_RC ARM_OUT ARM_REQS
run_stub_arm() {
    local d; d=$(newdir "$1")
    echo '{"responses":[{"content":"ok","output_tokens":5}]}' > "$d/fixture.json"
    if ! start_stub "$d/fixture.json" "$d"; then stop_stub; return 2; fi
    quickstart_config "$d" "$STUB_PORT" "$3"
    if [ -n "$2" ]; then printf '%s\n' "$2" > "$d/prog.naab"; else doc "$QS/agent_task.naab" > "$d/prog.naab"; fi
    echo "app settings" > "$d/app_config.json"
    echo "a note" > "$d/notes.txt"
    ARM_OUT=$(cd "$d" && API_SECRET_KEY=readme-claims-value README_CLAIMS_FAKE_KEY=x \
              timeout 60 "$NAAB" prog.naab </dev/null 2>&1); ARM_RC=$?
    stop_stub
    ARM_REQS=$(find "$d" -maxdepth 1 -name 'req_*.json' | wc -l | tr -d ' ')
    return 0
}

if [ "$IS_WINDOWS" -eq 1 ]; then
    unm "RC-02" "the agent stub is not run on Windows (tests/helpers/stub_platform.sh)"
    unm "RC-03" "the agent stub is not run on Windows"
else
    # shellcheck source=../helpers/stub_launch.sh
    source "$REPO/tests/helpers/stub_launch.sh"
    HERO_TEXT=$(readme_block "" 1 | grep -v '^\$ ' | grep -v '^[[:space:]]*$')

    if ! run_stub_arm rc02c "" 0; then
        unm "RC-02c" "the agent stub did not start"
    elif [ "$ARM_RC" -eq 0 ] && [ "$ARM_REQS" -ge 1 ] && grep -q '^sent: ' <<<"$(lines "$ARM_OUT")"; then
        pass "RC-02c" "control: without the patterns the program sends ($ARM_REQS request(s) reached the stub)"
    else
        fail "RC-02c" "control: without the patterns the program should send" "rc=$ARM_RC requests=$ARM_REQS $(grep -m1 Error <<<"$ARM_OUT")"
    fi

    if ! run_stub_arm rc02 "" 1; then
        unm "RC-02" "the agent stub did not start"
    else
        if [ "$ARM_RC" -eq 3 ] && [ "$ARM_REQS" -eq 0 ] && ! grep -q '^sent: ' <<<"$(lines "$ARM_OUT")"; then
            pass "RC-02a" "the quickstart is refused at the send: exit 3, no request reached the model"
        else
            fail "RC-02a" "the quickstart's pattern should refuse the send" "rc=$ARM_RC requests=$ARM_REQS"
        fi
        MISSING=""
        while IFS= read -r line; do
            grep -qxF -- "$line" <<<"$(lines "$ARM_OUT")" || MISSING="$MISSING | $line"
        done <<<"$HERO_TEXT"
        if [ -z "$HERO_TEXT" ]; then
            fail "RC-02b" "README's hero output block is empty or missing"
        elif [ -z "$MISSING" ]; then
            pass "RC-02b" "every line of the README's hero output is the engine's own text"
        else
            fail "RC-02b" "README hero lines the engine did not print" "$MISSING"
        fi
    fi

    if ! run_stub_arm rc03c "${HARVEST_PROG/__FILE__/notes.txt}" 1; then
        unm "RC-03c" "the agent stub did not start"
    elif [ "$ARM_RC" -eq 0 ] && [ "$ARM_REQS" -ge 1 ]; then
        pass "RC-03c" "control: reading notes.txt then sending is allowed"
    else
        fail "RC-03c" "control: reading a non-config file should send" "rc=$ARM_RC requests=$ARM_REQS $(grep -m1 Error <<<"$ARM_OUT")"
    fi
    if ! run_stub_arm rc03 "${HARVEST_PROG/__FILE__/app_config.json}" 1; then
        unm "RC-03" "the agent stub did not start"
    elif [ "$ARM_RC" -eq 3 ] && [ "$ARM_REQS" -eq 0 ] && grep -qF 'behavioral_sequences.config_harvest' <<<"$ARM_OUT"; then
        pass "RC-03" "the config_harvest pattern refuses a config read followed by a send"
    else
        fail "RC-03" "config_harvest should refuse reading app_config.json then sending" "rc=$ARM_RC requests=$ARM_REQS $(grep -m1 -E 'Error|Rule' <<<"$ARM_OUT")"
    fi
fi

# RC-02e: the README's own "run it" command, as written, from the repository
# root. No stub here: the agent is the real provider, so every proxy variable
# points at a closed port -- if the refusal ever stops working, the request
# fails locally instead of leaving the machine.
RUN_CMD=$(readme | grep -m1 -E '^[A-Z_]+=[^ ]+ \./build/naab-lang examples/quickstart/agent_task\.naab$')
if [ -z "$RUN_CMD" ]; then
    fail "RC-02e" "README no longer gives the command that runs the quickstart"
else
    read -r CMD_ENV CMD_BIN CMD_FILE <<<"$RUN_CMD"
    DEAD=http://127.0.0.1:9
    OUT=$(cd "$REPO" && env -u API_SECRET_KEY "$CMD_ENV" https_proxy=$DEAD HTTPS_PROXY=$DEAD \
          http_proxy=$DEAD HTTP_PROXY=$DEAD all_proxy=$DEAD ALL_PROXY=$DEAD no_proxy= NO_PROXY= \
          timeout 60 "$NAAB" "$CMD_FILE" </dev/null 2>&1); RC=$?
    if [ "$RC" -eq 3 ] && grep -qxF '  Rule: behavioral_sequences.credential_exfiltration' <<<"$(lines "$OUT")"; then
        pass "RC-02e" "the README's command (${RUN_CMD%% *} ...) is refused as shown: exit 3"
    else
        fail "RC-02e" "the README's command did not end as the README shows" "rc=$RC $(grep -m1 -E '^Error' <<<"$OUT")"
    fi
fi

# ---- RC-04: every json block -------------------------------------------------
NJSON=0
for DOCF in "$README" "$TOUR"; do
    DOCN=$(basename "$DOCF")
    for i in $(seq 1 "$(doc_count "$DOCF" json)"); do
        NJSON=$((NJSON + 1))
        BLOCK=$(doc_block "$DOCF" json "$i")
        TITLE="$DOCN json block $i ($(python3 -c 'import json,sys; print(",".join(json.load(sys.stdin).keys()))' <<<"$BLOCK" 2>/dev/null))"
        if ! python3 -c 'import json,sys; json.load(sys.stdin)' <<<"$BLOCK" 2>/dev/null; then
            fail "RC-04" "$TITLE is not valid JSON"; continue
        fi
        COV=$(python3 "$REPO/tools/template_coverage.py" --template - --unread <<<"$BLOCK" 2>&1); COV_RC=$?
        UNREAD=$(grep '^unread' <<<"$COV")
        if [ "$COV_RC" -ne 0 ]; then
            unm "RC-04" "$TITLE: template_coverage.py failed its self-test, keys not checked"
        elif [ -n "$UNREAD" ]; then
            fail "RC-04" "$TITLE has keys the loader never reads" "$(tr '\n' ' ' <<<"$UNREAD")"
        else
            pass "RC-04" "$TITLE: every key is one the loader reads (screened)"
        fi
        D=$(newdir "rc04_${DOCN}_$i")
        printf '%s\n' "$BLOCK" > "$D/govern.json"
        printf 'main {\n    print("RC04_LOADED")\n}\n' > "$D/m.naab"
        OUT=$(cd "$D" && GEMINI_API_KEY=x "$NAAB" m.naab </dev/null 2>&1); RC=$?
        if [ "$RC" -eq 0 ] && grep -qx 'RC04_LOADED' <<<"$(lines "$OUT")" && ! grep -qE "$DEAD_SETTING_RE" <<<"$OUT"; then
            pass "RC-04" "$TITLE loads and runs a program with no dead-setting warning"
        else
            fail "RC-04" "$TITLE did not load cleanly" "rc=$RC $(grep -m2 -E "Error|$DEAD_SETTING_RE" <<<"$OUT")"
        fi
    done
done
[ "$NJSON" -gt 0 ] || fail "RC-04" "no json blocks found (the extractor is broken)"

# ---- RC-05: CI commands ------------------------------------------------------
CI_IDX=$(readme_find bash '--governance-sarif')
D=$(newdir rc05)
echo '{"version": "5.0", "mode": "enforce"}' > "$D/govern.json"
printf 'main {\n    print("ci")\n}\n' > "$D/app.naab"
NCMD=0
if [ "$CI_IDX" -gt 0 ]; then
    while read -r prog file flag path rest; do
        [ "$prog" = "naab-lang" ] && [ "$file" = "app.naab" ] || continue
        case "$flag" in --governance-sarif|--governance-junit|--governance-report) ;; *) continue ;; esac
        NCMD=$((NCMD + 1))
        rm -f "$D/$path"
        OUT=$(run_in "$D" "$NAAB" app.naab "$flag" "$path"); RC=$?
        case "$flag" in
            --governance-sarif)  SIG='"version": "2.1.0"' ;;
            --governance-junit)  SIG='<testsuite' ;;
            --governance-report) SIG='"results"' ;;
        esac
        if [ "$RC" -eq 0 ] && [ -s "$D/$path" ] && grep -qF -- "$SIG" "$D/$path"; then
            pass "RC-05" "naab-lang app.naab $flag $path writes that report"
        else
            fail "RC-05" "naab-lang app.naab $flag $path did not write the report" "rc=$RC $(head -2 <<<"$OUT")"
        fi
    done < <(readme_block bash "$CI_IDX" | grep -v '^#')
fi
if [ "$NCMD" -lt 3 ]; then
    fail "RC-05" "expected 3 report commands in the README's CI block, found $NCMD"
fi
if readme_says "RC-05g" 'naab-gov lint app.naab'; then
    if [ ! -x "$NAAB_GOV" ]; then
        unm "RC-05g" "naab-gov is not built"
    else
        # A clean file passes and a file with a placeholder is refused: exit 0
        # alone would also be what a lint that checks nothing returns.
        L=$(newdir rc05g)
        echo '{"version": "5.0", "mode": "enforce", "code_quality": {"no_placeholders": {"level": "hard"}}}' > "$L/govern.json"
        printf 'main {\n    print("ci")\n}\n' > "$L/app.naab"
        printf 'fn compute(x) {\n    // TODO: implement this\n    return x\n}\nmain {\n    print(compute(1))\n}\n' > "$L/bad.naab"
        OUT=$(run_in "$L" "$NAAB_GOV" lint app.naab); RC=$?
        OUT2=$(run_in "$L" "$NAAB_GOV" lint bad.naab); RC2=$?
        if [ "$RC" -eq 0 ] && [ "$RC2" -eq 3 ] && grep -qF 'no_placeholders' <<<"$OUT2"; then
            pass "RC-05g" "naab-gov lint app.naab passes a clean file and refuses one with a placeholder (exit 3)"
        else
            fail "RC-05g" "naab-gov lint did not tell a clean file from a placeholder" "clean rc=$RC placeholder rc=$RC2"
        fi
    fi
fi

# ---- RC-06: signing ----------------------------------------------------------
SIGN_IDX=$(readme_find bash '--sign-governance')
if [ "$SIGN_IDX" -eq 0 ]; then
    fail "RC-06" "README has no signing commands block"
else
    D=$(newdir rc06); mkdir -p "$D/home" "$D/trust"
    echo '{"version": "5.0", "mode": "enforce"}' > "$D/govern.json"
    printf 'main {\n    print("SIGNED_RUN")\n}\n' > "$D/app.naab"
    SIGN_FAIL=""
    while IFS= read -r cmd; do
        cmd="${cmd%%#*}"
        [ -n "${cmd// /}" ] || continue
        argv=()
        read -r -a argv <<<"$cmd"
        [ "${argv[0]}" = "naab-lang" ] && argv[0]="$NAAB"
        (cd "$D" && HOME="$D/home" NAAB_TRUST_STORE_DIR="$D/trust" "${argv[@]}" </dev/null >"$D/sign.log" 2>&1) \
            || SIGN_FAIL="$SIGN_FAIL | $cmd: $(tail -1 "$D/sign.log")"
    done < <(readme_block bash "$SIGN_IDX")
    if [ -n "$SIGN_FAIL" ]; then
        if [ "$IS_WINDOWS" -eq 1 ]; then unm "RC-06" "signing commands failed on Windows:$SIGN_FAIL"
        else fail "RC-06" "a README signing command failed" "$SIGN_FAIL"; fi
    else
        OUT=$(cd "$D" && HOME="$D/home" NAAB_TRUST_STORE_DIR="$D/trust" "$NAAB" app.naab </dev/null 2>&1); RC=$?
        if [ "$RC" -eq 0 ] && grep -qx 'SIGNED_RUN' <<<"$(lines "$OUT")" && [ -f "$D/govern.json.sig" ]; then
            pass "RC-06a" "the README's signing commands work as written; the signed config runs"
        else
            fail "RC-06a" "the signed config did not run" "rc=$RC $(grep -m1 -E 'Error|INTEGRITY' <<<"$OUT")"
        fi
        printf '{"version": "5.0", "mode": "enforce", "languages": {"allowed": ["python"]}}\n' > "$D/govern.json"
        OUT=$(cd "$D" && HOME="$D/home" NAAB_TRUST_STORE_DIR="$D/trust" "$NAAB" app.naab </dev/null 2>&1); RC=$?
        if [ "$RC" -eq 3 ] && grep -qF 'INTEGRITY BLOCK' <<<"$OUT"; then
            pass "RC-06b" "after an edit, the signed project is an integrity block (exit 3)"
        else
            fail "RC-06b" "an edited signed config should be an integrity block" "rc=$RC $(head -2 <<<"$OUT")"
        fi
    fi
fi

# ---- RC-07: every naab block -------------------------------------------------
# The quickstart program's copy is checked by RC-02 (it needs an agent) and
# skipped here once RC-02d has shown it is the shipped file.
for DOCF in "$README" "$TOUR"; do
    DOCN=$(basename "$DOCF")
    NNAAB=$(doc_count "$DOCF" naab)
    MIN=3; [ "$DOCF" = "$TOUR" ] && MIN=10
    [ "$NNAAB" -ge "$MIN" ] || fail "RC-07" "expected at least $MIN naab blocks in $DOCN, found $NNAAB"
    for i in $(seq 1 "$NNAAB"); do
        BLOCK=$(doc_block "$DOCF" naab "$i")
        if cmp -s <(printf '%s\n' "$BLOCK") <(doc "$QS/agent_task.naab"); then continue; fi
        FIRST=$(grep -m1 -vE '^\s*(//.*)?$' <<<"$BLOCK" | head -c 50)
        D=$(newdir "rc07_${DOCN}_$i")
        printf '%s\n' "$BLOCK" > "$D/block.naab"
        echo "$OPEN_CFG" > "$D/govern.json"
        OUT=$(cd "$D" && timeout 120 "$NAAB" block.naab </dev/null 2>&1); RC=$?
        STDOUT_LINES=$(lines "$OUT")
        if grep -qE "$PY_MISSING_RE" <<<"$OUT" && grep -qF '<<python' <<<"$BLOCK"; then
            unm "RC-07" "$DOCN block $i ($FIRST): no Python executor here"; continue
        fi
        if grep -qF 'http.' <<<"$BLOCK" && grep -qE 'HTTP request failed|Could not resolve|Couldn.t connect' <<<"$OUT"; then
            # The HTTP part cannot be measured here; the rest of the block still
            # can. Drop the paragraph from the "// HTTP" comment to the next
            # blank line and check what remains.
            unm "RC-07" "$DOCN block $i ($FIRST): its HTTP lines need network to api.github.com"
            BLOCK=$(awk '/\/\/ HTTP/ { skip = 1 } skip && /^[[:space:]]*$/ { skip = 0 } !skip' <<<"$BLOCK")
            if grep -qF 'http.' <<<"$BLOCK"; then
                fail "RC-07" "$DOCN block $i: could not separate its HTTP lines (no '// HTTP' paragraph)"; continue
            fi
            printf '%s\n' "$BLOCK" > "$D/block.naab"
            OUT=$(cd "$D" && timeout 120 "$NAAB" block.naab </dev/null 2>&1); RC=$?
            STDOUT_LINES=$(lines "$OUT")
            FIRST="$FIRST, without its HTTP lines"
        fi
        if [ "$RC" -ne 0 ] || grep -q '^Error' <<<"$STDOUT_LINES"; then
            fail "RC-07" "$DOCN block $i ($FIRST) failed" "rc=$RC $(grep -m1 '^Error' <<<"$STDOUT_LINES")"; continue
        fi
        # Values promised by print-line comments: a quoted string or a number.
        WANT_MISS=""; NWANT=0
        while IFS= read -r want; do
            [ -n "$want" ] || continue
            NWANT=$((NWANT + 1))
            grep -qxF -- "$want" <<<"$STDOUT_LINES" || WANT_MISS="$WANT_MISS | $want"
        done < <(sed -nE 's/^.*print\(.*\)[[:space:]]*\/\/[[:space:]]*"(.*)"[[:space:]]*$/\1/p; s/^.*print\(.*\)[[:space:]]*\/\/[[:space:]]*(-?[0-9]+(\.[0-9]+)?)[[:space:]]*$/\1/p' <<<"$BLOCK")
        if [ -n "$WANT_MISS" ]; then
            fail "RC-07" "$DOCN block $i ($FIRST) did not print what its comments say" "$WANT_MISS"
        else
            pass "RC-07" "$DOCN block $i ($FIRST) runs, and prints the $NWANT value(s) its comments promise"
        fi
    done
done

# ---- RC-08: the stdlib table -------------------------------------------------
# One program per module, each function called with no arguments inside a
# try. A missing function says so in its error ("Unknown x function: f",
# "... has no function 'f'", "f(): unknown function"); any other error -- the
# wrong number of arguments -- means the function exists. Functions with side
# effects at no arguments are not called.
D=$(newdir rc08)
echo "$AUDIT_CFG" > "$D/govern.json"
TABLE=$( { readme; doc "$TOUR"; } | grep -E '^\| `[a-z_]+` \| ')
NROWS=$(grep -c . <<<"$TABLE")
NFN=0; MISSING_FN=""
while IFS= read -r row; do
    [ -n "$row" ] || continue
    mod=$(sed -E 's/^\| `([a-z_]+)` .*/\1/' <<<"$row")
    fns=$(sed -E 's/^\| `[a-z_]+` \| (.*) \|$/\1/; s/\([^)]*\)//g; s/,/ /g' <<<"$row")
    prog="use $mod
main {"
    for fn in $fns; do
        case "$mod.$fn" in process.exit|process.kill|io.input|io.read_line|time.sleep) continue ;; esac
        NFN=$((NFN + 1))
        if [ "$mod.$fn" = "math.PI" ] || [ "$mod.$fn" = "math.E" ]; then
            prog="$prog
    print(\"FN=$fn=\" + string($mod.$fn))"
            continue
        fi
        prog="$prog
    try {
        let x = $mod.$fn()
        print(\"FN=$fn=ok\")
    } catch (e) {
        print(\"FN=$fn=\" + string(e))
    }"
    done
    prog="$prog
    print(\"RC08_END\")
}"
    printf '%s\n' "$prog" > "$D/$mod.naab"
    OUT=$(cd "$D" && timeout 60 "$NAAB" "$mod.naab" </dev/null 2>&1)
    if ! grep -qx 'RC08_END' <<<"$(lines "$OUT")"; then
        MISSING_FN="$MISSING_FN | $mod: the probe program did not finish ($(grep -m1 Error <<<"$OUT"))"
        continue
    fi
    for fn in $fns; do
        line=$(grep -m1 "^FN=$fn=" <<<"$OUT")
        case "$mod.$fn" in process.exit|process.kill|io.input|io.read_line|time.sleep) continue ;; esac
        if [ -z "$line" ] || grep -qiE "unknown[^=]*function[^=]*\b$fn\b|no function '$fn'|\b$fn\(\): unknown" <<<"$line"; then
            MISSING_FN="$MISSING_FN | $mod.$fn"
        fi
    done
done <<<"$TABLE"
if [ "$NFN" -lt 150 ]; then
    fail "RC-08" "only $NFN table functions were probed; the table parser is broken"
elif [ -z "$MISSING_FN" ]; then
    pass "RC-08" "all $NFN functions in the $NROWS-row stdlib table exist"
else
    fail "RC-08" "stdlib table lists functions the binary does not have" "$MISSING_FN"
fi

# ---- RC-09: languages --------------------------------------------------------
fold_lang() {
    case "$1" in
        bash|sh|shell) echo shell ;; cs|csharp|c#) echo csharp ;; go|golang) echo go ;;
        javascript|node|js) echo javascript ;; ts|typescript) echo typescript ;;
        sql|sqlite) echo sql ;; c++|cpp) echo cpp ;; *) echo "$1" ;;
    esac
}
D=$(newdir rc09)
echo "$AUDIT_CFG" > "$D/govern.json"
printf 'use codegen\nmain {\n    for l in codegen.supported_languages() { print("LANG=" + l) }\n}\n' > "$D/l.naab"
BIN_LANGS=$(run_in "$D" "$NAAB" l.naab | tr -d '\r' | sed -n 's/^LANG=//p' | while read -r l; do fold_lang "$l"; done | sort -u)
N_LISTS=0
for DOCF in "$README" "$TOUR"; do
    DOCN=$(basename "$DOCF")
    LIST=$(doc "$DOCF" | sed -n 's/^\*\*Supported languages:\*\* //p' | head -1)
    [ -n "$LIST" ] || continue
    N_LISTS=$((N_LISTS + 1))
    DOC_LANGS=$(tr '·' '\n' <<<"$LIST" | sed 's/^ *//; s/ *$//' | grep . | tr 'A-Z' 'a-z' | while read -r l; do fold_lang "$l"; done | sort -u)
    N_DOC=$(grep -c . <<<"$DOC_LANGS")
    if [ -z "$BIN_LANGS" ]; then
        fail "RC-09" "the binary listed no languages"
    elif [ "$DOC_LANGS" != "$BIN_LANGS" ]; then
        if [ "$IS_WINDOWS" -eq 1 ]; then
            unm "RC-09" "$DOCN: this build's language set differs (platform-dependent on Windows)"
        else
            fail "RC-09" "$DOCN's language list is not the binary's" \
                 "doc only: $(comm -23 <(echo "$DOC_LANGS") <(echo "$BIN_LANGS") | tr '\n' ' ') binary only: $(comm -13 <(echo "$DOC_LANGS") <(echo "$BIN_LANGS") | tr '\n' ' ')"
        fi
    else
        pass "RC-09a" "$DOCN's $N_DOC languages are exactly the binary's (aliases folded)"
    fi
    # Ecosystem table rows describe other projects ("8 languages" is NAAb Pivot's).
    for n in $(doc "$DOCF" | grep -v '^| \*\*\[' | grep -oE '\b[0-9]+ languages\b' | grep -oE '^[0-9]+' | sort -u); do
        if [ "$n" = "$N_DOC" ]; then pass "RC-09b" "$DOCN says $n languages, its list names $N_DOC"
        else fail "RC-09b" "$DOCN says $n languages, its list names $N_DOC"; fi
    done
done
[ "$N_LISTS" -gt 0 ] || fail "RC-09" "no 'Supported languages' list found in the README or the tour"

# ---- RC-10: modules ----------------------------------------------------------
TABLE_MODS=$(sed -E 's/^\| `([a-z_]+)` .*/\1/' <<<"$TABLE" | sort -u)
N_TABLE=$(grep -c . <<<"$TABLE_MODS")
STATED=$( { readme; doc "$TOUR"; } | grep -oE '\b[0-9]+[- ]modules?\b' | grep -oE '^[0-9]+' | sort -u)
if [ -z "$STATED" ]; then
    fail "RC-10" "no module count found in the README or the tour (update this arm)"
elif [ "$STATED" = "$N_TABLE" ]; then
    pass "RC-10" "every module count in the README and the tour says $N_TABLE, the table's row count"
else
    fail "RC-10" "module counts disagree with the table" "stated=$(tr '\n' ' ' <<<"$STATED") table=$N_TABLE"
fi
# The binary has no module listing, so the registered set is read from the
# registration lines in stdlib.cpp (screened: a module registered another way
# would be missed). RC-08 already loads every table module with `use`.
REG_MODS=$(sed -nE 's/^[[:space:]]*modules_\["([a-z_]+)"\][[:space:]]*=.*/\1/p' "$REPO/src/stdlib/stdlib.cpp" | sort -u)
if [ -z "$REG_MODS" ]; then
    unm "RC-10b" "no module registrations found in src/stdlib/stdlib.cpp (the reader is out of date)"
elif [ "$REG_MODS" = "$TABLE_MODS" ]; then
    pass "RC-10b" "the table names exactly the $N_TABLE registered modules (screened: read from stdlib.cpp)"
else
    fail "RC-10b" "the table and the registered modules differ" "$(comm -3 <(echo "$REG_MODS") <(echo "$TABLE_MODS") | tr '\n\t' '  ')"
fi

# ---- RC-11: the demo ---------------------------------------------------------
if [ "$IS_WINDOWS" -eq 1 ]; then
    unm "RC-11" "the demo is not run on Windows (its scenes use the shell executor)"
elif [ "$NAAB" != "$REPO/build/naab-lang" ]; then
    unm "RC-11" "the demo runs build/naab-lang, not \$NAAB=$NAAB"
else
    OUT=$(bash "$REPO/demo/governance_demo.sh" --fast </dev/null 2>&1); RC=$?
    if grep -qE "$PY_MISSING_RE" <<<"$OUT" && [ "$RC" -ne 0 ]; then
        unm "RC-11" "the demo needs the Python executor, which is missing here"
    elif [ "$RC" -eq 0 ]; then
        pass "RC-11" "demo/governance_demo.sh passes: every scene blocked for its own reason, scene 7 clean"
    else
        fail "RC-11" "demo/governance_demo.sh failed" "rc=$RC $(grep -m1 -E 'UNEXPECTED|Error' <<<"$OUT")"
    fi
fi

# ---- RC-12: the four enforcement levels --------------------------------------
# One sequence pattern (two environment reads) at each level, around a
# try/catch, so the level alone decides the outcome.
if readme_says "RC-12" '**HARD:**' && readme_says "RC-12" '**SOFT:**' \
   && readme_says "RC-12" '**ADVISORY:**' && readme_says "RC-12" '**DETECT:**'; then
    LV_PROG='use env
main {
    try {
        let a = env.get("README_LEVEL_ONE")
        let b = env.get("README_LEVEL_TWO")
        print("REACHED")
    } catch (e) {
        print("CAUGHT")
    }
    print("AFTER")
}'
    # $1=level $2..=extra flags -> LV_RC, LV_MARKS ("REACHED AFTER" etc.)
    level_run() {
        local d; d=$(newdir "rc12_$1")
        cat > "$d/govern.json" <<J
{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "behavioral_sequences": {"enabled": true, "patterns": [
   {"name": "two_reads", "sequence": ["env.get:*LEVEL_ONE*", "env.get:*LEVEL_TWO*"], "level": "$1", "rationale": "readme claims probe"}]}}
J
        printf '%s\n' "$LV_PROG" > "$d/p.naab"
        shift
        local out; out=$(cd "$d" && "$NAAB" p.naab "$@" </dev/null 2>&1); LV_RC=$?
        LV_MARKS=$(lines "$out" | grep -xE 'REACHED|CAUGHT|AFTER' | tr '\n' ' ' | sed 's/ $//')
    }
    level_run hard;       H=("$LV_RC" "$LV_MARKS")
    level_run hard --governance-override; HO=("$LV_RC" "$LV_MARKS")
    level_run soft;       S=("$LV_RC" "$LV_MARKS")
    level_run soft --governance-override; SO=("$LV_RC" "$LV_MARKS")
    level_run advisory;   A=("$LV_RC" "$LV_MARKS")
    level_run detect;     T=("$LV_RC" "$LV_MARKS")
    if [ "${A[0]}" = 0 ] && [ "${A[1]}" = "REACHED AFTER" ]; then
        pass "RC-12a" "ADVISORY warns and the program continues (control: the pattern alone does not stop it)"
    else
        fail "RC-12a" "ADVISORY should continue" "rc=${A[0]} marks='${A[1]}'"
    fi
    if [ "${H[0]}" = 3 ] && [ -z "${H[1]}" ] && [ "${HO[0]}" = 3 ] && [ -z "${HO[1]}" ]; then
        pass "RC-12h" "HARD exits 3, try/catch does not catch it, and --governance-override does not lift it"
    else
        fail "RC-12h" "HARD should exit 3 uncaught, with or without the override" "rc=${H[0]}/'${H[1]}' override rc=${HO[0]}/'${HO[1]}'"
    fi
    if [ "${S[0]}" = 3 ] && [ -z "${S[1]}" ] && [ "${SO[0]}" = 0 ] && [ "${SO[1]}" = "REACHED AFTER" ]; then
        pass "RC-12s" "SOFT exits 3, and runs with --governance-override"
    else
        fail "RC-12s" "SOFT should exit 3 and yield to the override" "rc=${S[0]}/'${S[1]}' override rc=${SO[0]}/'${SO[1]}'"
    fi
    if [ "${T[0]}" = 0 ] && [ "${T[1]}" = "CAUGHT AFTER" ]; then
        pass "RC-12d" "DETECT blocks, and try/catch catches it"
    else
        fail "RC-12d" "DETECT should be catchable" "rc=${T[0]} marks='${T[1]}'"
    fi
fi

# ---- RC-13: the program cannot read or overwrite its own policy --------------
if readme_says "RC-13" 'the program can neither read nor overwrite them'; then
    D=$(newdir rc13)
    echo '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}}' > "$D/govern.json"
    echo '{"note": "not the policy"}' > "$D/other.json"
    POLICY_BEFORE=$(cat "$D/govern.json")
    printf 'use file\nmain {\n    print("READ=" + file.read("other.json"))\n}\n' > "$D/c.naab"
    printf 'use file\nmain {\n    print("READ=" + file.read("govern.json"))\n}\n' > "$D/r.naab"
    printf 'use file\nmain {\n    file.write("govern.json", "{}")\n    print("WROTE")\n}\n' > "$D/w.naab"
    OUTC=$(run_in "$D" "$NAAB" c.naab); RCC=$?
    OUTR=$(run_in "$D" "$NAAB" r.naab); RCR=$?
    OUTW=$(run_in "$D" "$NAAB" w.naab); RCW=$?
    if [ "$RCC" -ne 0 ] || ! grep -qF 'READ={"note"' <<<"$OUTC"; then
        fail "RC-13" "control: reading another file failed, so the refusals prove nothing" "rc=$RCC"
    elif [ "$RCR" -eq 3 ] && ! grep -qF 'READ=' <<<"$OUTR" && [ "$RCW" -eq 3 ] && [ "$(cat "$D/govern.json")" = "$POLICY_BEFORE" ]; then
        pass "RC-13" "reading and writing govern.json are HARD blocks (exit 3), the file is unchanged, another file reads"
    else
        fail "RC-13" "govern.json should be unreadable and unwritable by the program" "read rc=$RCR write rc=$RCW"
    fi
fi

# ---- RC-14: shell disabled refuses a separate-process language ---------------
if readme_says "RC-14" 'refuses those languages outright'; then
    SH_PROG='main {
    let r = <<shell
echo ran > shell_marker.txt
echo done
>>
    print("AFTER")
}'
    shell_run() {  # $1=true|false -> SH_RC, SH_MARK
        local d; d=$(newdir "rc14_$1")
        echo "{\"version\": \"5.0\", \"mode\": \"enforce\", \"security\": {\"sandbox_level\": \"elevated\"},
  \"languages\": {\"allowed\": [\"shell\"]}, \"capabilities\": {\"shell\": {\"enabled\": $1}}}" > "$d/govern.json"
        printf '%s\n' "$SH_PROG" > "$d/s.naab"
        run_in "$d" "$NAAB" s.naab >/dev/null; SH_RC=$?
        SH_MARK=no; [ -f "$d/shell_marker.txt" ] && SH_MARK=yes
    }
    shell_run true;  ON=("$SH_RC" "$SH_MARK")
    shell_run false; OFF=("$SH_RC" "$SH_MARK")
    if [ "${ON[0]}" != 0 ] || [ "${ON[1]}" != yes ]; then
        unm "RC-14" "control: a <<shell>> block does not run here with shell enabled (rc=${ON[0]})"
    elif [ "${OFF[0]}" -eq 3 ] && [ "${OFF[1]}" = no ]; then
        pass "RC-14" "with shell.enabled false the <<shell>> block is refused (exit 3) and writes nothing; enabled, it runs"
    else
        fail "RC-14" "shell disabled should refuse the block" "rc=${OFF[0]} marker=${OFF[1]}"
    fi
fi

# ---- RC-15: CONTRA-013 -------------------------------------------------------
if readme_says "RC-15" 'NAAb prints the advisory `CONTRA-013` for any config with path rules'; then
    contra_run() {  # $1=arm $2=filesystem json -> CONTRA (count of lines)
        local d; d=$(newdir "rc15_$1")
        echo "{\"version\": \"5.0\", \"mode\": \"enforce\", \"security\": {\"sandbox_level\": \"elevated\"},
  \"capabilities\": {\"filesystem\": $2}}" > "$d/govern.json"
        printf 'main {\n    print("ok")\n}\n' > "$d/h.naab"
        CONTRA=$(run_in "$d" "$NAAB" h.naab | grep -c 'CONTRA-013')
    }
    contra_run with '{"mode": "read", "blocked_paths": ["secret/"]}'; WITH=$CONTRA
    contra_run without '{"mode": "read"}'; WITHOUT=$CONTRA
    if [ "$WITH" -gt 0 ] && [ "$WITHOUT" -eq 0 ]; then
        pass "RC-15" "CONTRA-013 is printed for a config with path rules, and not without them"
    else
        fail "RC-15" "CONTRA-013 should follow the path rules" "with=$WITH without=$WITHOUT"
    fi
fi

# ---- RC-16: the MCP server's tools -------------------------------------------
MCP_LINE=$(readme | grep -m1 -F '`naab-lang mcp` serves')
DOC_TOOLS=$(grep -oE '`naab_[a-z_]+`' <<<"$MCP_LINE" | tr -d '`' | sort -u)
if [ -z "$DOC_TOOLS" ]; then
    fail "RC-16" "README no longer names the MCP server's tools (update this arm)"
else
    D=$(newdir rc16)
    echo '{"version": "5.0", "mode": "enforce"}' > "$D/govern.json"
    BIN_TOOLS=$(cd "$D" && printf '%s\n' \
        '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"readme-claims","version":"1"}}}' \
        '{"jsonrpc":"2.0","method":"notifications/initialized"}' \
        '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}' \
        | timeout 30 "$NAAB" mcp 2>"$D/mcp.err" \
        | python3 -c '
import json, sys
for line in sys.stdin:
    try:
        m = json.loads(line)
    except ValueError:
        continue
    if m.get("id") == 2:
        for t in m.get("result", {}).get("tools", []):
            print(t.get("name"))
' | tr -d '\r' | sort -u)
    # The server is POSIX-only for now (mcp_server.cpp, _WIN32 branch), and
    # the README says so; a build that says it has no server measures nothing.
    if [ -z "$BIN_TOOLS" ] && grep -qF 'is not available on this platform' "$D/mcp.err"; then
        if readme_says "RC-16" 'not available on Windows yet'; then
            unm "RC-16" "naab-lang mcp is not available on this platform, as the README says"
        fi
    elif [ -z "$BIN_TOOLS" ]; then
        fail "RC-16" "the MCP server listed no tools" "$(head -2 "$D/mcp.err")"
    elif [ "$DOC_TOOLS" = "$BIN_TOOLS" ]; then
        pass "RC-16" "naab-lang mcp lists exactly the $(grep -c . <<<"$DOC_TOOLS") tools the README names"
    else
        fail "RC-16" "README's MCP tools differ from the server's" "README: $(tr '\n' ' ' <<<"$DOC_TOOLS") server: $(tr '\n' ' ' <<<"$BIN_TOOLS")"
    fi
fi

# ---- RC-17: the error text the tour quotes -----------------------------------
D=$(newdir rc17)
echo "$AUDIT_CFG" > "$D/govern.json"
ERR_TEXT=$(doc_block "$TOUR" "" 1 | grep -v '^[[:space:]]*$')
if ! grep -qF 'array.append' <<<"$(doc "$TOUR")" || [ -z "$ERR_TEXT" ]; then
    fail "RC-17" "the tour no longer quotes the array.append error (update this arm)"
else
    printf 'main {\n    let a = [1]\n    array.append(a, 2)\n}\n' > "$D/e.naab"
    OUT=$(run_in "$D" "$NAAB" e.naab)
    MISSING=""
    while IFS= read -r line; do
        grep -qxF -- "$line" <<<"$(lines "$OUT")" || MISSING="$MISSING | $line"
    done <<<"$ERR_TEXT"
    if [ -z "$MISSING" ]; then pass "RC-17a" "the tour's quoted error is the engine's text, line for line"
    else fail "RC-17a" "lines of the tour's quoted error the engine did not print" "$MISSING"; fi
fi
# Bullets of the form - `code` → "message": the code, run, prints the message.
NBUL=0; BAD_BUL=""
while IFS=$'\t' read -r code msg; do
    [ -n "$code" ] || continue
    NBUL=$((NBUL + 1))
    printf 'main {\n    %s\n}\n' "$code" > "$D/b$NBUL.naab"
    OUT=$(run_in "$D" "$NAAB" "b$NBUL.naab")
    grep -qF -- "$msg" <<<"$OUT" || BAD_BUL="$BAD_BUL | $code -> $msg"
done < <(doc "$TOUR" | sed -nE 's/^- `([^`]+)` → "([^"]+)".*/\1\t\2/p')
if [ "$NBUL" -eq 0 ]; then
    fail "RC-17b" "no '\`code\` → \"message\"' bullets found in the tour (update this arm)"
elif [ -z "$BAD_BUL" ]; then
    pass "RC-17b" "each of the tour's $NBUL mistake-detection bullets prints the message it quotes"
else
    fail "RC-17b" "mistake-detection bullets whose message the engine did not print" "$BAD_BUL"
fi

# ---- RC-18: counts the README states, read from the source (screened) --------
N_BUILTIN_DOC=$(readme | grep -oE '\b[0-9]+ built-in\b' | grep -oE '^[0-9]+' | sort -u)
N_BUILTIN_SRC=$(awk '/^void BehavioralSequenceDetector::buildDefaultPatterns\(\)/,/^}/' "$REPO/src/runtime/behavioral_sequence.cpp" \
    | grep -oE '\.name *= *"[a-z_]+"' | wc -l | tr -d ' ')
if [ -z "$N_BUILTIN_DOC" ]; then
    fail "RC-18a" "README no longer states how many built-in patterns there are (update this arm)"
elif [ "$N_BUILTIN_SRC" -eq 0 ]; then
    unm "RC-18a" "no pattern names found in buildDefaultPatterns() (the reader is out of date)"
elif [ "$N_BUILTIN_DOC" = "$N_BUILTIN_SRC" ]; then
    pass "RC-18a" "README says $N_BUILTIN_DOC built-in patterns; buildDefaultPatterns() names $N_BUILTIN_SRC (screened)"
else
    fail "RC-18a" "built-in pattern count differs" "README=$N_BUILTIN_DOC source=$N_BUILTIN_SRC"
fi
N_SIG_DOC=$(readme | grep -oE '\b[0-9]+ signals\b' | grep -oE '^[0-9]+' | sort -u)
N_SIG_SRC=$(awk '/kCddSignalKeys\[NUM_CDD_SIGNALS\] = \{/,/^\};/' "$REPO/include/naab/behavioral_sequence.h" \
    | grep -cE '^[[:space:]]*"[a-z_]+",?[[:space:]]*//')
if [ -z "$N_SIG_DOC" ]; then
    fail "RC-18b" "README no longer states how many drift signals there are (update this arm)"
elif [ "$N_SIG_SRC" -eq 0 ]; then
    unm "RC-18b" "no entries found in kCddSignalKeys (the reader is out of date)"
elif [ "$N_SIG_DOC" = "$N_SIG_SRC" ]; then
    pass "RC-18b" "README says $N_SIG_DOC signals; kCddSignalKeys has $N_SIG_SRC (screened)"
else
    fail "RC-18b" "drift signal count differs" "README=$N_SIG_DOC source=$N_SIG_SRC"
fi

# ---- RC-19: Go and PHP blocks without their headers --------------------------
if grep -qF 'a Go block gets `package main` and a PHP block gets `<?php` added' <<<"$(doc "$TOUR")"; then
    hdr_run() {  # $1=lang $2=block body $3=marker
        local d; d=$(newdir "rc19_$1")
        echo "{\"version\": \"5.0\", \"mode\": \"enforce\", \"security\": {\"sandbox_level\": \"elevated\"},
  \"languages\": {\"allowed\": [\"$1\"]}}" > "$d/govern.json"
        printf 'main {\n    let v = <<%s\n%s\n>>\n    print("[" + v + "]")\n}\n' "$1" "$2" > "$d/h.naab"
        local out; out=$(cd "$d" && timeout 120 "$NAAB" h.naab </dev/null 2>&1)
        grep -qF "[$3]" <<<"$(lines "$out")"
    }
    if ! command -v go >/dev/null 2>&1; then unm "RC-19g" "go is not installed here"
    elif hdr_run go 'import "fmt"
func main() { fmt.Println("readme go ok") }' 'readme go ok'; then
        pass "RC-19g" "a Go block with no package line runs"
    else fail "RC-19g" "a Go block with no package line did not run"; fi
    if ! command -v php >/dev/null 2>&1; then unm "RC-19p" "php is not installed here"
    elif hdr_run php 'echo "readme php ok";' 'readme php ok'; then
        pass "RC-19p" "a PHP block with no <?php tag runs"
    else fail "RC-19p" "a PHP block with no <?php tag did not run"; fi
else
    fail "RC-19" "the tour no longer says Go and PHP blocks get their headers (update this arm)"
fi

# ---- RC-20: live guidance ----------------------------------------------------
if readme_says "RC-20" 'The program may not write a source'; then
    D=$(newdir rc20)
    cat > "$D/govern.json" <<'J'
{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "code_quality": {"no_placeholders": {"level": "hard"}},
 "guidance": {"sources": ["guide.md"]}}
J
    printf '# Project playbook\n\n## No placeholders\n\nRC20_GUIDANCE_MARKER: write the function in full.\n' > "$D/guide.md"
    GUIDE_BEFORE=$(cat "$D/guide.md")
    printf 'fn compute(x) {\n    // TODO: implement this\n    return x\n}\nmain {\n    print(compute(1))\n}\n' > "$D/p.naab"
    printf 'use file\nmain {\n    file.write("notes.md", "x")\n    print("WROTE")\n}\n' > "$D/wc.naab"
    printf 'use file\nmain {\n    file.write("guide.md", "x")\n    print("WROTE")\n}\n' > "$D/w.naab"
    OUT=$(run_in "$D" "$NAAB" p.naab); RC=$?
    if [ "$RC" -eq 3 ] && grep -qF 'Project guidance (guide.md > No placeholders):' <<<"$OUT" && grep -qF 'RC20_GUIDANCE_MARKER' <<<"$OUT"; then
        pass "RC-20a" "a violation shows the guidance section named for its rule under the message"
    else
        fail "RC-20a" "the violation did not show its guidance section" "rc=$RC"
    fi
    OUTC=$(run_in "$D" "$NAAB" wc.naab); RCC=$?
    OUTW=$(run_in "$D" "$NAAB" w.naab); RCW=$?
    if [ "$RCC" -ne 0 ] || ! grep -qx 'WROTE' <<<"$(lines "$OUTC")"; then
        fail "RC-20b" "control: writing an ordinary file failed, so the refusal proves nothing" "rc=$RCC"
    elif [ "$RCW" -eq 3 ] && [ "$(cat "$D/guide.md")" = "$GUIDE_BEFORE" ]; then
        pass "RC-20b" "writing a guidance source is a HARD block (exit 3); an ordinary file writes"
    else
        fail "RC-20b" "the program should not be able to write a guidance source" "rc=$RCW"
    fi
fi

# ---- RC-21: secrets in agent replies -----------------------------------------
if readme_says "RC-21" '`code_quality.no_secrets` (off by default; when on, a hit ends the run)'; then
    if [ "$IS_WINDOWS" -eq 1 ]; then
        unm "RC-21" "the agent stub is not run on Windows"
    else
        # A key-shaped value built at run time (the precedent in test_guidance.sh),
        # so no literal secret sits in this file.
        FAKE_KEY="AKIA$(printf 'Q%.0s' $(seq 1 16))"
        sec_run() {  # $1=arm $2=code_quality json fragment -> SEC_RC SEC_OUT
            local d; d=$(newdir "$1")
            printf '{"responses":[{"content":"here is the key %s","output_tokens":9}]}' "$FAKE_KEY" > "$d/fixture.json"
            if ! start_stub "$d/fixture.json" "$d"; then stop_stub; return 2; fi
            echo "{\"version\": \"5.0\", \"mode\": \"enforce\" $2,
  \"agents\": {\"assistant\": {\"provider\": \"gemini\", \"model\": \"stub-model\",
    \"api_base\": \"http://127.0.0.1:$STUB_PORT\", \"api_key_env\": \"README_CLAIMS_FAKE_KEY\", \"max_tokens\": 50}}}" > "$d/govern.json"
            printf 'use agent\nmain {\n    let h = agent.create("assistant")\n    let r = agent.send(h, "hello")\n    print("REPLY_RECEIVED")\n}\n' > "$d/p.naab"
            SEC_OUT=$(cd "$d" && README_CLAIMS_FAKE_KEY=x timeout 60 "$NAAB" p.naab </dev/null 2>&1); SEC_RC=$?
            stop_stub
            return 0
        }
        if ! sec_run rc21_off ""; then unm "RC-21" "the agent stub did not start"
        else
            OFF=("$SEC_RC" "$(grep -c '^REPLY_RECEIVED' <<<"$(lines "$SEC_OUT")")")
            if sec_run rc21_on ', "code_quality": {"no_secrets": {"level": "hard"}}'; then
                if [ "${OFF[0]}" = 0 ] && [ "${OFF[1]}" = 1 ] && [ "$SEC_RC" -eq 3 ] \
                   && grep -qF 'code_quality.no_secrets' <<<"$SEC_OUT" && ! grep -q '^REPLY_RECEIVED' <<<"$(lines "$SEC_OUT")"; then
                    pass "RC-21" "a reply holding a key reaches the program by default; with no_secrets on, the run ends (exit 3)"
                else
                    fail "RC-21" "reply secret scanning should be off by default and end the run when on" "off rc=${OFF[0]} on rc=$SEC_RC"
                fi
            else unm "RC-21" "the agent stub did not start"; fi
        fi
    fi
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
