#!/usr/bin/env bash
# A123 (Bundle 16): a .naab test that exits 0 but prints a failure is a FAIL.
#
# run-all-tests.sh judged a .naab test by its exit status alone, so sixteen
# tests that counted their own failures, printed them and exited 0 were
# counted as passing (A123). run_test() now reads every exit-0 run's output
# through silent_failure_marker() (tests/helpers/silent_failure.sh); a test
# pinned in KNOWN_SILENT_FAILURE_TESTS is reported as known instead, and a
# pinned test that no longer prints a failure is a FAIL, so the pin is removed
# when its fix lands.
#
# Part A runs the classifier over a table of lines. Part B runs the REAL
# run-all-tests.sh (.naab phase only) in a scratch tree whose tests/ holds
# planted programs, against the real interpreter, and reads its verdicts.
#
#   SG-A*  the classifier: each row's verdict (hit / miss)
#   SG-00  CONTROL: a clean test printing `FAIL: 0` passes -- the negative
#          control (the A123 sweep matched 7 such runs)
#   SG-01  a planted test printing `FAIL: planted` under exit 0 is a FAIL --
#          the guard's subject
#   SG-02  CONTROL: a test that throws is still a FAIL (the ordinary path)
#   SG-03  a pinned test that prints a failure is counted known silent, not
#          failed, and the run passes
#   SG-04  a pinned test that prints none is a FAIL (remove the pin)
#   SG-05  CONTROL: the planted test passes with the guard removed from a copy
#          of run-all-tests.sh -- SG-01 measures the guard, not the fixture
#   SG-06  a test that printed a failure because the platform has no executor
#          for a REAL language ("No executor found for language: shell" --
#          build-windows has none, and 27 tests failed silently that way,
#          observed on 3d0303b7) counts as missing executor, not as a FAIL
#   SG-06c CONTROL: the same with a misspelled language (`pyton`) is a FAIL --
#          only a name the language table knows is an absence
#   SG-07  the non-zero path reads the same list: a test that throws after the
#          same line counts as missing executor
#   SG-07c CONTROL: an old signature on the non-zero path ("Python support not
#          available for expression values") still counts as missing executor
#   SG-08  the startup warning "Python support not available (Python blocks
#          disabled)" -- printed on EVERY run of a build without Python, so
#          every build-windows run -- does not excuse a test with no Python
#          block: it stays a FAIL (matched alone, it made every failing test
#          on such a build missing executor, a plain `throw` too: observed on
#          build-windows, dd8d81b7)
#   SG-08c CONTROL: the same warning for a test that has a Python block counts
#          as missing executor (0cf74009's intent)
#   SG-08e the FAIL line's excerpt is the failure, not the startup warning (the
#          first-line excerpt showed only the warning for every build-windows
#          failure, so the log never said why a test failed; observed d0b1c20f)
#
# Red first (observed 2026-11-06): against run-all-tests.sh as of 01fb3552
# (no guard), SG-01, SG-03, SG-03b and SG-04 fail -- SG-05 is that run.
# Kill table (observed 2026-11-06):
#   M63 the `FAIL: 0` exclusion removed      -> SG-00, SG-03b, SG-A08, SG-A09
#   M64 the pin list ignored                 -> SG-03, SG-03b, SG-04
#   M66 the exit-0 path skips missing_executor_reason -> SG-06, SG-03b
#   M67 any language name counts, not only the table's -> SG-06c, SG-03b
#   M68 the startup warning counts without a Python block -> SG-08, SG-03b
#   M69 the startup-warning rule dropped                  -> SG-08c, SG-03b
#   M70 run_test passes no test source to the classifier  -> SG-08c, SG-03b
#   M71 the Python tags not read from the language table  -> SG-08c, SG-03b
#   M72 the FAIL excerpt back to `head -1`                 -> SG-08e
# Red first for SG-08 (observed 2026-11-06): HEAD bc63a462's runner reads
# sg_warn_throw.naab as missing executor (SG-08, SG-03b fail). On build-windows,
# where every run prints that warning, dd8d81b7's runner read every planted
# test the same way (SG-01, SG-02, SG-03b, SG-06, SG-06c, SG-07; observed).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${1:-$REPO/build/naab-lang}"
case "$NAAB" in /*) ;; *) NAAB="$PWD/$NAAB" ;; esac

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -12 | sed 's/^/       /'; }

echo "=== A test that exits 0 but prints a failure is a FAIL ==="
# shellcheck source=../helpers/silent_failure.sh
source "$REPO/tests/helpers/silent_failure.sh"
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-silentfail.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
trap 'rm -rf "$W"' EXIT

# --- Part A: the classifier -------------------------------------------------
row() {  # row ID WANT(hit|miss) TEXT (printf-interpreted)
    local id="$1" want="$2" got
    printf "$3" > "$W/row.txt"
    if silent_failure_marker "$W/row.txt" >/dev/null; then got=hit; else got=miss; fi
    if [ "$got" = "$want" ]; then ok "$id" "$want: $(printf '%q' "$(printf "$3")")"
    else bad "$id" "expected $want, got $got: $(printf '%q' "$(printf "$3")")"; fi
}
row SG-A01 hit  'FAIL: planted\n'
row SG-A02 hit  '  \xe2\x9c\x97 FAIL: Nim proc (got: 42)\n'
row SG-A03 hit  'FAILED: Module could not find data.txt\n'
row SG-A04 hit  'FAIL: 10\n'
row SG-A05 hit  'UNEXPECTED FAILURES: 2\n'
row SG-A06 hit  'Failures: 1\n'
row SG-A07 hit  'ok\nFAIL test 1: got 5\nFAIL: 1\n'
row SG-A08 miss 'PASS: 8\nFAIL: 0\n'
row SG-A09 miss 'FAIL: 0\r\n'
row SG-A10 miss 'GRACEFUL_FAIL: true\n'
row SG-A11 miss 'Failures: 0\n'
row SG-A12 miss 'all good\n'
row SG-A13 miss ''
if silent_failure_marker "$W/no-such-file" >/dev/null; then bad SG-A14 "a missing file is a miss"
else ok SG-A14 "miss: a missing file"; fi

# --- Part B: the real runner over planted tests ------------------------------
if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then
    bad SG-B "naab-lang not built (UNMEASURABLE, not a pass)"
    echo ""; echo "Results: $PASS passed, $FAIL failed"; exit 1
fi

# tree DIR GUARD(yes|no) PINNED_PRINTS(yes|no): a scratch repo root running
# run-all-tests.sh's .naab phase. The pinned name is the real pin's, so the
# real KNOWN_SILENT_FAILURE_TESTS entry is what is exercised.
tree() {
    local d="$1" guard="$2" pinned="$3"
    mkdir -p "$d/build" "$d/tests/bugs" "$d/tests/helpers" "$d/home"
    ln -s "$NAAB" "$d/build/naab-lang"
    # naab-gov gives run-all-tests.sh the language table (KNOWN_LANGS)
    for g in "$(dirname "$NAAB")/naab-gov" "$REPO/build/naab-gov"; do
        if [ -x "$g" ]; then ln -s "$g" "$d/build/naab-gov"; break; fi
    done
    cp "$REPO/tests/helpers/silent_failure.sh" "$d/tests/helpers/"
    if [ "$guard" = yes ]; then
        cp "$REPO/run-all-tests.sh" "$d/run-all-tests.sh"
    else
        # The guard removed: an exit-0 run is a pass, as before A123.
        python3 - "$REPO/run-all-tests.sh" "$d/run-all-tests.sh" <<'PY' || return 1
import sys
s = open(sys.argv[1], encoding="utf-8").read()
s = s.replace('marker=$(silent_failure_marker "$output_file") || marker=""', 'marker=""', 1)
s = s.replace('local known="${KNOWN_SILENT_FAILURE_TESTS[$test_name]:-}"', 'local known=""', 1)
open(sys.argv[2], "w", encoding="utf-8").write(s)
PY
        grep -q 'marker=""$' "$d/run-all-tests.sh" || return 1
    fi
    printf 'main {\n    print("PASS: 3")\n    print("FAIL: 0")\n}\n' > "$d/tests/bugs/sg_clean.naab"
    printf 'main {\n    print("FAIL: planted")\n}\n' > "$d/tests/bugs/sg_silent.naab"
    printf 'main {\n    throw "planted"\n}\n' > "$d/tests/bugs/sg_throw.naab"
    printf 'main {\n    print("No executor found for language: shell")\n    print("FAIL: clean value blocked")\n}\n' > "$d/tests/bugs/sg_noexec.naab"
    printf 'main {\n    print("No executor found for language: pyton")\n    print("FAIL: clean value blocked")\n}\n' > "$d/tests/bugs/sg_noexec_typo.naab"
    printf 'main {\n    print("No executor found for language: shell")\n    throw "planted"\n}\n' > "$d/tests/bugs/sg_noexec_throw.naab"
    printf 'main {\n    print("Python support not available for expression values")\n    throw "planted"\n}\n' > "$d/tests/bugs/sg_nopy_throw.naab"
    printf 'main {\n    print("[WARN] Python support not available (Python blocks disabled)")\n    throw "planted"\n}\n' > "$d/tests/bugs/sg_warn_throw.naab"
    printf 'main {\n    print("[WARN] Python support not available (Python blocks disabled)")\n    if false {\n        let v = <<python\n1\n>>\n    }\n    throw "planted"\n}\n' > "$d/tests/bugs/sg_warn_py_throw.naab"
    if [ "$pinned" = yes ]; then
        printf 'main {\n    print("FAIL: pinned")\n}\n' > "$d/tests/bugs/test_chaos_module_boundary.naab"
    else
        printf 'main {\n    print("all good")\n}\n' > "$d/tests/bugs/test_chaos_module_boundary.naab"
    fi
}
OUT=""; RC=0
runner() {  # runner DIR GUARD PINNED -> OUT, RC; a tree that cannot be built
            # leaves OUT empty, so every arm reading it fails (not a stale pass)
    OUT=""; RC=99
    tree "$1" "$2" "$3" || { echo "  (tree $1 could not be built)"; return 1; }
    OUT=$(cd "$1" && HOME="$1/home" NAAB_TEST_PHASE=naab timeout 300 bash run-all-tests.sh </dev/null 2>&1); RC=$?
}
line() { grep -E "^  [A-Z]+: $1( |$)" <<<"$OUT" | head -1; }

runner "$W/g" yes yes
if grep -q '^  PASS: sg_clean.naab$' <<<"$OUT"; then
    ok SG-00 "CONTROL: a clean test printing FAIL: 0 passes"
else
    bad SG-00 "CONTROL: sg_clean.naab should pass (rc=$RC)" "$(line sg_clean.naab)"$'\n'"$(tail -20 <<<"$OUT")"
fi
if grep -q '^  FAIL: sg_silent.naab (exited 0 but printed a failure)$' <<<"$OUT" \
   && grep -q 'FAIL: planted' <<<"$OUT"; then
    ok SG-01 "a planted test printing a failure under exit 0 is a FAIL, the line quoted"
else
    bad SG-01 "sg_silent.naab should be a FAIL (rc=$RC)" "$(line sg_silent.naab)"
fi
if grep -q '^  FAIL: sg_throw.naab$' <<<"$OUT"; then
    ok SG-02 "CONTROL: a test that throws is still a FAIL"
else
    bad SG-02 "sg_throw.naab should be a FAIL" "$(line sg_throw.naab)"
fi
if grep -q '^  XFAIL: test_chaos_module_boundary.naab (prints a failure under exit 0, pinned: A121' <<<"$OUT" \
   && grep -qE '^  Known silent: +1 ' <<<"$OUT"; then
    ok SG-03 "a pinned test printing a failure is counted known silent, with its record"
else
    bad SG-03 "the pinned test should be counted known silent" "$(line test_chaos_module_boundary.naab)"
fi
if [ "$RC" -ne 0 ] && grep -qE '^  Unexpected fails: 4$' <<<"$OUT"; then
    ok SG-03b "the run fails on exactly the four failures (sg_silent, sg_throw, sg_noexec_typo, sg_warn_throw)"
else
    bad SG-03b "expected rc!=0 and 4 unexpected fails (rc=$RC)" "$(grep -E 'Unexpected fails|Known silent' <<<"$OUT")"
fi
if grep -q '^  FAIL: sg_warn_throw.naab$' <<<"$OUT"; then
    ok SG-08 "the build-wide Python warning does not excuse a test with no Python block"
else
    bad SG-08 "sg_warn_throw.naab should be a FAIL" "$(line sg_warn_throw.naab)"
fi
# What the FAIL line shows: the old first-line excerpt was the warning itself
# on every build-windows failure (observed at d0b1c20f), never the failure.
EXC=$(grep -A1 '^  FAIL: sg_warn_throw.naab$' <<<"$OUT" | tail -1)
if grep -q 'planted' <<<"$EXC" && ! grep -q 'Python support not available' <<<"$EXC"; then
    ok SG-08e "the FAIL excerpt skips the startup warning and shows the failure"
else
    bad SG-08e "the FAIL excerpt should show the throw, not the warning" "$EXC"
fi
if grep -q '^  XFAIL: sg_warn_py_throw.naab (missing executor)$' <<<"$OUT"; then
    ok SG-08c "CONTROL: the same warning excuses a test that has a Python block"
else
    bad SG-08c "sg_warn_py_throw.naab should be missing executor" "$(line sg_warn_py_throw.naab)"
fi
if [ ! -e "$W/g/build/naab-gov" ]; then
    bad SG-06 "naab-gov not built: the language table is unreadable (UNMEASURABLE, not a pass)"
elif grep -q '^  XFAIL: sg_noexec.naab (missing executor: no shell executor on this platform; it printed a failure under exit 0)$' <<<"$OUT"; then
    ok SG-06 "a silent failure from a missing shell executor counts as missing executor"
else
    bad SG-06 "sg_noexec.naab should be missing executor" "$(line sg_noexec.naab)"
fi
if grep -q '^  FAIL: sg_noexec_typo.naab (exited 0 but printed a failure)$' <<<"$OUT"; then
    ok SG-06c "CONTROL: a misspelled language is a FAIL, not an absence"
else
    bad SG-06c "sg_noexec_typo.naab should be a FAIL" "$(line sg_noexec_typo.naab)"
fi
if grep -q '^  XFAIL: sg_noexec_throw.naab (missing executor: no shell executor on this platform)$' <<<"$OUT"; then
    ok SG-07 "the non-zero path reads the same list"
else
    bad SG-07 "sg_noexec_throw.naab should be missing executor" "$(line sg_noexec_throw.naab)"
fi
if grep -q '^  XFAIL: sg_nopy_throw.naab (missing executor)$' <<<"$OUT"; then
    ok SG-07c "CONTROL: the non-zero path keeps its old signatures"
else
    bad SG-07c "sg_nopy_throw.naab should be missing executor" "$(line sg_nopy_throw.naab)"
fi

runner "$W/u" yes no
if grep -q '^  FAIL: test_chaos_module_boundary.naab (pinned as printing a failure, printed none: remove its entry)$' <<<"$OUT"; then
    ok SG-04 "a pinned test that prints no failure is a FAIL (remove the pin)"
else
    bad SG-04 "the pinned test printing nothing should be a FAIL" "$(line test_chaos_module_boundary.naab)"
fi

runner "$W/n" no yes
if grep -q '^  PASS: sg_silent.naab$' <<<"$OUT"; then
    ok SG-05 "CONTROL: with the guard removed the planted test passes -- SG-01 measures the guard"
else
    bad SG-05 "CONTROL: without the guard sg_silent.naab should pass" "$(line sg_silent.naab)"$'\n'"$(tail -12 <<<"$OUT")"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
