#!/usr/bin/env bash
# ============================================================
# schedule_perturb.sh -- the concurrency suites with their threads squeezed
# onto fewer CPUs (Bundle 9, "schedule exploration, if ThreadSanitizer is quiet")
#
#   bash tests/tsan/schedule_perturb.sh [ROUNDS]     # default 3; normal build in build/
#
# ThreadSanitizer finds data races; it does not find an ORDER a program cannot
# survive -- a check that holds only when one thread usually wins. This runs
# each suite in tests/tsan/suites.txt ROUNDS times under each regime:
#   all    no pinning (the scheduler's own choice)
#   cpu2   pinned to two CPUs (taskset -c 0,1)
#   cpu1   pinned to one CPU (taskset -c 0): every thread interleaves on one
#          core, so a thread runs until it blocks or its time slice ends -- a
#          very different set of orders from the free run
# A suite that passes free and fails pinned is an order the free runs never
# produced -- a lead to triage, since a suite that asserts on timing can fail
# pinned for that reason alone. This is PERTURBATION, not systematic exploration: it widens the
# orders sampled, it does not enumerate them (rr's chaos mode, or a controlled
# scheduler, would; neither is installed here). A clean run says these rounds
# under these regimes found nothing, not that no bad order exists.
#
#   SP-00   CONTROL: a planted suite that fails exactly when it can see one
#           CPU fails under cpu1 and passes free -- the pinning is applied and
#           a failing verdict reaches this report
#   SP-<regime>/<suite>   passes in every round
#
# Report-only, not in CI: about 8 suites x 3 regimes x ROUNDS runs. Needs
# taskset (util-linux) and at least two CPUs; a regime that needs more CPUs
# than the machine has is UNMEASURABLE.
# ============================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROUNDS="${1:-3}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/naab_sched_XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -15 | sed 's/^/       | /'; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }
finish() {
    echo ""
    echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
    [ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ] || exit 1
    exit 0
}

echo "=== Schedule perturbation: the concurrency suites on fewer CPUs (Bundle 9) ==="

if ! command -v taskset >/dev/null 2>&1; then unm SP-00 "taskset not found -- every arm UNMEASURABLE"; finish; fi
CPUS="$(nproc 2>/dev/null || echo 1)"
if [ "$CPUS" -lt 2 ]; then unm SP-00 "one CPU here: pinning changes nothing -- every arm UNMEASURABLE"; finish; fi

# regime NAME -> the command prefix, or empty when the machine cannot provide it
regime_prefix() {
    case "$1" in
        all)  echo "" ;;
        cpu2) [ "$CPUS" -ge 3 ] && echo "taskset -c 0,1" || echo "-" ;;
        cpu1) echo "taskset -c 0" ;;
    esac
}
REGIMES=(all cpu2 cpu1)

# ---- SP-00: the planted control ----------------------------------------------
# nproc reports the CPUs this process may run on, so it sees the pinning.
cat > "$WORK/planted.sh" <<'P'
#!/usr/bin/env bash
n="$(nproc)"
if [ "$n" -eq 1 ]; then echo "  FAIL [planted] ran on one CPU"; exit 1; fi
echo "  PASS [planted] ran on $n CPUs"; exit 0
P
free_rc=0; bash "$WORK/planted.sh" >/dev/null 2>&1 || free_rc=$?
pin_rc=0; taskset -c 0 bash "$WORK/planted.sh" >/dev/null 2>&1 || pin_rc=$?
if [ "$free_rc" -eq 0 ] && [ "$pin_rc" -ne 0 ]; then
    pass SP-00 "CONTROL: a planted one-CPU failure fails under cpu1 and passes free"
else
    unm SP-00 "CONTROL: the planted suite did not fail only when pinned (free rc=$free_rc, cpu1 rc=$pin_rc) -- every arm UNMEASURABLE"; finish
fi

SUITES=()
while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    SUITES+=("$line")
done < "$ROOT/tests/tsan/suites.txt"
[ "${#SUITES[@]}" -gt 0 ] || { unm SP-01 "tests/tsan/suites.txt lists no suite"; finish; }

for reg in "${REGIMES[@]}"; do
    prefix="$(regime_prefix "$reg")"
    for s in "${SUITES[@]}"; do
        name="$(basename "$s" .sh)"
        if [ "$prefix" = "-" ]; then unm "SP-$reg/$name" "$CPUS CPUs: pinning to two is the free run here"; continue; fi
        if [ ! -f "$ROOT/$s" ]; then fail "SP-$reg/$name" "suite missing"; continue; fi
        bad=0; last=""
        for r in $(seq 1 "$ROUNDS"); do
            # $prefix is unquoted on purpose: empty, or "taskset -c LIST".
            # shellcheck disable=SC2086
            out="$(cd "$ROOT" && timeout 1800 $prefix bash "$s" 2>&1)"; rc=$?
            if [ "$rc" -ne 0 ]; then bad=$((bad+1)); last="round $r, exit $rc"$'\n'"$out"; fi
        done
        if [ "$bad" -eq 0 ]; then pass "SP-$reg/$name" "passes in $ROUNDS of $ROUNDS rounds"
        else fail "SP-$reg/$name" "failed in $bad of $ROUNDS rounds" "$last"; fi
    done
done
finish
