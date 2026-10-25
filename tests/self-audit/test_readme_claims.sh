#!/usr/bin/env bash
# ============================================================
# test_readme_claims.sh -- does README.md still describe the binary?
#
# The README is what a visitor reads first, and nothing checked it. Reviewed
# against the build on 2026-10-24, it had drifted in every section: the hero
# output was not the engine's text (and the rule name in the real text was
# printed as "<redacted>"), the CI commands wrote a JSON report to a file named
# "sarif", four keys of the config example were read by nothing, the signing
# commands failed as written, the pipeline example errored on both engines,
# optional chaining gave the wrong value on the VM, and the stdlib table listed
# functions that do not exist. Each was fixed; this suite is what keeps them
# fixed. It reads README.md itself -- its code blocks, its commands, its tables
# -- so a claim edited in the README is checked as edited.
#
# Arms (each an observation of the binary, never of the README alone):
#   RC-00  the binary runs a program at all (without it every arm is vacuous)
#   RC-01  Quick Start: no govern.json refuses (exit 4, names `naab-lang init`);
#          after `naab-lang init`, the README's hello.naab prints its greeting
#   RC-02  the hero: README's first config + the three-step program is refused
#          with exit 3, no request reaches the (stub) model, and every line of
#          the README's hero output appears verbatim in the engine's output.
#          RC-02c: the same program without the patterns does send.
#   RC-03  the README's second pattern (config_harvest) refuses a config read
#          followed by a send; RC-03c: reading another file still sends
#   RC-04  every ```json block: valid, every key read by the loader
#          (tools/template_coverage.py --unread, screened), and loads with no
#          "has no effect" / "not a setting" warning
#   RC-05  the CI commands write the report each one names, in that format
#   RC-06  the signing commands run as written; a signed config runs, an edited
#          one is an INTEGRITY BLOCK
#   RC-07  every ```naab block runs (exit 0, no Error:), and every value its
#          print-line comments promise (`print(x)  // "B"`) is printed
#   RC-08  every function in the stdlib table exists in its module
#   RC-09  the README's language list is the binary's language set (aliases
#          folded), and the At a Glance count matches it
#   RC-10  the module count stated is the table's, and both lists agree
#   RC-11  the demo script passes (every scene blocked for its own reason)
#
# Outcomes are PASS, FAIL or UNMEASURABLE. UNMEASURABLE is for a missing
# prerequisite (no embedded Python, no network, the agent stub on Windows), and
# it says which; it is never used for a claim that did not hold.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
NAAB_GOV="${NAAB_GOV:-$REPO/build/naab-gov}"
README="${README_UNDER_TEST:-$REPO/README.md}"   # override: mutation checks of this suite

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
echo "=== README claims vs the binary ==="

if [ ! -f "$README" ]; then
    fail "SETUP" "README.md not found at $README"
    echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
fi

# ---- README access, in the shell's own path vocabulary (no native python
# opens a path here: under MSYS2 it could not). CR is dropped so a CRLF
# checkout reads the same as an LF one.
readme() { tr -d '\r' < "$README"; }

# readme_block LANG N -> the Nth fenced block whose info string is LANG
# ("" = a bare ``` fence). Every fence toggles, so a closing fence is never
# mistaken for an opening one.
readme_block() {
    readme | awk -v lang="$1" -v want="$2" '
        /^```/ { if (open) { open = 0; cur = 0; next }
                 open = 1; tag = substr($0, 4)
                 if (tag == lang) { n++; if (n == want) cur = 1 }
                 next }
        cur { print }'
}
readme_count() {
    readme | awk -v lang="$1" '
        /^```/ { if (open) { open = 0; next } open = 1; if (substr($0, 4) == lang) n++; next }
        END { print n + 0 }'
}
# The block of LANG containing a fixed string; prints its index (0 = none).
readme_find() {
    local lang="$1" needle="$2" n i
    n=$(readme_count "$lang")
    for i in $(seq 1 "$n"); do
        if grep -qF -- "$needle" <<<"$(readme_block "$lang" "$i")"; then echo "$i"; return; fi
    done
    echo 0
}

newdir() { local d="$WORK/$1"; rm -rf "$d"; mkdir -p "$d"; echo "$d"; }
run_in() { local d="$1"; shift; (cd "$d" && "$@" </dev/null 2>&1); }

