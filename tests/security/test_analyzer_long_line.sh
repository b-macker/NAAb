#!/usr/bin/env bash
# ============================================================
# test_analyzer_long_line.sh -- a long line in a polyglot block cannot crash the interpreter
#
# The polyglot-optimization analyzers (on by default) scan block source with
# std::regex, which libstdc++ runs by recursion -- about one stack frame per
# character an unbounded repetition consumes. A single ~40 KB line exhausted
# the stack: SIGSEGV (exit 139) on both engines under a plain enforce config,
# so no verdict was rendered at all. The analyzers now bound each line they
# scan (include/naab/analyzer/bounded_input.h); the program itself is not
# touched, which LL-01..03 check by the length the block reports.
#
#   LL-00  CONTROL: a 100-character line runs on both engines (the fixture works)
#   LL-01  a 40 KB line runs on both engines and the block sees all of it
#   LL-02  a 200 KB line, same
#   LL-03  a 120 KB line dense with what the analyzer's regexes match
#          (`ab.cd(` repeated), same. Coverage, NOT a regression arm: this
#          shape did not crash the pre-fix build (its matches are short);
#          LL-01/LL-02 are the arms that fail there with exit 139.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }
skip_all() { for id in LL-00 LL-01 LL-02 LL-03; do skip "$id" "$1"; done
             echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0; }

echo "=== A long line in a polyglot block cannot crash the interpreter ==="

[ -x "$NAAB" ] || skip_all "naab-lang not built (UNMEASURABLE)"
command -v python3 >/dev/null 2>&1 || skip_all "python3 unavailable to generate fixtures (UNMEASURABLE)"

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-longline.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust   # the unsigned govern.json below must not meet a populated trust store
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT
echo '{ "mode": "enforce" }' > "$W/govern.json"   # analyzers at their defaults

# make <name> <unit> <repeat> -> a program whose python block holds one line of
# <unit> * <repeat> characters inside a string and reports its length
make() {
    python3 - "$W/$1.naab" "$2" "$3" <<'PY'
import sys
path, unit, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
body = 'x = "' + unit * n + '"\nlen(x)\n'
open(path, "w").write('main {\n    let r = <<python\n' + body + '>>\n    print("LEN=" + string(r))\n}\n')
PY
}
make short "a" 100
make l40k  "a" 40000
make l200k "a" 200000
make dense "ab.cd(" 20000

# check <id> <name> <expected len> <description>
check() {
    local bad_engines="" out rc
    for flag in "" "--tree-walk"; do
        out=$(cd "$W" && timeout 120 "$NAAB" "$2.naab" $flag 2>&1); rc=$?
        case "$out" in
            *"LEN=$3"*) [ "$rc" -eq 0 ] || bad_engines="$bad_engines ${flag:-vm}:rc=$rc" ;;
            *) bad_engines="$bad_engines ${flag:-vm}:rc=$rc" ;;
        esac
    done
    if [ -z "$bad_engines" ]; then ok "$1" "$4"
    else bad "$1" "$4" "$bad_engines (139 = SIGSEGV)"; fi
}

# A build without the embedded Python executor (the Windows runner) says so,
# and its blocks return no value, so no arm can see LEN=. That self-declared
# limitation is UNMEASURABLE; any other control failure still fails below.
probe=$(cd "$W" && timeout 60 "$NAAB" short.naab 2>&1)
case "$probe" in
    *LEN=100*) ;;
    *"Python support not available"*) skip_all "this build has no embedded Python executor (UNMEASURABLE)" ;;
esac

check LL-00 short 100    "CONTROL: a 100-character line runs on both engines"
check LL-01 l40k  40000  "a 40 KB line runs on both engines, block sees all of it"
check LL-02 l200k 200000 "a 200 KB line runs on both engines"
check LL-03 dense 120000 "a 120 KB regex-dense line runs on both engines"

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
