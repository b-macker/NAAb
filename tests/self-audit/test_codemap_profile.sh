#!/usr/bin/env bash
# ============================================================
# test_codemap_profile.sh -- `codemap profile` reports what actually ran
#
# tools/codemap/cgprofile.py runs a command under valgrind's callgrind and
# turns the run into folded stacks, flame/icicle SVGs, a summary, and observed
# caller -> callee edges that `callers/callees --run` and `ran` read. A profile
# that maps a frame to the wrong symbol is worse than none -- it says code ran
# that did not -- so every mapping rule is checked on the codemap fixture,
# compiled and run, with a control on the same run:
#
#   P-01  without valgrind: the install command is printed, nothing is written
#   P-02  the run is recorded: exit status, instruction count, folded stacks
#   P-03  a virtual call is narrowed by the run: drive() reached Leaf::run,
#         and Derived::run -- which DID run, from registerExecutor -- reads
#         "not on this edge", not "not run" (control: the static map lists both)
#   P-04  a call through std::function is observed (the static map has it
#         only as [value])
#   P-05  lambdas are matched by owner, SIGNATURE and ordinal: b is GCC's
#         `{lambda(std::string const&)#1}` though it is the third lambda, and
#         c, taking std::string by value, is a separate `{lambda(std::string)#1}`
#   P-06  a function the run never called is reported NOT executed (control:
#         sink, called from six places, is counted six times)
#   P-07  a thread's work hangs off the thread, not the function that started
#         it, and glibc's own `start_thread` is never matched to the fixture's
#         (system-library frames do not map, whatever their name)
#   P-08  flame and icicle SVGs are well-formed and opposite in orientation;
#         `flame --focus main` draws sink, which the whole-run graph elides as
#         narrower than a pixel beside the loader's start-up cost
#   P-09  folded stacks are `a;b;c COST`, with no empty frame names
#   P-10  the summary is ASCII and says the cost is instructions, not time
#   P-11  --run with a profile that does not exist is refused
#   P-13  a frame is matched by its parameter types, not its name alone:
#         over(int) ran twice, over(string) once and over(double) never, so
#         a match by name alone fails whichever is listed first (a); the const and the
#         non-const Box::get are counted apart, 1 and 3 (b); Pt's own
#         constructor ran once though the implicit copy constructor -- same
#         name -- ran twice more (c); hold(shared_ptr<int>) and
#         hold(shared_ptr<double>), alike once template arguments are dropped,
#         are counted apart, 1 and 2 (d); the two static helper(int)s, one per
#         file, are told apart by the frame's source file, 2 and 3 (e)
#   P-14  GCC's [abi:cxx11] tag (every function returning std::string) does
#         not hide a function (a); a parameter written as an alias matches its
#         canonical demangled type (b); a variadic template matches its
#         instantiations with an empty pack and with two arguments (c)
#   P-15  a lambda inside a lambda is numbered within its owner, so the second
#         top-level lambda is #2 of the function (a); a generic lambda's
#         templated operator()<int> is matched (b)
#   P-16  frames the compiler writes for a declaration are never the
#         declaration: a this-adjusting thunk is not counted as Pair::r (a), and
#         `TLS wrapper function for tl_name` is not reported as a call to the
#         VARIABLE tl_name (b; control: the index has tl_name, so a name-only
#         match had something to land on)
#   P-17  repo frames that match no single definition are enumerated, exactly:
#         the implicit Pt copy constructor and the ambiguous pick_t templates,
#         nothing else (a); a function such a frame could be reads UNMEASURED,
#         not NOT executed (b; control: dead_function, P-06, stays NOT executed)
#   P-18  the fixture built WITHOUT debug information (the NAAb default build
#         has no -g): the summary says so and calls the implicit-member count
#         UNMEASURABLE instead of printing 0, and the two static helper(int)s --
#         which only a source file tells apart -- read UNMEASURED (control:
#         overloads are still told apart by their parameter types)
#   P-12  the callgrind parser, on a hand-written file (no valgrind needed):
#         a `cob=` names the object of the NEXT call only (P-12a), and a
#         function's own `fn=` block decides its object over an earlier call
#         record's (P-12b) -- each rule alone, so either can fail by itself
#
# The index arms need libclang (to map frames onto symbols), valgrind and a
# C++ compiler; each missing piece is UNMEASURABLE, not a pass.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
FIXTURE="$SCRIPT_DIR/codemap_fixture"
. "$REPO/tests/helpers/native_path.sh"
export PYTHONDONTWRITEBYTECODE=1

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | sed 's/^/       | /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }
finish() { echo "Result: $PASS passed, $FAIL failed, $SKIP skipped"; [ "$FAIL" -eq 0 ]; exit $?; }

