#!/bin/bash
# tools/govmeasure: the instrument that measures what the block checks catch
# against labels made outside this repository (Bundle 13 PR 2). Report only.
# This suite tests the INSTRUMENT, with no network and no dataset: the
# measurement itself runs nightly (.github/workflows/governance-measure.yml).
#
#   GM-01   the scorer's own self-test: Wilson interval, a stand-in checker of
#           known behaviour scored exactly (15/20, 2/20, precision 15/17), an
#           always-firing rule (difference 0), a background rule not counted,
#           and the UNMEASURABLE outcomes (dead checker, failed control,
#           unreadable answers, a checker that never flags)
#   GM-02   the Juliet adapter's self-test: variants split, comments and
#           guards removed, shared text kept in both
#   GM-03   the EvalPlus labeller's self-test: right/wrong/raising/missing/
#           endless/huge solutions judged; a child with no verdict is an
#           ERROR, never a pass (UNMEASURABLE on Windows: POSIX limits)
#   GM-04   the real naab-gov on six fixture blocks: the two rules the map
#           names flag the three stubs (2 + 1) and none of the three plain
#           blocks -- the scorer reads the real binary's answers
#   GM-04c  CONTROL: the same fixture with its labels reversed still exits 0
#           and reports 0/3 caught, 3/3 false alarms: the exit status never
#           depends on a rate, and the numbers follow the labels
#   GM-05   --plant dead (a checker that prints nothing): exit 2,
#           UNMEASURABLE, and no table
#   GM-06   a control the checker does not flag: exit 2, the control named
#   GM-07   datasets.py: a file matching its pin is kept (PASS); a pin that
#           does not match removes the file and exits 1; a download that
#           fails exits 2 (UNMEASURABLE)
#   GM-08   usage: no cases file, a file that is not JSON lines -> exit 4
#   GM-09   the nightly workflow runs on a schedule or by hand, never on a
#           pull request, and fetches through datasets.py (structural)
#   GM-10   naab_lint.py (A132: what the lint-only checks flag in NAAb
#           code): its self-test against the real naab-gov -- a flaw in NAAb
#           text counts both ways, one inside a polyglot block counts for the
#           whole file only, a file lint cannot parse is left out and
#           counted, and a lint that prints nothing or never flags the
#           planted file is UNMEASURABLE
#   GM-11   semgrep_tests.py: its self-test on a test file written here in
#           Semgrep's marking -- one case per marked place, marker comments
#           removed, each case its own stretch, todo marks left out, only
#           `security` directories read, a rule with no CWE left out
#   GM-12   datasets.py, a git source: a repository at its pinned commit is
#           kept; a pin that is not the checkout's commit is UNMEASURABLE
#           (git cannot fetch it) and leaves nothing behind
#
# Kill table (each mutant applied to tools/govmeasure, this suite re-run;
# observed 2026-11-11):
#   background rules counted as findings            -> GM-01 (ST-04), GM-04
#   the dead-checker probe skipped                  -> GM-01 (ST-05; GM-05 stays
#       green: every case is then unreadable, which is still exit 2)
#   a failed control ignored                        -> GM-01 (ST-06), GM-06
#   labels ignored (every case scored as bad)       -> GM-01 (ST-02), GM-04, GM-04c
#   exit 1 when nothing mapped is caught            -> GM-04c
#   the hash comparison skipped in datasets.py      -> GM-07
#   a child with no verdict counted as PASS         -> GM-03 (ET-07)
#   comments kept in Juliet variants                -> GM-02 (JT-03)
#   naab_lint: block bodies not emptied              -> GM-10 (NL-01, NL-05, NL-06)
#   naab_lint: an unparseable file counted           -> GM-10 (NL-03)
#   naab_lint: the two controls skipped              -> GM-10 (NL-08)
#   semgrep_tests: an end-of-line mark keeps its comment -> GM-11
#   semgrep_tests: every directory read              -> GM-11
#   semgrep_tests: todo marks taken as labels        -> GM-11
#   datasets.py: a stale checkout kept               -> GM-12
#   datasets.py: the fetched commit not compared     -> GM-12
#   datasets.py: verify does not compare the commit  -> GM-12
#   scorer: --group ignored                          -> GM-01
# Red first: every arm fails on master, which has no tools/govmeasure.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
GOV="${NAAB_GOV:-$ROOT/build/naab-gov}"
T="$ROOT/tools/govmeasure"
FX="$SCRIPT_DIR/govmeasure_fixture"

PASS=0; FAIL=0; SKIP=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       | /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2 -- UNMEASURABLE"; }
has() { grep -qF -- "$2" <<<"$1"; }