# A config under which README code blocks can run: the languages they use, the
# environment readable, the network allowed for the http example.
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
if grep -qx 'RC00_MARKER_42' <<<"$(tr -d '\r' <<<"$OUT")"; then
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
    EXPECT_HELLO=$(readme_block naab "$HELLO_IDX" | sed -n 's/.*print("Hello, " + name + "!").*/x/p')
    D=$(newdir rc01b)
    run_in "$D" "$NAAB" init >/dev/null
    readme_block naab "$HELLO_IDX" > "$D/hello.naab"
    OUT=$(run_in "$D" "$NAAB" hello.naab); RC=$?
    if [ ! -f "$D/govern.json" ]; then
        fail "RC-01b" "naab-lang init wrote no govern.json"
    elif [ "$RC" -eq 0 ] && grep -qx 'Hello, World!' <<<"$(tr -d '\r' <<<"$OUT")" && [ -n "$EXPECT_HELLO" ]; then
        pass "RC-01b" "after naab-lang init, the README's hello.naab prints Hello, World!"
    else
        fail "RC-01b" "hello.naab after init did not print Hello, World!" "rc=$RC $(grep -m2 -E 'Error|Hello' <<<"$OUT")"
    fi
fi

# ---- RC-02 / RC-03: the hero, against a stub model --------------------------
HERO_PROG='use env
use crypto
use agent
main {
    let secret = env.get("API_SECRET_KEY")
    let encoded = crypto.base64_encode(secret)
    let h = agent.create("worker")
    let r = agent.send(h, encoded)
    print("SENT")
}'
HARVEST_PROG='use file
use agent
main {
    let cfg = file.read("__FILE__")
    let h = agent.create("worker")
    let r = agent.send(h, cfg)
    print("SENT")
}'
HERO_CFG_IDX=$(readme_find json '"credential_exfiltration"')

# $1=arm dir  $2=port  $3=1 to keep the README's behavioral_sequences, 0 to drop it
hero_config() {
    python3 -c '
import json, sys
cfg = json.load(sys.stdin)
if sys.argv[2] == "0":
    cfg.pop("behavioral_sequences", None)
cfg["agents"] = {"worker": {"provider": "gemini", "model": "stub-model",
    "api_base": "http://127.0.0.1:" + sys.argv[1], "api_key_env": "README_CLAIMS_FAKE_KEY",
    "max_tokens": 50}}
sys.stdout.write(json.dumps(cfg, indent=1))
' "$2" "$3" < <(readme_block json "$HERO_CFG_IDX") > "$1/govern.json"
}

# $1=arm id  $2=program  $3=keep patterns (1/0)  -> sets ARM_RC ARM_OUT ARM_REQS
run_stub_arm() {
    local d; d=$(newdir "$1")
    echo '{"responses":[{"content":"ok","output_tokens":5}]}' > "$d/fixture.json"
    if ! start_stub "$d/fixture.json" "$d"; then stop_stub; return 2; fi
    hero_config "$d" "$STUB_PORT" "$3"
    printf '%s\n' "$2" > "$d/prog.naab"
    echo "app settings" > "$d/app_config.json"
    echo "a note" > "$d/notes.txt"
    ARM_OUT=$(cd "$d" && API_SECRET_KEY=readme-claims-value README_CLAIMS_FAKE_KEY=x \
              timeout 60 "$NAAB" prog.naab </dev/null 2>&1); ARM_RC=$?
    stop_stub
    ARM_REQS=$(find "$d" -maxdepth 1 -name 'req_*.json' | wc -l | tr -d ' ')
    return 0
}

if [ "$HERO_CFG_IDX" -eq 0 ]; then
    fail "RC-02" "README has no config block with the credential_exfiltration pattern"
elif [ "$IS_WINDOWS" -eq 1 ]; then
    unm "RC-02" "the agent stub is not run on Windows (tests/helpers/stub_platform.sh)"
    unm "RC-03" "the agent stub is not run on Windows"
else
    # shellcheck source=../helpers/stub_launch.sh
    source "$REPO/tests/helpers/stub_launch.sh"
    HERO_TEXT=$(readme_block "" 1 | grep -v '^\$ ' | grep -v '^[[:space:]]*$')

    if ! run_stub_arm rc02c "$HERO_PROG" 0; then
        unm "RC-02c" "the agent stub did not start"
    elif [ "$ARM_RC" -eq 0 ] && [ "$ARM_REQS" -ge 1 ] && grep -qx 'SENT' <<<"$ARM_OUT"; then
        pass "RC-02c" "control: without the patterns the program sends ($ARM_REQS request(s) reached the stub)"
    else
        fail "RC-02c" "control: without the patterns the program should send" "rc=$ARM_RC requests=$ARM_REQS $(grep -m1 Error <<<"$ARM_OUT")"
    fi

    if ! run_stub_arm rc02 "$HERO_PROG" 1; then
        unm "RC-02" "the agent stub did not start"
    else
        if [ "$ARM_RC" -eq 3 ] && [ "$ARM_REQS" -eq 0 ] && ! grep -qx 'SENT' <<<"$ARM_OUT"; then
            pass "RC-02a" "the README's pattern refuses the send: exit 3, no request reached the model"
        else
            fail "RC-02a" "the README's pattern should refuse the send" "rc=$ARM_RC requests=$ARM_REQS"
        fi
        MISSING=""
        while IFS= read -r line; do
            grep -qxF -- "$line" <<<"$(tr -d '\r' <<<"$ARM_OUT")" || MISSING="$MISSING | $line"
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

