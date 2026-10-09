#!/usr/bin/env bash
# Per-file governance check for the "Governance Check" workflow.
#
# Usage: governance_check_files.sh FILE...
#   NAAB      naab-lang binary (default ./build/naab-lang)
#   NAAB_GOV  naab-gov binary  (default ./build/naab-gov)
#   REPORT_DIR where the SARIF/JUnit/JSON reports go (default .governance-reports)
#
# Prints each file's output, then `checked=N` and `failed=N` as the last two
# lines (the workflow appends them to $GITHUB_OUTPUT). Exits 1 if any file
# failed. It lives in a real file, like merge_sarif.py, so that
# tests/self-audit/test_governance_check_files.sh can reach it; while it was
# inline YAML nothing could.
#
# A file with a main block is RUN, as before. A file with no main block under
# a discoverable govern.json is LINTED with naab-gov instead. Such a file is a
# module: it defines functions and executes nothing, and running it under
# requirements.main_block can only report that it has no main block (exit 3
# for every module, whatever its code says). naab-gov lint applies the
# source checks and the per-function checks a run applies before executing
# (it compiles the file), and the main_block requirement, which is about
# programs, not at all. A main-less file with no govern.json is run as
# before: there is no policy to lint against, and the run checks nothing.
#
# A file that does not parse goes down the run path and fails there, as it
# always did.

NAAB="${NAAB:-./build/naab-lang}"
NAAB_GOV="${NAAB_GOV:-./build/naab-gov}"
REPORT_DIR="${REPORT_DIR:-.governance-reports}"

mkdir -p "$REPORT_DIR"
CHECKED=0
FAILED=0

for f in "$@"; do
    CHECKED=$((CHECKED + 1))

    # Walk up to find govern.json; if none found, run without governance.
    SEARCH_DIR=$(dirname "$f")
    FOUND_GOV=false
    while [ "$SEARCH_DIR" != "." ] && [ "$SEARCH_DIR" != "/" ]; do
        if [ -f "$SEARCH_DIR/govern.json" ]; then
            FOUND_GOV=true
            break
        fi
        SEARCH_DIR=$(dirname "$SEARCH_DIR")
    done
    if [ -f "./govern.json" ]; then
        FOUND_GOV=true
    fi

    # Ask the parser, not a grep: a comment or a string can say "main {".
    # Captured and matched with case -- no pipe, so no SIGPIPE under pipefail.
    PARSE_OUT=$("$NAAB" parse "$f" 2>&1)
    MODE=run
    case "$PARSE_OUT" in
        *"Has main: no"*) [ "$FOUND_GOV" = true ] && MODE=lint ;;
    esac

    echo "=== Checking: $f ($MODE) ==="
    if [ "$MODE" = lint ]; then
        "$NAAB_GOV" lint "$f" --config-from-file \
            --sarif "$REPORT_DIR/sarif-${CHECKED}.json" \
            --junit "$REPORT_DIR/junit-${CHECKED}.xml" 2>&1
        RC=$?
    else
        GOV_FLAG=""
        [ "$FOUND_GOV" = false ] && GOV_FLAG="--no-governance"
        "$NAAB" run "$f" \
            $GOV_FLAG \
            --governance-sarif "$REPORT_DIR/sarif-${CHECKED}.json" \
            --governance-junit "$REPORT_DIR/junit-${CHECKED}.xml" \
            --governance-report "$REPORT_DIR/report-${CHECKED}.json" \
            2>&1
        RC=$?
    fi
    if [ "$RC" -ne 0 ]; then
        echo "--- FAILED: $f ($MODE, exit $RC)"
        FAILED=$((FAILED + 1))
    fi
done

echo "checked=$CHECKED"
echo "failed=$FAILED"
[ "$FAILED" -eq 0 ]
