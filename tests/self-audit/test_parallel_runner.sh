#!/usr/bin/env bash
# ============================================================
# test_parallel_runner.sh -- tools/testrunner/parallel.py reports what ran, faithfully
#
# The runner is report-only and earns trust only by agreeing with
# run-all-tests.sh. Every property below is something a parallel runner can
# get wrong silently, and each is paired with a control showing the check can
# fail (docs/investigation-method.md, "a negative result without a positive
# control is not a result").
#
# Runs against a stand-in repo -- a copy of tools/testrunner next to a fake
# run-all-tests.sh that lists hand-made units -- so each property can be forced
# both ways without the real suite. The runner derives REPO from its own path.
#
#   PR-01  every listed unit runs and is counted (completeness)
#   PR-02  verdicts follow run-all-tests.sh's policy: rc 0 PASS, other FAIL,
#          124 on a plain suite TIMEOUT, 124 on the absorb kind SKIP-TIMEOUT;
#          the runner exits non-zero when anything failed
#   PR-03  isolation: a file one unit writes to $HOME does not reach a later one
#   PR-03c CONTROL: the same pair under --shared --jobs 1 DOES leak, so PR-03
#          can fail
#   PR-04  a file a unit leaves in its HOME is reported; engine and toolchain
#          state (caches, security log) is reported apart, not as a leftover
#   PR-05  an EXCLUSIVE unit overlaps no other unit
#   PR-05c CONTROL: two ordinary units under --jobs 2 DO overlap, so PR-05's
#          "no overlap" is not true of every schedule
#   PR-06  compare flags an exit-status difference and a skip-marker difference
#   PR-06c CONTROL: compare of a result with itself is EQUIVALENT
#   PR-08  a SKIP marker is counted when the suite colours it (a colour code
#          directly before the word defeats a word-boundary match)
#   PR-07  the real run-all-tests.sh list mode refuses any phase but shell, and
#          lists the units this suite is registered among (itself included)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== The parallel runner reports what ran, faithfully ==="

if ! command -v python3 >/dev/null 2>&1 || ! command -v timeout >/dev/null 2>&1; then
    for id in PR-01 PR-02 PR-03 PR-03c PR-04 PR-05 PR-05c PR-06 PR-06c PR-07 PR-08; do
        skip "$id" "python3 or timeout unavailable (UNMEASURABLE)"; done
    echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        for id in PR-01 PR-02 PR-03 PR-03c PR-04 PR-05 PR-05c PR-06 PR-06c PR-07 PR-08; do
            skip "$id" "the runner is POSIX-only (UNMEASURABLE here)"; done
        echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0 ;;
esac

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-prunner.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
trap 'rm -rf "$W"' EXIT

# The runner's own HOME: --shared runs units in it, so it must never be the
# real one. TMPDIR too: the runner keeps a unit's leftovers for inspection and
# list mode creates a capture dir, so both must land inside $W to be removed.
export HOME="$W/realhome"; mkdir -p "$HOME"
export TMPDIR="$W/tmp"; mkdir -p "$TMPDIR"

F="$W/fake"
mkdir -p "$F/tools" "$F/u" "$F/tests/self-audit"
cp -r "$REPO/tools/testrunner" "$F/tools/"
git -C "$F" init -q 2>/dev/null

# units: name -> body. Each fake unit is a bash script under $F/u/.
mk() { printf '#!/usr/bin/env bash\n%s\n' "$2" > "$F/$1"; }
# The fake run-all-tests.sh: list mode writes the plan in $F/plan; the naab phase is skipped
# with --no-naab-phase throughout.
write_plan() {  # lines: kind<TAB>timeout<TAB>path
    printf '%s\n' "$@" > "$F/plan"
    cat > "$F/run-all-tests.sh" <<'EOF'
#!/usr/bin/env bash
[ -n "${NAAB_TEST_LIST:-}" ] || { echo "fake runner: list mode only" >&2; exit 1; }
cat "$(dirname "$0")/plan" >> "$NAAB_TEST_LIST"
EOF
}
T=$'\t'
runp() { python3 "$F/tools/testrunner/parallel.py" run --no-naab-phase --out "$W/out" "$@" > "$W/run.out" 2>&1; }
jq_py() { python3 -c "import json,sys;d=json.load(sys.stdin);$1" < "$W/out/results.json"; }

