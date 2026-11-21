#!/usr/bin/env bash
# ============================================================
# test_telemetry_after_cap.sh -- every recorded result reaches telemetry (A183)
#
# THE DEFECTS (one class: a check result is recorded and never written)
#
# (a) writeTelemetry() resumed from a vector INDEX ("results only ever
#     accumulate", #111), while capCheckResultsLocked() evicts from the front
#     once MAX_CHECK_RESULTS (10,000) is reached. At the cap the index equals
#     the size and stops moving, so every result recorded after a dump at the
#     cap was never written. Results evicted before their first dump were lost
#     without a count.
# (b) main.cpp's outer handlers for a fatal error flushed the advisory summary
#     and never wrote the reports, so a HARD refusal made after the run's last
#     report write -- a must_produce contract runs after execute() -- was printed
#     and absent from telemetry and the JSON report even with five governed
#     calls (observed on master c1312132).
# (c) a clean VM run wrote its reports inside execute() and never again, so
#     everything recorded after the program finished -- the after-run audit,
#     the execution contracts' passes, the baseline check -- never reached
#     telemetry or the reports (observed: a passing must_produce, no row).
#
# Every writer now numbers its result (record_id), telemetry resumes after the
# highest number it has written, the summary event reports results evicted
# before they were written (evicted_before_written), every fatal exit writes
# the reports through flushGovernanceOnFatalExit(), and a clean run writes
# them once more at its end. The stderr "Telemetry: N events" line reports the
# run's running total, so the last line printed still matches the file
# (test_telemetry_count.sh).
#
#   TC-00  CONTROL: the program runs and the contract refuses it (exit 3, the
#          must_produce error printed) -- the refusal the arms look for exists
#   TC-01  (b) with 5 governed calls the refusal is a RuleViolation row in
#          telemetry
#   TC-01r (b) and in the JSON report
#   TC-02  (a)+(b) with 5,200 calls (past the cap) the refusal is still written
#   TC-02e (a) the summary event counts the results the cap evicted before
#          they were written
#   TC-02c CONTROL: under the cap no eviction is reported
#   TC-03a (c) on a clean exit, a contract PASS recorded after execute()'s
#          report is written
#   TC-03  (a)+(c) the same past the cap
#   TC-04  every file above verifies as a chain (the extra report write on the
#          exit path writes nothing twice and does not break the chain)
#
# Kill table (each mutant applied alone, rebuilt, this suite run; observed
# 2026-11-21 on the default build):
#   M1 the fatal-exit report write removed       -> TC-01 TC-01r TC-02 red
#   M2 telemetry resumes from an index again      -> TC-02 TC-03 red
#   M3 the eviction count removed                 -> TC-02e red
#   M4 the clean-run final report write removed   -> TC-03a TC-03 red
# On master c1312132: TC-01, TC-01r, TC-02, TC-02e, TC-03a, TC-03 red; the
# controls and TC-04 green.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_telcap_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2${3:+ -- $3}"; }

echo "=== test_telemetry_after_cap.sh ==="
if [ ! -x "$NAAB" ]; then echo "  UNMEASURABLE: naab-lang not built"; echo "Result: 0 passed, 0 failed, 1 unmeasurable"; exit 1; fi

# run ID CALLS EXPECT: CALLS governed file.exists() calls (each records two
# results under a path rule), then double_it's must_produce contract, which
# EXPECT (4 = refused, 3 = passes) decides. Sets RC, OUT, D.
run() {
    D="$TEST_TMP/$1"; mkdir -p "$D"
    cat > "$D/govern.json" <<EOF
{"mode": "enforce", "security": {"sandbox_level": "elevated"},
 "capabilities": {"filesystem": {"mode": "read", "blocked_paths": ["vault/"]}},
 "telemetry": {"enabled": true, "output_file": "tele.jsonl",
               "tamper_evidence": {"enabled": true}},
 "contracts": {"level": "hard", "functions": {"double_it": {"must_produce": [{"args": [2], "expect": $3}]}}}}
EOF
    printf 'use file\nfn double_it(x) {\n    return x + 1\n}\nmain {\n    for i in 0..%s {\n        let e = file.exists("a.txt")\n    }\n    print("DONE")\n}\n' "$2" > "$D/t.naab"
    RC=0; OUT=$(cd "$D" && timeout 120s "$NAAB" run --governance-report report.json t.naab 2>&1) || RC=$?
}
# row_count FILE RULE TYPE: lines of that event type and rule
row_count() { grep -F "\"event_type\":\"$3\"" "$1" 2>/dev/null | grep -cF "\"rule_name\":\"$2\""; }
evicted_of() { grep -F '"event_type":"GovernanceCheckSummary"' "$1" 2>/dev/null | grep -oE '"evicted_before_written":[0-9]+' | grep -oE '[0-9]+$' | awk '{s+=$1} END {print s+0}'; }
RULE="contracts.double_it.must_produce"