echo "=== codemap profile reports what actually ran ==="

if ! command -v python3 >/dev/null 2>&1; then
    skip P-ALL "python3 not found"
    finish
fi

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
cp -R "$FIXTURE/." "$W/"
mkdir -p "$W/build"
cd "$W" || exit 1
CM="$(native_path "$REPO/tools/codemap/codemap.py")"
cm() { python3 "$CM" --repo . --cache .cache/codemap "$@" 2>&1; }

# ---------------------------------------------------------------- P-01
OUT="$(CODEMAP_VALGRIND=/nonexistent/valgrind-codemap cm profile --name nope -- /bin/true)"; rc=$?
if [ "$rc" -ne 0 ] && grep -q "apt-get install valgrind" <<<"$OUT" && [ ! -e .cache/codemap/profiles/nope ]; then
    ok P-01 "without valgrind: the install command is printed and nothing is written"
else
    bad P-01 "a missing valgrind was not reported, or a profile was written (rc=$rc)" "$OUT"
fi

# ---------------------------------------------------------------- P-12
# The object a frame belongs to decides whether it may map onto the index at
# all, so the two rules that set it are checked on input written for each.
PARSE="$(python3 - "$(native_path "$REPO/tools/codemap")" <<'EOF' 2>&1
import os, sys, tempfile
sys.path.insert(0, sys.argv[1])
import cgprofile
def objs(text):
    fd, path = tempfile.mkstemp()
    with os.fdopen(fd, "w") as fh:
        fh.write(text)
    try:
        return cgprofile.parse_callgrind(path)["obj"]
    finally:
        os.remove(path)
# a: main (in the program) calls puts (in libc, named by cob=) and then
# Leaf::run with no cob= -- so Leaf::run is in main's object, the program.
a = objs("""events: Ir
ob=(1) /work/build/fixture
fn=(1) main
cob=(2) /usr/lib/x86_64-linux-gnu/libc.so.6
cfn=(2) puts
calls=1 0
0 100
cfn=(3) Leaf::run()
calls=1 0
0 20
0 5
""")
print("A", a.get("Leaf::run()"))
# b: an earlier call record names Worker::go's object as libc (inconsistent
# input), and Worker::go's own block then names the program.
b = objs("""events: Ir
ob=(1) /work/build/fixture
fn=(1) main
cob=(2) /usr/lib/x86_64-linux-gnu/libc.so.6
cfn=(3) Worker::go()
calls=1 0
0 9
ob=(1)
fn=(3)
0 9
""")
print("B", b.get("Worker::go()"))
EOF
)"
if grep -q "^A /work/build/fixture$" <<<"$PARSE"; then
    ok P-12a "a cob= names the object of the next call only; Leaf::run stays in the program"
else
    bad P-12a "a stale cob= leaked into a later call record" "$PARSE"
fi
if grep -q "^B /work/build/fixture$" <<<"$PARSE"; then
    ok P-12b "a function's own fn= block decides its object"
else
    bad P-12b "an earlier call record's object outranked the function's own block" "$PARSE"
fi

# ---------------------------------------------------------------- harness
if ! command -v valgrind >/dev/null 2>&1; then
    skip P-00 "valgrind not installed (apt-get install valgrind); run arms not measured"
    finish
fi
CXX="$(command -v c++ || command -v g++ || true)"
if [ -z "$CXX" ]; then
    skip P-00 "no C++ compiler to build the fixture"
    finish
