#!/usr/bin/env bash
# ============================================================
# test_codemap.sh -- tools/codemap answers what its fixture says, and says
# when it cannot
#
# codemap (tools/codemap/codemap.py) indexes the C++ source with libclang and
# answers callers/callees/reach/types/threads/tags queries, so a reviewer can
# replace a grep sweep with one query. A map that silently drops an edge kind
# is worse than no map: an empty answer reads as "nothing calls this". So every
# edge kind it claims is planted in tests/self-audit/codemap_fixture/ and
# asserted here, each beside a control that the same query CAN return an
# answer -- an absence assertion with no control passes for a broken index.
#
#   CM-00  harness: libclang parses the fixture with no errors and the index
#          holds both translation units (else the index arms are UNMEASURABLE)
#   CM-01  [ast] direct callers; control: an uncalled function has 0 callers
#   CM-02  [lambda] a call inside a lambda belongs to the lambda, which the
#          function defines; a call TO the lambda lands on it, not on an
#          operator() named by an absolute path
#   CM-03  [value] a function passed as a std::function is reported where it
#          is taken as a value
#   CM-04  [virtual] a call through Base::run reaches both overrides, and the
#          class tree lists them two levels down
#   CM-05  [thread] a std::thread start, and what the thread runs
#   CM-06  [dispatch] a stdlib-shaped call() branch maps `module.fn` to the
#          code in THAT branch only (control: the other branch's callee);
#          CM-06b the trailing else is reached by no NAAb name
#   CM-07  [dispatch] registerExecutor maps a language to the registered class,
#          not to the parameter's base class
#   CM-08  reach: an entry point reaching the sink only through the gate is
#          not reported, one reaching it without the gate is; control: with no
#          gate both are reported. A gate naming nothing is refused.
#   CM-09  blind spot: code under #ifdef _WIN32 is not indexed on this build
#          (control: a function beside it is)
#   CM-10  includes, both directions
#   CM-11  task tags and record citations; a label-shaped "A4:" is not a citation
#   CM-12  uml: inheritance edges, {abstract} only on the pure virtual
#   CM-13  uml --render refuses a jar whose sha256 is not pinned, without running it
#   CM-14  incremental index: unchanged = 0 parsed; a header edit re-parses
#          both files that include it, a .cpp edit only that file
#   CM-15  --json is valid JSON and every output is ASCII
#   CM-16  without libclang: index explains how to install it and fails;
#          queries on an existing index still work
#   CM-17  paths: main reaches sink; the reverse direction finds nothing
#   CM-18  every bookmark in tools/codemap/anchors.txt still resolves in this
#          tree (no index: screened by grep), and --check fails on a broken one
#   CM-19  a lambda stored in a named variable is found by that name (control:
#          an unnamed lambda keeps its location name)
#   CM-20  writers: ++ and = on a field, += through an overloaded operator on
#          std::atomic, push_back as a mutation; a function that only READS the
#          field is not a writer, and a local variable is not recorded (control:
#          the same query returns three sites for the function that does write)
#   CM-21  [predispatch]: the allowlist read from the fixture's
#          filesystemAccessMode gates fixfile.read before dispatch and not
#          fixfile.write, which it maps to ""; CM-21b: with the dispatcher no
#          longer calling the gate, the edges are withheld and stats says why
#   CM-22  a function DEFINED in two files on one USR (main(int, char**) in a
#          two-file repo, as naab-lang's and naab-gov's are) is stored once,
#          and stats lists it with the copy kept; a static helper of the same
#          name in both files (file-qualified USRs) is not listed; CM-22c: the
#          fixture, whose two files each have a static helper(int), reads
#          "none"
#   CM-23  `codemap status` -- the line the session-start hook prints -- tells
#          the states apart, with libclang FORCED by stand-in modules so the
#          arms do not depend on this machine: (a) libclang loads, a compile
#          database, no index -> NO INDEX and how to build one; (b) no compile
#          database -> configure first; (c) bindings absent -> UNAVAILABLE;
#          (d) bindings import but the library does not load -> UNAVAILABLE
#          too (importing is not loading); after the index is built: (e) index
#          READY with nothing newer, then a source touched after the index ->
#          "newer than it"; (f) READY but libclang missing -> cannot refresh.
#          Every status line carries the authority sentence (the method decides)
#   CM-24  .claude/hooks/session-start.sh, run as a local session runs it,
#          prints exactly the status line and exits 0 (a); (c) with no
#          codemap to ask it prints "status UNMEASURABLE" and still exits 0 --
#          silence there would read as "nothing to report"
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
FIXTURE="$SCRIPT_DIR/codemap_fixture"
. "$REPO/tests/helpers/native_path.sh"
# The tool must not write into the tree it is reading (tools/codemap/__pycache__).
export PYTHONDONTWRITEBYTECODE=1

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | sed 's/^/       | /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }

