#!/usr/bin/env bash
# ============================================================
# test_guidance_sqlite.sh — live guidance from a database (docs/plan-live-guidance.md)
#
# A `sqlite` guidance source is opened READ-ONLY and asked one SELECT per
# trigger; column 1 is the heading, column 2 the text, in the query's own
# order. The trigger reaches the query as BOUND PARAMETERS (:kind :arg :agent
# :topic :keywords :keywords_or), never as SQL text.
#
#   reaches the model / script  S-01 pull by topic, S-11 :keywords, S-12 FTS5
#                               ranking, S-02 terminal block (both engines),
#                               S-20 agent request on a paid signal
#   live                        S-09 a row changed mid-run is what arrives
#   bound, not spliced          S-03 a quote in the topic finds nothing (S-01
#                               is the control that the topic itself matches)
#   read-only                   S-05 a write, a second statement and ATTACH are
#                               refused; the table is unchanged afterwards
#   quick                       S-07 a runaway query is interrupted at its
#                               deadline; S-20 reports the observed lookup time
#   governed                    S-04 the program cannot write the database or
#                               its -wal; S-06 a blocked path is not read;
#                               S-08 a secret row is withheld (control S-08b)
#   config                      S-10 an unknown parameter is reported
#
# Every refusal is paired with a success through the same fixture.
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

FAKE_KEY="AKIA$(printf 'Q%.0s' $(seq 1 16))"

# make_db DIR [variant]: data/kb.db with one guidance table (and FTS5 when the
# library has it). Rows carry unique markers so a request body can be read.
make_db() {
    mkdir -p "$1/data"
    python3 - "$1/data/kb.db" "$FAKE_KEY" "${2:-v1}" <<'PY'
import sqlite3, sys
path, key, variant = sys.argv[1], sys.argv[2], sys.argv[3]
db = sqlite3.connect(path)
db.execute("CREATE TABLE guidance(topic TEXT, heading TEXT, body TEXT, priority INT)")
mig = "DB_MIGRATION_k4: roll back with the rollback script." if variant == "v1" \
      else "DB_MIGRATION_k4_EDITED: after the edit, use the new rollback script."
rows = [
  ("response_repetition", "Vary your answers", "DB_REPEAT_k4: never repeat an answer; restate it with new detail.", 5),
  ("mandate_alignment", "Stay on billing", "DB_MANDATE_k4: answer only about the billing service.", 5),
  ("code_quality.no_placeholders", "Stubs policy", "DB_STUBS_k4: unfinished work goes to stubs/ with a ticket.", 5),
  ("migration", "Migrations", mig, 1),
  ("secret", "Staging key", "DB_SECRET_k4: the staging key is %s." % key, 1),
]
db.executemany("INSERT INTO guidance VALUES (?,?,?,?)", rows)
try:
    db.execute("CREATE VIRTUAL TABLE kb_fts USING fts5(heading, body)")
    db.executemany("INSERT INTO kb_fts VALUES (?,?)", [
        ("Frontend styling", "DB_CSS_k4: layout and colors come from the shared stylesheet."),
        ("Migrations", "DB_FTS_MIG_k4: a failed migration is rolled back with the rollback script."),
        ("Billing", "DB_BILL_k4: totals are the sum of invoice lines."),
    ])
except sqlite3.OperationalError:
    pass
db.commit(); db.close()
PY
}
row_count() { python3 -c "import sqlite3,sys;print(sqlite3.connect(sys.argv[1]).execute('select count(*) from guidance').fetchone()[0])" "$1/data/kb.db"; }

# cfg DIR QUERY RULES [EXTRA_TOP] [SOURCE_EXTRA]
cfg() {
    local d="$1" q="$2" rules="$3" top="${4:-}" sx="${5:-}"
    python3 - "$d/govern.json" "$q" "$rules" "$top" "$sx" <<'PY'
import json, sys
path, q, rules, top, sx = sys.argv[1:6]
src = {"type": "sqlite", "path": "data/kb.db", "query": q}
if sx: src.update(json.loads(sx))
cfg = {"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
       "telemetry": {"enabled": True, "output_file": "tele.jsonl"},
       "guidance": {"enabled": True, "sources": {"kb": src}, "rules": json.loads(rules)}}
if top: cfg.update(json.loads(top))
open(path, "w").write(json.dumps(cfg, indent=1))
PY
}
# run DIR ENGINE PROG -> "rc=<n>|<stdout+stderr>"
run() {
    local d="$1" eng="$2" prog="$3"
    printf '%s\n' "$prog" > "$d/p.naab"
    local args=("$NAAB"); [ "$eng" = tw ] && args+=(--tree-walk); args+=(p.naab)
    local out; out="$(cd "$d" && "${args[@]}" 2>&1)"; local rc=$?
    printf 'rc=%s|%s' "$rc" "$out"
}
PULL='use governance
use array
main {
    let a = governance.guidance("TOPIC", "kb")
    print("N=" + string(array.length(a)))
    if array.length(a) > 0 { print("T0=" + a[0].get("text")); print("SEL=" + a[0].get("select")) }
}'
pull() { printf '%s' "${PULL//TOPIC/$1}"; }
NOOP_RULE='[{"on": "start", "from": "kb", "to": ["agent"]}]'
Q_TOPIC='SELECT heading, body FROM guidance WHERE topic = :topic ORDER BY priority DESC'

