#!/usr/bin/env bash
# ============================================================
# test_codemap_coverage.sh -- `codemap coverage` / tested / untested / impact
#
# tools/codemap/gcovmap.py maps gcov's per-function execution counts onto the
# index by FILE and LINE, one GCOV_PREFIX tree per test unit, and `impact` joins
# that with a change. A coverage claim that is wrong in the reassuring direction
# ("tested") hides untested code; one wrong the other way sends a reviewer
# after code that runs. Each rule is checked on the codemap fixture, compiled
# with --coverage and run as three "units":
#
#   A  ./fixture            the ordinary run
#   B  ./fixture refuse     refuse(), then _exit(3) -- a HARD block's shape
#   (no GCOV_PREFIX)        an ordinary run whose environment lost the prefix
#
#   C-01  refuse() is executed by B only -- its counters survive _exit(3)
#         because src/testing/coverage_exit.c (the NAAB_COVERAGE wrap) dumps
#         them; control C-01c: the same build WITHOUT the wrap loses B's run
#         entirely, so C-01 is the wrap's doing
#   C-02  sink is executed by A and B, with counts; dead_function by none
#   C-03  lambdas and overloads are placed by position: via_two_lambdas::b
#         and over(int)/over(string)/over(double) get their own counts (2/1/0);
#         one_liner and the lambda on its line are told apart (b)
#   C-04  never_linked() (extra.cpp, indexed, linked into nothing) reads NOT
#         IN THE BUILD, never "executed by no unit"
#   C-05  untested lists dead_function and over(double), not sink
#   C-06  a run that lost GCOV_PREFIX is collected as (unattributed), not
#         credited to a unit and not dropped
#   C-07  impact: uncommitted edits to sink, dead_function and a line INSIDE
#         via_two_lambdas' body -> all three changed, the units listed,
#         dead_function under "NO unit executes"; control: impact of an
#         unchanged tree touches nothing
#   C-08  impact on a stale index says so (the file is newer than the index);
#         a changed file the index does not hold (.gitignore) is not "stale"
#   C-09  ingest without a coverage build is refused with the build command
#   C-11  a source file edited after the instrumented build compiled it: gcov's
#         line numbers are the build's, so its functions read UNMEASURED and
#         the ingest says STALE BUILD -- never credited by a position that
#         may now hold another function; control (c): other.cpp, unedited, is
#         still measured; (b) after lines are inserted above everything, no
#         function in the edited file reads executed
#   C-12  a coverage run measured at another commit says so on every read;
#         control: before the commit nothing says so
#   C-13  apply_fn, a function template instantiated WITH a lambda type
#         (templ.cpp), maps to apply_fn and its lambda to the lambda: gcov's
#         name for the instantiation carries `{lambda(` in its template and
#         parameter lists, and taking that for a lambda left it unmatched (it
#         read UNMEASURED on NAAb's ThreadPool::enqueue and SafeRegex::
#         executeWithTimeout); C-10 is also red then, since its line joins
#         the unmatched set
#   C-14  the ingest's worker processes change nothing: --jobs 1 and --jobs 3
#         over the same units give the same coverage.json (meta aside)
#   C-15  a process stopped by SIGTERM (a server a test `kill`s, as
#         test_rest_hard_block_survives.sh does `naab-lang api`) still writes
#         its counters and still dies of SIGTERM (status 143); control (c):
#         the build without coverage_exit.c, stopped the same way, writes
#         none -- so C-15 is the handler's doing
#   C-16  a run records which units it covered, from the parallel.py
#         results.json beside its units: "full" (no --only, the .naab phase
#         ran, every listed unit ran) reads complete with no warning; "part"
#         (--only, no .naab phase) prints PARTIAL COVERAGE on every read and
#         in `coverage list`; a run with no results.json (fx) prints
#         COVERAGE COMPLETENESS UNKNOWN -- the control that the warning is
#         not printed on every read is "full"
#   C-17  with no --cov, a read takes the newest COMPLETE run over a newer
#         partial one and names the run it passed over; `status` names the
#         same run (c). Before, the default was the newest run, and a
#         one-suite --only run answered every later `untested`
#   C-18  a "no unit" reading names the run's timed-out units (a SIGKILLed
#         unit writes no counters); control: the run without timeouts names
#         none
#   C-19  a run recorded before completeness was kept: codemap's own `run`
#         always named its --only regex in `source`, so it reads PARTIAL; an
#         old ingest reads unknown
#   C-20  `status` -- the line the session-start hook prints -- gives the
#         refresh commands whenever the run a read would use is not current,
#         and names why: (a) recorded at another commit, (b) not complete,
#         (d) files changed after its build, (e) no run at all; control (c):
#         a complete run at HEAD with no stale file prints no refresh. Each
#         reason is checked alone where the fixture allows (JSON
#         refresh_reasons), so a mutant dropping one reason turns its arm red
#   C-21  the recovery CLAUDE.md and the skill give for a run stopped after
#         its test phase works on the layout `coverage run` leaves: counters
#         in .cache/codemap/coverage/NAME/gcda and parallel.py's results in
#         NAME/parallel, no coverage.json yet. `coverage ingest --name NAME
#         --units-dir .cache/codemap/coverage/NAME/gcda` maps them in place,
#         reads completeness and unit names from NAME/parallel, and keeps the
#         counters; control: before it, the stopped run is not readable
#   C-10  positions matched to no single definition are enumerated EXACTLY:
#         the one line holding twin_a and twin_b, nothing else -- so a new
#         fixture shape that fails to map turns this red; both twins read
#         UNMEASURED (twin_b never ran, but only position could say so), and
#         untested does not list twin_b as executed by no unit
#   C-22  roadmap item 1: helper_binaries takes a helper binary a suite calls
#         from the instrumented build when it is there, from build/ when only
#         that has it (and says "uninstrumented"), and reports one in neither
#         "absent". coverage run used to take every helper from build/
#   C-23  the fixture's second program -- the only route to only_in_second --
#         as helper_binaries picks it (instrumented) credits only_in_second to
#         its unit; control: build/'s copy, what coverage run used to take,
#         writes nothing and only_in_second is never credited
#   C-29  roadmap item 8: the second program's main is indexed beside the
#         fixture's (one USR, two files); each is stored per file, so the
#         second's is credited to its run and neither is an unmatched position
#         (stored once, gov_main.cpp's main on NAAb had no entry of its own and
#         its position read UNMEASURED in every run)
#   C-24  run_extra_units runs a codemap-only unit with its own GCOV_PREFIX, and
#         `coverage ingest` names it gtest:naab_unit_tests (EXTRA_UNIT_DISPLAY),
#         so the recovery path names it too; a unit not built reads "absent";
#         control: dead_function still reads no unit
#   C-24c a "no unit" reading names the binaries THAT run did not instrument
#         (from its metadata); an ingest, which records none, says so; a run
#         with everything instrumented names none
#   C-24b extra_units runs the GoogleTest binary through the repo's own runner
#         (its exclusions apply) and reports a test program not built as None
#   C-25  roadmap item 2: impact lists, under each changed function, the
#         tracked .md lines naming it (NOTES.md's X1 row for dead_function);
#         sink, named only inside the longer identifier sink_all, and
#         via_two_lambdas, named nowhere, list none; control (c): --no-records
#         lists none and still lists the functions
#   C-26  roadmap item 3: a commit changing only Markdown documentation is no
#         reason to refresh coverage and reads print no "recorded at" note;
#         (b) a CLAUDE.md (project_context reads it by name), a .md under
#         tests/ and a non-Markdown file under docs/ each keep both; (c) a
#         commit git cannot compare keeps the reason and says why
#   C-27  roadmap item 7: `untested --group` puts sib_a and sib_b (group.cpp,
#         both called only by caller_ab) in one group; control: lone_fn, which
#         shares no caller, and caller_ab, which has none, stand alone
#   C-28  roadmap item 5: `sweep init` lists group.cpp's four untested functions
#         in docs/sweeps/grp.tsv and refuses to overwrite it; `sweep status`
#         counts one done (state AND evidence), one open, one state without
#         evidence (not done); a row whose function was renamed is flagged NOT
#         IN INDEX and not counted done even with state and evidence; --add adds
#         only what the worklist lacks and changes no row
#
# Needs a C++ compiler, gcov and libclang; each missing piece is UNMEASURABLE.
# ============================================================
set -uo pipefail
# Every fixture run below names its own GCOV_PREFIX, and C-06 needs one run with
# none. Under `codemap coverage run` each unit inherits a prefix of its own
# (parallel.py --unit-env), so the "prefix-less" run wrote into that one and
# C-06 failed in the A64 sweep run for that reason alone (reproduced: 30 of 31
# with GCOV_PREFIX set, 31 of 31 without).
unset GCOV_PREFIX GCOV_PREFIX_STRIP

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
FIXTURE="$SCRIPT_DIR/codemap_fixture"
WRAP="$REPO/src/testing/coverage_exit.c"
. "$REPO/tests/helpers/native_path.sh"
export PYTHONDONTWRITEBYTECODE=1

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | sed 's/^/       | /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }
finish() { echo "Result: $PASS passed, $FAIL failed, $SKIP skipped"; [ "$FAIL" -eq 0 ]; exit $?; }