fi
if ! grep -q LIBCLANG_OK <<<"$(python3 -c "import clang.cindex as ci; ci.Index.create(); print('LIBCLANG_OK')" 2>&1)"; then
    skip P-00 "libclang Python bindings unavailable (python3-clang); frames cannot be mapped"
    finish
fi
python3 - <<'EOF'
import json, os
w = os.getcwd()
cmds = [{"directory": os.path.join(w, "build"), "file": os.path.join(w, f),
         "arguments": ["c++", "-std=c++17", "-I" + os.path.join(w, "include"), "-c",
                       os.path.join(w, f), "-o", f + ".o"]}
        for f in ("fixture.cpp", "other.cpp")]
with open(os.path.join(w, "build", "compile_commands.json"), "w") as fh:
    json.dump(cmds, fh, indent=1)
EOF
BUILD="$("$CXX" -std=c++17 -O0 -g -Iinclude fixture.cpp other.cpp -o build/fixture -pthread 2>&1)"; brc=$?
IDX="$(cm index --jobs 2)"; irc=$?
if [ "$brc" -ne 0 ] || [ "$irc" -ne 0 ]; then
    skip P-00 "fixture did not build (rc=$brc) or index (rc=$irc) here"
    printf '%s\n%s\n' "$BUILD" "$IDX" | sed 's/^/       | /'
    finish
fi

# ---------------------------------------------------------------- P-02
OUT="$(cm profile --name fx -- ./build/fixture)"; rc=$?
P=.cache/codemap/profiles/fx
if [ "$rc" -eq 0 ] && grep -q "exit status 0, [0-9,]* Ir" <<<"$OUT" && [ -s "$P/folded.txt" ] \
   && python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert d['total_ir'] > 0 and d['edges']" "$P/run.json" 2>/dev/null; then
    ok P-02 "the run is recorded: exit status, instructions, stacks, observed edges"
else
    bad P-02 "profile run not recorded (rc=$rc)" "$OUT"
    finish
fi

# ---------------------------------------------------------------- P-03
RUN="$(cm callees drive --depth 1 --run fx)"
STATIC="$(cm callees drive --depth 1)"
DER="$(cm ran fx --fn Derived::run)"
if grep -q "Leaf::run .*\[virtual, ran 1x\]" <<<"$RUN" && grep -q "Derived::run .*\[virtual, not on this edge\]" <<<"$RUN" \
   && grep -q "called 1 times in run fx" <<<"$DER" \
   && grep -q "Derived::run .*\[virtual\]" <<<"$STATIC" && grep -q "Leaf::run .*\[virtual\]" <<<"$STATIC"; then
    ok P-03 "the run narrows drive()'s virtual call to Leaf::run; Derived::run ran elsewhere, not on this edge"
else
    bad P-03 "virtual call not narrowed by the run, or an override that ran elsewhere read as not run" "$RUN
$DER
$STATIC"
fi

# ---------------------------------------------------------------- P-04 / P-05
CS="$(cm callers sink --depth 1 --run fx)"
if grep -q "<- via_value .*\[value, ran 1x\]" <<<"$CS"; then
    ok P-04 "a call through std::function is observed (statically only [value])"
else
    bad P-04 "the std::function call was not observed" "$CS"
fi
TWO="$(cm ran fx --fn via_two_lambdas)"
if grep -q "<- via_two_lambdas::b .*\[ast, ran 1x\]" <<<"$CS" \
   && grep -q "via_two_lambdas::a  *1x" <<<"$TWO" && grep -q "via_two_lambdas::b  *1x" <<<"$TWO" \
   && grep -q "via_two_lambdas::c  *1x" <<<"$TWO"; then
    ok P-05 "lambdas matched by signature and ordinal: b (string const&) is #1 though third, c (string) is its own #1"
else
    bad P-05 "lambda frames matched to the wrong lambda" "$CS
$TWO"
fi

# ---------------------------------------------------------------- P-06
DEAD="$(cm ran fx --fn dead_function)"
SINK="$(cm ran fx --fn sink)"
if grep -q "NOT executed in run fx" <<<"$DEAD" && grep -q "called 6 times in run fx" <<<"$SINK"; then
    ok P-06 "an uncalled function is NOT executed; sink is counted at all six call sites"
