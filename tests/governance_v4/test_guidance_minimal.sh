#!/usr/bin/env bash
# ============================================================
# test_guidance_minimal.sh — guidance configured by listing sources, nothing else
#
# The owner's requirement (2026-10-12): "the settings in govern.json ... should
# just be what source to add". Every config in this file is exactly
#
#     "guidance": { "sources": [ ...paths... ] }
#
# -- no enabled, no rules, no anchors, no query -- and the engine routes by
# itself: a section or row NAMED for what happened (the check that blocked,
# the drift signal that paid, the agent starting), or a failed validation's
# keywords. The noise arms are the controls: with no section named for the
# event and no strong match, NOTHING is delivered.
#
#   terminal          M-01 a doc section and a db row named for the check,
#                     both engines; M-02 nothing named -> nothing (noise)
#   agent             M-03 signal by name (control: no section -> nothing),
#                     M-04 start by agent name (control: no agent section ->
#                     nothing, no keyword fallback), M-05 validation keywords
#                     (control: a vague failure -> nothing)
#   database          M-06 a bare guidance(topic, body) table; a row added
#                     mid-run reaches the next request (not the one before)
#   switches          M-07 "enabled": false turns it off; M-08 a database with
#                     no guidance table warns with the fix
#   governed          M-09 list-form sources are write-protected too
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
TEST_TMP="$(mktemp -d)"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_STARTED=0
cleanup() {
    [ "$STUB_STARTED" = 1 ] && stop_stub >/dev/null 2>&1
    command -v stop_swap_operators >/dev/null 2>&1 && stop_swap_operators
    teardown_isolated_trust
    [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"
}
trap cleanup EXIT
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }
python3 -c "import sqlite3" 2>/dev/null || { echo "python3 sqlite3 module unavailable"; exit 1; }

# A playbook written the way a team would: headings named for things.
playbook() {  # $1 dir, $2 "full" | "unrelated"
    mkdir -p "$1/docs"
    if [ "$2" = full ]; then cat > "$1/docs/playbook.md" <<'EOF'
# Team playbook

## No placeholders
MIN_STUBS_m2: unfinished work goes in stubs/ with a ticket number.

## Response repetition
MIN_REPEAT_m2: never send the same answer twice; restate it with new detail.

## worker
MIN_WORKER_m2: you maintain billing; read the README first.

## Database migrations
MIN_DB_m2: a failed migration is rolled back with the rollback script; check the schema version first.

## No PII
MIN_PII_m2: keep personal data out of programs; load it from the vault at runtime.
EOF
    else cat > "$1/docs/playbook.md" <<'EOF'
# Team playbook

## Lunch
MIN_LUNCH_m2: the cafeteria opens at noon.

## Billing service
MIN_BILLING_m2: the billing service totals invoice lines nightly.
EOF
    fi
}
make_db() {  # $1 dir, $2 variant
    mkdir -p "$1/data"
    python3 - "$1/data/guidance.db" "${2:-v1}" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("CREATE TABLE guidance(topic TEXT, body TEXT)")
txt = "MIN_DBROW_m2: open a ticket for every stub." if sys.argv[2] == "v1" else "MIN_DBROW_m2_EDITED: tickets now go to the new tracker."
rows = [
    ("code_quality.no_placeholders", txt),
    ("response_repetition", "MIN_DBREPEAT_m2: vary every answer."),
]
if sys.argv[2] == "v2":   # a row added while the program runs
    rows.append(("validation_failed", "MIN_DBVALID_m2: rerun the failing test before answering."))
db.executemany("INSERT INTO guidance VALUES (?, ?)", rows)
db.commit(); db.close()
PY
}
# cfg DIR SOURCES_JSON [EXTRA_TOP] — the guidance block is ONLY the sources list.
cfg() {
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},\n "telemetry": {"enabled": true, "output_file": "tele.jsonl"},\n %s\n "guidance": {"sources": %s}}\n' "${3:-}" "$2" > "$1/govern.json"
}
run() {  # $1 dir, $2 engine, $3 program -> "rc=N|stdout+stderr"
    printf '%s\n' "$3" > "$1/p.naab"
    local args=("$NAAB"); [ "$2" = tw ] && args+=(--tree-walk); args+=(p.naab)
    local out; out="$(cd "$1" && "${args[@]}" 2>&1)"; local rc=$?
    printf 'rc=%s|%s' "$rc" "$out"
}
PLACEHOLDER='fn work() {
    // TODO: finish this later
    return 1
}
main {
    print(work())
}'
QC='"code_quality": {"no_placeholders": {"enabled": true, "level": "hard"}},'