echo "=== codemap answers what its fixture says ==="

if ! command -v python3 >/dev/null 2>&1; then
    skip CM-ALL "python3 not found"
    echo "Result: $PASS passed, $FAIL failed, $SKIP skipped"
    exit 0
fi

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
cp -R "$FIXTURE/." "$W/"
mkdir -p "$W/build"
# Paths below are RELATIVE to $W and resolved by python itself, so the shell's
# path vocabulary never reaches a native python3 (tests/helpers/native_path.sh).
cd "$W" || exit 1
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

# A native python3 (MSYS2) cannot open the shell's /d/a/... spelling.
CM="$(native_path "$REPO/tools/codemap/codemap.py")"
cm() { python3 "$CM" --repo . --cache .cache/codemap "$@" 2>&1; }

# ---------------------------------------------------------------- CM-16 (part)
# Index without libclang must say how to install it, and fail -- not build an
# empty index that every later query would read as "nothing calls this".
NOCLANG="$(python3 -c "
import runpy, sys
sys.modules['clang'] = None            # import clang -> ImportError
sys.argv = ['codemap', '--repo', '.', '--cache', '.cache/noclang', 'index']
try:
    runpy.run_path('$CM', run_name='__main__')
except SystemExit as e:
    print('EXIT', e.code if isinstance(e.code, int) else 1)
    if not isinstance(e.code, int): print(e.code)
" 2>&1)"
if grep -q "python3-clang" <<<"$NOCLANG" && grep -q "^EXIT 1" <<<"$NOCLANG" && [ ! -e .cache/noclang/codemap.db ]; then
    ok CM-16a "index without libclang names the package to install and builds nothing"
else
    bad CM-16a "index without libclang did not explain itself and fail" "$NOCLANG"
fi

# ---------------------------------------------------------------- CM-23 (a-d)
mkdir -p fake-missing/clang fake-broken/clang fake-ok/clang bare
printf 'raise ImportError("hidden by test_codemap.sh")\n' > fake-missing/clang/__init__.py
: > fake-broken/clang/__init__.py
printf 'class Index:\n    @staticmethod\n    def create():\n        raise OSError("libclang.so: cannot open shared object file")\n' > fake-broken/clang/cindex.py
: > fake-ok/clang/__init__.py
printf 'class Index:\n    @staticmethod\n    def create():\n        return object()\n' > fake-ok/clang/cindex.py
cmst() { PYTHONPATH="$1" python3 "$CM" --repo "$2" --cache "$3" status 2>&1; }
AUTH="docs/investigation-method.md decides"
S_OK="$(cmst fake-ok . .cache/none)"
S_BARE="$(cmst fake-ok bare bare/.cache)"
S_MISS="$(cmst fake-missing . .cache/none)"
S_BROKE="$(cmst fake-broken . .cache/none)"
if grep -q '^codemap: NO INDEX: `python3 tools/codemap/codemap.py index` builds one' <<<"$S_OK" \
   && grep -q "coverage: none" <<<"$S_OK" && grep -q "$AUTH" <<<"$S_OK"; then
    ok CM-23a "status: libclang loads, no index -> NO INDEX with the command that builds one"