echo "=== govmeasure: the instrument behind 'what governance catches' (Bundle 13) ==="
if ! command -v python3 >/dev/null 2>&1; then
    echo "  FAIL [setup] python3 not found -- UNMEASURABLE, not a pass"; exit 1
fi
W="$(mktemp -d "${TMPDIR:-/tmp}/govmeasure-XXXXXX")"
trap 'rm -rf "$W"' EXIT

run() { OUT="$("$@" 2>"$W/err.txt")"; RC=$?; OUT="${OUT//$'\r'/}"; ERR="$(cat "$W/err.txt")"; }

# --- GM-01..03: the tools' own self-tests --------------------------------
run python3 "$T/govmeasure.py" selftest
if [ "$RC" = 0 ] && has "$OUT" "selftest: 0 failed" && [ "$(grep -c '  PASS \[ST-' <<<"$OUT")" = 11 ]; then
    pass GM-01 "the scorer's self-test: 11 arms"
else fail GM-01 "the scorer's self-test" "$OUT"; fi

run python3 "$T/juliet.py" selftest
if [ "$RC" = 0 ] && has "$OUT" "selftest: 0 failed" && [ "$(grep -c '  PASS \[JT-' <<<"$OUT")" = 7 ]; then
    pass GM-02 "the Juliet adapter's self-test: 7 arms"
else fail GM-02 "the Juliet adapter's self-test" "$OUT"; fi

run python3 "$T/evalplus.py" selftest
if [ "$RC" = 2 ] && has "$OUT" "UNMEASURABLE on Windows"; then
    skip GM-03 "the labelling child needs POSIX resource limits"
elif [ "$RC" = 0 ] && has "$OUT" "selftest: 0 failed" && [ "$(grep -c '  PASS \[ET-' <<<"$OUT")" = 9 ]; then
    pass GM-03 "the EvalPlus labeller's self-test: 9 arms"
else fail GM-03 "the EvalPlus labeller's self-test" "$OUT"; fi

# --- GM-04..06: the real naab-gov ---------------------------------------
if [ ! -x "$GOV" ] && [ ! -x "$GOV.exe" ]; then
    for id in GM-04 GM-04c GM-05 GM-06; do skip "$id" "naab-gov is not built"; done
else
    SC=(python3 "$T/govmeasure.py" score)
    run "${SC[@]}" "$FX/cases.jsonl" --gov "$GOV" --map "$FX/map.json" --json "$W/r.json"
    if [ "$RC" = 0 ] && has "$OUT" "| (any mapped rule) | 3/3 (100.0%)" &&
       has "$OUT" "| code_quality.no_placeholders * | 2/3" &&
       has "$OUT" "| code_quality.no_oversimplification * | 1/3" &&
       has "$OUT" "| 0/3 (0.0%)" && has "$OUT" "python TODO stub (fired)" &&
       ! grep -q '^| contradiction' <<<"$OUT"; then
        pass GM-04 "the real naab-gov: the mapped rules flag 3 of 3 stubs and 0 of 3 plain blocks"
    else fail GM-04 "the fixture's report is not as expected (rc=$RC)" "$OUT
$ERR"; fi

    python3 "$FX/reverse_labels.py" "$FX/cases.jsonl" "$W/reversed.jsonl"
    run "${SC[@]}" "$W/reversed.jsonl" --gov "$GOV" --map "$FX/map.json"
    if [ "$RC" = 0 ] && has "$OUT" "| (any mapped rule) | 0/3 (0.0%)" &&
       has "$OUT" "| 3/3 (100.0%) | 43.8-100.0% | -100.0 pts | 0.0% |"; then
        pass GM-04c "CONTROL: labels reversed -> 0/3 caught, 3/3 false alarms, and still exit 0"
    else fail GM-04c "reversed labels should reverse the numbers and still exit 0 (rc=$RC)" "$OUT"; fi

    run "${SC[@]}" "$FX/cases.jsonl" --gov "$GOV" --map "$FX/map.json" --plant dead
    if [ "$RC" = 2 ] && has "$OUT" "UNMEASURABLE" && ! has "$OUT" "| rule |" && has "$ERR" "UNMEASURABLE"; then
        pass GM-05 "a checker that prints nothing: exit 2, UNMEASURABLE, no table"
    else fail GM-05 "a dead checker should be UNMEASURABLE (rc=$RC)" "$OUT"; fi

    run "${SC[@]}" "$FX/cases.jsonl" --gov "$GOV" --map "$FX/map_bad_control.json"
    if [ "$RC" = 2 ] && has "$OUT" "plain block" && ! has "$OUT" "| rule |"; then
        pass GM-06 "a control the checker does not flag: exit 2, the control named, no table"
    else fail GM-06 "a failed control should be UNMEASURABLE (rc=$RC)" "$OUT"; fi