echo "=== codemap coverage maps gcov onto the index ==="

CXX="$(command -v g++ || true)"
CC="$(command -v gcc || true)"
if ! command -v python3 >/dev/null 2>&1 || ! command -v git >/dev/null 2>&1; then
    skip C-ALL "python3 or git not found"; finish
fi
if [ -z "$CXX" ] || [ -z "$CC" ] || ! command -v gcov >/dev/null 2>&1; then
    skip C-ALL "GCC and gcov are needed (coverage is GCC-only)"; finish
fi
if ! grep -q LIBCLANG_OK <<<"$(python3 -c "import clang.cindex as ci; ci.Index.create(); print('LIBCLANG_OK')" 2>&1)"; then
    skip C-ALL "libclang Python bindings unavailable (python3-clang)"; finish
fi

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
R="$W/repo"
mkdir -p "$R"
cp -R "$FIXTURE/." "$R/"
cd "$R" || exit 1
# A second program, the fixture's stand-in for a helper binary a suite calls
# (naab-lsp, naab-verify-audit) or a C++ test program (naab_unit_tests): only it
# reaches only_in_second(). Its main is indexed beside the fixture's own (other.cpp):
# one name, one USR (both main(int, char**): a different signature is a different
# USR and no collision at all), two definitions, as naab-lang's and naab-gov's are --
# each stored per file (roadmap item 8), so neither adds an unmatched position
# (C-10 pins those) and each is credited to its own program (C-29).
printf '// reachable only through the second program\nint only_in_second(int x) { return x * 3 + 1; }\n' > second.cpp
printf 'int only_in_second(int x);\nint main(int argc, char** argv) { (void)argc; (void)argv; return only_in_second(2) == 7 ? 0 : 1; }\n' > second_main.cpp
# C-27/C-28: two siblings only caller_ab calls, and lone_fn; linked into the
# fixture, called by nothing, so every one is executed by no unit.
cat > group.cpp <<'GRP'
// roadmap items 5 and 7: siblings one caller reaches, and a function alone
int sib_a(int x) { return x + 1; }
int sib_b(int x) { return x + 2; }
int caller_ab(int x) { return sib_a(x) + sib_b(x); }
int lone_fn(int x) { return x * 5; }
GRP
# A decision record for C-25: it names dead_function in a record row, and
# `sink` only inside the longer identifier sink_all, which must not count.
cat > NOTES.md <<'EOF'
# Fixture notes

| Id | Finding |
|---|---|
| X1 | `dead_function` is never called |

## Helpers

