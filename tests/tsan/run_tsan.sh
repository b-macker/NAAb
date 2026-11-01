#!/usr/bin/env bash
# ============================================================
# run_tsan.sh -- the concurrency suites and unit tests under ThreadSanitizer
# (Bundle 9). CI's "ThreadSanitizer" job runs it; it is not part of
# run-all-tests.sh, which runs the normal build.
#
#   bash tests/tsan/run_tsan.sh            # build/ must hold a -DENABLE_TSAN=ON build
#
# What it guards: A93 (two batched tools on one VM, an async fn's tool on
# main's VM), A86 (report writers reading check results without the lock),
# A92 (the collector beside another engine), A65 (the async VM's governance
# state), and whatever concurrency defect comes next in the same suites.
#
# How a verdict is reached:
#   - Every process writes its reports to its own log file (TSAN_OPTIONS
#     log_path); exitcode=0 keeps TSan from changing any suite's own verdict.
#   - TS-00, the positive control: a planted race, compiled with the same
#     compiler, must produce a report in the same log directory. Without it a
#     quiet run could mean the logs were never written.
#   - TS-01: the build really is a ThreadSanitizer build (its runtime is linked).
#   - Each suite and the unit-test filter must pass as they do on the normal
#     build. A suite that fails is reported with its output tail.
#   - Every report in the logs is listed by its first NAAb frame; any report not
#     matched by tests/tsan/suppressions.txt fails the run. Each suppression
#     there names why it is not NAAb's defect.
# Output is the PASS / FAIL / UNMEASURABLE of each arm.
# ============================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
NAAB="$ROOT/build/naab-lang"
UNIT="$ROOT/build/naab_unit_tests"
SUPP="$ROOT/tests/tsan/suppressions.txt"
CXX="${CXX:-clang++}"
LOGDIR="$(mktemp -d "${TMPDIR:-/tmp}/naab_tsan_XXXXXX")"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/naab_tsan_work_XXXXXX")"

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -15 | sed 's/^/       | /'; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }
finish() {
    echo ""
    echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable (logs: $LOGDIR)"
    [ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ] || exit 1
    exit 0
}

# The suites the job covers: tests/tsan/suites.txt, shared with
# schedule_perturb.sh so the two cannot cover different suites.
SUITES=()
while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    SUITES+=("$line")
done < "$ROOT/tests/tsan/suites.txt"
if [ "${#SUITES[@]}" -eq 0 ]; then
    unm TS-02 "tests/tsan/suites.txt lists no suite"; finish
fi
UNIT_FILTER='ThreadVm.*:ReportSnapshot.*:GcEngineActivity.*:JsExecutorAdapterTest.*:PolyglotAsyncTest.*'

export TSAN_OPTIONS="log_path=$LOGDIR/tsan exitcode=0 halt_on_error=0 second_deadlock_stack=1 report_signal_unsafe=0 history_size=4${SUPP:+ suppressions=$SUPP}"

echo "=== ThreadSanitizer: concurrency suites (Bundle 9) ==="

# ---- TS-00: the instrument reports a planted race -----------------------------
if ! command -v "$CXX" >/dev/null 2>&1; then
    unm TS-00 "$CXX not found -- cannot build the planted race, every arm UNMEASURABLE"; finish
fi
CTRL_LOG="$(mktemp -d "$LOGDIR/control.XXXXXX")"
if ! "$CXX" -fsanitize=thread -g -O1 -pthread "$ROOT/tests/tsan/planted_race.cpp" -o "$WORK/planted_race" 2>"$WORK/cc.err"; then
    unm TS-00 "the planted race did not compile -- every arm UNMEASURABLE" ; tail -5 "$WORK/cc.err"; finish
fi
TSAN_OPTIONS="log_path=$CTRL_LOG/tsan exitcode=0" "$WORK/planted_race" >/dev/null 2>"$WORK/ctrl.err"
if grep -qs 'WARNING: ThreadSanitizer: data race' "$CTRL_LOG"/tsan.*; then
    pass TS-00 "CONTROL: a planted race is reported in the log directory"