else
    bad P-06 "execution counts wrong" "$DEAD
$SINK"
fi

# ---------------------------------------------------------------- P-07
WK="$(cm ran fx --fn worker)"
ST="$(cm ran fx --fn start_thread)"
if grep -q "start_thread::<lambda@fixture.cpp:36>  *1x" <<<"$WK" && grep -q "called 1 times in run fx" <<<"$ST" \
   && grep -q "calls (0):" <<<"$ST"; then
    ok P-07 "the thread's work hangs off its lambda; glibc's start_thread is not the fixture's"
else
    bad P-07 "thread attribution or the system-library filter is wrong" "$WK
$ST"
fi

# ---------------------------------------------------------------- P-13
OVER="$(cm ran fx --fn over)"
if grep -qE "^over  \(fixture.cpp:[0-9]+\): called 2 times in run fx" <<<"$OVER" \
   && grep -qE "^over  \(fixture.cpp:[0-9]+\): called 1 times in run fx" <<<"$OVER" \
   && grep -qE "^over  \(fixture.cpp:[0-9]+\): NOT executed in run fx" <<<"$OVER"; then
    ok P-13a "overloads are told apart by parameter types: over(int) 2x, over(string) 1x, over(double) never"
else
    bad P-13a "overloads merged or misattributed" "$OVER"
fi
BOX="$(cm ran fx --fn Box::get)"
if grep -q "): called 1 times in run fx" <<<"$BOX" && grep -q "): called 3 times in run fx" <<<"$BOX"; then
    ok P-13b "the const and non-const Box::get are counted apart (1 and 3)"
else
    bad P-13b "const and non-const overloads merged" "$BOX"
fi
PT="$(cm ran fx --fn Pt::Pt)"
if grep -q "called 1 times in run fx" <<<"$PT" && ! grep -q "called 3 times" <<<"$PT"; then
    ok P-13c "Pt's constructor ran once; the implicit copy constructor's two calls are not credited to it"
else
    bad P-13c "the implicit copy constructor was matched to the user constructor" "$PT"
fi

HOLD="$(cm ran fx --fn hold)"
if grep -q "): called 1 times in run fx" <<<"$HOLD" && grep -q "): called 2 times in run fx" <<<"$HOLD"; then
    ok P-13d "hold(shared_ptr<int>) and hold(shared_ptr<double>) are counted apart (1 and 2)"
else
    bad P-13d "overloads differing in a template argument were merged" "$HOLD"
fi
HELP="$(cm ran fx --fn helper)"
if grep -q "^helper  (fixture.cpp:[0-9]*): called 2 times in run fx" <<<"$HELP" \
   && grep -q "^helper  (other.cpp:[0-9]*): called 3 times in run fx" <<<"$HELP"; then
    ok P-13e "the static helper(int) of each file is told apart by the frame's source file (2 and 3)"
else
    bad P-13e "same-named static functions in different files were merged or lost" "$HELP"
fi

# ---------------------------------------------------------------- P-14
ML="$(cm ran fx --fn make_label)"
if grep -q "called 2 times in run fx" <<<"$ML"; then
    ok P-14a "a function GCC tags [abi:cxx11] is matched (called 2 times)"
else
    bad P-14a "an abi-tagged frame was not matched" "$ML"
fi
TA="$(cm ran fx --fn takes_alias)"
if grep -q "called 1 times in run fx" <<<"$TA"; then
    ok P-14b "a parameter written as an alias matches its canonical demangled type"
else
    bad P-14b "an alias-typed parameter did not match" "$TA"
fi

CA="$(cm ran fx --fn count_args)"
if grep -q "called 2 times in run fx" <<<"$CA"; then
    ok P-14c "a variadic template matches its instantiations, empty pack and two arguments"
else
    bad P-14c "a parameter-pack instantiation was not matched" "$CA"
fi