echo ""
echo "== terminal: a block ends with what the project says about that check"
for eng in vm tw; do
    d="$TEST_TMP/m01_$eng"; mkdir -p "$d"; playbook "$d" full; make_db "$d"
    cfg "$d" '["docs/playbook.md", "data/guidance.db"]' "$QC"
    r="$(run "$d" "$eng" "$PLACEHOLDER")"
    named="$(grep '"GUIDANCE_DELIVERED"' "$d/tele.jsonl" 2>/dev/null | grep -c '"select":"name"')"
    if [[ "$r" == rc=3* ]] && [[ "$r" == *"Project guidance (docs/playbook.md > No placeholders):"*MIN_STUBS_m2* ]] \
       && [[ "$r" == *"Project guidance (data/guidance.db > code_quality.no_placeholders):"*MIN_DBROW_m2* ]] \
       && [[ "$r" != *MIN_REPEAT_m2* ]] && [ "$named" -eq 2 ]; then
        pass "M-01/$eng" "sources-only config: the block ends with the doc section and the db row NAMED for the check, and nothing else"
    else
        fail "M-01/$eng" "named guidance missing from the block" "${r: -500}"
    fi
done
d="$TEST_TMP/m02"; mkdir -p "$d"; playbook "$d" unrelated; cfg "$d" '["docs/playbook.md"]' "$QC"
r="$(run "$d" vm "$PLACEHOLDER")"
if [[ "$r" == rc=3*Placeholder* ]] && [[ "$r" != *"Project guidance"* ]]; then
    pass "M-02" "noise control: with no section named for the check, the block carries no guidance"
else
    fail "M-02" "unrelated guidance attached, or the block did not happen" "${r: -300}"
fi

# M-10: a check whose name has no usable keywords (the extractor drops words of
# three letters or fewer: "no", "pii") is still found -- by NAME. Without name
# matching this arm has nothing to match on.
d="$TEST_TMP/m10"; mkdir -p "$d"; playbook "$d" full
cfg "$d" '["docs/playbook.md"]' '"code_quality": {"no_pii": {"enabled": true, "level": "hard"}},'
r="$(run "$d" vm 'fn contact() {
    let ssn = "123-45-6789"
    return ssn
}
main {
    print(contact())
}')"
if [[ "$r" == rc=3* ]] && [[ "$r" == *"Project guidance (docs/playbook.md > No PII):"*MIN_PII_m2* ]]; then
    pass "M-10" "a check with no usable keywords (code_quality.no_pii) is matched by NAME to the section 'No PII'"
else
    fail "M-10" "name matching did not find 'No PII'" "${r: -400}"
fi

d="$TEST_TMP/m07"; mkdir -p "$d"; playbook "$d" full
printf '{"version": "5.0", "mode": "enforce", %s "guidance": {"enabled": false, "sources": ["docs/playbook.md"]}}\n' "$QC" > "$d/govern.json"
r="$(run "$d" vm "$PLACEHOLDER")"
if [[ "$r" == rc=3*Placeholder* ]] && [[ "$r" != *"Project guidance"* ]]; then
    pass "M-07" "\"enabled\": false switches it off with the sources still listed (M-01 is the on control)"
else
    fail "M-07" "enabled:false did not switch guidance off" "${r: -300}"
fi

d="$TEST_TMP/m08"; mkdir -p "$d/data"
python3 -c "import sqlite3,sys;db=sqlite3.connect(sys.argv[1]);db.execute('create table notes(x text)');db.commit()" "$d/data/other.db"
cfg "$d" '["data/other.db"]' "$QC"
r="$(run "$d" vm "$PLACEHOLDER")"
if [[ "$r" == rc=3* ]] && [[ "$r" == *"has no table named guidance"*"guidance(topic, body)"* ]]; then
    pass "M-08" "a database with no guidance table warns and names the fix; the block still happens"
else
    fail "M-08" "missing-table warning absent" "${r: -300}"
fi