# ---- small run, refused ---------------------------------------------------------
run small 5 4
SMALL="$D"
if [ "$RC" = 3 ] && grep -q 'DONE' <<<"$OUT" && grep -q "must_produce: 'double_it'" <<<"$OUT"; then
    pass TC-00 "CONTROL: the program ran and the contract refused it (exit 3)"
else
    fail TC-00 "the refusal the arms look for did not happen; nothing below can be read" "rc=$RC $(grep -m2 -E 'DONE|rror' <<<"$OUT" | tr '\n' ' ')"
    echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
fi
if [ "$(row_count "$SMALL/tele.jsonl" "$RULE" RuleViolation)" -ge 1 ]; then
    pass TC-01 "the post-run HARD refusal is a RuleViolation row in telemetry"
else
    fail TC-01 "the refusal was printed and never written to telemetry"
fi
if grep -qF "$RULE" "$SMALL/report.json" 2>/dev/null; then
    pass TC-01r "the refusal is in the JSON report"
else
    fail TC-01r "the refusal is missing from the JSON report"
fi
if [ "$(evicted_of "$SMALL/tele.jsonl")" = 0 ]; then
    pass TC-02c "CONTROL: under the cap no eviction is reported"
else
    fail TC-02c "an eviction was reported under the cap" "$(evicted_of "$SMALL/tele.jsonl")"
fi

# ---- past the cap, refused ---------------------------------------------------
run big 5200 4
BIG="$D"
if [ "$RC" = 3 ] && [ "$(row_count "$BIG/tele.jsonl" "$RULE" RuleViolation)" -ge 1 ]; then
    pass TC-02 "past the 10,000-result cap the refusal is still written"
else
    fail TC-02 "past the cap the refusal never reached telemetry" "rc=$RC rows=$(row_count "$BIG/tele.jsonl" "$RULE" RuleViolation)"
fi
EV=$(evicted_of "$BIG/tele.jsonl")
if [ "$EV" -ge 1 ]; then
    pass TC-02e "the summary counts $EV result(s) the cap evicted before they were written"
else
    fail TC-02e "results evicted before they were written are not counted" "evicted_before_written=$EV"
fi

# ---- past the cap, clean exit: a pass recorded after execute()'s report -------
run pass_small 5 3
P_SMALL="$D"; P_SMALL_RC=$RC
run pass_big 5200 3
P_BIG="$D"
if [ "$P_SMALL_RC" = 0 ] && [ "$(row_count "$P_SMALL/tele.jsonl" "$RULE" GovernanceCheck)" -ge 1 ]; then
    pass TC-03a "on a clean exit, the contract's pass (recorded after execute()) is written"
else
    fail TC-03a "a result recorded after the program finished never reached telemetry" "rc=$P_SMALL_RC"
fi
if [ "$RC" = 0 ] && [ "$(row_count "$P_BIG/tele.jsonl" "$RULE" GovernanceCheck)" -ge 1 ]; then
    pass TC-03 "past the cap, a result recorded after the first report write is written"
else
    fail TC-03 "past the cap, results recorded after the first report write were lost" "rc=$RC rows=$(row_count "$P_BIG/tele.jsonl" "$RULE" GovernanceCheck)"
fi

# ---- every file verifies as a chain ------------------------------------------
bad=""
for f in "$SMALL" "$BIG" "$P_SMALL" "$P_BIG"; do
    (cd "$f" && "$NAAB" --verify-telemetry-chain tele.jsonl >/dev/null 2>&1) || bad="$bad $(basename "$f")"
done
if [ -z "$bad" ]; then
    pass TC-04 "every run's telemetry verifies as a chain"
else
    fail TC-04 "a chain does not verify" "$bad"
fi

echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