fi

# --- GM-10: the A132 measurement tool ------------------------------------
if [ ! -x "$GOV" ] && [ ! -x "$GOV.exe" ]; then
    skip GM-10 "naab-gov is not built"
else
    run python3 "$T/naab_lint.py" selftest --gov "$GOV"
    if [ "$RC" = 0 ] && has "$OUT" "selftest: 0 failed" && [ "$(grep -c '  PASS \[NL-' <<<"$OUT")" = 8 ]; then
        pass GM-10 "the A132 tool's self-test against the real naab-gov: 8 arms"
    else fail GM-10 "the A132 tool's self-test" "$OUT"; fi
fi

# --- GM-11: the Semgrep test-file adapter --------------------------------
run python3 "$T/semgrep_tests.py" selftest
if [ "$RC" = 0 ] && has "$OUT" "selftest: 0 failed" && [ "$(grep -c '  PASS \[SG-' <<<"$OUT")" = 7 ]; then
    pass GM-11 "the Semgrep adapter's self-test: 7 arms"
else fail GM-11 "the Semgrep adapter's self-test" "$OUT"; fi

# --- GM-12: a git source pinned to a commit ---------------------------------
if ! command -v git >/dev/null 2>&1; then
    skip GM-12 "git not found"
else
    mkdir -p "$W/gs/tool" "$W/gs/origin" "$W/gs/dl"
    cp "$T/datasets.py" "$W/gs/tool/"
    ( cd "$W/gs/origin" && git init -q && git config user.email t@example.invalid && git config user.name t &&
      git config uploadpack.allowAnySHA1InWant true &&
      printf 'one\n' > a.txt && git add a.txt && git commit -q -m one ) >/dev/null 2>&1
    C1="$(git -C "$W/gs/origin" rev-parse HEAD 2>/dev/null)"
    if [ -z "$C1" ]; then
        skip GM-12 "could not build the local repository"
    else
        BR="$(git -C "$W/gs/origin" symbolic-ref --short HEAD 2>/dev/null)"
        ( cd "$W/gs" && python3 - "$C1" "${BR:-master}" <<'PY'
import json, pathlib, sys
origin = pathlib.Path("origin").resolve().as_uri()
json.dump({"good": {"git": origin, "commit": sys.argv[1], "what": "the pinned commit"},
           "wrong": {"git": origin, "commit": "0" * 40, "what": "a commit the repository does not have"},
           "branch": {"git": origin, "commit": sys.argv[2], "what": "a branch name, which moves, in place of a commit"}},
          open("tool/datasets.json", "w"))
PY
        )
        (cd "$W/gs" && python3 tool/datasets.py fetch dl good >g1.txt 2>&1); g1=$?
        (cd "$W/gs" && python3 tool/datasets.py fetch dl wrong >g2.txt 2>&1); g2=$?
        (cd "$W/gs" && python3 tool/datasets.py fetch dl branch >g5.txt 2>&1); g5=$?
        # The pin moves to a second commit: the stale checkout must be
        # replaced by it, and `verify` against a pin the checkout is not at
        # must not pass.
        ( cd "$W/gs/origin" && printf 'two\n' > b.txt && git add b.txt && git commit -q -m two ) >/dev/null 2>&1
        C2="$(git -C "$W/gs/origin" rev-parse HEAD 2>/dev/null)"
        ( cd "$W/gs" && python3 - "$C2" <<'PY'
import json, pathlib, sys
origin = pathlib.Path("origin").resolve().as_uri()
json.dump({"good": {"git": origin, "commit": sys.argv[1], "what": "the pin, moved"}}, open("tool/datasets.json", "w"))
PY
        )
        (cd "$W/gs" && python3 tool/datasets.py verify dl good >g3.txt 2>&1); g3=$?
        (cd "$W/gs" && python3 tool/datasets.py fetch dl good >g4.txt 2>&1); g4=$?
        if [ "$g1" = 2 ]; then
            skip GM-12 "git could not fetch the local control repository ($(head -1 "$W/gs/g1.txt"))"
        elif [ "$g1" = 0 ] && grep -q '^PASS good' "$W/gs/g1.txt" &&
             [ "$g2" != 0 ] && ! grep -q '^PASS' "$W/gs/g2.txt" && [ ! -e "$W/gs/dl/wrong/a.txt" ] &&
             [ "$g5" != 0 ] && ! grep -q '^PASS' "$W/gs/g5.txt" && [ ! -e "$W/gs/dl/branch/a.txt" ] &&
             [ "$g3" != 0 ] && ! grep -q '^PASS' "$W/gs/g3.txt" &&
             [ "$g4" = 0 ] && [ -f "$W/gs/dl/good/b.txt" ]; then
            pass GM-12 "a repository at its pinned commit is kept; a commit it does not have is not; a stale checkout fails verify and is replaced on fetch"
        else
            fail GM-12 "the git source was not handled as pinned (good rc=$g1, wrong rc=$g2, branch rc=$g5, verify-stale rc=$g3, refetch rc=$g4)" "$(cat "$W/gs"/g?.txt)"
        fi
    fi