echo ""
echo "== Part 1: pull, terminal, binding, read-only, deadline, policy, secrets, live"

d="$TEST_TMP/s01"; mkdir -p "$d"; make_db "$d"; cfg "$d" "$Q_TOPIC" "$NOOP_RULE"
r1="$(run "$d" vm "$(pull migration)")"
r0="$(run "$d" vm "$(pull nonexistent_topic)")"
if [[ "$r1" == rc=0*"N=1"*"T0=DB_MIGRATION_k4: roll back"*"SEL=query"* ]] && [[ "$r0" == rc=0*"N=0"* ]]; then
    pass "S-01" "governance.guidance reads the row whose topic matches (select=query); an unknown topic returns nothing"
else
    fail "S-01" "pull from the database wrong" "${r1:0:300} / ${r0:0:120}"
fi

r3="$(run "$d" vm "$(pull "migration' OR '1'='1")")"
if [[ "$r3" == rc=0*"N=0"* ]] && [ "$(row_count "$d")" = 5 ]; then
    pass "S-03" "a quote in the topic is a bound VALUE: it matches no row (S-01 shows the topic alone does)"
else
    fail "S-03" "the topic was spliced into the SQL" "${r3:0:300}"
fi

d="$TEST_TMP/s11"; mkdir -p "$d"; make_db "$d"
cfg "$d" "SELECT heading, body FROM guidance WHERE :keywords LIKE '%' || topic || '%'" "$NOOP_RULE"
r="$(run "$d" vm "$(pull "how do I undo a broken migration safely")")"
if [[ "$r" == rc=0*"N=1"*"T0=DB_MIGRATION_k4"* ]]; then
    pass "S-11" ":keywords carries the trigger's keywords into the query (a LIKE over topics finds Migrations)"
else
    fail "S-11" ":keywords not bound as expected" "${r:0:300}"
fi

d="$TEST_TMP/s12"; mkdir -p "$d"; make_db "$d"
if python3 -c "import sqlite3,sys;sqlite3.connect(sys.argv[1]).execute('select count(*) from kb_fts')" "$d/data/kb.db" 2>/dev/null; then
    cfg "$d" "SELECT heading, body FROM kb_fts WHERE kb_fts MATCH :keywords_or ORDER BY rank LIMIT 3" "$NOOP_RULE"
    r="$(run "$d" vm "$(pull "rollback script for a failed migration")")"
    if [[ "$r" == rc=0*"T0=DB_FTS_MIG_k4"* ]]; then
        pass "S-12" "full-text search (FTS5 MATCH :keywords_or ORDER BY rank) puts the migration entry first"
    elif [[ "$r" == *"no such module"* ]] || [[ "$r" == *"query refused"* && "$r" == *fts5* ]]; then
        skip "S-12" "the engine's SQLite has no FTS5: ${r:0:160}"
    else
        fail "S-12" "FTS5 ranking did not surface the migration entry" "${r:0:300}"
    fi
else
    skip "S-12" "python's SQLite could not build the FTS5 fixture"
fi