# ---- RC-04: every json block -------------------------------------------------
NJSON=$(readme_count json)
if [ "$NJSON" -eq 0 ]; then fail "RC-04" "README has no json blocks (the extractor is broken)"; fi
for i in $(seq 1 "$NJSON"); do
    BLOCK=$(readme_block json "$i")
    TITLE="json block $i ($(python3 -c 'import json,sys; print(",".join(json.load(sys.stdin).keys()))' <<<"$BLOCK" 2>/dev/null))"
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
    D=$(newdir "rc04_$i")
    printf '%s\n' "$BLOCK" > "$D/govern.json"
    printf 'main {\n    print("RC04_LOADED")\n}\n' > "$D/m.naab"
    OUT=$(cd "$D" && GEMINI_API_KEY=x "$NAAB" m.naab </dev/null 2>&1); RC=$?
    if [ "$RC" -eq 0 ] && grep -qx 'RC04_LOADED' <<<"$(tr -d '\r' <<<"$OUT")" && ! grep -qE "$DEAD_SETTING_RE" <<<"$OUT"; then
        pass "RC-04" "$TITLE loads and runs a program with no dead-setting warning"
    else
        fail "RC-04" "$TITLE did not load cleanly" "rc=$RC $(grep -m2 -E "Error|$DEAD_SETTING_RE" <<<"$OUT")"
    fi
done

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
if grep -qF 'naab-gov lint app.naab' "$README"; then
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
        if [ "$RC" -eq 0 ] && grep -qx 'SIGNED_RUN' <<<"$(tr -d '\r' <<<"$OUT")" && [ -f "$D/govern.json.sig" ]; then
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
NNAAB=$(readme_count naab)
[ "$NNAAB" -ge 10 ] || fail "RC-07" "expected at least 10 naab blocks in the README, found $NNAAB"
for i in $(seq 1 "$NNAAB"); do
    BLOCK=$(readme_block naab "$i")
    FIRST=$(grep -m1 -vE '^\s*(//.*)?$' <<<"$BLOCK" | head -c 50)
    D=$(newdir "rc07_$i")
    printf '%s\n' "$BLOCK" > "$D/block.naab"
    echo "$OPEN_CFG" > "$D/govern.json"
    OUT=$(cd "$D" && timeout 120 "$NAAB" block.naab </dev/null 2>&1); RC=$?
    STDOUT_LINES=$(tr -d '\r' <<<"$OUT")
    if grep -qE "$PY_MISSING_RE" <<<"$OUT" && grep -qF '<<python' <<<"$BLOCK"; then
        unm "RC-07" "block $i ($FIRST): no Python executor here"; continue
    fi
    if grep -qF 'http.' <<<"$BLOCK" && grep -qE 'HTTP request failed|Could not resolve|Couldn.t connect' <<<"$OUT"; then
        # The HTTP part cannot be measured here; the rest of the block still
        # can. Drop the paragraph from the "// HTTP" comment to the next blank
        # line and check what remains.
        unm "RC-07" "block $i ($FIRST): its HTTP lines need network to api.github.com"
        BLOCK=$(awk '/\/\/ HTTP/ { skip = 1 } skip && /^[[:space:]]*$/ { skip = 0 } !skip' <<<"$BLOCK")
        if grep -qF 'http.' <<<"$BLOCK"; then
            fail "RC-07" "block $i: could not separate its HTTP lines (no '// HTTP' paragraph)"; continue
        fi
        printf '%s\n' "$BLOCK" > "$D/block.naab"
        OUT=$(cd "$D" && timeout 120 "$NAAB" block.naab </dev/null 2>&1); RC=$?
        STDOUT_LINES=$(tr -d '\r' <<<"$OUT")
        FIRST="$FIRST, without its HTTP lines"
    fi
    if [ "$RC" -ne 0 ] || grep -q '^Error' <<<"$STDOUT_LINES"; then
        fail "RC-07" "block $i ($FIRST) failed" "rc=$RC $(grep -m1 '^Error' <<<"$STDOUT_LINES")"; continue
    fi
    # Values promised by print-line comments: a quoted string or a number.
    WANT_MISS=""; NWANT=0
    while IFS= read -r want; do
        [ -n "$want" ] || continue
        NWANT=$((NWANT + 1))
        grep -qxF -- "$want" <<<"$STDOUT_LINES" || WANT_MISS="$WANT_MISS | $want"
    done < <(sed -nE 's/^.*print\(.*\)[[:space:]]*\/\/[[:space:]]*"(.*)"[[:space:]]*$/\1/p; s/^.*print\(.*\)[[:space:]]*\/\/[[:space:]]*(-?[0-9]+(\.[0-9]+)?)[[:space:]]*$/\1/p' <<<"$BLOCK")
    if [ -n "$WANT_MISS" ]; then
        fail "RC-07" "block $i ($FIRST) did not print what its comments say" "$WANT_MISS"
    else
        pass "RC-07" "block $i ($FIRST) runs, and prints the $NWANT value(s) its comments promise"
    fi
