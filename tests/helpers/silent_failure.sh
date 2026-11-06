#!/usr/bin/env bash
# silent_failure_marker FILE -- print the first line of FILE that reports a
# failure, and return 0; return 1 when no line does (or FILE is missing).
#
# run-all-tests.sh judges a .naab test by its exit status. A test that counts
# its own failures, prints them and exits 0 is counted as passing: sixteen did
# (A123, Bundle 16). run_test() therefore reads the output of every test that
# exits 0 through this function, and a line it returns turns the pass into a
# FAIL. Sourced by run-all-tests.sh and tests/self-audit/test_silent_failure_guard.sh,
# so the rule has one copy.
#
# A failure line is the marker set the A123 sweep measured (screened, 23 hits
# over every .naab test, observed 2026-11-05):
#   FAIL: <x> / FAILED: <x>        -- not preceded by a letter or '_' (so
#                                     GRACEFUL_FAIL: is not one), and <x> not
#                                     the count 0 (`FAIL: 0` is a clean tally)
#   FAILURES: N / Failures: N      -- N from 1 (UNEXPECTED FAILURES: N too)
# Blind spot, stated: a test that reports failure in other words (a cross mark
# alone, "Failed: 1", "not ok") is not caught. The set is narrow on purpose,
# so that it only misses and never invents: a false FAIL here would train
# people to work around the guard.
#
# No pipes: run-all-tests.sh runs under `set -e`, and callers use pipefail
# (CLAUDE.md, the pipefail + grep -q gotcha). grep -m1 reads the file itself.
silent_failure_marker() {
    local f="$1" hit=""
    [ -f "$f" ] || return 1
    hit=$(grep -a -m1 -E \
        '(^|[^A-Za-z_])(FAIL|FAILED):[[:space:]]*([^0[:space:]]|0[0-9]|$)|(FAILURES|Failures):[[:space:]]*[1-9]' \
        "$f" 2>/dev/null) || return 1
    hit="${hit%$'\r'}"
    printf '%s\n' "$hit"
    return 0
}