fi

# --- GM-07: datasets pinned by hash ---------------------------------------
if ! command -v curl >/dev/null 2>&1; then
    skip GM-07 "curl not found"
else
    mkdir -p "$W/ds/tool" "$W/ds/src" "$W/ds/dl"
    cp "$T/datasets.py" "$W/ds/tool/"
    printf 'pinned bytes\n' > "$W/ds/src/a.zip"
    (cd "$W/ds" && python3 - <<'PY'
import hashlib, json, os, pathlib
src = pathlib.Path("src/a.zip").resolve()
h = hashlib.sha256(src.read_bytes()).hexdigest()
url = src.as_uri()
json.dump({"good": {"url": url, "sha256": h, "what": "matches"},
           "changed": {"url": url, "sha256": "0" * 64, "what": "pin does not match"},
           "gone": {"url": url + ".absent", "sha256": h, "what": "cannot be downloaded"}},
          open("tool/datasets.json", "w"))
PY
    )
    ok=1; why=""
    (cd "$W/ds" && python3 tool/datasets.py fetch dl good >o1.txt 2>&1); r1=$?
    if [ "$r1" = 2 ]; then
        # The control itself could not be fetched: this curl does not read a
        # file:// URL in the form this python writes it (a Windows runner).
        skip GM-07 "curl could not fetch the local control file ($(head -1 "$W/ds/o1.txt"))"
        ok=skip
    else
    (cd "$W/ds" && python3 tool/datasets.py fetch dl changed >o2.txt 2>&1); r2=$?
    (cd "$W/ds" && python3 tool/datasets.py fetch dl gone >o3.txt 2>&1); r3=$?
    { [ "$r1" = 0 ] && grep -q '^PASS good' "$W/ds/o1.txt" && [ -f "$W/ds/dl/good.zip" ]; } || { ok=0; why="$why good(rc=$r1)"; }
    { [ "$r2" = 1 ] && grep -q '^FAIL changed' "$W/ds/o2.txt" && [ ! -e "$W/ds/dl/changed.zip" ]; } || { ok=0; why="$why changed(rc=$r2)"; }
    { [ "$r3" = 2 ] && grep -q '^UNMEASURABLE gone' "$W/ds/o3.txt" && [ ! -e "$W/ds/dl/gone.zip" ]; } || { ok=0; why="$why gone(rc=$r3)"; }
    fi
    if [ "$ok" = skip ]; then :
    elif [ "$ok" = 1 ]; then pass GM-07 "a matching file is kept; a pin mismatch removes it (exit 1); a failed download is UNMEASURABLE (exit 2)"
    else fail GM-07 "datasets.py did not treat:$why" "$(cat "$W/ds"/o?.txt)"; fi
fi

# --- GM-08: usage ---------------------------------------------------------
printf 'not json\n' > "$W/bad.jsonl"
python3 "$T/govmeasure.py" score >/dev/null 2>&1; u1=$?
python3 "$T/govmeasure.py" score "$W/bad.jsonl" >/dev/null 2>&1; u2=$?
if [ "$u1" = 4 ] && [ "$u2" = 4 ]; then pass GM-08 "no cases file, and a file that is not JSON lines, are exit 4"
else fail GM-08 "usage errors: no file rc=$u1, not JSON rc=$u2 (want 4 and 4)"; fi

# --- GM-09: the workflow (structural) ---------------------------------------
WF="$ROOT/.github/workflows/governance-measure.yml"
if [ ! -f "$WF" ]; then fail GM-09 "the nightly workflow is missing"
elif grep -q 'schedule:' "$WF" && grep -q 'workflow_dispatch' "$WF" && ! grep -qE '^\s*(pull_request|push):' "$WF" &&
     grep -q 'datasets.py fetch' "$WF"; then
    pass GM-09 "the measurement runs on a schedule or by hand, never on a pull request, through datasets.py"
else fail GM-09 "the workflow's triggers or its fetch step are not as decided"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
