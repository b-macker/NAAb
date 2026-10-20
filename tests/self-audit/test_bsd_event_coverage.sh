#!/usr/bin/env bash
# ============================================================
# test_bsd_event_coverage.sh -- every function of every module the BSD stdlib
# event table covers is classified (tools/bsd_event_coverage.py)
#
# include/naab/bsd_stdlib_events.h is what behavioural sequence detection
# knows about a stdlib call; a name it does not list records no event and is
# never pre-checked. env.get_all (the same branch as env.list) and env.set (the
# same branch as env.set_var) went unseen that way (A22). This suite fails when
# a covered module accepts a name the table does not list, so an alias is
# classified the day it is added.
#
#   CV-00  the repository: nothing missing, nothing stale
#   CV-01  control: a table row removed reads MISSING
#   CV-02  control: a new function in a module reads MISSING
#   CV-03  control: a row for a function no module has reads STALE
#   CV-04  control: an unreadable module is UNMEASURABLE (exit 2), not a pass
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOOL="$ROOT/tools/bsd_event_coverage.py"
_SYSTMP="${TMPDIR:-/tmp}"
T="$(mktemp -d "${_SYSTMP}/bsd-cov-XXXXXX")"
trap 'rm -rf "${T:?}"' EXIT
# python3 is a native program under MSYS2: hand it paths in its vocabulary.
source "$ROOT/tests/helpers/native_path.sh"

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

# A scratch copy of exactly what the tool reads, so each control edits a copy.
copy_tree() {  # $1=dest
    mkdir -p "$1/include/naab" "$1/src/stdlib"
    cp "$ROOT/include/naab/bsd_stdlib_events.h" "$1/include/naab/"
    for f in env_impl file_impl stdlib csv_impl http_impl process_impl crypto_impl \
             path_impl log_impl agent_impl; do
        cp "$ROOT/src/stdlib/$f.cpp" "$1/src/stdlib/"
    done
}
run_tool() {  # $1=root -> OUT, RC
    OUT="$(python3 "$(native_path "$TOOL")" --src "$(native_path "$1")" 2>&1)"; RC=$?
}

echo ""
echo "BSD stdlib event table coverage"
echo ""

run_tool "$ROOT"
if [ "$RC" = 0 ] && grep -q "^OK: every function of" <<<"$OUT"; then
    pass "CV-00" "every function of every covered module is classified ($(grep -c 'functions,' <<<"$OUT") modules)"
else
    fail "CV-00" "the table does not classify every covered function" "$(grep -E 'MISSING|STALE|BROKEN|DUPLICATE|FAIL|UNMEASURABLE' <<<"$OUT" | head -8 | tr '\n' ' ')"
fi

copy_tree "$T/c1"
python3 - "$(native_path "$T/c1/include/naab/bsd_stdlib_events.h")" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8", errors="surrogateescape").read()
row = 'vmEvent("env", "get_all",   T::ENV_READ),'
assert row in s, "fixture row not found"
open(p, "w", encoding="utf-8", errors="surrogateescape").write(s.replace(row, "", 1))
PY
run_tool "$T/c1"
if [ "$RC" = 1 ] && grep -q "^MISSING  env.get_all$" <<<"$OUT"; then
    pass "CV-01" "control: a removed row reads MISSING env.get_all"
else
    fail "CV-01" "a removed row was not reported" "rc=$RC $(tail -3 <<<"$OUT" | tr '\n' ' ')"
fi

copy_tree "$T/c2"
python3 - "$(native_path "$T/c2/src/stdlib/env_impl.cpp")" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8", errors="surrogateescape").read()
old = '"get", "set_var", "set", "has", "delete_var", "get_all",'
assert old in s, "fixture hasFunction line not found"
open(p, "w", encoding="utf-8", errors="surrogateescape").write(s.replace(old, old + ' "dump_all",', 1))
PY
run_tool "$T/c2"
if [ "$RC" = 1 ] && grep -q "^MISSING  env.dump_all$" <<<"$OUT"; then
    pass "CV-02" "control: a new module function reads MISSING env.dump_all"
else
    fail "CV-02" "a new module function was not reported" "rc=$RC $(tail -3 <<<"$OUT" | tr '\n' ' ')"
fi

copy_tree "$T/c3"
python3 - "$(native_path "$T/c3/include/naab/bsd_stdlib_events.h")" <<'PY'
import sys
p = sys.argv[1]
s = open(p, encoding="utf-8", errors="surrogateescape").read()
row = 'vmEvent("env", "get_all",   T::ENV_READ),'
open(p, "w", encoding="utf-8", errors="surrogateescape").write(s.replace(row, row + '\n    noEvent("env", "no_such_fn"),', 1))
PY
run_tool "$T/c3"
if [ "$RC" = 1 ] && grep -q "^STALE    env.no_such_fn$" <<<"$OUT"; then
    pass "CV-03" "control: a row for a function no module has reads STALE"
else
    fail "CV-03" "a stale row was not reported" "rc=$RC $(tail -3 <<<"$OUT" | tr '\n' ' ')"
fi

copy_tree "$T/c4"
sed -i 's/bool EnvModule::hasFunction/bool EnvModuleRenamed::hasFunction/' "$T/c4/src/stdlib/env_impl.cpp"
run_tool "$T/c4"
if [ "$RC" = 2 ] && grep -q "^UNMEASURABLE" <<<"$OUT"; then
    pass "CV-04" "control: an unreadable module is UNMEASURABLE (exit 2)"
else
    fail "CV-04" "a broken probe did not report itself" "rc=$RC $(tail -3 <<<"$OUT" | tr '\n' ' ')"
fi

echo ""
echo "  Passed: $PASS  Failed: $FAIL"
[ "$FAIL" -eq 0 ] || exit 1
echo "ALL PASSED"