d="$TEST_TMP/m09"; mkdir -p "$d"; playbook "$d" full; make_db "$d"; cfg "$d" '["docs/playbook.md", "data/guidance.db"]'
w1="$(run "$d" vm 'use file
main {
    file.write("docs/playbook.md", "OVERWRITTEN")
    print("WROTE")
}')"
w2="$(run "$d" vm 'use file
main {
    file.write("data/guidance.db-wal", "crafted")
    print("WROTE")
}')"
w3="$(run "$d" vm 'use file
main {
    file.write("notes.md", "ordinary")
    print("WROTE")
}')"
if [[ "$w3" == rc=0*WROTE* ]] && [[ "$w1" == rc=3* ]] && [[ "$w2" == rc=3* ]] && [[ "$w1$w2" != *WROTE* ]]; then
    pass "M-09" "list-form sources are write-protected (document, database -wal); another file writes fine"
else
    fail "M-09" "a list-form source was writable" "${w1:0:120} / ${w2:0:120} / ${w3:0:80}"
fi

# ---------------------------------------------------------------------------
echo ""
echo "== agent: the next request carries what the project says about the event"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;; *) WINDOWS=0 ;; esac
[ -n "${WINDIR:-}" ] && WINDOWS=1
if [ "$WINDOWS" = 1 ]; then
    skip "M-AGENT" "stub-backed arms are not run on Windows (stub_platform.sh)"
else
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
source "$SCRIPT_DIR/../helpers/config_swap.sh"
export FAKE_KEY_GUIDE="fake-key-guidance"
FIXTURE="$TEST_TMP/fixture.json"
cat > "$FIXTURE" <<'EOF'
{"responses": [{"content": "The billing total is computed from the invoice lines.", "input_tokens": 12, "output_tokens": 24}]}
EOF
CDD='"behavioral_sequences": {"enabled": true}, "advisory_escalation": {"enabled": false},
 "context_drift": {"enabled": true, "level": "advisory", "check_interval_turns": 1, "reality_checkpoint": {"enabled": false}},'
agent_run() {  # $1 dir, $2 sources json, $3 extra top, $4 program body
    local d="$1"
    start_stub "$FIXTURE" "$d" >/dev/null 2>&1 || { echo "STUB_FAIL"; return; }
    STUB_STARTED=1
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},\n "telemetry": {"enabled": true, "output_file": "tele.jsonl"},\n %s\n "agents": {"worker": {"provider": "gemini", "model": "stub-model", "api_base": "http://127.0.0.1:%s", "api_key_env": "FAKE_KEY_GUIDE", "max_tokens": 200, "max_turns": 40, "system_prompt": "You maintain the billing service."}},\n "guidance": {"sources": %s}}\n' \
        "$3" "$STUB_PORT" "$2" > "$d/govern.json"
    printf 'use agent\nuse file\nuse time\nmain {\n    let h = agent.create("worker")\n%s\n    print("RUN_DONE")\n}\n' "$4" > "$d/p.naab"
    local out; out="$( (cd "$d" && timeout 120s "$NAAB" p.naab 2>"$d/err.txt") )"
    stop_stub; STUB_STARTED=0
    printf '%s' "$out"
}
has() { case "$(cat "$1/req_$2.json" 2>/dev/null)" in *"$3"*) return 0;; *) return 1;; esac; }
SENDS3='    let i = 0
    while i < 3 { i = i + 1; let r = agent.send(h, "Explain the invoice total.") }'

d="$TEST_TMP/m03"; mkdir -p "$d"; playbook "$d" full
out="$(agent_run "$d" '["docs/playbook.md"]' "$CDD" "$SENDS3")"
dc="$TEST_TMP/m03c"; mkdir -p "$dc"; playbook "$dc" unrelated
outc="$(agent_run "$dc" '["docs/playbook.md"]' "$CDD" "$SENDS3")"
if [[ "$out" != *RUN_DONE* || "$outc" != *RUN_DONE* ]]; then
    fail "M-03" "runs did not complete" "$out / $outc"
elif has "$d" 3 MIN_REPEAT_m2 && ! has "$d" 2 MIN_REPEAT_m2 && ! has "$dc" 3 "Project guidance"; then
    pass "M-03" "a paid signal fetched the section named for it (Response repetition); with no such section nothing is sent"
else
    fail "M-03" "signal routing wrong" "named req3=$(has "$d" 3 MIN_REPEAT_m2 && echo y || echo n) control req3=$(has "$dc" 3 'Project guidance' && echo y || echo n)"