done

# ---- RC-08: the stdlib table -------------------------------------------------
# One program per module, each function called with no arguments inside a
# try. A missing function says so in its error ("Unknown x function: f",
# "... has no function 'f'", "f(): unknown function"); any other error -- the
# wrong number of arguments -- means the function exists. Functions with side
# effects at no arguments are not called.
D=$(newdir rc08)
echo "$AUDIT_CFG" > "$D/govern.json"
TABLE=$(readme | grep -E '^\| `[a-z_]+` \| ')
NROWS=$(grep -c . <<<"$TABLE")
NFN=0; MISSING_FN=""
while IFS= read -r row; do
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
    if ! grep -qx 'RC08_END' <<<"$(tr -d '\r' <<<"$OUT")"; then
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
README_LANGS=$(readme | sed -n 's/^\*\*Supported languages:\*\* //p' | tr '·' '\n' \
    | sed 's/^ *//; s/ *$//' | grep . | tr 'A-Z' 'a-z' | while read -r l; do fold_lang "$l"; done | sort -u)
D=$(newdir rc09)
echo "$AUDIT_CFG" > "$D/govern.json"
printf 'use codegen\nmain {\n    for l in codegen.supported_languages() { print("LANG=" + l) }\n}\n' > "$D/l.naab"
BIN_LANGS=$(run_in "$D" "$NAAB" l.naab | tr -d '\r' | sed -n 's/^LANG=//p' | while read -r l; do fold_lang "$l"; done | sort -u)
N_README=$(grep -c . <<<"$README_LANGS")
GLANCE_N=$(readme | grep -m1 '^| \*\*Polyglot Execution\*\*' | grep -oE '[0-9]+ languages' | grep -oE '[0-9]+')
if [ -z "$BIN_LANGS" ]; then
    fail "RC-09" "the binary listed no languages"
elif [ "$README_LANGS" != "$BIN_LANGS" ]; then
    if [ "$IS_WINDOWS" -eq 1 ]; then
        unm "RC-09" "this build's language set differs from the README's (platform-dependent on Windows)"
    else
        fail "RC-09" "README's language list is not the binary's" \
             "README only: $(comm -23 <(echo "$README_LANGS") <(echo "$BIN_LANGS") | tr '\n' ' ') binary only: $(comm -13 <(echo "$README_LANGS") <(echo "$BIN_LANGS") | tr '\n' ' ')"
    fi
else
    pass "RC-09a" "README's $N_README languages are exactly the binary's (aliases folded)"
fi
if [ "${GLANCE_N:-x}" = "$N_README" ]; then
    pass "RC-09b" "At a Glance says $GLANCE_N languages, the list names $N_README"
else
    fail "RC-09b" "At a Glance language count does not match the list" "glance=${GLANCE_N:-none} list=$N_README"
fi

# ---- RC-10: modules ----------------------------------------------------------
TABLE_MODS=$(sed -E 's/^\| `([a-z_]+)` .*/\1/' <<<"$TABLE" | sort -u)
GLANCE_MODS=$(readme | grep -m1 '^| \*\*Standard Library\*\*' | sed -E 's/.* modules — //; s/ \|$//' | tr ',' '\n' | sed 's/^ *//; s/ *$//' | sort -u)
STATED=$(readme | grep -oE '\b[0-9]+ (standard library )?modules\b' | grep -oE '^[0-9]+' | sort -u)
N_TABLE=$(grep -c . <<<"$TABLE_MODS")
if [ "$STATED" = "$N_TABLE" ] && [ "$TABLE_MODS" = "$GLANCE_MODS" ]; then
    pass "RC-10" "every module count in the README says $N_TABLE, and both module lists agree"
else
    fail "RC-10" "module counts or lists disagree" "stated=$(tr '\n' ' ' <<<"$STATED") table=$N_TABLE glance-vs-table: $(comm -3 <(echo "$GLANCE_MODS") <(echo "$TABLE_MODS") | tr '\n\t' '  ')"
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

echo ""
echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
