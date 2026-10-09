#!/usr/bin/env bash
# ============================================================
# test_pipefail_grep.sh -- no `PRODUCER | grep -q` under pipefail
#
# THE DEFECT CLASS
#
# Under `set -o pipefail` a pipeline's status is the last non-zero status in
# it, so a pipeline ending in `grep -q` does not answer "was the pattern
# found":
#
#   * when the producer exits non-zero, the pipeline fails although grep
#     matched. `"$NAAB" prog | grep -q blocked` is false for every program
#     that prints "blocked" and exits 3; `! "$NAAB" prog | grep -q SECRET`
#     then PASSES whether or not the secret was printed;
#   * when the producer is still writing after grep has matched and exited,
#     its write fails: it takes SIGPIPE (141), or -- where SIGPIPE is
#     ignored, as on GitHub's runners -- gets EPIPE, prints "write error:
#     Broken pipe" and exits 1. Above the pipe's capacity that is certain;
#     below it, it is a race -- a flake.
#
#     "$NAAB" prog 2>&1 | grep -q blocked        <- defect
#     grep -q blocked <<<"$("$NAAB" prog 2>&1)"   <- safe: no pipe, no producer
#
# WHY A GUARD AND NOT A NOTE. The trap is written down in CLAUDE.md (a test's
# OUTPUT CHANNEL, third shape) and in docs/investigation-method.md, and was
# fixed site by site four times (c47eefc7, 05f26e02, 7cc189db, 24d6d1d8). The
# tree still held 1,414 sites when this guard landed. One of them made
# test_r13_fixes.sh T-API2-1 SKIP on every run: `strings naab-lang | grep -q`
# fails under pipefail because strings is still writing.
#
# ZERO, NOT A BASELINE. Every site was converted when this landed, and the
# fix is mechanical (`tools/testlint/pipefail_grep.py fix` rewrites the
# echo/printf ones), so a new site is a regression, not legacy.
#
#   PG-00  the scanner's own self-test (planted sites found, look-alikes not)
#   PG-01  PREMISE, measured here: under pipefail a producer that exits
#          non-zero after printing the match makes `| grep -q` false, and the
#          here-string form true. If bash ever stops doing this the guard is
#          moot, and this arm says so instead of passing silently
#   PG-02  PREMISE, measured here: a producer that outgrows the pipe makes
#          `| grep -q` false (141, or 1 where SIGPIPE is ignored) --
#          deterministic, not a race
#   PG-03  the tree holds no site (tests/ tools/ examples/ .github/ .claude/
#          run-all-tests.sh, plus helpers those suites source)
#   PG-04  POSITIVE CONTROL: a site planted into a REAL registered suite is
#          found, at its line. Without this, a scanner broken into silence
#          reports a clean tree
#   PG-05  NEGATIVE CONTROL: the same real suite unmodified is not flagged
#   PG-06  the rewrite `fix` performs changes the verdict: the planted
#          `echo "$X" | grep -q` is false on a 1 MB X before, true after, and
#          the rewritten text is no longer flagged
#
# Scripts go to the scanner on STDIN, never as /tmp paths: under MSYS2
# python3 is a native Windows build and cannot open an MSYS path
# (test_shell_path_handoff.sh). The repo scan passes only relative paths.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO" || exit 1

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

TOOL="tools/testlint/pipefail_grep.py"
if ! command -v python3 >/dev/null 2>&1; then
    echo "  SKIP [PG-*] python3 not found -- UNMEASURABLE, not a pass"; exit 0
fi
if [ ! -f "$TOOL" ]; then
    bad "PG-00" "scanner $TOOL is missing" "every arm below would be vacuous"
    echo ""; echo "pipefail grep -q guard: $PASS passed, $FAIL failed"; exit 1
fi

echo "=== pipefail + grep -q guard ==="

# PG-00 -- the scanner decides its planted cases right.
ST_OUT="$(python3 "$TOOL" --selftest 2>&1)"; ST_RC=$?
if [ "$ST_RC" -eq 0 ] && grep -q ', 0 failed$' <<<"$ST_OUT"; then
    ok "PG-00" "scanner self-test: $(grep 'case(s)' <<<"$ST_OUT")"
else
    bad "PG-00" "scanner self-test failed (rc=$ST_RC)" "$(grep -v '^ok' <<<"$ST_OUT" | head -5)"
fi

# PG-01 -- exit-status inversion. Each form is run in a subshell with
# pipefail on, and its status captured; the producer prints the match.
( set -o pipefail; (printf 'match\n'; exit 3) | grep -q match ); PIPED=$?
( set -o pipefail; grep -q match <<<"$(printf 'match\n'; exit 3)" ); HERE=$?
if [ "$PIPED" -ne 0 ] && [ "$HERE" -eq 0 ]; then
    ok "PG-01" "producer exiting 3 after the match: piped form $PIPED, here-string form 0"
else
    bad "PG-01" "premise did not reproduce: piped=$PIPED here-string=$HERE" \
        "expected piped non-zero and here-string 0; if bash changed, re-read this guard's purpose"