else
    bad CM-23a "status did not report a missing index" "$S_OK"
fi
if grep -q "^codemap: NO INDEX: configure the build first" <<<"$S_BARE"; then
    ok CM-23b "status: no compile database -> configure first"
else
    bad CM-23b "status did not report the missing compile database" "$S_BARE"
fi
if grep -q "^codemap: UNAVAILABLE: no index, and libclang missing" <<<"$S_MISS" && grep -q "$AUTH" <<<"$S_MISS"; then
    ok CM-23c "status: bindings absent -> UNAVAILABLE, gates by hand"
else
    bad CM-23c "status did not report libclang absent" "$S_MISS"
fi
if grep -q "^codemap: UNAVAILABLE: no index, and libclang broken" <<<"$S_BROKE"; then
    ok CM-23d "status: bindings import but the library does not load -> UNAVAILABLE (import is not load)"
else
    bad CM-23d "status took importable bindings for a working libclang" "$S_BROKE"
fi

# ---------------------------------------------------------------- CM-24c
HOOK="$REPO/.claude/hooks/session-start.sh"
HK="$(env -u CLAUDE_CODE_REMOTE CLAUDE_PROJECT_DIR="$(native_path "$W/bare")" bash "$HOOK" 2>&1)"; hrc=$?
if [ "$hrc" -eq 0 ] && grep -q "^codemap: status UNMEASURABLE" <<<"$HK"; then
    ok CM-24c "the hook with no codemap to ask says status UNMEASURABLE and exits 0"
else
    bad CM-24c "the hook's failed status query was silent or failed the hook (rc=$hrc)" "$HK"
fi

# ---------------------------------------------------------------- CM-18
# Bookmarks resolve against THIS tree. No index needed: symbol anchors are
# located by grep (screened), grep anchors read the file.
ANCH="$(cd "$REPO" && python3 tools/codemap/codemap.py --cache .cache/codemap-absent-$$ anchors --check 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && ! grep -q UNRESOLVED <<<"$ANCH" && grep -q "^enforce " <<<"$ANCH"; then
    ok CM-18 "every bookmark in tools/codemap/anchors.txt resolves ($(grep -c ':[0-9]' <<<"$ANCH") lines)"
else
    bad CM-18 "a bookmark no longer resolves (rename? update tools/codemap/anchors.txt)" "$ANCH"
fi
printf 'gone\tsymbol:no_such_function_anywhere_codemap\tcontrol\nfine\tgrep:fixture.cpp:dead_function\tcontrol\n' > anchors.txt
BROKEN="$(cm --anchors anchors.txt anchors --check)"; rc=$?
if [ "$rc" -ne 0 ] && grep -q "^gone .*UNRESOLVED" <<<"$BROKEN" && grep -q "^fine .*fixture.cpp:" <<<"$BROKEN"; then
    ok CM-18c "--check fails on a broken bookmark and still resolves the good one (control)"
else
    bad CM-18c "--check did not separate a broken bookmark from a good one (rc=$rc)" "$BROKEN"
fi

# ---------------------------------------------------------------- CM-00
LIBCLANG="$(python3 -c "
import clang.cindex as ci
ci.Index.create()
print('LIBCLANG_OK')" 2>&1)"
if ! grep -q LIBCLANG_OK <<<"$LIBCLANG"; then
    skip CM-00 "libclang Python bindings unavailable (apt-get install python3-clang); index arms not run"
    echo "Result: $PASS passed, $FAIL failed, $SKIP skipped"
    [ "$FAIL" -eq 0 ]; exit $?
fi
IDX="$(cm index --jobs 2)"; rc=$?
STATS="$(cm --json stats)"
SHAPE="$(python3 -c "
import json, sys
d = json.loads(sys.stdin.read())
print('sources', d['sources'], 'errors', d['parse_errors'], 'symbols', d['symbols'])
" <<<"$STATS" 2>&1)"
if [ "$rc" -eq 0 ] && grep -q "^sources 2 errors 0 symbols [1-9][0-9]" <<<"$SHAPE"; then
    ok CM-00 "fixture indexed: $SHAPE"
