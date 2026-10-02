#!/usr/bin/env bash
# run_timed.sh -- run run-all-tests.sh unchanged, and measure where the time goes.
#
# REPORT-ONLY. The verdict is run-all-tests.sh's own exit status, returned
# unchanged; the timing report is produced afterwards and can never alter it.
# A failure inside the measurement prints a note and is otherwise ignored.
#
# Usage (from anywhere; env such as NAAB_TEST_PHASE passes through):
#   bash tools/testtiming/run_timed.sh [args for run-all-tests.sh]
# Outputs (directory: $NAAB_TIMING_DIR, default ./test-timing):
#   calls.tsv    one line per `timeout` call: start, end, rc, cwd, argv
#   timing.json  per-suite and per-test durations, for comparing runs
#   timing.md    the human-readable summary (also appended to
#                $GITHUB_STEP_SUMMARY when that is set)
#
# How: every suite and every .naab test is started through `timeout`, so a
# shim placed ahead of the real `timeout` on PATH sees all of them. Work that
# does not go through `timeout` is not attributed; the report shows that gap
# as "unattributed" rather than hiding it.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT="${NAAB_TIMING_DIR:-$REPO/test-timing}"
# Absolute, always: $OUT/bin goes on PATH, and a RELATIVE PATH entry stops
# resolving the moment a suite cd's elsewhere -- the shim would silently drop
# out of every suite that changes directory, and the report would show their
# work as unattributed instead of failing.
mkdir -p "$OUT" && OUT="$(cd "$OUT" && pwd)" || { echo "run_timed: cannot use $OUT" >&2; cd "$REPO" && exec bash run-all-tests.sh "$@"; }

REAL_TIMEOUT="$(command -v timeout 2>/dev/null || true)"
if [ -z "$REAL_TIMEOUT" ]; then
    # Nothing to wrap: run the suite exactly as CI always has.
    echo "run_timed: 'timeout' not found; running without timing" >&2
    cd "$REPO" && exec bash run-all-tests.sh "$@"
fi

mkdir -p "$OUT/bin"
cp "$HERE/timeout-shim" "$OUT/bin/timeout"
chmod +x "$OUT/bin/timeout"
: > "$OUT/calls.tsv"

start=$(date +%s.%N 2>/dev/null || date +%s)
( cd "$REPO" && \
  PATH="$OUT/bin:$PATH" NAAB_REAL_TIMEOUT="$REAL_TIMEOUT" NAAB_TIMING_LOG="$OUT/calls.tsv" \
  bash run-all-tests.sh "$@" )
rc=$?
end=$(date +%s.%N 2>/dev/null || date +%s)

PY="$(command -v python3 2>/dev/null || command -v python 2>/dev/null || true)"
if [ -n "$PY" ]; then
    # Bytes in on stdin, bytes out on stdout -- no path crosses into python.
    # Under MSYS2 python3 is a native Windows build, and a path that reaches
    # it unconverted cannot be opened (see CLAUDE.md, "a test's OUTPUT
    # CHANNEL..."). The report needs no paths at all.
    for fmt in md json; do
        "$PY" "$HERE/report.py" --format "$fmt" --start "$start" --end "$end" \
            --exit-code "$rc" < "$OUT/calls.tsv" > "$OUT/timing.$fmt" \
            || echo "run_timed: $fmt report failed (verdict unaffected)" >&2
    done
    cat "$OUT/timing.md" 2>/dev/null || true
    if [ -n "${GITHUB_STEP_SUMMARY:-}" ] && [ -s "$OUT/timing.md" ]; then
        cat "$OUT/timing.md" >> "$GITHUB_STEP_SUMMARY" 2>/dev/null || true
    fi
else
    echo "run_timed: python not found; raw timings are in $OUT/calls.tsv" >&2
fi
exit "$rc"