# ---------------------------------------------------------------- P-15
SEC="$(cm ran fx --fn nested_lambdas::second)"
INN="$(cm ran fx --fn nested_lambdas::outer::inner)"
if grep -q "called 2 times in run fx" <<<"$SEC" && grep -q "called 1 times in run fx" <<<"$INN"; then
    ok P-15a "a lambda inside a lambda is numbered within its owner; the second top-level lambda ran 2x"
else
    bad P-15a "nested lambda numbering confused" "$SEC
$INN"
fi
GEN="$(cm ran fx --fn generic_lambda::g)"
if grep -q "called 2 times in run fx" <<<"$GEN"; then
    ok P-15b "a generic lambda's operator()<int> is matched"
else
    bad P-15b "a generic lambda was not matched" "$GEN"
fi

# ---------------------------------------------------------------- P-16
PR="$(cm ran fx --fn Pair::r)"
if grep -q "called 1 times in run fx" <<<"$PR"; then
    ok P-16a "a call through a this-adjusting thunk counts once, as Pair::r"
else
    bad P-16a "the thunk was counted as Pair::r, or the call was lost" "$PR"
fi
TLS="$(cm callees touch_tls --run fx)"
TLV="$(cm find tl_name)"
if grep -q "var_decl .*tl_name" <<<"$TLV" && ! grep -q "tl_name" <<<"$TLS"; then
    ok P-16b "the TLS wrapper is not reported as a call to the variable tl_name (which the index has)"
elif ! grep -q "var_decl .*tl_name" <<<"$TLV"; then
    bad P-16b "control: the index has no tl_name, so this arm cannot fail" "$TLV"
else
    bad P-16b "a compiler-generated frame was matched to the variable it serves" "$TLS"
fi

# ---------------------------------------------------------------- P-17
UM="$(python3 -c "import json,sys; print(' '.join(sorted('%s/%s' % (u['name'], u['arity']) for u in json.load(open(sys.argv[1]))['unmatched'])))" "$P/run.json" 2>&1)"
if [ "$UM" = "Pt::Pt/1 pick_t/1" ] && grep -q "Pt::Pt/1" "$P/summary.txt" && grep -q "pick_t/1" "$P/summary.txt"; then
    ok P-17a "unmatched repo frames are exactly the implicit Pt copy and the ambiguous pick_t, and the summary says so"
else
    bad P-17a "unmatched repo frames are not exactly {Pt::Pt/1, pick_t/1}: '$UM'" "$(grep -A4 'matched to no single' "$P/summary.txt")"
fi
PK="$(cm ran fx --fn pick_t)"
if [ "$(grep -c "^pick_t  (include/fixture.h:10[34]): UNMEASURED in run fx" <<<"$PK")" -ge 2 ] \
   && ! grep -q "NOT executed" <<<"$PK" && grep -q "NOT executed in run fx" <<<"$DEAD"; then
    ok P-17b "both pick_t templates read UNMEASURED, not NOT executed; dead_function stays NOT executed"
else
    bad P-17b "an ambiguous frame's candidates were reported as not run" "$PK"
fi

# ---------------------------------------------------------------- P-18
NDB="$("$CXX" -std=c++17 -O0 -Iinclude fixture.cpp other.cpp -o build/fixture_nodebug -pthread 2>&1)"; nrc=$?
if [ "$nrc" -ne 0 ]; then
    skip P-18 "the fixture did not build without -g (rc=$nrc)"
else
    OUT="$(cm profile --name nd -- ./build/fixture_nodebug)"; rc=$?
    NDS="$(cat .cache/codemap/profiles/nd/summary.txt 2>&1)"
    NDH="$(cm ran nd --fn helper)"
    NDO="$(cm ran nd --fn over)"
    if [ "$rc" -eq 0 ] && grep -q "NO DEBUG INFO" <<<"$NDS" && grep -q "UNMEASURABLE without debug info" <<<"$NDS" \
       && [ "$(grep -c "^helper  (.*): UNMEASURED in run nd" <<<"$NDH")" -eq 2 ] \
       && grep -q "): called 2 times in run nd" <<<"$NDO" && grep -q "): called 1 times in run nd" <<<"$NDO"; then
        ok P-18 "without -g: the summary says so, implicit members are UNMEASURABLE, the helpers UNMEASURED; overloads still apart"
    else
        bad P-18 "a run without debug info was reported as if measured (rc=$rc)" "$NDS