else
    skip CM-00 "the fixture did not index cleanly here (rc=$rc: $SHAPE) -- index arms not run"
    printf '%s\n' "$IDX" | sed 's/^/       | /'
    echo "Result: $PASS passed, $FAIL failed, $SKIP skipped"
    [ "$FAIL" -eq 0 ]; exit $?
fi

# ---------------------------------------------------------------- CM-01
OUT="$(cm callers sink --depth 1)"
if grep -q "<- gated_caller .*\[ast\] fixture.cpp:10" <<<"$OUT" && grep -q "<- ungated_caller .*\[ast\] fixture.cpp:12" <<<"$OUT"; then
    ok CM-01 "direct callers carry their call site"
else
    bad CM-01 "direct callers of sink missing or misplaced" "$OUT"
fi
DEAD="$(cm explain dead_function)"
if grep -q "callers   0 sites" <<<"$DEAD" && grep -q "at        fixture.cpp:" <<<"$DEAD"; then
    ok CM-01c "an uncalled function is found and has 0 callers (the same query answers above)"
else
    bad CM-01c "dead_function not found, or reported with callers" "$DEAD"
fi

# ---------------------------------------------------------------- CM-02
OUT="$(cm callers sink --depth 2)"
if grep -q "<- via_lambda::f .*\[ast\] fixture.cpp:18" <<<"$OUT" \
   && grep -q "<- via_lambda .*\[lambda\] fixture.cpp:18" <<<"$OUT"; then
    ok CM-02 "a call inside a lambda belongs to the lambda; the lambda hangs off its function"
else
    bad CM-02 "lambda attribution missing" "$OUT"
fi
OUT="$(cm callees via_lambda --depth 1)"
if grep -q "via_lambda::f .*\[ast\] fixture.cpp:19" <<<"$OUT" && ! grep -q "lambda at" <<<"$OUT"; then
    ok CM-02b "calling the lambda lands on the lambda, not on an operator() named by its path"
else
    bad CM-02b "the call to the lambda was not attributed to it" "$OUT"
fi

# ---------------------------------------------------------------- CM-03
OUT="$(cm callers sink --depth 1)"
if grep -q "<- via_value .*\[value\] fixture.cpp:24" <<<"$OUT"; then
    ok CM-03 "a function bound to std::function is reported where it is taken as a value"
else
    bad CM-03 "value reference missing" "$OUT"
fi

# ---------------------------------------------------------------- CM-04
OUT="$(cm callees drive --depth 1)"
if grep -q "Derived::run .*\[virtual\]" <<<"$OUT" && grep -q "Leaf::run .*\[virtual\]" <<<"$OUT" \
   && grep -q "Base::run .*\[ast\]" <<<"$OUT"; then
    ok CM-04 "a call through Base::run reaches both overrides"
else
    bad CM-04 "virtual dispatch not expanded" "$OUT"
fi
OUT="$(cm callers Leaf::run --depth 1)"
TYPES="$(cm types Base)"
if grep -q "<- drive .*\[virtual\]" <<<"$OUT" && grep -q "^  v Derived" <<<"$TYPES" && grep -q "^    v Leaf" <<<"$TYPES"; then
    ok CM-04b "an override's callers include base-class call sites; the class tree is two levels"
else
    bad CM-04b "override callers or class tree wrong" "$OUT
$TYPES"
fi

# ---------------------------------------------------------------- CM-05
OUT="$(cm threads)"
if grep -q "fixture.cpp:36  start_thread  via std::thread" <<<"$OUT" && grep -q "which calls: worker" <<<"$OUT"; then
    ok CM-05 "the thread start and what it runs"
else
    bad CM-05 "thread start missing" "$OUT"
fi

# ---------------------------------------------------------------- CM-06
OUT="$(cm callers read_impl --depth 1)"
CTRL="$(cm callees naab:fixfile.read --depth 1)"
if grep -q "<- naab fixfile.read() .*\[dispatch\]" <<<"$OUT" && grep -q -- "-> read_impl" <<<"$CTRL" \
   && ! grep -q "write_impl" <<<"$CTRL"; then
    ok CM-06 "naab fixfile.read() maps to the code in its own branch only"