# Terminal block from the database, both engines; control: a non-matching rule.
PLACEHOLDER_PROG='fn work() {
    // TODO: finish this later
    return 1
}
main {
    print(work())
}'
for eng in vm tw; do
    d="$TEST_TMP/s02_$eng"; mkdir -p "$d"; make_db "$d"
    cfg "$d" "SELECT heading, body FROM guidance WHERE topic = :arg" \
        '[{"on": "violation:code_quality.no_placeholders", "from": "kb", "to": ["terminal"]}]' \
        '{"code_quality": {"no_placeholders": {"enabled": true, "level": "hard"}}}'
    r="$(run "$d" "$eng" "$PLACEHOLDER_PROG")"
    dc="$TEST_TMP/s02c_$eng"; mkdir -p "$dc"; make_db "$dc"
    cfg "$dc" "SELECT heading, body FROM guidance WHERE topic = :arg" \
        '[{"on": "violation:code_quality.no_secrets", "from": "kb", "to": ["terminal"]}]' \
        '{"code_quality": {"no_placeholders": {"enabled": true, "level": "hard"}}}'
    rc_="$(run "$dc" "$eng" "$PLACEHOLDER_PROG")"
    if [[ "$rc_" != rc=3*Placeholder* ]]; then
        fail "S-02/$eng" "control: the placeholder must be blocked" "${rc_:0:200}"
    elif [[ "$rc_" == *DB_STUBS_k4* ]]; then
        fail "S-02/$eng" "a rule for another check attached the database row"
    elif [[ "$r" == rc=3* ]] && [[ "$r" == *"Project guidance (data/kb.db > Stubs policy):"* ]] && [[ "$r" == *DB_STUBS_k4* ]]; then
        pass "S-02/$eng" "the HARD block ends with the database row for that rule (:arg = the rule name)"
    else
        fail "S-02/$eng" "the block message lacks the database row" "${r:0:400}"
    fi
done

# Read-only: a write, a second statement and ATTACH are refused; the table is
# unchanged. S-01 is the control that the same source reads fine.
d="$TEST_TMP/s05"; mkdir -p "$d"; make_db "$d"
ok=1; detail=""
for q in "DELETE FROM guidance" \
         "SELECT heading, body FROM guidance; DELETE FROM guidance" \
         "ATTACH 'data/kb.db' AS other" \
         "PRAGMA table_info(guidance)"; do
    cfg "$d" "$q" "$NOOP_RULE"
    r="$(run "$d" vm "$(pull migration)")"
    # The warning prints when the query runs, BEFORE the program's own output:
    # test each part on its own, not their order.
    if [[ "$r" == rc=0* ]] && [[ "$r" == *"N=0"* ]] && [[ "$r" == *"query refused"* ]]; then :; else
        ok=0; detail="$detail | [$q] ${r:0:200}"; fi
done
if [ "$ok" = 1 ] && [ "$(row_count "$d")" = 5 ]; then
    pass "S-05" "a write, a second statement, ATTACH and a PRAGMA statement are each refused with a warning; the table keeps its 5 rows"
else
    fail "S-05" "a non-read-only query was not refused" "$detail rows=$(row_count "$d")"
fi

# Deadline: a runaway query is interrupted; the run continues quickly.
d="$TEST_TMP/s07"; mkdir -p "$d"; make_db "$d"
cfg "$d" "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM c LIMIT 2000000000) SELECT 'h', count(*) FROM c" \
    "$NOOP_RULE" "" '{"timeout_ms": 50}'
# The outer 30 s bound only matters if the deadline is broken: without it the
# query counts to two billion and this arm would wait minutes to fail.
printf '%s\n' "$(pull anything)" > "$d/p.naab"
t0=$(date +%s%N); r="$(cd "$d" && timeout 30s "$NAAB" p.naab 2>&1)"; rcq=$?; r="rc=$rcq|$r"; t1=$(date +%s%N)
ms=$(( (t1 - t0) / 1000000 ))
if [[ "$r" == rc=0*"N=0"* ]] && [[ "$r" == *"was interrupted"* ]] && [ "$ms" -lt 10000 ]; then
    pass "S-07" "a runaway query is interrupted at its 50 ms deadline; the program finished in ${ms} ms (S-01 is the fast control)"
else
    fail "S-07" "the deadline did not hold" "${ms}ms ${r:0:250}"
fi

# Unknown parameter: reported and bound to NULL.
d="$TEST_TMP/s10"; mkdir -p "$d"; make_db "$d"
cfg "$d" "SELECT heading, body FROM guidance WHERE topic = :nonsense" "$NOOP_RULE"
r="$(run "$d" vm "$(pull migration)")"
if [[ "$r" == rc=0*"N=0"* ]] && [[ "$r" == *':nonsense'*'bound to NULL'* ]]; then
    pass "S-10" "an unknown query parameter is reported and bound to NULL"
else
    fail "S-10" "unknown parameter not reported" "${r:0:300}"
fi

