#!/usr/bin/env bash
# ============================================================
# test_test_timing.sh -- the timing tool measures the suite without changing it
#
# tools/testtiming/run_timed.sh runs run-all-tests.sh with a shim in front of
# `timeout`, so every suite and every .naab test start is recorded. It is
# report-only: CI keeps run-all-tests.sh's verdict. That claim holds only if
# the shim is invisible to what it wraps, so each way it could leak is checked,
# and each check is shown able to fail (a check that cannot fail proves
# nothing -- docs/investigation-method.md, "Validate changes by reverting them").
#
#   TT-01  exit status passes through unchanged: 0, 1, 124 (expired), 127
#   TT-02  stdin reaches the command (a bare background job would get /dev/null)
#   TT-03  a signal to the shim reaches the command -- no orphan is left
#   TT-03c CONTROL: the same probe, on a shim with forwarding removed, DOES see
#          the orphan, so TT-03 can fail
#   TT-04  each call is recorded as one line; nothing is recorded when the log
#          variable is unset
#   TT-05  run_timed.sh returns the wrapped suite's exit status, unchanged
#   TT-06  the report counts top-level work only: a call inside a suite's time
#          window is that suite's own work and is not counted again
#   TT-06c CONTROL: the same calls without the enclosing suite ARE counted, so
#          TT-06's exclusion is structural, not a blanket drop
#   TT-07  report output is ASCII and the JSON parses
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOOLS="$REPO/tools/testtiming"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-ttiming.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
trap 'rm -rf "$W"' EXIT

echo "=== The timing tool measures without changing what it measures ==="

# The REAL timeout, found while no shim is on PATH. When this suite itself runs
# under run_timed.sh, `command -v timeout` is the shim -- skip past it.
REAL=""
IFS=: read -r -a _dirs <<<"$PATH"
for d in "${_dirs[@]}"; do
    [ -x "$d/timeout" ] || continue
    if ! grep -q "timeout-shim" "$d/timeout" 2>/dev/null; then REAL="$d/timeout"; break; fi
done
if [ -z "$REAL" ]; then
    for id in TT-01 TT-02 TT-03 TT-03c TT-04 TT-05; do skip "$id" "no real 'timeout' on this platform (UNMEASURABLE)"; done
else
    SHIM="$W/bin/timeout"; mkdir -p "$W/bin"; cp "$TOOLS/timeout-shim" "$SHIM"; chmod +x "$SHIM"
    export NAAB_REAL_TIMEOUT="$REAL"

    # TT-01
    got=""
    for want in 0 1 127; do
        case $want in
            0) "$SHIM" 5s true ;;
            1) "$SHIM" 5s false ;;
            127) "$SHIM" 5s /nonexistent-command-$$ 2>/dev/null ;;
        esac
        got="$got $?"
    done
    "$SHIM" 1s sleep 5; got="$got $?"
    if [ "$got" = " 0 1 127 124" ]; then ok "TT-01" "exit status passes through (0 1 127 124)"
    else bad "TT-01" "exit status changed" "got:$got"; fi

    # TT-02
    out=$(printf 'STDIN_REACHED\n' | "$SHIM" 5s cat)
    case "$out" in
        STDIN_REACHED) ok "TT-02" "stdin reaches the wrapped command" ;;
        *) bad "TT-02" "stdin did not reach the command" "got: '$out'" ;;
    esac

    # TT-03 / TT-03c: an outer timeout TERMs the shim after 1 s; the command is a
    # sleep with a unique duration, which must not survive.
    orphan_probe() {  # $1 = shim to test -> prints the number of surviving sleeps
        local mark="97.$$$RANDOM"
        "$REAL" 1s "$1" 30s sleep "$mark" >/dev/null 2>&1
        sleep 1
        # pgrep -x matches the process NAME, so the pgrep process itself (named
        # pgrep) can never match; -a prints arguments for the duration filter.
        local n=0
        if command -v pgrep >/dev/null 2>&1; then
            n=$(pgrep -a -x sleep 2>/dev/null | awk -v m="$mark" '$NF==m' | wc -l | tr -d ' ')
            pgrep -a -x sleep 2>/dev/null | awk -v m="$mark" '$NF==m {print $1}' | xargs -r kill 2>/dev/null
        else
            n=UNMEASURABLE
        fi
        echo "$n"
    }
    n=$(orphan_probe "$SHIM")
    sed '/^trap /d' "$SHIM" > "$W/bin/timeout-notrap"; chmod +x "$W/bin/timeout-notrap"
    nc=$(orphan_probe "$W/bin/timeout-notrap")
    if [ "$n" = UNMEASURABLE ] || [ "$nc" = UNMEASURABLE ]; then
        skip "TT-03" "pgrep unavailable (UNMEASURABLE)"; skip "TT-03c" "pgrep unavailable (UNMEASURABLE)"
    else
        if [ "$n" = 0 ]; then ok "TT-03" "a signal to the shim reaches the command (no orphan)"
        else bad "TT-03" "the wrapped command survived its shim being killed" "orphans: $n"; fi
        if [ "$nc" -ge 1 ]; then ok "TT-03c" "CONTROL: without forwarding the orphan IS seen ($nc), so TT-03 can fail"
        else bad "TT-03c" "control saw no orphan -- TT-03 cannot distinguish forwarding from its absence"; fi
    fi

    # TT-04
    log="$W/calls.tsv"; : > "$log"
    NAAB_TIMING_LOG="$log" "$SHIM" 5s true
    NAAB_TIMING_LOG="$log" "$SHIM" 5s false
    "$SHIM" 5s true    # no log variable: must not write
    lines=$(wc -l < "$log" | tr -d ' ')
    rcs=$(cut -f3 "$log" | tr '\n' ' ')
    if [ "$lines" = 2 ] && [ "$rcs" = "0 1 " ]; then ok "TT-04" "one record per logged call, none without the log variable"
    else bad "TT-04" "unexpected records" "lines=$lines rcs='$rcs'"; fi

    # TT-05: run_timed.sh against a stand-in repo whose run-all-tests.sh makes
    # one timed call and exits 3. REPO is derived from the script's location,
    # so a copy of tools/testtiming next to a fake runner is a complete repo.
    F="$W/fakerepo"; mkdir -p "$F/tools"; cp -r "$TOOLS" "$F/tools/"
    printf '#!/usr/bin/env bash\ntimeout 5s true\nexit 3\n' > "$F/run-all-tests.sh"
    PATH="$(dirname "$REAL"):$PATH" NAAB_TIMING_DIR="$W/timed" bash "$F/tools/testtiming/run_timed.sh" > "$W/timed.out" 2>&1
    rc=$?
    rows=$(wc -l < "$W/timed/calls.tsv" 2>/dev/null | tr -d ' ')
    if [ "$rc" = 3 ] && [ "${rows:-0}" -ge 1 ] && [ -s "$W/timed/timing.md" ]; then
        ok "TT-05" "run_timed.sh returns the suite's exit status (3) and still writes the report"
    else bad "TT-05" "exit status or report wrong" "rc=$rc rows=${rows:-none}"; fi