`sink_all` is a longer identifier; the shorter name never appears alone here.
EOF
CM="$(native_path "$REPO/tools/codemap/codemap.py")"
cm() { python3 "$CM" --repo . --cache .cache/codemap "$@" 2>&1; }
cmc() { local c="$1"; shift; python3 "$CM" --repo . --cache "$c" "$@" 2>&1; }   # another cache
reasons() { python3 -c 'import json, sys; d = json.load(sys.stdin); c = d["coverage"] or {}
print("%s|%s" % (",".join(c.get("refresh_reasons", [])), "refresh" if d["coverage_refresh"] else "none"))'; }
REFRESH='refresh: `cmake -S . -B build-cov -DNAAB_COVERAGE=ON && make -C build-cov naab-lang naab-gov naab-lsp naab-verify-audit naab_unit_tests naab_governance test_governance_c_api -j4`, then `python3 tools/codemap/codemap.py coverage run` in the background'
g() { git -c user.name=fixture -c user.email=fixture@example.invalid -c commit.gpgsign=false "$@"; }
git init -q . && git config core.autocrlf false
printf '.cache/\nbuild/\nbuild-nowrap/\nbuild2/\nplain/\nunits/\n' > .gitignore
g add -A && g commit -q -m "fixture"

# --- builds: with the wrap (as NAAB_COVERAGE links naab-lang) and without it -------
build() {  # dir wrap(yes|no)
    mkdir -p "$1"
    ( cd "$1" && for f in fixture other templ group; do
          "$CXX" -std=c++17 -O0 -g --coverage -I../include -c "../$f.cpp" -o "$f.cpp.o" || exit 1
      done
      if [ "$2" = yes ]; then
          "$CC" -c "$WRAP" -o coverage_exit.o || exit 1
          "$CXX" --coverage -Wl,--wrap=_exit -Wl,--wrap=_Exit fixture.cpp.o other.cpp.o templ.cpp.o group.cpp.o coverage_exit.o -o fixture -pthread
      else
          "$CXX" --coverage fixture.cpp.o other.cpp.o templ.cpp.o group.cpp.o -o fixture -pthread
      fi ) > "$W/build-$2.log" 2>&1
}
build_second() {  # dir --coverage|"" -- the second program, instrumented (build2) or not (plain)
    mkdir -p "$1"
    ( cd "$1" && "$CXX" -std=c++17 -O0 -g $2 -c ../second.cpp -o second.cpp.o \
          && "$CXX" -std=c++17 -O0 -g $2 -c ../second_main.cpp -o second_main.cpp.o \
          && "$CXX" $2 second.cpp.o second_main.cpp.o -o second ) > "$W/build-second$2.log" 2>&1
}
if ! build build yes || ! build build-nowrap no || ! build_second build2 --coverage || ! build_second plain ""; then
    skip C-ALL "the fixture did not build with --coverage here"; cat "$W"/build-*.log | sed 's/^/       | /'; finish
fi
python3 - <<'EOF'
import json, os
w = os.getcwd()
json.dump([{"directory": os.path.join(w, "build"), "file": os.path.join(w, f),
            "arguments": ["c++", "-std=c++17", "-I" + os.path.join(w, "include"), "-c", os.path.join(w, f), "-o", f + ".o"]}
           for f in ("fixture.cpp", "other.cpp", "extra.cpp", "templ.cpp", "second.cpp", "second_main.cpp", "group.cpp")],
          open(os.path.join(w, "build", "compile_commands.json"), "w"))
EOF
IDX="$(cm index --jobs 2)"; irc=$?
if [ "$irc" -ne 0 ]; then skip C-ALL "the fixture did not index (rc=$irc)"; finish; fi

U="$R/units"
mkdir -p "$U"
GCOV_PREFIX="$U/000_A" ./build/fixture >/dev/null 2>&1; arc=$?
GCOV_PREFIX="$U/001_B" ./build/fixture refuse >/dev/null 2>&1; brc=$?
./build/fixture >/dev/null 2>&1                    # no prefix: written into build/ itself
if [ "$arc" -ne 0 ] || [ "$brc" -ne 3 ]; then
    bad C-00 "the fixture runs did not behave as designed (A rc=$arc, B rc=$brc; want 0 and 3)"; finish
fi
ING="$(cm coverage ingest --name fx --units-dir units --build-dir build)"; rc=$?
if [ "$rc" -ne 0 ]; then bad C-00 "ingest failed (rc=$rc)" "$ING"; finish; fi

# ---------------------------------------------------------------- C-01 / C-01c
REF="$(cm tested refuse --cov fx)"
if grep -q "executed by 1 unit" <<<"$REF" && grep -qE "^ +[0-9]+  001_B$" <<<"$REF"; then
    ok C-01 "refuse() is executed by B alone: its counters survived _exit(3) through the wrap"
else
    bad C-01 "the _exit(3) run's coverage is missing or misattributed" "$REF"
fi
mkdir -p "$W/nowrap"
GCOV_PREFIX="$W/nowrap/001_B" ./build-nowrap/fixture refuse >/dev/null 2>&1
GCOV_PREFIX="$W/nowrap/000_A" ./build-nowrap/fixture >/dev/null 2>&1
NOW="$(cm coverage ingest --name nowrap --units-dir "$W/nowrap" --build-dir build-nowrap)"
NREF="$(cm tested refuse --cov nowrap)"; NSINK="$(cm tested sink --cov nowrap)"
if grep -q "executed by NO unit" <<<"$NREF" && grep -qE "^ +[0-9]+  000_A$" <<<"$NSINK"; then
    ok C-01c "control: without the wrap the _exit(3) run leaves no counters -- C-01 is the wrap's doing"
else
    bad C-01c "control: the unwrapped build kept the _exit run, or recorded nothing at all" "$NREF
$NSINK"
fi

# ---------------------------------------------------------------- C-02 / C-03
SINK="$(cm tested sink --cov fx)"; DEAD="$(cm tested dead_function --cov fx)"
if grep -qE "^ +[0-9]+  000_A$" <<<"$SINK" && grep -qE "^ +1  001_B$" <<<"$SINK" \
   && grep -q "executed by NO unit of coverage fx" <<<"$DEAD"; then
    ok C-02 "sink: units A and B (B once, from refuse); dead_function: no unit"
else
    bad C-02 "per-unit execution wrong" "$SINK
$DEAD"
fi
LB="$(cm tested via_two_lambdas::b --cov fx)"; OV="$(cm tested over --cov fx)"
if grep -qE "^ +1  000_A$" <<<"$LB" \
   && [ "$(grep -cE '^ +2  000_A$' <<<"$OV")" -eq 1 ] && [ "$(grep -cE '^ +1  000_A$' <<<"$OV")" -eq 1 ] \
   && [ "$(grep -c 'executed by NO unit' <<<"$OV")" -eq 1 ]; then
    ok C-03 "lambdas and overloads are placed by position: b once; over 2 / 1 / none"
else
    bad C-03 "a lambda or an overload was merged or misplaced" "$LB
$OV"
fi

OL="$(cm tested one_liner --cov fx)"
OLL="one_liner::<lambda@fixture.cpp:$(grep -n '^int one_liner()' fixture.cpp | cut -d: -f1)>"
LAM="$(cm tested "$OLL" --cov fx 2>&1)"
if grep -qE "^ +1  000_A$" <<<"$OL" && [ -n "$OLL" ] && grep -qE "^ +1  000_A$" <<<"$LAM"; then
    ok C-03b "one_liner and the lambda on the same line are each matched (by kind, not only by line)"
else
    bad C-03b "a function and its same-line lambda were not told apart" "$OL
lambda: $OLL
$LAM"
fi

# ---------------------------------------------------------------- C-04 / C-05
NL="$(cm tested never_linked --cov fx)"
if grep -q "NOT IN THE BUILD" <<<"$NL" && ! grep -q "executed by NO unit" <<<"$NL"; then
    ok C-04 "a function no binary links reads NOT IN THE BUILD, not untested"
else
    bad C-04 "unlinked code was reported as measured" "$NL"
fi
UT="$(cm untested --under fixture.cpp --cov fx --per-file 50)"
if grep -q "dead_function" <<<"$UT" && grep -q "over " <<<"$UT" && ! grep -qE "^ +sink " <<<"$UT"; then
    ok C-05 "untested lists dead_function and the unused overload, not sink"
else
    bad C-05 "untested list wrong" "$UT"
fi

# ---------------------------------------------------------------- C-06
if grep -q "(unattributed)" <<<"$ING" && python3 -c "
import json,sys
c=json.load(open('.cache/codemap/coverage/fx/coverage.json'))
assert '(unattributed)' in c['units'], c['units']
assert any('(unattributed)' in v for v in c['executed'].values())
" 2>/dev/null; then
    ok C-06 "a run that lost GCOV_PREFIX is collected as (unattributed)"
else
    bad C-06 "the prefix-less run was dropped or credited to a unit" "$ING"
fi

# ---------------------------------------------------------------- C-16 .. C-19
# Two runs over the same units, each with the parallel.py results.json a real
# run leaves beside its units (paths relative to the fixture repo, see C-00).
mkdir -p cfull/units cpart/units
cp -R units/000_A units/001_B cfull/units/
cp -R units/000_A cpart/units/
python3 - <<'EOF'
import json, os
def results(path, only, naab_phase, rows):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    json.dump({"schema": 1, "mode": {"jobs": 1, "order": "plan", "seed": None, "isolated": True,
                                     "only": only, "naab_phase": naab_phase},
               "units_listed": len(rows), "units_run": len(rows), "interrupted": False,
               "results": [{"index": i, "key": k, "verdict": v} for i, (k, v) in enumerate(rows)]},
              open(path, "w"))
results("cfull/parallel/results.json", None, True,
        [("A", "PASS"), ("B", "PASS"), ("C", "TIMEOUT"), ("NAAB_TEST_PHASE=naab run-all-tests.sh", "PASS")])
results("cpart/parallel/results.json", "A", False, [("A", "PASS")])
EOF
IFULL="$(cm coverage ingest --name full --units-dir cfull/units --build-dir build)"
python3 -c "import os, time; t = time.time() - 60; os.utime('.cache/codemap/coverage/full/coverage.json', (t, t))"
IPART="$(cm coverage ingest --name part --units-dir cpart/units --build-dir build)"
LIST="$(cm coverage list)"
RFULL="$(cm tested dead_function --cov full)"
RPART="$(cm tested dead_function --cov part)"
RUNK="$(cm tested dead_function --cov fx)"
if grep -q "complete: every unit the suite listed (4) ran" <<<"$IFULL" \
   && grep -q "PARTIAL: only the units matching --only 'A' ran; the .naab test phase did not run" <<<"$IPART" \
   && grep -qE "^full +[^ ]+ +complete " <<<"$LIST" && grep -qE "^part +[^ ]+ +PARTIAL " <<<"$LIST" \
   && grep -qE "^fx +[^ ]+ +unknown " <<<"$LIST" \
   && grep -q "^PARTIAL COVERAGE: run part did not cover the whole suite" <<<"$RPART" \
   && grep -q "^COVERAGE COMPLETENESS UNKNOWN: run fx -- no parallel.py results.json" <<<"$RUNK" \
   && ! grep -qE "PARTIAL|UNKNOWN" <<<"$RFULL"; then
    ok C-16 "complete / PARTIAL / unknown recorded from results.json; every partial or unknown read says so, a complete one does not"
else
    bad C-16 "a run's completeness was not recorded or not reported" "$IFULL
$IPART
$LIST
--- full:
$RFULL
--- part:
$RPART
--- fx:
$RUNK"
fi
RDEF="$(cm tested dead_function)"
SDEF="$(cm status)"
if grep -q "^(reading coverage full, the newest complete run; newer: .*part (partial)" <<<"$RDEF" \
   && grep -q "executed by NO unit of coverage full" <<<"$RDEF" && ! grep -q "PARTIAL COVERAGE" <<<"$RDEF"; then
    ok C-17 "with no --cov the complete run is read over a newer partial one, and the passed-over run is named"
else
    bad C-17 "the default read took a partial run (or hid the newer one)" "$RDEF"
fi
if grep -q "coverage: full (complete" <<<"$SDEF"; then
    ok C-17c "status names the same run a read would use"
else
    bad C-17c "status names a different coverage run" "$SDEF"
fi
JDEF="$(cm --json status | reasons)"
if grep -q "coverage: full (complete)\. " <<<"$SDEF" && ! grep -qF "refresh:" <<<"$SDEF" && [ "$JDEF" = "|none" ]; then
    ok C-20c "control: a complete run at HEAD with no stale file prints no refresh"
else
    bad C-20c "status asked to refresh a current, complete run" "$JDEF
$SDEF"
fi
mkdir -p .cache/c_part/coverage && cp -R .cache/codemap/coverage/part .cache/c_part/coverage/
SPART="$(cmc .cache/c_part status)"; JPART="$(cmc .cache/c_part --json status | reasons)"
if grep -qF "coverage: part (PARTIAL; $REFRESH" <<<"$SPART" && [ "$JPART" = "not complete|refresh" ]; then
    ok C-20b "a partial run at HEAD: status gives the refresh commands, for that reason alone"
else
    bad C-20b "a partial run was not flagged for refresh (or for another reason)" "$JPART
$SPART"
fi
EMPTY="$(cmc .cache/c_none status)"
if grep -qF "coverage: none ($REFRESH" <<<"$EMPTY"; then
    ok C-20e "no coverage run at all: status gives the same refresh commands"
else
    bad C-20e "status with no coverage run did not say how to make one" "$EMPTY"
fi
if grep -q "executed by NO unit of coverage full.* 1 unit(s) of this run timed out (C)" <<<"$RFULL" \
   && ! grep -q "timed out" <<<"$RPART"; then
    ok C-18 "a no-unit reading names the run's timed-out unit; control: a run without timeouts names none"
else
    bad C-18 "timed-out units were not named on a no-unit reading (or were invented)" "$RFULL
---
$RPART"
fi
python3 - <<'EOF'
import json, os
src = json.load(open(".cache/codemap/coverage/fx/coverage.json"))
for name, source in (("legacyrun", "suite (parallel.py, test_rest_hard_block)"), ("legacyingest", "ingest units")):
    d = ".cache/codemap/coverage/" + name
    os.makedirs(d, exist_ok=True)
    json.dump(dict(src, meta={"name": name, "source": source, "at": "2026-10-17T00:00:00Z", "head": ""}),
              open(os.path.join(d, "coverage.json"), "w"))
EOF
LIST2="$(cm coverage list)"
if grep -qE "^legacyrun +[^ ]+ +PARTIAL " <<<"$LIST2" && grep -qE "^legacyingest +[^ ]+ +unknown " <<<"$LIST2"; then
    ok C-19 "an old codemap --only run reads PARTIAL from its recorded source; an old ingest reads unknown"
else
    bad C-19 "a run recorded before completeness was misread" "$LIST2"
fi

# ---------------------------------------------------------------- C-21
# What `coverage run` leaves when it is stopped after the suite, before the mapping
# writes coverage.json (run_suite_for_coverage: units in NAME/gcda, results in NAME/parallel)
STOPPED=.cache/codemap/coverage/stopped
mkdir -p "$STOPPED/gcda" && cp -R cfull/units/. "$STOPPED/gcda/" && cp -R cfull/parallel "$STOPPED/"
BEFORE="$(cm tested sink --cov stopped)"
REC="$(cm coverage ingest --name stopped --units-dir "$STOPPED/gcda" --build-dir build)"
AFTER="$(cm tested sink --cov stopped)"
if grep -q "no coverage 'stopped'" <<<"$BEFORE" \
   && grep -q "complete: every unit the suite listed (4) ran" <<<"$REC" \
   && grep -qE "^ +[0-9]+  A$" <<<"$AFTER" && grep -qE "^ +[0-9]+  B$" <<<"$AFTER" \
   && [ -n "$(find "$STOPPED/gcda" -name '*.gcda' | head -1)" ]; then
    ok C-21 "a run stopped before its mapping is re-mapped in place by the documented ingest: complete, units named, counters kept"
else
    bad C-21 "the documented recovery did not re-map a stopped run" "$BEFORE
---
$REC
---
$AFTER"
fi

# ---------------------------------------------------------------- C-07 / C-08
NOCH="$(cm impact --cov fx)"
sed -i.bak 's/^void sink(const std::string& path) { (void)path; }/void sink(const std::string\& path) { (void)path; (void)0; }/' fixture.cpp
sed -i.bak 's/^void dead_function() { (void)0; }/void dead_function() { (void)1; }/' fixture.cpp && rm -f fixture.cpp.bak
sed -i.bak 's/^    (void)c("u");$/    (void)c("uu");/' fixture.cpp && rm -f fixture.cpp.bak
# age the index rather than the file: a coarse-mtime filesystem could otherwise
# give the edit and the earlier index the same second
touch -d '-1 hour' .cache/codemap/codemap.db 2>/dev/null || touch -t 200001010000 .cache/codemap/codemap.db
STALE="$(cm impact --cov fx)"
cm index --jobs 2 >/dev/null
printf 'units-extra/\n' >> .gitignore           # tracked, changed after the index, not indexed
IMP="$(cm impact --cov fx)"
if grep -q "touch 0 indexed functions" <<<"$NOCH" \
   && grep -qE "^ +sink +fixture.cpp:" <<<"$IMP" && grep -qE "^ +dead_function +fixture.cpp:" <<<"$IMP" \
   && grep -qE "^ +via_two_lambdas +fixture.cpp:" <<<"$IMP" \
   && grep -qE "^ +[0-9]+ fn .* 000_A$" <<<"$IMP" && grep -qE "^ +[0-9]+ fn .* 001_B$" <<<"$IMP" \
   && grep -q "dead_function" <<<"$(grep -A3 "NO unit executes" <<<"$IMP")"; then
    ok C-07 "impact: sink and dead_function changed; sink's units listed; dead_function executed by no unit"
else
    bad C-07 "impact wrong (or the unchanged-tree control touched something)" "$NOCH
---
$IMP"
fi
if grep -q "STALE INDEX: fixture.cpp" <<<"$STALE" && ! grep -q "STALE INDEX" <<<"$IMP"; then
    ok C-08 "impact on an index older than the changed file says so; after re-indexing it does not"
else
    bad C-08 "a stale index was not reported (or a fresh one was)" "$STALE"
fi

# ---------------------------------------------------------------- C-25 / C-25c
# the "doc" lines printed under one changed function in impact's output
docs_of() { awk -v fn="$1" '$0 ~ "^     " fn " +fixture[.]cpp:" {on=1; next}
                            on && /^         doc  / {print; next} {on=0}' <<<"$2"; }
DDOC="$(docs_of dead_function "$IMP")"; SDOC="$(docs_of sink "$IMP")"; VDOC="$(docs_of via_two_lambdas "$IMP")"
IMPJ="$(cm --json impact --cov fx)"
DJSON="$(python3 -c 'import json, sys; d = json.load(sys.stdin)
print(";".join("%s=%s" % (c["name"], ",".join("%s:%d" % (r["file"], r["line"]) for r in c["records"]))
               for c in d["changed"] if c["name"] in ("dead_function", "sink")))' <<<"$IMPJ" 2>&1)"
if grep -qE "doc  NOTES.md:[0-9]+ \[X1\] " <<<"$DDOC" && [ -z "$SDOC" ] && [ -z "$VDOC" ] \
   && grep -q "records: 1 of 3 changed functions are named" <<<"$IMP" \
   && grep -qE "dead_function=NOTES.md:[0-9]+" <<<"$DJSON" && grep -qE "(^|;)sink=(;|$)" <<<"$DJSON"; then
    ok C-25 "impact lists the record naming dead_function (X1); sink (named only inside sink_all) and via_two_lambdas list none"
else
    bad C-25 "impact's records are missing, or a longer identifier / an unnamed function counted" "dead_function: $DDOC
sink: $SDOC
via_two_lambdas: $VDOC
json: $DJSON
$IMP"
fi
NOREC="$(cm impact --cov fx --no-records)"
if ! grep -q "^         doc  " <<<"$NOREC" && ! grep -q "records:" <<<"$NOREC" \
   && grep -qE "^ +dead_function +fixture.cpp:" <<<"$NOREC"; then
    ok C-25c "control: --no-records prints the changed functions and no record lines"
else
    bad C-25c "--no-records still searched the records, or dropped the changed functions" "$NOREC"
fi

# ---------------------------------------------------------------- C-13
AF="$(cm tested apply_fn --cov fx)"
AFL="uses_apply::<lambda@templ.cpp:$(grep -n 'return apply_fn(\[\]' templ.cpp | cut -d: -f1)>"
LAM2="$(cm tested "$AFL" --cov fx 2>&1)"
if grep -qE "^ +1  000_A$" <<<"$AF" && grep -qE "^ +1  001_B$" <<<"$AF" && grep -qE "^ +1  000_A$" <<<"$LAM2"; then
    ok C-13 "a template instantiated with a lambda type maps to the template; its lambda to the lambda"
else
    bad C-13 "the template (or its lambda) was not matched by position" "$AF
lambda: $AFL
$LAM2"
fi

# ---------------------------------------------------------------- C-14
cm coverage ingest --name j1 --units-dir units --build-dir build --jobs 1 >/dev/null
cm coverage ingest --name j3 --units-dir units --build-dir build --jobs 3 >/dev/null
SAME="$(python3 -c "
import json
a, b = (json.load(open('.cache/codemap/coverage/%s/coverage.json' % n)) for n in ('j1', 'j3'))
for d in (a, b):
    d.pop('meta', None)
print('SAME' if a == b and a['executed'] else 'DIFFER %s' % sorted(k for k in a if a.get(k) != b.get(k)))
" 2>&1)"
if [ "$SAME" = "SAME" ]; then
    ok C-14 "the ingest gives the same result with 1 and with 3 worker processes"
else
    bad C-14 "the result depends on the number of ingest workers" "$SAME"
fi

# ---------------------------------------------------------------- C-10
TW="$(grep -n '^int twin_a()' fixture.cpp | cut -d: -f1)"
UMS="$(python3 -c "import json; print(' '.join(sorted(json.load(open('.cache/codemap/coverage/fx/coverage.json'))['unmatched'])))" 2>&1)"
TA="$(cm tested twin_a --cov fx)"; TB="$(cm tested twin_b --cov fx)"
UT2="$(cm untested --under fixture.cpp --cov fx --per-file 50)"
if [ "$UMS" = "fixture.cpp:$TW" ] && grep -q "UNMEASURED" <<<"$TA" && grep -q "UNMEASURED" <<<"$TB" \
   && ! grep -qE "^ +twin_b " <<<"$UT2" && grep -q "1 UNMEASURED\|2 UNMEASURED" <<<"$UT2"; then
    ok C-10 "unmatched positions are exactly the twin line; both twins read UNMEASURED, not untested"
else
    bad C-10 "unmatched set or the UNMEASURED reading wrong: '$UMS' (want fixture.cpp:$TW)" "$TA
$TB
$(head -2 <<<"$UT2")"
fi

# ---------------------------------------------------------------- C-11 / C-12
# fixture.cpp was edited in place by C-07 (no line moved) after build/ compiled it
ING2="$(cm coverage ingest --name fx2 --units-dir units --build-dir build)"
SK2="$(cm tested sink --cov fx2)"; UT3="$(cm untested --under fixture.cpp --cov fx2)"
MN2="$(cm tested other_helpers --cov fx2)"
if grep -q "STALE BUILD: 1 indexed files changed after the instrumented build compiled them (fixture.cpp)" <<<"$ING2" \
   && grep -q "UNMEASURED in coverage fx2 -- its file changed after the instrumented build" <<<"$SK2" \
   && grep -q " 0 executed by some unit, 0 compiled but executed by NO unit" <<<"$UT3"; then
    ok C-11 "a file edited after the build: STALE BUILD, its functions UNMEASURED, none executed or untested"
else
    bad C-11 "an edited file was still mapped by the build's line numbers" "$ING2
$SK2
$(head -1 <<<"$UT3")"
fi
mkdir -p .cache/c_stale/coverage && cp -R .cache/codemap/coverage/fx2 .cache/c_stale/coverage/
SSTALE="$(cmc .cache/c_stale status)"; JSTALE="$(cmc .cache/c_stale --json status | reasons)"
if grep -qF "1 file(s) changed after its build; $REFRESH" <<<"$SSTALE" \
   && [ "$JSTALE" = "not complete,1 file(s) changed after its build|refresh" ]; then
    ok C-20d "a run whose build predates an edited file: status names the stale file count and the refresh"
else
    bad C-20d "a stale-build run was not flagged for refresh" "$JSTALE
$SSTALE"
fi
if grep -qE "^ +1  000_A$" <<<"$MN2"; then
    ok C-11c "control: other.cpp, not edited, is still measured -- the stale check is per file"
else
    bad C-11c "control: an unedited file lost its coverage" "$MN2"
fi
{ printf '// shifted\n// shifted\n// shifted\n// shifted\n// shifted\n'; cat fixture.cpp; } > fixture.cpp.new && mv fixture.cpp.new fixture.cpp
cm index --jobs 2 >/dev/null
cm coverage ingest --name fx3 --units-dir units --build-dir build >/dev/null
UT4="$(cm untested --under fixture.cpp --cov fx3)"
if grep -q " 0 executed by some unit" <<<"$UT4" && grep -qE "[1-9][0-9]* UNMEASURED" <<<"$UT4"; then
    ok C-11b "after a line shift no function in the edited file is credited by the build's positions"
else
    bad C-11b "a shifted file was credited by the build's line numbers" "$(head -1 <<<"$UT4")"
fi
CLEAN="$(cm tested other_helpers --cov fx)"
g add -A && g commit -q -m "edit"
MOVED="$(cm tested other_helpers --cov fx)"
if ! grep -q "was recorded at" <<<"$CLEAN" && grep -q "(coverage fx was recorded at [0-9a-f]*; HEAD is [0-9a-f]*" <<<"$MOVED"; then
    ok C-12 "a coverage run from another commit says so; at its own commit it does not"
else
    bad C-12 "the commit a coverage run is about was not reported (or was, at its own commit)" "$CLEAN
---
$MOVED"
fi
SMOVED="$(cm status)"; JMOVED="$(cm --json status | reasons)"
if grep -qE "coverage: [a-z0-9]+ \(complete, recorded at [0-9a-f]+; refresh: " <<<"$SMOVED" && grep -qF "; $REFRESH" <<<"$SMOVED" \
   && [ "$JMOVED" = "recorded at another commit|refresh" ]; then
    ok C-20 "a complete run recorded at another commit: status gives the refresh commands, for that reason alone"
else
    bad C-20 "a run from another commit was not flagged for refresh" "$JMOVED
$SMOVED"
fi

# ---------------------------------------------------------------- C-15 / C-15c
stop_by_term() {  # binary units-dir -> exit status of the stopped process
    rm -f "$W/ready"
    GCOV_PREFIX="$2/000_D" FIXTURE_READY_FILE="$W/ready" "$1" >/dev/null 2>&1 &
    local pid=$! i
    for i in $(seq 1 100); do [ -f "$W/ready" ] && break; sleep 0.1; done
    [ -f "$W/ready" ] || { kill -KILL "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; echo "never-ready"; return; }
    kill -TERM "$pid"
    for i in $(seq 1 100); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "$pid" 2>/dev/null; then              # bounded: a swallowed SIGTERM must not hang the suite
        kill -KILL "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; echo "survived-SIGTERM"; return
    fi
    wait "$pid"; echo "$?"
}
SRC1="$(stop_by_term ./build/fixture "$W/sig")"
SRC0="$(stop_by_term ./build-nowrap/fixture "$W/signowrap")"
NSIG="$(find "$W/sig" -name '*.gcda' 2>/dev/null | wc -l | tr -d ' ')"
NNOW="$(find "$W/signowrap" -name '*.gcda' 2>/dev/null | wc -l | tr -d ' ')"
if [ "$SRC1" = "143" ] && [ "$NSIG" -gt 0 ]; then
    cm coverage ingest --name sig --units-dir "$W/sig" --build-dir build >/dev/null
    WS="$(cm tested uses_apply --cov sig)"
    if grep -qE "^ +1  000_D$" <<<"$WS"; then
        ok C-15 "a process stopped by SIGTERM wrote its counters ($NSIG files) and still died of SIGTERM (143)"
    else
        bad C-15 "the SIGTERM run's counters were written but not credited" "$WS"
    fi
else
    bad C-15 "a process stopped by SIGTERM lost its counters or its signal status" "status=$SRC1 files=$NSIG"
fi
if [ "$SRC0" = "143" ] && [ "$NNOW" -eq 0 ]; then
    ok C-15c "control: without coverage_exit.c the same SIGTERM stop writes nothing -- C-15 is the handler's doing"
else
    bad C-15c "control: the unwrapped build kept its counters, or was not stopped by SIGTERM" "status=$SRC0 files=$NNOW"
fi

# ---------------------------------------------------------------- C-09
mkdir -p "$W/empty-build"
OUT="$(cm coverage ingest --name x --units-dir units --build-dir "$W/empty-build")"; rc=$?
if [ "$rc" -ne 0 ] && grep -q "DNAAB_COVERAGE=ON" <<<"$OUT"; then
    ok C-09 "ingest without a coverage build is refused with the command that makes one"
else
    bad C-09 "a build directory without .gcno files was accepted (rc=$rc)" "$OUT"
fi

# ---------------------------------------------------------------- C-22 / C-23 / C-24
# Roadmap item 1. `coverage run` copied every helper binary a suite calls from the
# ordinary build/, so code only naab-lsp or naab-verify-audit reaches read
# "executed by NO unit"; and the C++ test programs (naab_unit_tests, the C API's)
# were never run at all. The fixture's second program plays each part: build2/ is
# the instrumented build, plain/ the ordinary one.
py() { python3 -c "import sys; sys.path.insert(0, '$(native_path "$REPO/tools/codemap")'); import codemap
$1" 2>&1; }
cp plain/second plain/onlyplain
PICK="$(py 'for n, src, o in codemap.helper_binaries("plain", "build2", ["second", "onlyplain", "nowhere"]):
    print(n, o, src)')"
if grep -qx "second instrumented build2/second" <<<"$PICK" \
   && grep -qx "onlyplain uninstrumented plain/onlyplain" <<<"$PICK" \
   && grep -qx "nowhere absent None" <<<"$PICK"; then
    ok C-22 "a helper binary comes from the instrumented build when it is there, else from build/ (said so), else absent"
else
    bad C-22 "helper binaries were not taken from the instrumented build" "$PICK"
fi
# C-23: the binary helper_binaries picks writes counters; the one from build/ -- what
# coverage run used to copy -- writes none, so only_in_second, which is in no other
# binary, read NOT IN THE BUILD (code a helper shares with naab-lang read "executed by
# NO unit" instead; either way it was never credited)
SRC2="$(py 'print(codemap.helper_binaries("plain", "build2", ["second"])[0][1])')"
mkdir -p units2 units2c
GCOV_PREFIX="$R/units2/000_S" "./$SRC2" >/dev/null 2>&1; s2=$?
GCOV_PREFIX="$R/units2c/000_S" ./plain/second >/dev/null 2>&1; s2c=$?
cm coverage ingest --name second --units-dir units2 --build-dir build2 >/dev/null
cm coverage ingest --name secondc --units-dir units2c --build-dir build2 >/dev/null
T2="$(cm tested only_in_second --cov second)"
T2C="$(cm tested only_in_second --cov secondc)"
if [ "$s2" -eq 0 ] && grep -qE "^ +1  000_S$" <<<"$T2" && grep -q "NOT IN THE BUILD" <<<"$T2C" \
   && ! grep -qE "^ +[0-9]+  000_S$" <<<"$T2C" && [ "$s2c" -eq 0 ]; then
    ok C-23 "only_in_second, reached only through the second program: executed when that program is the instrumented copy, never credited when it is build/'s -- what coverage run used to copy"
else
    bad C-23 "the instrumented helper's run was not credited (or the build/ copy's was)" "picked: $SRC2 rc=$s2/$s2c
$T2
---
$T2C"
fi
# C-29 (roadmap item 8): the second program's main and the fixture's share a USR.
# Stored once, only one of the two had an entry -- gov_main.cpp's main on NAAb was
# unmatched in every run. Stored per file, each is credited to the program that ran it.
TM2="$(cm tested second_main.cpp:main --cov second)"
TMO="$(cm tested other.cpp:main --cov second)"
FXO="$(cm tested other.cpp:main --cov fx)"
FX2="$(cm tested second_main.cpp:main --cov fx)"
UM2="$(python3 -c "import json; print(' '.join(sorted(json.load(open('.cache/codemap/coverage/second/coverage.json'))['unmatched'])))" 2>&1)"
if grep -qE "^ +1  000_S$" <<<"$TM2" && grep -q "NOT IN THE BUILD" <<<"$TMO" \
   && grep -qE "executed by [1-9][0-9]* unit" <<<"$FXO" && grep -q "NOT IN THE BUILD" <<<"$FX2" \
   && ! grep -q "second_main" <<<"$UM2"; then
    ok C-29 "two mains on one USR, each read in its own program: the second's run credits second_main.cpp's, the fixture's runs other.cpp's, and neither position is unmatched"
else
    bad C-29 "a multiply-defined main was not mapped per file" "$TM2
---
$TMO
---
$FXO
---
$FX2
--- unmatched: $UM2"
fi
# C-24: an extra unit runs with its own prefix under its codemap-only name, and the
# name survives a re-map by `coverage ingest` (EXTRA_UNIT_DISPLAY); an unbuilt one is
# reported absent, not an error. Control: dead_function still reads no unit.
mkdir -p units3
EXTRA="$(py "st, names = codemap.run_extra_units('$R/units3', [('900_gtest_naab_unit_tests', 'gtest:naab_unit_tests', ['$R/build2/second']), ('901_capi_test_governance_c_api', 'capi:test_governance_c_api', None)], '$R')
for k in sorted(st): print(k, '=', st[k])")"
cm coverage ingest --name extra --units-dir units3 --build-dir build2 >/dev/null
T3="$(cm tested only_in_second --cov extra)"
if grep -qx "gtest:naab_unit_tests = ran (exit 0)" <<<"$EXTRA" \
   && grep -q "^capi:test_governance_c_api = absent" <<<"$EXTRA" \
   && grep -qE "^ +1  gtest:naab_unit_tests$" <<<"$T3" \
   && grep -q "executed by NO unit" <<<"$(cm tested dead_function --cov fx)"; then
    ok C-24 "an extra unit runs with its own prefix and reads under its codemap-only name; an unbuilt one is absent, not an error"
else
    bad C-24 "the extra unit was not run, not credited, or not named" "$EXTRA
---
$T3"
fi

# C-24c: a "no unit" reading names what THAT run did not instrument. An ingest records
# no binaries, so it says so; a run whose metadata lists naab-lsp uninstrumented and the
# C API test absent names both, and a run with everything instrumented names none.
python3 - <<'PYMETA'
import json, os, shutil
for name, binaries, extra in (("blind", {"naab-lsp": "uninstrumented", "naab-gov": "instrumented"},
                               {"capi:test_governance_c_api": "absent", "gtest:naab_unit_tests": "ran (exit 0)"}),
                              ("seeing", {"naab-lsp": "instrumented"}, {"gtest:naab_unit_tests": "ran (exit 0)"})):
    dst = os.path.join(".cache/codemap/coverage", name)
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(".cache/codemap/coverage/fx", dst)
    p = os.path.join(dst, "coverage.json")
    d = json.load(open(p)); d.setdefault("meta", {}).update(name=name, binaries=binaries, extra_units=extra)
    json.dump(d, open(p, "w"))
PYMETA
NB_FX="$(cm tested dead_function --cov fx)"; NB_BL="$(cm tested dead_function --cov blind)"; NB_SE="$(cm tested dead_function --cov seeing)"
if grep -q "does not record which binaries it instrumented" <<<"$NB_FX" \
   && grep -q "(this run's: naab-lsp, capi:test_governance_c_api)" <<<"$NB_BL" \
   && grep -q "a test reaching it only through an uninstrumented binary is not seen)" <<<"$NB_SE" \
   && ! grep -q "this run's:" <<<"$NB_SE"; then
    ok C-24c "a no-unit reading names the run's own uninstrumented binaries; an ingest says it does not know; a fully instrumented run names none"
else
    bad C-24c "the no-unit note does not reflect what the run instrumented" "$NB_FX
---
$NB_BL
---
$NB_SE"
fi

# C-24b: which C++ test programs a run has, and how each is run: the GoogleTest binary
# through the repo's own runner (its exclusions apply), the C API test directly; one
# not built is None. Fake instrumented build: the gtest binary only.
mkdir -p "$W/fakecov" && : > "$W/fakecov/naab_unit_tests" && chmod +x "$W/fakecov/naab_unit_tests"
EU="$(py "for d, disp, argv in codemap.extra_units('/copy', '$W/fakecov'): print(d, disp, argv)")"
if grep -qx "900_gtest_naab_unit_tests gtest:naab_unit_tests \['bash', '/copy/tests/unit/run_unit_tests.sh', '$W/fakecov/naab_unit_tests'\]" <<<"$EU" \
   && grep -qx "901_capi_test_governance_c_api capi:test_governance_c_api None" <<<"$EU"; then
    ok C-24b "the GoogleTest binary runs through the repo's runner; a test program not built is None"
else
    bad C-24b "extra_units chose the wrong programs or the wrong way to run them" "$EU"
fi

# ---------------------------------------------------------------- C-27 (roadmap item 7)
GRP="$(cm untested --under group.cpp --cov fx --group)"
GJ="$(cm --json untested --under group.cpp --cov fx --group | python3 -c 'import json, sys; d = json.load(sys.stdin)
print("groups=" + ";".join("+".join(sorted(m["name"] for m in g["members"])) + "@" + ",".join(g["shared_callers"])
                           for g in d["groups"]))
print("alone=" + ",".join(sorted(m["name"] for m in d["alone"])))' 2>&1)"
if [ "$GJ" = "groups=sib_a+sib_b@caller_ab
alone=caller_ab,lone_fn" ] && grep -q "group of 2, shared callers: caller_ab" <<<"$GRP"; then
    ok C-27 "untested --group: sib_a and sib_b form one group by their shared caller; lone_fn and caller_ab stand alone"
else
    bad C-27 "untested siblings were not grouped, or an unrelated function joined them" "$GJ
$GRP"
fi

# ---------------------------------------------------------------- C-28 (roadmap item 5)
SW="$(cm sweep init grp --under group.cpp --cov fx)"
SWHEAD="$(head -1 docs/sweeps/grp.tsv)"
SWAGAIN="$(cm sweep init grp --under group.cpp --cov fx)"; swrc=$?
# record outcomes by hand, as a sweep would: one with evidence, one without; and
# rename a third row's function, as a later refactor would
python3 - <<'PYEDIT'
p = "docs/sweeps/grp.tsv"
out = []
for ln in open(p).read().splitlines():
    c = ln.split("\t")
    if c[0] == "sib_a":
        c[3:7] = ["lost", "remove", "abc1234 git log -S", "owner"]
    elif c[0] == "sib_b":
        c[3:7] = ["decided", "none", "", "owner"]
    elif c[0] == "lone_fn":
        c[0] = "lone_fn_renamed"; c[3:7] = ["working", "none", "a test", "owner"]
    out.append("\t".join(c))
open(p, "w").write("\n".join(out) + "\n")
PYEDIT
SWS="$(cm sweep status grp)"
SWJ="$(cm --json sweep status grp | python3 -c 'import json, sys; d = json.load(sys.stdin)
print("%(rows)d %(done)d %(open)d %(state_without_evidence)d" % d, ",".join(d["not_in_index"]), ",".join(d["open_rows"]))' 2>&1)"
SWADD="$(cm sweep init grp --under group.cpp --cov fx --add)"
if grep -q "4 functions listed" <<<"$SW" && [ "$swrc" -ne 0 ] && grep -q "exists" <<<"$SWAGAIN" \
   && [ "$SWJ" = "4 1 1 1 lone_fn_renamed caller_ab" ] && grep -q "NOT IN INDEX (1)" <<<"$SWS" \
   && grep -q "STATE WITHOUT EVIDENCE (1)" <<<"$SWS" && grep -q "1 functions added (3 already listed)" <<<"$SWADD" \
   && grep -qE "of coverage fx \((complete|partial|unknown)\), under group.cpp" <<<"$SWHEAD" \
   && grep -q "their recorded actions (classified is not acted on): remove 1$" <<<"$SWS"; then
    ok C-28 "sweep: init lists 4 and refuses to overwrite; its header names the run's completeness; status: 1 done (its action shown), 1 open, 1 state without evidence; a renamed row is flagged and not done; --add adds only the function the rename left unlisted"
else
    bad C-28 "the sweep worklist miscounted, overwrote, passed a renamed row as done, wrote a raw header, or did not show the done rows' actions" "$SW
$SWHEAD
$SWAGAIN
$SWJ
$SWS
$SWADD"
fi

# ---------------------------------------------------------------- C-26 (roadmap item 3)
# Last, because its commits would put a "recorded at another commit" note into the
# reads of every arm after it. Files are added by name: units2/units3 are untracked.
gc() { g add -- "$@" && g commit -q -m "c26 $1"; }
D0="$(git rev-parse --short HEAD)"
mkdir -p .cache/c_doc/coverage && cp -R .cache/codemap/coverage/fx .cache/c_doc/coverage/
cp .cache/codemap/codemap.db .cache/c_doc/          # `tested` reads the index
python3 - "$D0" <<'EOF'
import json, sys
p = ".cache/c_doc/coverage/fx/coverage.json"
d = json.load(open(p)); d.setdefault("meta", {})["head"] = sys.argv[1]; json.dump(d, open(p, "w"))
EOF
J0="$(cmc .cache/c_doc --json status | reasons)"
printf '\nMore notes.\n' >> NOTES.md; mkdir -p docs; printf '# Extra\n' > docs/extra.md
gc NOTES.md docs/extra.md; D1="$(git rev-parse --short HEAD)"
S1="$(cmc .cache/c_doc status)"; J1="$(cmc .cache/c_doc --json status | reasons)"
T1="$(cmc .cache/c_doc tested sink --cov fx)"
mkdir -p sub && printf '# Rules\n' > sub/CLAUDE.md && gc sub/CLAUDE.md; D2="$(git rev-parse --short HEAD)"
mkdir -p tests && printf '# Case\n' > tests/case.md && gc tests/case.md; D3="$(git rev-parse --short HEAD)"
printf '{}\n' > docs/data.json && gc docs/data.json; D4="$(git rev-parse --short HEAD)"
J4="$(cmc .cache/c_doc --json status | reasons)"; T4="$(cmc .cache/c_doc tested sink --cov fx)"
CC="$(py "for x, y in (('$D0', '$D1'), ('$D1', '$D2'), ('$D2', '$D3'), ('$D3', '$D4'), ('0000000a', '$D4')):
    print('%s %s' % codemap.coverage_commit_change('.', x, y))")"
if [ "$J0" != "${J0#*recorded at another commit}" ]; then
    bad C-26 "control: a run at HEAD already read as recorded at another commit" "$J0"
elif [ "$J1" = "${J1#*recorded at another commit}" ] && grep -q "recorded at $D0 (only documentation changed since)" <<<"$S1" \
     && ! grep -q "was recorded at" <<<"$T1" && grep -qx "False 2 Markdown file(s)" <<<"$(sed -n 1p <<<"$CC")"; then
    ok C-26 "a commit changing only Markdown is no reason to refresh, and reads print no note"
else
    bad C-26 "a documentation-only commit still asked for a refresh (or the note was printed)" "$J1
$S1
$T1
$CC"
fi
if [ "$(sed -n 2,4p <<<"$CC")" = "True sub/CLAUDE.md
True tests/case.md
True docs/data.json" ] && [ "$J4" != "${J4#*recorded at another commit}" ] && grep -q "was recorded at $D0" <<<"$T4"; then
    ok C-26b "a CLAUDE.md (read by name), a .md under tests/ and a non-Markdown file under docs/ each keep the reason and the note"
else
    bad C-26b "a commit that can change a run was taken for documentation" "$CC
$J4
$T4"
fi
if grep -qx "None commit 0000000a is not in this checkout (a shallow clone, or a run recorded elsewhere)" <<<"$(sed -n 5p <<<"$CC")"; then
    ok C-26c "a commit git cannot compare keeps the reason and says why"
else
    bad C-26c "an unknown commit was not reported as uncomparable" "$CC"
fi

finish
