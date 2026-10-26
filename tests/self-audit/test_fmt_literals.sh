#!/usr/bin/env bash
# ============================================================
# test_fmt_literals.sh -- every literal fmt format string is valid (A42)
#
# THE DEFECT CLASS
#
# fmt checks a format string at COMPILE time only under C++20. NAAb builds as
# C++17, so `fmt::format("returns {valid, error}", name)` compiles and throws
# at run time -- and the strings most likely to hold a stray `{` are help and
# error messages, which is where a reader is told what went wrong. A42 found
# two, both dead since they were written:
#
#   agent_impl.cpp  agent.<unknown>()'s help lists `returns {valid, error}`:
#                   fmt reads `{valid` as a named argument, so a typo such as
#                   agent.sned() printed "argument not found" instead of the
#                   list of functions (both engines)
#   main.cpp        `naab-lang blocks test` prints `main { ... }`: "invalid
#                   format string", and the process aborts (exit 134)
#
# The checker is tools/fmtcheck/fmt_literals.py (screened: a small tokenizer,
# not a compiler). Its `--cxx20` mode is the authoritative cross-check -- every
# unit through `-std=gnu++20 -fsyntax-only`, where fmt's consteval checker
# rejects these -- too slow for this suite (about 4 minutes on 4 cores) and
# blind to LOG_*, whose format reaches fmt through a std::string.
#
#   FL-00  the scanner's own self-test (planted defects flagged, look-alikes
#          not: escapes, specs, nested width, a brace in an ARGUMENT, a
#          runtime format, a commented-out call)
#   FL-01  PROBE USABILITY: the tree scan read thousands of literal formats.
#          A scanner broken into seeing nothing would otherwise report a
#          clean tree
#   FL-02  the tree holds no invalid literal format
#   FL-03  POSITIVE CONTROL on the REAL files: both A42 texts planted back
#          into agent_impl.cpp and main.cpp are found, at their sites
#   FL-04  POSITIVE CONTROL for the LOG_* path, in a real file that uses it
#   FL-05  the symptom, both engines: agent.<unknown>() prints its help
#   FL-06  the symptom: `blocks test` prints its text instead of aborting
#   FL-07  CONTROL for FL-05: a known agent function still runs (the help
#          arm would also "pass" if the module were missing entirely)
#
# Planted copies go to the scanner on STDIN, never as /tmp paths: under MSYS2
# python3 is a native Windows build and cannot open an MSYS path.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO" || exit 1

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2 -- UNMEASURABLE, not a pass"; }

TOOL="tools/fmtcheck/fmt_literals.py"
NAAB="$REPO/build/naab-lang"
if ! command -v python3 >/dev/null 2>&1; then
    echo "  SKIP [FL-*] python3 not found -- UNMEASURABLE, not a pass"; exit 0
fi
if [ ! -f "$TOOL" ]; then
    bad "FL-00" "scanner $TOOL is missing" "every arm below would be vacuous"
    echo ""; echo "fmt literal guard: $PASS passed, $FAIL failed"; exit 1
fi

echo "=== fmt literal format strings ==="

# FL-00
ST_OUT="$(python3 "$TOOL" --selftest 2>&1)"; ST_RC=$?
if [ "$ST_RC" -eq 0 ] && grep -q ', 0 wrong$' <<<"$ST_OUT"; then
    ok "FL-00" "scanner self-test: $(grep '^selftest:' <<<"$ST_OUT")"
else
    bad "FL-00" "scanner self-test failed (rc=$ST_RC)" "$(grep -v '^  ok' <<<"$ST_OUT" | head -5)"
fi

# FL-01 / FL-02
SCAN="$(python3 "$TOOL" 2>&1)"; SCAN_RC=$?
CHECKED="$(sed -n 's/.* \([0-9][0-9]*\) literal format(s) checked.*/\1/p' <<<"$SCAN")"
if [ -n "$CHECKED" ] && [ "$CHECKED" -ge 2000 ]; then
    ok "FL-01" "the scan read $CHECKED literal format strings"