# --- PR-01 / PR-02 -------------------------------------------------------------
mk u/pass.sh 'exit 0'
mk u/fail.sh 'exit 1'
mk u/hang.sh 'sleep 30'
mk u/absorb.sh 'sleep 30'
write_plan "shell${T}60s${T}u/pass.sh" "shell${T}60s${T}u/fail.sh" \
           "shell${T}1s${T}u/hang.sh" "shell-skip124${T}1s${T}u/absorb.sh"
runp --jobs 2; rc=$?
got=$(jq_py "print(d['units_listed'], d['units_run'])" 2>/dev/null)
if [ "$got" = "4 4" ]; then ok "PR-01" "all 4 listed units ran and were counted"
else bad "PR-01" "listed/run mismatch" "got: '$got'"; fi

got=$(jq_py "print(' '.join(r['key'].split('/')[-1]+'='+r['verdict'] for r in d['results']))" 2>/dev/null)
if [ "$got" = "pass.sh=PASS fail.sh=FAIL hang.sh=TIMEOUT absorb.sh=SKIP-TIMEOUT" ] && [ "$rc" -ne 0 ]; then
    ok "PR-02" "verdicts follow run-all-tests.sh's policy, and a failure fails the runner"
else bad "PR-02" "verdict mapping or exit status wrong" "rc=$rc got: '$got'"; fi

# --- PR-03 / PR-03c --------------------------------------------------------------
mk u/writer.sh 'echo leaked > "$HOME/marker"'
mk u/reader.sh '[ ! -e "$HOME/marker" ]'
write_plan "shell${T}60s${T}u/writer.sh" "shell${T}60s${T}u/reader.sh"
runp --jobs 1
got=$(jq_py "print([r['verdict'] for r in d['results'] if r['key']=='u/reader.sh'][0])" 2>/dev/null)
if [ "$got" = "PASS" ]; then ok "PR-03" "a file written to one unit's HOME does not reach the next"
else bad "PR-03" "state leaked between isolated units" "reader: $got"; fi

rm -f "$HOME/marker"
runp --jobs 1 --shared
got=$(jq_py "print([r['verdict'] for r in d['results'] if r['key']=='u/reader.sh'][0])" 2>/dev/null)
if [ "$got" = "FAIL" ]; then ok "PR-03c" "CONTROL: with a shared HOME the same pair DOES leak, so PR-03 can fail"
else bad "PR-03c" "control saw no leak -- the probe cannot detect one" "reader: $got"; fi
rm -f "$HOME/marker"

# --- PR-04 --------------------------------------------------------------------------
mk u/litter.sh 'echo x > "$HOME/junk.txt"; mkdir -p "$HOME/.naab/cache"; echo m > "$HOME/.naab/cache/metadata.txt"'
write_plan "shell${T}60s${T}u/litter.sh"
runp --jobs 1
got=$(jq_py "r=d['results'][0];print(','.join(r['left_in_home'])+'|'+','.join(r['home_state']))" 2>/dev/null)
case "$got" in
    "junk.txt|"*metadata.txt*) ok "PR-04" "a unit's leftover is reported; engine/toolchain state is kept apart" ;;
    *) bad "PR-04" "leftovers misreported" "got: '$got'" ;;
esac

# --- PR-05 / PR-05c ----------------------------------------------------------------------
# A unit at an EXCLUSIVE path records its window; ordinary units sleep long
# enough that any overlap with it would be visible.
mk tests/self-audit/test_prescan_canaries.sh 'date +%s.%N > "$PR_STAMP.excl.start"; sleep 1; date +%s.%N > "$PR_STAMP.excl.end"'
mk u/slowA.sh 'date +%s.%N > "$PR_STAMP.A.start"; sleep 2; date +%s.%N > "$PR_STAMP.A.end"'
mk u/slowB.sh 'date +%s.%N > "$PR_STAMP.B.start"; sleep 2; date +%s.%N > "$PR_STAMP.B.end"'
export PR_STAMP="$W/stamp"
write_plan "shell${T}60s${T}tests/self-audit/test_prescan_canaries.sh" \
           "shell${T}60s${T}u/slowA.sh" "shell${T}60s${T}u/slowB.sh"
