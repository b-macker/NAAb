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
#   C-10  positions matched to no single definition are enumerated EXACTLY:
#         the one line holding twin_a and twin_b, nothing else -- so a new
#         fixture shape that fails to map turns this red; both twins read
#         UNMEASURED (twin_b never ran, but only position could say so), and
#         untested does not list twin_b as executed by no unit
#
# Needs a C++ compiler, gcov and libclang; each missing piece is UNMEASURABLE.
# ============================================================
set -uo pipefail

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
CM="$(native_path "$REPO/tools/codemap/codemap.py")"
cm() { python3 "$CM" --repo . --cache .cache/codemap "$@" 2>&1; }
g() { git -c user.name=fixture -c user.email=fixture@example.invalid -c commit.gpgsign=false "$@"; }
git init -q . && git config core.autocrlf false
printf '.cache/\nbuild/\nbuild-nowrap/\nunits/\n' > .gitignore
g add -A && g commit -q -m "fixture"

# --- builds: with the wrap (as NAAB_COVERAGE links naab-lang) and without it -------
build() {  # dir wrap(yes|no)
    mkdir -p "$1"
    ( cd "$1" && for f in fixture other templ; do
          "$CXX" -std=c++17 -O0 -g --coverage -I../include -c "../$f.cpp" -o "$f.cpp.o" || exit 1
      done
      if [ "$2" = yes ]; then
          "$CC" -c "$WRAP" -o coverage_exit.o || exit 1
          "$CXX" --coverage -Wl,--wrap=_exit -Wl,--wrap=_Exit fixture.cpp.o other.cpp.o templ.cpp.o coverage_exit.o -o fixture -pthread
      else
          "$CXX" --coverage fixture.cpp.o other.cpp.o templ.cpp.o -o fixture -pthread
      fi ) > "$W/build-$2.log" 2>&1
}
if ! build build yes || ! build build-nowrap no; then
    skip C-ALL "the fixture did not build with --coverage here"; cat "$W"/build-*.log | sed 's/^/       | /'; finish
fi
python3 - <<'EOF'
import json, os
w = os.getcwd()
json.dump([{"directory": os.path.join(w, "build"), "file": os.path.join(w, f),
            "arguments": ["c++", "-std=c++17", "-I" + os.path.join(w, "include"), "-c", os.path.join(w, f), "-o", f + ".o"]}
           for f in ("fixture.cpp", "other.cpp", "extra.cpp", "templ.cpp")],
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

finish