else
    bad "FL-01" "the scan read ${CHECKED:-no} literal format strings (expected 2000+)" \
        "the extractor is broken; FL-02 below would be vacuous: $(tail -2 <<<"$SCAN")"
fi
if [ "$SCAN_RC" -eq 0 ] && grep -q '^0 problem(s)$' <<<"$SCAN"; then
    ok "FL-02" "no invalid literal format string in the tree"
else
    bad "FL-02" "invalid literal format strings (rc=$SCAN_RC):" "$(head -20 <<<"$SCAN")"
    echo "       A literal brace in a format string is written {{ or }}."
fi

# FL-03 -- plant both A42 texts back into the real files.
plant() { # FILE FIXED_TEXT BROKEN_TEXT -> the planted file on stdout; rc 1 if FIXED_TEXT absent
    # Bytes in, bytes out: the sources hold non-ASCII (em dashes, arrows), and
    # Python's text stdout uses the locale's encoding -- cp1252 under MSYS2,
    # ASCII under a C locale -- which cannot write them (UnicodeEncodeError,
    # first seen on build-windows; reproduce with PYTHONUTF8=0
    # PYTHONCOERCECLOCALE=0 LC_ALL=C).
    python3 - "$1" "$2" "$3" <<'PY'
import sys
path, fixed, broken = sys.argv[1:4]
text = open(path, "rb").read()
fixed_b, broken_b = fixed.encode("utf-8"), broken.encode("utf-8")
if text.count(fixed_b) != 1:
    sys.exit(1)
sys.stdout.buffer.write(text.replace(fixed_b, broken_b))
PY
}
# rc 1 = the text is not in the source; anything else = the helper itself broke.
plant_problem() { # LABEL RC
    if [ "$2" -eq 1 ]; then echo "$1: the fixed text is no longer in the source"; else echo "$1: the planting helper failed (rc=$2)"; fi
}
A_FIXED='returns {{valid, error}}\n"'
A_BROKEN='returns {valid, error}\n"'
M_FIXED='main {{ let result = alias.function_name(args) }}'
M_BROKEN='main { let result = alias.function_name(args) }'
A_PLANT="$(plant src/stdlib/agent_impl.cpp "$A_FIXED" "$A_BROKEN")"; A_RC=$?
M_PLANT="$(plant src/cli/main.cpp "$M_FIXED" "$M_BROKEN")"; M_RC=$?
if [ "$A_RC" -ne 0 ] || [ "$M_RC" -ne 0 ]; then
    bad "FL-03" "could not plant the A42 texts (agent rc=$A_RC, main rc=$M_RC)" \
        "$( [ "$A_RC" -ne 0 ] && plant_problem agent_impl.cpp "$A_RC"; [ "$M_RC" -ne 0 ] && plant_problem main.cpp "$M_RC")"
else
    A_OUT="$(python3 "$TOOL" --label src/stdlib/agent_impl.cpp - <<<"$A_PLANT" 2>&1)"
    M_OUT="$(python3 "$TOOL" --label src/cli/main.cpp - <<<"$M_PLANT" 2>&1)"
    if grep -q '^src/stdlib/agent_impl.cpp:[0-9]*: fmt::format: .*{valid' <<<"$A_OUT" \
       && grep -q '^1 problem(s)$' <<<"$A_OUT" \
       && grep -q '^src/cli/main.cpp:[0-9]*: fmt::print: .*main {' <<<"$M_OUT" \
       && grep -q '^1 problem(s)$' <<<"$M_OUT"; then
        ok "FL-03" "both A42 texts, planted into the real files, are found (and nothing else)"
    else
        bad "FL-03" "a planted A42 text was not found" "agent: $(tail -3 <<<"$A_OUT") | main: $(tail -3 <<<"$M_OUT")"
    fi
fi