# Policy: a database under a blocked path is not read.
d="$TEST_TMP/s06"; mkdir -p "$d"; make_db "$d"
cfg "$d" "$Q_TOPIC" "$NOOP_RULE" '{"capabilities": {"filesystem": {"blocked_paths": ["data"]}}}'
r="$(run "$d" vm "$(pull migration)")"
if [[ "$r" == rc=0*"N=0"* ]] && [[ "$r" == *"is not read: the filesystem policy"* ]]; then
    pass "S-06" "a database under a blocked path is not read (S-01 is the readable control)"
else
    fail "S-06" "a blocked database was read" "${r:0:300}"
fi

# Secrets: a row holding a secret is withheld under no_secrets; delivered without it.
d="$TEST_TMP/s08"; mkdir -p "$d"; make_db "$d"
cfg "$d" "$Q_TOPIC" "$NOOP_RULE" '{"code_quality": {"no_secrets": {"enabled": true, "level": "hard"}}}'
r="$(run "$d" vm "$(pull secret)")"
dc="$TEST_TMP/s08b"; mkdir -p "$dc"; make_db "$dc"; cfg "$dc" "$Q_TOPIC" "$NOOP_RULE"
rc_="$(run "$dc" vm "$(pull secret)")"
if [[ "$rc_" != rc=0*"T0=DB_SECRET_k4"* ]]; then
    fail "S-08b" "control: without the secret scan the row must be returned" "${rc_:0:200}"
else
    pass "S-08b" "control: with no_secrets off the secret row is returned (the fixture reaches it)"
fi
if [[ "$r" == rc=0*"N=0"* ]] && [[ "$r" != *"$FAKE_KEY"* ]] && grep -q '"GUIDANCE_WITHHELD".*"secret"\|"secret".*"GUIDANCE_WITHHELD"' "$d/tele.jsonl"; then
    pass "S-08" "a database row holding a secret is withheld (telemetry reason secret); the run is not blocked"
else
    fail "S-08" "secret row not withheld" "${r:0:300}"
fi

# Write protection: the database and its -wal; control: another file.
for eng in vm tw; do
    d="$TEST_TMP/s04_$eng"; mkdir -p "$d"; make_db "$d"; cfg "$d" "$Q_TOPIC" "$NOOP_RULE"
    rdb="$(run "$d" "$eng" 'use file
main {
    file.write("data/kb.db", "OVERWRITTEN")
    print("WROTE")
}')"
    rwal="$(run "$d" "$eng" 'use file
main {
    file.write("data/kb.db-wal", "crafted")
    print("WROTE")
}')"
    rctl="$(run "$d" "$eng" 'use file
main {
    file.write("data/notes.txt", "ordinary")
    print("WROTE")
}')"
    if [[ "$rctl" != rc=0*WROTE* ]]; then
        fail "S-04/$eng" "control: an ordinary write must succeed" "${rctl:0:200}"
    elif [[ "$rdb" == rc=3* ]] && [[ "$rwal" == rc=3* ]] && [[ "$rdb$rwal" != *WROTE* ]] \
         && [ ! -f "$d/data/kb.db-wal" ] && [ "$(row_count "$d")" = 5 ]; then
        pass "S-04/$eng" "the program cannot write the database or its -wal sidecar; another file writes fine"
    else
        fail "S-04/$eng" "the database or its sidecar was writable" "${rdb:0:150} / ${rwal:0:150}"
    fi
done