else
    bad CM-06 "stdlib dispatch wrong" "$OUT
$CTRL"
fi
OUT="$(cm callers unknown_impl --depth 1)"
if grep -q "<- FileModule::call .*\[ast\]" <<<"$OUT" && ! grep -q "\[dispatch\]" <<<"$OUT"; then
    ok CM-06b "the else branch is called by call() but by no NAAb name"
else
    bad CM-06b "dispatch leaked into the else branch" "$OUT"
fi

# ---------------------------------------------------------------- CM-07
OUT="$(cm callees lang:fixlang --depth 2)"
if grep -q -- "-> Derived .*\[dispatch\]" <<<"$OUT" && grep -q -- "-> Derived::run .*\[member\]" <<<"$OUT" \
   && ! grep -q -- "-> Base " <<<"$OUT"; then
    ok CM-07 "<<fixlang>> maps to the registered class and its methods, not the base"
else
    bad CM-07 "executor registration wrong" "$OUT"
fi

# ---------------------------------------------------------------- CM-08
GATED="$(cm reach sink --gate gate)"
OPEN="$(cm reach sink)"
if grep -q "^     entry_ungated" <<<"$GATED" && ! grep -q "^     entry_gated" <<<"$GATED" \
   && grep -q "gated_caller  fixture.cpp:10" <<<"$GATED"; then
    ok CM-08 "with the gate: the gated entry point is withheld, the ungated one reported"
else
    bad CM-08 "reach with a gate wrong" "$GATED"
fi
if grep -q "^     entry_gated" <<<"$OPEN" && grep -q "^     entry_ungated" <<<"$OPEN"; then
    ok CM-08c "without a gate both entry points are reported (the gate is what withheld one)"
else
    bad CM-08c "reach without a gate did not report both entry points" "$OPEN"
fi
OUT="$(cm reach sink --gate no_such_gate_codemap)"; rc=$?
if [ "$rc" -ne 0 ] && grep -q "matches no symbol" <<<"$OUT"; then
    ok CM-08b "a gate that names nothing is refused, not silently ignored"
else
    bad CM-08b "a misspelt gate was accepted (rc=$rc)" "$OUT"
fi

# ---------------------------------------------------------------- CM-09
OUT="$(cm find windows_only)"
CTRL="$(cm find dead_function)"
if ! grep -q "windows_only" <<<"$OUT" && grep -q "dead_function" <<<"$CTRL"; then
    ok CM-09 "code under #ifdef _WIN32 is not indexed on this build -- a documented blind spot, observed"
else
    bad CM-09 "the preprocessor blind spot is not what the tool says" "$OUT
$CTRL"
fi

# ---------------------------------------------------------------- CM-10
OUT="$(cm includes fixture.cpp)"
BY="$(cm includes include/fixture.h --by)"
if grep -q "include/fixture.h" <<<"$OUT" && grep -q "fixture.cpp" <<<"$BY" && grep -q "other.cpp" <<<"$BY"; then
    ok CM-10 "includes, both directions"
else
    bad CM-10 "includes wrong" "$OUT
$BY"
fi

# ---------------------------------------------------------------- CM-11
OUT="$(cm tags --file fixture.cpp)"
if grep -q "TODO .*(codemap) fixture task tag" <<<"$OUT" && grep -q "ref:A45" <<<"$OUT" && ! grep -q "ref:A4 " <<<"$OUT"; then
    ok CM-11 "a task tag and a record citation; a label-shaped A4: is not a citation"
else
    bad CM-11 "tags wrong" "$OUT"
fi

# ---------------------------------------------------------------- CM-12
OUT="$(cm uml Base)"
if grep -q "^Base <|-- Derived$" <<<"$OUT" && grep -q "^Derived <|-- Leaf$" <<<"$OUT" \
   && grep -q "{abstract} run()" <<<"$OUT" && grep -q "^  run()$" <<<"$OUT"; then
    ok CM-12 "uml: both inheritance edges; {abstract} on Base::run only"