# FL-04 -- the LOG_* macros forward their format through a std::string, so
# only this scanner checks them. Plant one missing argument into a real user.
L_FIXED='LOG_DEBUG("[Manifest] Found: {}\n", manifest_path.string());'
L_BROKEN='LOG_DEBUG("[Manifest] Found: {} in {}\n", manifest_path.string());'
L_PLANT="$(plant src/manifest/manifest_loader.cpp "$L_FIXED" "$L_BROKEN")"; L_RC=$?
if [ "$L_RC" -ne 0 ]; then
    bad "FL-04" "could not plant the LOG_DEBUG defect (rc=$L_RC)" \
        "$(plant_problem manifest_loader.cpp "$L_RC")"
else
    L_OUT="$(python3 "$TOOL" --label src/manifest/manifest_loader.cpp - <<<"$L_PLANT" 2>&1)"
    if grep -q 'LOG_DEBUG: 2 field(s) but 1 argument(s)' <<<"$L_OUT" && grep -q '^1 problem(s)$' <<<"$L_OUT"; then
        ok "FL-04" "a LOG_DEBUG with a missing argument, planted into a real file, is found"
    else
        bad "FL-04" "the planted LOG_DEBUG defect was not found" "$(tail -3 <<<"$L_OUT")"
    fi
fi

# FL-05..FL-07 -- the symptom, through the binary.
if [ ! -x "$NAAB" ]; then
    skip "FL-05..07" "build/naab-lang not built"
else
    # The unsigned govern.json below must not meet a populated trust store.
    source "$REPO/tests/helpers/trust_setup.sh"
    setup_isolated_trust
    W="$(mktemp -d)"; trap 'rm -rf "$W"; teardown_isolated_trust' EXIT
    printf '{"version":"4.0","mode":"enforce","security":{"sandbox_level":"elevated"}}\n' > "$W/govern.json"
    printf 'use agent\nmain {\n    agent.sned("x")\n}\n' > "$W/unknown.naab"
    printf 'use agent\nuse io\nmain {\n    io.write("got=" + agent.extract_code("```py\\nx=1\\n```", "py") + "\\n")\n}\n' > "$W/known.naab"
    # The tree-walker is refused under enforce (exit 4); these arms are about
    # its help text, so it runs beside a mode-off copy of the config.
    . "$REPO/tests/helpers/tree_walk_refusal.sh"
    mkdir -p "$W/off"; cp "$W/govern.json" "$W/unknown.naab" "$W/known.naab" "$W/off/"
    tw_off_config "$W/off/govern.json" || bad "FL-05/tree-walk" "could not write the mode-off config"
    for eng in vm tree-walk; do
        flag=""; D="$W"; [ "$eng" = "tree-walk" ] && { flag="--tree-walk"; D="$W/off"; }
        OUT="$(cd "$D" && "$NAAB" run $flag unknown.naab 2>&1)"; RC=$?
        if [ "$RC" -eq 1 ] && grep -q "Unknown function 'agent.sned'" <<<"$OUT" \
           && grep -q 'returns {valid, error}' <<<"$OUT" && ! grep -q 'argument not found' <<<"$OUT"; then
            ok "FL-05/$eng" "agent.sned() prints its help (rc=1), braces rendered"
        else
            bad "FL-05/$eng" "agent.sned() did not print its help (rc=$RC)" "$(head -3 <<<"$OUT")"
        fi
        OUT="$(cd "$D" && "$NAAB" run $flag known.naab 2>&1)"; RC=$?
        if [ "$RC" -eq 0 ] && grep -q '^got=x=1$' <<<"$OUT"; then
            ok "FL-07/$eng" "control: agent.extract_code() runs"
        else
            bad "FL-07/$eng" "control: agent.extract_code() did not run (rc=$RC)" "$(head -3 <<<"$OUT")"
        fi
    done
    OUT="$(cd "$W" && HOME="$W/home" "$NAAB" blocks test 2>&1)"; RC=$?
    if [ "$RC" -eq 1 ] && grep -q 'main { let result = alias.function_name(args) }' <<<"$OUT"; then
        ok "FL-06" "blocks test prints its text (rc=1, not an abort)"
    else
        bad "FL-06" "blocks test did not print its text (rc=$RC)" "$(tail -3 <<<"$OUT")"
    fi
fi

echo ""
echo "fmt literal guard: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
