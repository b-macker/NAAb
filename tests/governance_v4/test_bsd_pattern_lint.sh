#!/usr/bin/env bash
# test_bsd_pattern_lint.sh -- load-time BSD pattern lint: can it ever fire, and
#   is it subsumed by another (Bundle 10.1; A113)
#
# Two report-only load warnings, neither of which changes a verdict (a pattern
# that cannot fire blocked nothing anyway; a subsumed pattern's twin still
# fires). SOUNDNESS is the whole point: a false "can never fire" or "subsumed"
# would push an operator to delete a pattern that DOES fire -- a loosening by
# mistake -- so every warning here is emitted only on a provable case, and each
# has a control proving a pattern that fires is never flagged.
#
#   PL-00c  CONTROL: a valid single pattern loads with neither warning and the
#           program runs
#   PL-01   a step with no matcher ("" in the sequence) -> "can never fire",
#           naming the pattern and the step index
#   PL-01c  CONTROL: the same pattern with a real step in place of the empty one
#           draws no "can never fire" warning
#   PL-02   two patterns with identical steps where one's window is wider at an
#           equal-or-stricter level -> the narrower is reported subsumed
#   PL-02c  CONTROL: identical steps but DIFFERENT detail globs -> not reported
#           (the two match different events; neither is redundant)
#   PL-02d  CONTROL: identical steps but incomparable windows (wider gap yet a
#           weaker level) -> not reported (neither fires whenever the other does)
#   PL-03   report-only: a config carrying both a dead and a subsumed pattern
#           still LOADS and runs (exit 0) -- the lint never refuses
#
# Kill table (observed 2026-11-01, each mutant built and run alone; controls
# PL-00c/PL-01c/PL-02c/PL-02d stay green under both):
#   the empty-step warning block removed       -> PL-01 red
#   the subsumption block removed              -> PL-02 red
#
# Red first (observed 2026-11-01): PL-01 and PL-02 assert warnings this branch
# adds, absent on master -- the two mutants above reproduce the master state.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_bsdlint_XXXXXX")
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
trap 'rm -rf "$TEST_TMP"; teardown_isolated_trust' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -4 | sed 's/^/       -> /'; }

# load ID PATTERNS_JSON -> RC, OUT (stderr+stdout), RAN(yes/no)
load() {
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    printf '{"version":"3.0","mode":"enforce","behavioral_sequences":{"enabled":true,"patterns":[%s]}}\n' "$2" > "$d/govern.json"
    printf 'main { print("RAN-OK") }\n' > "$d/p.naab"
    RC=0; OUT=$(cd "$d" && timeout 30 "$NAAB" p.naab 2>&1) || RC=$?
    RAN=no; grep -q 'RAN-OK' <<<"$OUT" && RAN=yes
}

echo "=== BSD pattern lint: emptiness and subsumption at load (A113) ==="

load pl00 '{"name":"ok","sequence":["file.read","file.write"],"level":"hard"}'
if [ "$RAN" = yes ] && ! grep -qiE 'can never fire|subsumed' <<<"$OUT"; then
    pass PL-00c "CONTROL: a valid pattern loads with neither warning and runs"
else fail PL-00c "a valid pattern drew a lint warning or did not run" "ran=$RAN $(grep -iE 'never fire|subsumed' <<<"$OUT" | head -1)"; fi

load pl01 '{"name":"emptystep","sequence":["file.read",""],"level":"hard"}'
if grep -q 'pattern "emptystep": step 1 has no matcher' <<<"$OUT" && grep -q 'can never fire' <<<"$OUT"; then
    pass PL-01 "a step with no matcher is reported as never-firing, naming the step"
else fail PL-01 "an empty step was not reported" "$(grep -i warning <<<"$OUT" | head -2)"; fi

load pl01c '{"name":"emptystep","sequence":["file.read","file.write"],"level":"hard"}'
if ! grep -qi 'can never fire' <<<"$OUT"; then pass PL-01c "CONTROL: a real step draws no never-fire warning"
else fail PL-01c "a firing pattern was called never-firing" "$(grep -i 'never fire' <<<"$OUT" | head -1)"; fi

load pl02 '{"name":"wide","sequence":["file.read","file.write"],"max_gap":10,"level":"hard"},{"name":"narrow","sequence":["file.read","file.write"],"max_gap":5,"level":"soft"}'
if grep -q 'pattern "narrow" is subsumed by "wide"' <<<"$OUT"; then
    pass PL-02 "the narrower pattern is reported subsumed by the wider, stricter one"
else fail PL-02 "subsumption was not reported" "$(grep -i subsumed <<<"$OUT" | head -2)"; fi

load pl02c '{"name":"a","sequence":["file.read:*x*","file.write"],"level":"hard"},{"name":"b","sequence":["file.read:*y*","file.write"],"level":"hard"}'
if ! grep -qi 'subsumed' <<<"$OUT"; then pass PL-02c "CONTROL: different detail globs are not called subsumed"
else fail PL-02c "patterns matching different events were called redundant" "$(grep -i subsumed <<<"$OUT" | head -1)"; fi

load pl02d '{"name":"wg","sequence":["file.read","file.write"],"max_gap":10,"level":"soft"},{"name":"ng","sequence":["file.read","file.write"],"max_gap":5,"level":"hard"}'
if ! grep -qi 'subsumed' <<<"$OUT"; then pass PL-02d "CONTROL: incomparable windows (wider gap vs stricter level) are not called subsumed"
else fail PL-02d "incomparable patterns were called subsumed" "$(grep -i subsumed <<<"$OUT" | head -1)"; fi

load pl03 '{"name":"dead","sequence":["file.read",""],"level":"hard"},{"name":"wide","sequence":["env.get","net_connect"],"max_gap":10,"level":"hard"},{"name":"narrow","sequence":["env.get","net_connect"],"max_gap":5,"level":"hard"}'
if [ "$RC" -eq 0 ] && [ "$RAN" = yes ]; then pass PL-03 "report-only: a config with a dead and a subsumed pattern still loads and runs"
else fail PL-03 "the lint refused a config instead of warning" "rc=$RC ran=$RAN"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ] || exit 1
exit 0