else
    bad CM-12 "uml text wrong" "$OUT"
fi

# ---------------------------------------------------------------- CM-13
printf 'not a jar\n' > fake.jar
# A `java` first on PATH that records being run: the refusal must come BEFORE
# anything executes the jar.
mkdir -p fakebin
printf '#!/bin/sh\ntouch "%s/java_ran"\nexit 0\n' "$W" > fakebin/java
chmod +x fakebin/java
OUT="$(CODEMAP_PLANTUML_JAR=fake.jar PATH="$W/fakebin:$PATH" cm uml Base --render --out base.puml)"; rc=$?
if [ "$rc" -eq 3 ] && grep -q "refusing to run" <<<"$OUT" && [ -s base.puml ] && [ ! -e java_ran ]; then
    ok CM-13 "--render refuses an unpinned jar; the .puml text is still written"
else
    bad CM-13 "--render ran or misreported an unpinned jar (rc=$rc)" "$OUT"
fi

# ---------------------------------------------------------------- CM-14
OUT="$(cm index)"
if grep -q "2 sources, 0 to parse" <<<"$OUT"; then
    ok CM-14a "an unchanged tree re-parses nothing"
else
    bad CM-14a "unchanged tree re-parsed" "$OUT"
fi
printf '\n// edited\n' >> include/fixture.h
OUT="$(cm index)"
if grep -q "2 sources, 2 to parse" <<<"$OUT"; then
    ok CM-14b "a header edit re-parses every file that includes it"
else
    bad CM-14b "header edit not propagated" "$OUT"
fi
printf '\n// edited\n' >> other.cpp
OUT="$(cm index)"
if grep -q "2 sources, 1 to parse" <<<"$OUT"; then
    ok CM-14c "a .cpp edit re-parses only that file"
else
    bad CM-14c ".cpp edit re-parse count wrong" "$OUT"
fi

# ---------------------------------------------------------------- CM-15
JS="$(python3 "$CM" --repo . --cache .cache/codemap --json explain sink 2>&1)"
if python3 -c "import json,sys; d=json.loads(sys.stdin.read()); assert d['name']=='sink' and len(d['callers'])>=5" <<<"$JS" 2>/dev/null; then
    ok CM-15a "--json explain parses and carries the callers"
else
    bad CM-15a "--json output is not usable JSON" "$JS"
fi
ALLOUT="$(cm explain sink; cm reach sink --gate gate; cm threads; cm uml Base; cm tags; cm stats)"
if LC_ALL=C grep -q '[^[:print:][:space:]]' <<<"$ALLOUT"; then
    bad CM-15b "non-ASCII bytes in output" "$(LC_ALL=C grep -n '[^[:print:][:space:]]' <<<"$ALLOUT" | head -5)"
else
    ok CM-15b "every output is ASCII ($(wc -l <<<"$ALLOUT") lines checked)"
fi

# ---------------------------------------------------------------- CM-16 (part)
OUT="$(python3 -c "
import runpy, sys
sys.modules['clang'] = None
sys.argv = ['codemap', '--repo', '.', '--cache', '.cache/codemap', 'callers', 'sink', '--depth', '1']
try:
    runpy.run_path('$CM', run_name='__main__')
except SystemExit:
    pass
" 2>&1)"
if grep -q "<- gated_caller" <<<"$OUT"; then
    ok CM-16b "queries on an existing index work without libclang"
else
    bad CM-16b "a query needed libclang" "$OUT"
fi

# ---------------------------------------------------------------- CM-17
OUT="$(cm paths other.cpp:main sink)"; rc=$?
REV="$(cm paths sink other.cpp:main)"; rrc=$?
if [ "$rc" -eq 0 ] && grep -q "^path 1" <<<"$OUT" && [ "$rrc" -ne 0 ] && grep -q "no call path" <<<"$REV"; then
    ok CM-17 "main reaches sink; sink does not reach main (direction is respected)"