fi

# TT-06 / TT-06c / TT-07: the report alone, on an authored log.
#   suite A 100..200 contains a naab-shaped call 110..120 and a nested suite
#   130..140; a top-level naab test runs 300..302.
if ! command -v python3 >/dev/null 2>&1; then
    for id in TT-06 TT-06c TT-07; do skip "$id" "python3 unavailable (UNMEASURABLE)"; done
else
    T=$'\t'
    {
        printf '100%s200%s0%s/r%s-k 30s 600s bash tests/a.sh\n' "$T" "$T" "$T" "$T"
        printf '110%s120%s0%s/r%s60s ./build/naab-lang run inner.naab\n' "$T" "$T" "$T" "$T"
        printf '130%s140%s0%s/r%s120s bash tests/nested.sh\n' "$T" "$T" "$T" "$T"
        printf '300%s302%s1%s/r%s10s ./build/naab-lang run top.naab\n' "$T" "$T" "$T" "$T"
    } > "$W/syn.tsv"
    json=$(python3 "$TOOLS/report.py" --format json --start 0 --end 400 --exit-code 0 < "$W/syn.tsv")
    names=$(printf '%s' "$json" | python3 -c "import json,sys;d=json.load(sys.stdin);print(' '.join(sorted(x['name'] for x in d['suites']+d['naab_tests'])))" 2>/dev/null)
    if [ "$names" = "tests/a.sh top.naab" ]; then ok "TT-06" "calls inside a suite's window are not counted again"
    else bad "TT-06" "wrong top-level set" "got: '$names'"; fi

    grep -v 'tests/a.sh' "$W/syn.tsv" > "$W/syn2.tsv"
    json2=$(python3 "$TOOLS/report.py" --format json --start 0 --end 400 --exit-code 0 < "$W/syn2.tsv")
    names2=$(printf '%s' "$json2" | python3 -c "import json,sys;d=json.load(sys.stdin);print(' '.join(sorted(x['name'] for x in d['suites']+d['naab_tests'])))" 2>/dev/null)
    if [ "$names2" = "inner.naab tests/nested.sh top.naab" ]; then
        ok "TT-06c" "CONTROL: without the enclosing suite the same calls ARE counted"
    else bad "TT-06c" "exclusion is not structural" "got: '$names2'"; fi

    md=$(python3 "$TOOLS/report.py" --format md --start 0 --end 400 --exit-code 0 < "$W/syn.tsv")
    if LC_ALL=C grep -q '[^[:print:][:space:]]' <<<"$md$json"; then
        bad "TT-07" "report output contains non-ASCII bytes"
    elif ! printf '%s' "$json" | python3 -c "import json,sys;json.load(sys.stdin)" 2>/dev/null; then
        bad "TT-07" "JSON report does not parse"
    else
        case "$md" in
            *"Test timing"*) ok "TT-07" "report output is ASCII and the JSON parses" ;;
            *) bad "TT-07" "Markdown report is empty or malformed" ;;
        esac
    fi
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