# Live: a row changed by an operator mid-run is what the next lookup returns.
source "$SCRIPT_DIR/../helpers/config_swap.sh"
d="$TEST_TMP/s09"; mkdir -p "$d"; make_db "$d"; cfg "$d" "$Q_TOPIC" "$NOOP_RULE"
mkdir -p "$TEST_TMP/s09_new"; make_db "$TEST_TMP/s09_new" v2
start_swap_operator "$d"
r="$(run "$d" vm "use governance
use file
use time
main {
    let a = governance.guidance(\"migration\", \"kb\")
    print(\"A=\" + a[0].get(\"text\"))
    file.write(\".swap_req\", \"$TEST_TMP/s09_new/data/kb.db\t$d/data/kb.db\n\")
    file.write(\".swap_req.ready\", \"\")
    let waited = 0
    while !file.exists(\".swap_done\") && waited < 200 { time.sleep(0.05); waited = waited + 1 }
    if file.exists(\".swap_done\") { print(\"SWAPPED\") }
    let b = governance.guidance(\"migration\", \"kb\")
    print(\"B=\" + b[0].get(\"text\"))
}")"
# The arm measures the engine only if the operator's copy landed BEFORE the
# second lookup: the program saw the done marker, and the database on disk is
# the replacement. A swap that did not happen (the operator does not run on
# Windows -- test_capability_list_ratchet.sh R-00 records the same) is
# UNMEASURABLE, not a stale read.
if [[ "$r" != *"B="* ]]; then
    skip "S-09" "the program did not reach the second lookup: ${r:0:200}"
elif [[ "$r" != *SWAPPED* ]] || ! cmp -s "$TEST_TMP/s09_new/data/kb.db" "$d/data/kb.db"; then
    skip "S-09" "the external swap did not land before the second lookup, so a stale read cannot be told from no swap"
elif [[ "$r" == *"A=DB_MIGRATION_k4: roll back"* ]] && [[ "$r" == *"B=DB_MIGRATION_k4_EDITED"* ]]; then
    pass "S-09" "the database changed mid-run; the next lookup returned the new row with no reload"
else
    fail "S-09" "the live change was not read" "${r:0:300}"
fi

# ---------------------------------------------------------------------------
# Part 2 — agent channel through the stub
# ---------------------------------------------------------------------------
echo ""
echo "== Part 2: agent channel (stub)"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;; *) WINDOWS=0 ;; esac
[ -n "${WINDIR:-}" ] && WINDOWS=1
if [ "$WINDOWS" = 1 ]; then
    skip "S-20" "stub-backed arms are not run on Windows (stub_platform.sh)"
else
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
export FAKE_KEY_GUIDE="fake-key-guidance"
FIXTURE="$TEST_TMP/fixture.json"
cat > "$FIXTURE" <<'EOF'
{"responses": [{"content": "The billing total is computed from the invoice lines.", "input_tokens": 12, "output_tokens": 24}]}
EOF
d="$TEST_TMP/s20"; mkdir -p "$d"; make_db "$d"
if start_stub "$FIXTURE" "$d" >/dev/null 2>&1; then
    STUB_STARTED=1
    cfg "$d" "SELECT heading, body FROM guidance WHERE topic = :arg" \
        '[{"on": "signal:response_repetition", "from": "kb", "to": ["agent"]}]' \
        "{\"behavioral_sequences\": {\"enabled\": true}, \"advisory_escalation\": {\"enabled\": false},
          \"context_drift\": {\"enabled\": true, \"level\": \"advisory\", \"check_interval_turns\": 1, \"reality_checkpoint\": {\"enabled\": false}},
          \"agents\": {\"worker\": {\"provider\": \"gemini\", \"model\": \"stub-model\", \"api_base\": \"http://127.0.0.1:$STUB_PORT\",
            \"api_key_env\": \"FAKE_KEY_GUIDE\", \"max_tokens\": 200, \"max_turns\": 40,
            \"system_prompt\": \"You maintain the billing service.\"}}}"
    printf 'use agent\nmain {\n    let h = agent.create("worker")\n    let i = 0\n    while i < 4 { i = i + 1; let r = agent.send(h, "Explain the invoice total.") }\n    print("RUN_DONE")\n}\n' > "$d/p.naab"
    out="$( (cd "$d" && timeout 120s "$NAAB" p.naab 2>"$d/err.txt") )"
    stop_stub; STUB_STARTED=0
    has() { case "$(cat "$d/req_$1.json" 2>/dev/null)" in *"$2"*) return 0;; *) return 1;; esac; }
    lm="$(python3 - "$d/tele.jsonl" <<'PY'
import json, sys
ev = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8", errors="replace") if '"GUIDANCE_DELIVERED"' in l]
ok = [e for e in ev if e.get("select") == "query" and e.get("text", "").startswith("DB_REPEAT_k4")]
print("%s %s" % (len(ok), ok[0].get("lookup_ms") if ok else "-"))
PY
)"
    if [[ "$out" != *RUN_DONE* ]]; then
        fail "S-20" "run did not complete" "$out / $(tail -3 "$d/err.txt")"
    elif ! has 1 DB_REPEAT_k4 && ! has 2 DB_REPEAT_k4 && has 3 DB_REPEAT_k4 && [[ "$lm" == 1* ]] \
         && grep -q "Vary your answers" "$d/err.txt"; then
        pass "S-20" "a paid signal fetched the database row into the next request (:arg = the signal); telemetry carries the row as text; lookup ${lm#* } ms (observed, this machine); listed at the end of the run"
    else
        fail "S-20" "database guidance did not reach the agent" "req3=$(has 3 DB_REPEAT_k4 && echo y || echo n) telemetry=$lm"
    fi
else
    fail "S-20" "stub failed to start"
fi
fi  # not Windows

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT unmeasurable"
[ "$FAIL_COUNT" -eq 0 ]
