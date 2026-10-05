#!/usr/bin/env bash
# ============================================================
# skip_tally.sh -- what each suite said it could NOT measure
#
# WHY. A suite that cannot measure something on a platform reports SKIP or
# UNMEASURABLE and still exits 0, so a green job says nothing about how much
# was actually tested. Windows has no embedded Python, no git in MSYS2, no
# pgrep and no POSIX-only tools, and every one of those turns arms into skips
# under a passing verdict. Nobody could say how many. This lists them, per
# suite, with the reason each arm gave, on every platform -- so "what is off
# on Windows" is a diff of two lists rather than a guess.
#
# REPORT ONLY. skip_tally never fails and never touches the caller's
# verdict: it always returns 0.
#
# WHAT COUNTS. One LINE per marker-bearing line, after colour codes are
# stripped. The marker set and word boundaries are the ones
# tools/testrunner/parallel.py counts (SKIP_RE: SKIP|SKIPPED|UNMEASURABLE|XFAIL
# as whole words, case-sensitive), so "skipping" in prose and SKIPPING are not
# counted. parallel.py counts OCCURRENCES and this counts LINES (a line saying
# "SKIP ... UNMEASURABLE" is one arm here, two markers there);
# tests/self-audit/test_skip_tally.sh checks the two agree on which lines
# carry a marker.
#
# NO PYTHON. The capture directory is an MSYS path on Windows, which a native
# python cannot open; awk runs inside the shell's own path vocabulary.
#
# Usage:  skip_tally CAPTURE_DIR [TSV_OUT]
#   CAPTURE_DIR  one <suite>.log per suite (run-all-tests.sh's captures)
#   TSV_OUT      optional; writes "suite<TAB>line" per skip line, sorted, for
#                comparing platforms (diff two of these)
# ============================================================

skip_tally_lines() {
    # stdin: one suite's output. stdout: its skip lines, colour-stripped,
    # trimmed, at most 200 characters each.
    awk '{
        gsub(/\033\[[0-9;]*[A-Za-z]/, "")
        gsub(/\r/, "")
        if ($0 ~ /(^|[^A-Za-z0-9_])(SKIP|SKIPPED|UNMEASURABLE|XFAIL)([^A-Za-z0-9_]|$)/) {
            sub(/^[ \t]+/, "")
            print substr($0, 1, 200)
        }
    }'
}

skip_tally() {
    local dir="$1" tsv="${2:-}" f name lines n
    local suites=0 with=0 total=0 body=""
    if [ -z "$dir" ] || [ ! -d "$dir" ]; then
        echo "  Skip tally: UNMEASURABLE -- no capture directory (${dir:-unset})"
        return 0
    fi
    if [ -n "$tsv" ]; then
        mkdir -p "$(dirname "$tsv")" 2>/dev/null || true
        { : > "$tsv"; } 2>/dev/null || tsv=""
    fi
    for f in "$dir"/*.log; do
        [ -f "$f" ] || continue
        suites=$((suites + 1))
        name="$(basename "$f" .log)"
        lines="$(skip_tally_lines < "$f")"
        [ -n "$lines" ] || continue
        n=$(printf '%s\n' "$lines" | wc -l | tr -d ' ')
        with=$((with + 1))
        total=$((total + n))
        body+="$(printf '%6d  %s' "$n" "$name")"$'\n'
        body+="$(printf '%s\n' "$lines" | sed 's/^/          | /')"$'\n'
        if [ -n "$tsv" ]; then
            printf '%s\n' "$lines" | awk -v s="$name" '{print s "\t" $0}' >> "$tsv" 2>/dev/null || true
        fi
    done
    if [ -n "$tsv" ] && [ -s "$tsv" ]; then
        LC_ALL=C sort -o "$tsv" "$tsv" 2>/dev/null || true
    fi
    echo ""
    echo "  Skip tally (report only, verdict unchanged): $total skipped/unmeasurable arm(s) in $with of $suites suite(s)"
    if [ "$suites" -eq 0 ]; then
        echo "    UNMEASURABLE -- the capture directory holds no suite output"
    elif [ -n "$body" ]; then
        printf '%s' "$body"
    fi
    [ -n "$tsv" ] && echo "    (also written to $tsv)"
    return 0
}