runp --jobs 3
overlap() {  # $1 $2 -> "yes" when the two recorded windows overlap
    python3 - "$PR_STAMP" "$1" "$2" <<'PY'
import sys
base, a, b = sys.argv[1:]
r = lambda n, e: float(open("%s.%s.%s" % (base, n, e)).read())
print("yes" if r(a, "start") < r(b, "end") and r(b, "start") < r(a, "end") else "no")
PY
}
e1=$(overlap excl A 2>/dev/null); e2=$(overlap excl B 2>/dev/null); ab=$(overlap A B 2>/dev/null)
if [ "$e1" = no ] && [ "$e2" = no ]; then ok "PR-05" "the exclusive unit overlapped nothing"
else bad "PR-05" "an exclusive unit ran alongside another" "excl/A=$e1 excl/B=$e2"; fi
if [ "$ab" = yes ]; then ok "PR-05c" "CONTROL: ordinary units under --jobs 3 DO overlap, so PR-05 can fail"
else bad "PR-05c" "ordinary units never overlapped -- PR-05 proves nothing" "A/B=$ab"; fi
unset PR_STAMP

# --- PR-08 ----------------------------------------------------------------------------
mk u/colour.sh 'printf "  \033[1;33mSKIP\033[0m [X-01] coloured\n  SKIP [X-02] plain\n"'
write_plan "shell${T}60s${T}u/colour.sh"
runp --jobs 1
got=$(jq_py "print(d['results'][0]['skip_markers'])" 2>/dev/null)
if [ "$got" = "2" ]; then ok "PR-08" "a coloured SKIP is counted alongside a plain one"
else bad "PR-08" "skip markers miscounted" "expected 2, got '$got'"; fi

# --- PR-06 / PR-06c -------------------------------------------------------------------------
mkj() {  # $1 file, $2 rc of u/a.sh, $3 skips of u/b.sh
    printf '{"results":[{"key":"u/a.sh","rc":%s,"skip_markers":0},{"key":"u/b.sh","rc":0,"skip_markers":%s}]}\n' "$2" "$3" > "$1"
}
mkj "$W/ref.json" 0 0; mkj "$W/cand.json" 1 2
cmp_out=$(python3 "$F/tools/testrunner/parallel.py" compare "$W/ref.json" "$W/cand.json"); crc=$?
case "$cmp_out" in
    *"Exit status differs: 1"*"u/a.sh"*"Skip-marker count differs: 1"*"u/b.sh"*"NOT EQUIVALENT"*)
        [ "$crc" -ne 0 ] && ok "PR-06" "compare flags both an exit-status and a skip-marker difference" \
                         || bad "PR-06" "compare found the differences but exited 0" ;;
    *) bad "PR-06" "compare missed a difference" "$(printf '%s' "$cmp_out" | tail -5)" ;;
esac
cmp_out=$(python3 "$F/tools/testrunner/parallel.py" compare "$W/ref.json" "$W/ref.json"); crc=$?
case "$cmp_out" in
    *EQUIVALENT) [ "$crc" -eq 0 ] && ok "PR-06c" "CONTROL: a result compared with itself is EQUIVALENT" \
                                  || bad "PR-06c" "self-compare exited non-zero" ;;
    *) bad "PR-06c" "self-compare not equivalent" ;;
esac

# --- PR-07: the real run-all-tests.sh ------------------------------------------------------------
if [ ! -f "$REPO/build/naab-lang" ]; then
    skip "PR-07" "no build/naab-lang -- run-all-tests.sh refuses to start (UNMEASURABLE)"
else
    L="$W/real-plan.tsv"
    ( cd "$REPO" && NAAB_TEST_PHASE=naab NAAB_TEST_LIST="$L" bash run-all-tests.sh > "$W/l1.out" 2>&1 ); r1=$?
    ( cd "$REPO" && NAAB_TEST_PHASE=shell NAAB_TEST_LIST="$L" bash run-all-tests.sh > "$W/l2.out" 2>&1 ); r2=$?
    n=$(wc -l < "$L" 2>/dev/null | tr -d ' ')
    if [ "$r1" -ne 0 ] && [ "$r2" -eq 0 ] && [ "${n:-0}" -ge 100 ] \
       && grep -q "tests/self-audit/test_parallel_runner.sh" "$L" \
       && grep -q "^shell-skip124${T}" "$L" && grep -q "^python${T}" "$L"; then
        ok "PR-07" "list mode refuses the naab phase and lists $n units, every kind, this suite among them"
    else bad "PR-07" "list mode wrong" "naab-phase rc=$r1 shell rc=$r2 units=${n:-none}"; fi
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