fi

d="$TEST_TMP/m04"; mkdir -p "$d"; playbook "$d" full
out="$(agent_run "$d" '["docs/playbook.md"]' "" '    let r = agent.send(h, "Hello")')"
dc="$TEST_TMP/m04c"; mkdir -p "$dc"; playbook "$dc" unrelated
outc="$(agent_run "$dc" '["docs/playbook.md"]' "" '    let r = agent.send(h, "Hello")')"
if [[ "$out" == *RUN_DONE* ]] && has "$d" 1 MIN_WORKER_m2 && ! has "$d" 1 MIN_DB_m2 \
   && [[ "$outc" == *RUN_DONE* ]] && ! has "$dc" 1 "Project guidance"; then
    pass "M-04" "an agent's first request carries the section named for it (worker); with none, nothing -- not even a section whose heading matches the agent's prompt (Billing service)"
else
    fail "M-04" "start routing wrong" "named=$(has "$d" 1 MIN_WORKER_m2 && echo y || echo n) control=$(has "$dc" 1 'Project guidance' && echo y || echo n)"
fi

VALPROG() { printf '    let a = agent.send(h, "Summarise the invoice flow.")\n    agent.record_validation(h, false, "%s")\n    let b = agent.send(h, "Try again.")' "$1"; }
d="$TEST_TMP/m05"; mkdir -p "$d"; playbook "$d" full
out="$(agent_run "$d" '["docs/playbook.md"]' "" "$(VALPROG 'migration rollback failed: schema version mismatch')")"
dc="$TEST_TMP/m05c"; mkdir -p "$dc"; playbook "$dc" full
outc="$(agent_run "$dc" '["docs/playbook.md"]' "" "$(VALPROG 'it did not work')")"
if [[ "$out" == *RUN_DONE* ]] && has "$d" 2 MIN_DB_m2 && [[ "$outc" == *RUN_DONE* ]] && ! has "$dc" 2 "Project guidance"; then
    pass "M-05" "a failed validation fetched the section its detail strongly matches; a vague failure fetches nothing"
else
    fail "M-05" "validation routing wrong" "strong=$(has "$d" 2 MIN_DB_m2 && echo y || echo n) vague=$(has "$dc" 2 'Project guidance' && echo y || echo n)"
fi

# M-06: a bare guidance(topic, body) table, and a row ADDED mid-run. The same
# validation failure is recorded before and after the database changes: before,
# the database has no row for it and the next request carries nothing from it;
# after, the next request carries the new row -- read from the database at
# that trigger, with no reload and no query written.
d="$TEST_TMP/m06"; mkdir -p "$d"; make_db "$d"
mkdir -p "$TEST_TMP/m06_new"; make_db "$TEST_TMP/m06_new" v2
start_swap_operator "$d"
out="$(agent_run "$d" '["data/guidance.db"]' "$CDD" "$SENDS3
    agent.record_validation(h, false, \"it did not work\")
    let r4 = agent.send(h, \"Try again.\")
    file.write(\".swap_req\", \"$TEST_TMP/m06_new/data/guidance.db\t$d/data/guidance.db\n\")
    file.write(\".swap_req.ready\", \"\")
    let waited = 0
    while !file.exists(\".swap_done\") && waited < 200 { time.sleep(0.05); waited = waited + 1 }
    agent.record_validation(h, false, \"it did not work\")
    let r5 = agent.send(h, \"Try again.\")")"
if [[ "$out" != *RUN_DONE* ]]; then
    fail "M-06" "run did not complete" "$out / $(tail -3 "$d/err.txt")"
elif has "$d" 3 MIN_DBREPEAT_m2 && ! has "$d" 4 MIN_DBVALID_m2 && has "$d" 5 MIN_DBVALID_m2; then
    pass "M-06" "a plain guidance(topic, body) table routes by topic with no query; a row added mid-run reaches the next request (not before it existed)"
else
    fail "M-06" "table-mode database guidance wrong" "signal req3=$(has "$d" 3 MIN_DBREPEAT_m2 && echo y || echo n) before req4=$(has "$d" 4 MIN_DBVALID_m2 && echo y || echo n) after req5=$(has "$d" 5 MIN_DBVALID_m2 && echo y || echo n)"
fi
fi  # not Windows

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT unmeasurable"
[ "$FAIL_COUNT" -eq 0 ]