else
    bad CM-17 "paths wrong (rc=$rc, reverse rc=$rrc)" "$OUT
$REV"
fi

# ---------------------------------------------------------------- CM-19
OUT="$(cm find named_cb)"
CTRL="$(cm callers sink --depth 1)"
THR="$(cm callers worker --depth 1)"
if grep -q "^lambda  *via_named_lambda::named_cb .*fixture.cpp:71" <<<"$OUT" \
   && grep -q "<- via_named_lambda::named_cb .*\[ast\]" <<<"$CTRL" \
   && grep -q "<- start_thread::<lambda@fixture.cpp:36>" <<<"$THR"; then
    ok CM-19 "a lambda in a named variable is found by the name; one passed to std::thread's constructor is not named after the thread"
else
    bad CM-19 "named lambda not findable, or a constructor argument took the variable's name" "$OUT
$CTRL
$THR"
fi

# ---------------------------------------------------------------- CM-20
CALLS="$(cm writers Budget::calls)"
TOKS="$(cm writers Budget::tokens)"
LOGW="$(cm writers Budget::log)"
if grep -q "charge .*\[write ++\] fixture.cpp:78" <<<"$CALLS" && grep -q "reset_budget .*\[write =\] fixture.cpp:82" <<<"$CALLS" \
   && ! grep -q "peek_budget" <<<"$CALLS"; then
    ok CM-20a "++ and = on a field are writes; a function that only reads it is not a writer"
else
    bad CM-20a "built-in writes wrong" "$CALLS"
fi
if grep -q "charge .*\[write +=\] fixture.cpp:79" <<<"$TOKS" && grep -q "charge .*\[mutate push_back\] fixture.cpp:80" <<<"$LOGW"; then
    ok CM-20b "+= through std::atomic's operator and push_back are recorded"
else
    bad CM-20b "overloaded/mutating writes missing" "$TOKS
$LOGW"
fi
LOC="$(cm writes local_only)"
CHG="$(cm writes charge)"
if grep -q "local_only writes 0 sites" <<<"$LOC" && grep -q "charge writes 3 sites" <<<"$CHG"; then
    ok CM-20c "a local variable is not recorded (the same query finds 3 writes in charge)"
else
    bad CM-20c "local filtering or write count wrong" "$LOC
$CHG"
fi

# ---------------------------------------------------------------- CM-21
STATS="$(cm stats)"
RD="$(cm reach read_impl --gate GovernanceEngine::checkPathAccess)"
WR="$(cm reach write_impl --gate GovernanceEngine::checkPathAccess)"
if grep -q "pre-dispatch gates: applied -- 1 NAAb names gated; callers checked: vm_dispatch" <<<"$STATS" \
   && grep -q "naab fixfile.read()  fixture.cpp:46  via GovernanceEngine::checkPathAccess" <<<"$RD" \
   && ! grep -q "^     naab fixfile.read()" <<<"$RD"; then
    ok CM-21 "the allowlist gates fixfile.read before dispatch; reach stops there"
else
    bad CM-21 "pre-dispatch gate not applied to fixfile.read" "$STATS
$RD"
fi
if grep -q "^     naab fixfile.write()" <<<"$WR"; then
    ok CM-21c "fixfile.write, which the allowlist maps to \"\", stays an ungated entry point"
else
    bad CM-21c "fixfile.write wrongly gated" "$WR"
fi
# The verification is what makes a gate edge trustworthy: take the gate call out
# of the only caller of the allowlist and the edges must be withheld.
python3 - <<'EOF'
p = "fixture.cpp"
s = open(p).read()
old = "        g.checkPathAccess(method);\n"
assert s.count(old) == 1
open(p, "w").write(s.replace(old, ""))
EOF
cm index >/dev/null
STATS="$(cm stats)"
RD="$(cm reach read_impl --gate GovernanceEngine::checkPathAccess)"
if grep -q "pre-dispatch gates: unverified: .*does not call checkPathAccess" <<<"$STATS" \
   && grep -q "^     naab fixfile.read()" <<<"$RD"; then
    ok CM-21b "with the gate call removed from the dispatcher, no gate edge is added and stats says why"