fi

# PG-02 -- SIGPIPE. seq writes ~1.3 MB; grep -q matches the first line and
# exits, so seq is writing into a closed pipe.
( set -o pipefail; seq 1 200000 2>/dev/null | grep -q '^1$' ); PIPED=$?
( set -o pipefail; grep -q '^1$' <<<"$(seq 1 200000)" ); HERE=$?
if [ "$PIPED" -ne 0 ] && [ "$HERE" -eq 0 ]; then
    ok "PG-02" "producer outgrowing the pipe: piped form $PIPED, here-string form 0"
else
    bad "PG-02" "premise did not reproduce: piped=$PIPED here-string=$HERE" \
        "expected piped non-zero (141 = SIGPIPE, or 1 = EPIPE where SIGPIPE is ignored) and here-string 0"
fi

# PG-03 -- the tree. This file is excluded by name, and only this file: PG-01
# and PG-02 above ARE the defect, run on purpose to measure it.
SCAN="$(python3 "$TOOL" scan tests tools examples .github .claude run-all-tests.sh \
        --exclude tests/self-audit/test_pipefail_grep.sh 2>&1)"; SCAN_RC=$?
if [ "$SCAN_RC" -eq 0 ] && grep -q '^0 site(s) left' <<<"$SCAN"; then
    ok "PG-03" "no PRODUCER | grep -q pipeline under pipefail in the tree"
else
    bad "PG-03" "pipelines ending in grep -q under pipefail (rc=$SCAN_RC):" \
        "$(head -20 <<<"$SCAN")"
    echo "       Fix: capture, then match without a pipe:"
    echo "         grep -q PAT <<<\"\$OUT\"      or      grep -q PAT <<<\"\$(cmd 2>&1)\""
    echo "       echo/printf sites are rewritten by: python3 $TOOL fix <path>"
fi

# PG-04 / PG-05 -- a real registered suite, with and without a planted site.
# Any pipefail suite would do; this one is registered and is not edited by
# this change, so its text is independent of the fix.
REAL="tests/self-audit/test_shell_path_handoff.sh"
if [ ! -f "$REAL" ] || ! grep -q 'pipefail' "$REAL"; then
    bad "PG-04" "control suite $REAL missing or no longer sets pipefail" \
        "pick another registered pipefail suite; without one PG-04/05 prove nothing"
else
    LINES=$(wc -l < "$REAL" | tr -d ' ')
    PLANT='if "$NAAB" probe.naab 2>&1 | grep -qi blocked; then :; fi'
    OUT4="$( { cat "$REAL"; printf '%s\n' "$PLANT"; } | python3 "$TOOL" scan-stdin 2>&1)"
    if grep -q "^<stdin>:$((LINES + 1)): \[command\]" <<<"$OUT4" \
       && grep -q '^1 site(s) left' <<<"$OUT4"; then
        ok "PG-04" "site planted at line $((LINES + 1)) of a real suite is found there"
    else
        bad "PG-04" "planted site not found at line $((LINES + 1))" "$(head -5 <<<"$OUT4")"
    fi
    OUT5="$(python3 "$TOOL" scan-stdin < "$REAL" 2>&1)"
    if grep -q '^0 site(s) left' <<<"$OUT5"; then
        ok "PG-05" "the same suite unmodified is not flagged"
    else
        bad "PG-05" "false positive on an unmodified real suite" "$(head -5 <<<"$OUT5")"
    fi
fi

# PG-06 -- the mechanical rewrite fixes the verdict, measured. The snippet is
# run as written and as `fix-stdin` rewrites it; X is 1 MB with the match on
# its first line, so the piped form's write fails every time. Only stdout
# is compared: where SIGPIPE is ignored (GitHub's runners) echo also prints
# "write error: Broken pipe" on stderr, and that line is not the verdict.
SNIP='set -o pipefail
X="match
$(head -c 1048576 /dev/zero | tr "\0" x)"
if echo "$X" | grep -q match; then echo FOUND; else echo MISSED; fi'
FIXED="$(printf '%s\n' "$SNIP" | python3 "$TOOL" fix-stdin 2>&1)"
BEFORE="$(bash -c "$SNIP" 2>/dev/null)"
AFTER="$(bash -c "$FIXED" 2>/dev/null)"
RESCAN="$(printf '%s\n' "$FIXED" | python3 "$TOOL" scan-stdin 2>&1)"
if [ "$BEFORE" = "MISSED" ] && [ "$AFTER" = "FOUND" ] \
   && grep -q 'grep <<<"$X" -q match' <<<"$FIXED" && grep -q '^0 site(s) left' <<<"$RESCAN"; then
    ok "PG-06" "rewrite turns MISSED into FOUND on a 1 MB match and is not re-flagged"
else
    bad "PG-06" "rewrite did not fix the verdict: before=$BEFORE after=$AFTER" \
        "rewritten line: $(grep 'grep' <<<"$FIXED" | head -1); rescan: $(tail -1 <<<"$RESCAN")"
fi

echo ""
echo "pipefail grep -q guard: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