else
    unm TS-00 "CONTROL: the planted race was not reported (kernel mmap_rnd_bits above 28 stops TSan) -- every arm UNMEASURABLE" ; tail -5 "$WORK/ctrl.err"; finish
fi

# ---- TS-01: the binaries are ThreadSanitizer builds ----------------------------
for b in "$NAAB" "$UNIT"; do
    if [ ! -x "$b" ]; then unm TS-01 "$b not built -- UNMEASURABLE"; finish; fi
    # A here-string, not a pipe: under pipefail `nm | grep -q` is false when grep
    # exits at the match and nm takes SIGPIPE (CLAUDE.md, the pipefail gotcha).
    if ! grep -q '__tsan_init' <<<"$(nm "$b" 2>/dev/null)"; then
        unm TS-01 "$b is not a ThreadSanitizer build (no __tsan_init) -- UNMEASURABLE"; finish
    fi
done
pass TS-01 "build/naab-lang and build/naab_unit_tests are ThreadSanitizer builds"

# ---- TS-02: the suites pass under TSan ------------------------------------------
for s in "${SUITES[@]}"; do
    name="$(basename "$s" .sh)"
    if [ ! -f "$ROOT/$s" ]; then fail "TS-02/$name" "suite missing"; continue; fi
    out="$(cd "$ROOT" && timeout 1800 bash "$s" 2>&1)"; rc=$?
    if [ "$rc" -eq 0 ]; then pass "TS-02/$name" "passes under ThreadSanitizer"
    else fail "TS-02/$name" "fails under ThreadSanitizer (exit $rc)" "$out"; fi
done

# ---- TS-03: the unit tests pass under TSan --------------------------------------
# The recorded exclusions (tests/unit/known_failures.txt) fail on the normal
# build too, so they are excluded here as tests/unit/run_unit_tests.sh does.
EXCL="$(grep -vE '^[[:space:]]*(#|$)' "$ROOT/tests/unit/known_failures.txt" | awk '{print $1}' | paste -sd: -)"
FILTER="$UNIT_FILTER${EXCL:+-$EXCL}"
out="$(cd "$ROOT/build" && timeout 1800 ./naab_unit_tests --gtest_filter="$FILTER" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && grep -q 'PASSED' <<<"$out"; then
    pass TS-03 "unit tests ($UNIT_FILTER) pass under ThreadSanitizer"
else
    fail TS-03 "unit tests fail under ThreadSanitizer (exit $rc)" "$out"
fi

# ---- TS-04: no unsuppressed report ----------------------------------------------
# A log file per process; each report starts with a WARNING line. List each by
# its first frame inside the repository, so a reader sees where to look.
reports=0
listing=""
for f in "$LOGDIR"/tsan.*; do
    [ -e "$f" ] || continue
    n=$(grep -c 'WARNING: ThreadSanitizer' "$f" 2>/dev/null || true)
    [ "${n:-0}" -gt 0 ] || continue
    reports=$((reports + n))
    first=$(grep -m1 -oE '(/[^ ]*)?(src|include|tests)/[^ :]+:[0-9]+' "$f" || echo "(no repository frame)")
    kind=$(grep -m1 -oE 'WARNING: ThreadSanitizer: [a-z -]+' "$f")
    listing+="$(basename "$f"): $n report(s), first: $kind at ${first#"$ROOT"/}"$'\n'
done
if [ "$reports" -eq 0 ]; then
    pass TS-04 "no unsuppressed ThreadSanitizer report from any suite or unit test"
else
    fail TS-04 "$reports unsuppressed ThreadSanitizer report(s); each is a race to fix or a suppression to justify" "$listing"
    echo "  first report in full:"
    for f in "$LOGDIR"/tsan.*; do
        grep -qs 'WARNING: ThreadSanitizer' "$f" && { sed -n '1,60p' "$f" | sed 's/^/       | /'; break; }
    done
fi
finish