else
    bad CM-21b "an unverified pre-dispatch gate was still applied" "$STATS
$RD"
fi

# ---------------------------------------------------------------- CM-22
FIXSTATS="$(cm stats)"
mkdir -p "$W/dup/build"
printf 'static int helper() { return 1; }\nint main(int argc, char** argv) { (void)argv; return helper() + argc; }\n' > "$W/dup/a.cpp"
printf 'static int helper() { return 2; }\nint main(int argc, char** argv) { (void)argv; return helper() - argc; }\n' > "$W/dup/b.cpp"
( cd "$W/dup" && python3 -c "
import json, os
w = os.getcwd()
json.dump([{'directory': os.path.join(w, 'build'), 'file': os.path.join(w, f),
            'arguments': ['c++', '-std=c++17', '-c', os.path.join(w, f), '-o', f + '.o']} for f in ('a.cpp', 'b.cpp')],
          open(os.path.join(w, 'build', 'compile_commands.json'), 'w'))" \
  && python3 "$CM" --repo . --cache .cache/codemap index --jobs 2 >/dev/null 2>&1 )
DUP="$( cd "$W/dup" && python3 "$CM" --repo . --cache .cache/codemap stats 2>&1 )"
if grep -q "defined in more than one file, stored once (1)" <<<"$DUP" \
   && grep -qE "^  main  kept (a|b)\.cpp; also (a|b)\.cpp$" <<<"$DUP" && ! grep -q "helper  kept" <<<"$DUP"; then
    ok CM-22 "main defined in two files is listed as stored once; the static helpers are not"
else
    bad CM-22 "a definition collision was not reported (or a static was)" "$(grep -A3 'defined in more' <<<"$DUP")"
fi
if grep -q "defined in more than one file: none" <<<"$FIXSTATS"; then
    ok CM-22c "control: the fixture's same-named static helpers are not a collision"
else
    bad CM-22c "the fixture reported a collision" "$(grep -A3 'defined in more' <<<"$FIXSTATS")"
fi

# ---------------------------------------------------------------- CM-23 (e, f) / CM-24a
cm index >/dev/null
S_READY="$(cm status)"
mkdir -p tools && cp -R "$REPO/tools/codemap" tools/
HK="$(env -u CLAUDE_CODE_REMOTE CLAUDE_PROJECT_DIR="$(native_path "$W")" bash "$HOOK" 2>&1)"; hrc=$?
if [ "$hrc" -eq 0 ] && grep -q "^codemap: index READY" <<<"$HK" && [ "$HK" = "$S_READY" ]; then
    ok CM-24a "the hook, run as a local session runs it, prints exactly the status line and exits 0"
else
    bad CM-24a "the hook did not print the status line (rc=$hrc)" "hook:   $HK
status: $S_READY"
fi
S_FMISS="$(cmst fake-missing . .cache/codemap)"
python3 -c "import os, time; t = time.time() + 3600; os.utime('fixture.cpp', (t, t))"
S_NEWER="$(cm status)"
if grep -q "^codemap: index READY" <<<"$S_READY" && ! grep -q "newer than it" <<<"$S_READY" \
   && grep -q "^codemap: index READY (1 indexed file(s) newer than it, e.g. fixture.cpp" <<<"$S_NEWER"; then
    ok CM-23e "status: a fresh index reads READY; a source touched after it is named as newer"
else
    bad CM-23e "status missed an index older than its sources (or flagged a fresh one)" "$S_READY
$S_NEWER"
fi
if grep -q "^codemap: index READY (cannot be refreshed here: libclang missing)" <<<"$S_FMISS"; then
    ok CM-23f "status: an existing index still answers without libclang, and says it cannot be refreshed"
else
    bad CM-23f "status misreported an index on a machine without libclang" "$S_FMISS"
fi

echo "Result: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