$NDH
$NDO"
    fi
fi

# ---------------------------------------------------------------- P-08
FOCUS="$(cm flame fx --focus main)"; frc=$?
ORIENT="$(python3 - "$P/flame-main.svg" "$P/icicle-main.svg" "$P/flame.svg" "$P/icicle.svg" <<'EOF' 2>&1
import sys, xml.etree.ElementTree as ET
ns = "{http://www.w3.org/2000/svg}"
for path in sys.argv[3:5]:                      # the whole-run graphs parse and have a root
    titles = [g.find(ns + "title").text for g in ET.parse(path).getroot().findall(ns + "g")]
    assert any(t.startswith("all (") for t in titles), "no root frame in " + path
ys = []
for path in sys.argv[1:3]:
    root = ET.parse(path).getroot()
    groups = root.findall(ns + "g")
    titles = [g.find(ns + "title").text for g in groups]
    assert any(t.startswith("sink (") for t in titles), "no sink frame in " + path
    allg = [g for g in groups if g.find(ns + "title").text.startswith("all (")][0]
    ys.append(float(allg.find(ns + "rect").get("y")))
    others = [float(g.find(ns + "rect").get("y")) for g in groups if g is not allg]
    ys.append(max(others) if "flame" in path else min(others))
flame_root, flame_other, icicle_root, icicle_other = ys
print("ORIENT_OK" if flame_root > flame_other and icicle_root < icicle_other else "ORIENT_BAD %s" % ys)
EOF
)"
if [ "$frc" -eq 0 ] && grep -q "^ORIENT_OK" <<<"$ORIENT"; then
    ok P-08 "SVGs parse; focused on main they draw sink; root at the bottom of one, the top of the other"
else
    bad P-08 "SVG malformed, focus failed, or orientation wrong (rc=$frc)" "$FOCUS
$ORIENT"
fi
OUT="$(cm flame fx --focus no_such_frame_codemap)"; rc=$?
if [ "$rc" -ne 0 ] && grep -q "no stack in run" <<<"$OUT"; then
    ok P-08b "a focus no stack passes through is refused, not drawn as an empty graph"
else
    bad P-08b "an unmatched focus was accepted (rc=$rc)" "$OUT"
fi

# ---------------------------------------------------------------- P-09
BADLINES="$(grep -cvE '^[^ ]+( [^ ]+)* [0-9]+$' "$P/folded.txt")"
EMPTY="$(grep -c ';;\|^;' "$P/folded.txt")"
LEAF="$(grep -cE '(^|;)main;drive;Leaf::run;sink( |;)' "$P/folded.txt")"
if [ "$BADLINES" -eq 0 ] && [ "$EMPTY" -eq 0 ] && [ "$LEAF" -gt 0 ]; then
    ok P-09 "folded stacks are 'a;b;c COST' with no empty frames; main;drive;Leaf::run;sink is there"
else
    bad P-09 "folded format wrong (bad=$BADLINES empty=$EMPTY leaf=$LEAF)" "$(head -5 "$P/folded.txt")"
fi

# ---------------------------------------------------------------- P-10
if LC_ALL=C grep -q '[^[:print:][:space:]]' "$P/summary.txt"; then
    bad P-10 "non-ASCII bytes in the summary"
elif grep -q "not time" "$P/summary.txt" && grep -q "hottest by SELF cost" "$P/summary.txt"; then
    ok P-10 "the summary is ASCII and says the cost is instructions, not time"
else
    bad P-10 "summary missing its caveat or sections" "$(head -12 "$P/summary.txt")"
fi

# ---------------------------------------------------------------- P-11
OUT="$(cm callers sink --run no-such-profile)"; rc=$?
if [ "$rc" -ne 0 ] && grep -q "no profile 'no-such-profile'" <<<"$OUT"; then
    ok P-11 "--run with an unknown profile is refused, not read as 'nothing ran'"
else
    bad P-11 "an unknown profile was accepted (rc=$rc)" "$OUT"
fi

finish
