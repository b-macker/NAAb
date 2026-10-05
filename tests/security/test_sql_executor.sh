#!/usr/bin/env bash
# ============================================================
# test_sql_executor.sh -- <<sql>> blocks: results, parameters, and containment
#
# The SQL executor runs in-process on SQLite, in one in-memory database per
# run (include/naab/sql_executor.h). These arms pin its behaviour on both
# engines, and above all its containment: a <<sql>> block must have no reach
# outside that database.
#
#   SQ-00  CONTROL: a SELECT returns its row (the executor runs at all)
#   SQ-01  values map by type: int, string, float, null
#   SQ-02  state carries across blocks, and <<sql>> / <<sqlite>> share ONE database
#   SQ-03  a bound variable binds as a parameter; an injection string stays data
#   SQ-04  a parameter with no bound variable is an error, not NULL
#   SQ-05  ATTACH (would open/create a file) is refused, and no file appears
#   SQ-06  VACUUM INTO (would write a file) is refused, and no file appears
#   SQ-07  load_extension (would load native code) is refused
#   SQ-08  --timeout stops a runaway recursive query
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== <<sql>> blocks: results, parameters, containment ==="

if [ ! -x "$NAAB" ]; then
    for id in SQ-00 SQ-01 SQ-02 SQ-03 SQ-04 SQ-05 SQ-06 SQ-07 SQ-08; do skip "$id" "naab-lang not built (UNMEASURABLE)"; done
    echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-sql.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust   # the unsigned govern.json below must not meet a populated trust store
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT
echo '{ "version": "4.0", "mode": "off" }' > "$W/govern.json"

# both <file> <expected substring> -> "" when both engines print it, else a reason
both() {
    local why="" out flag
    for flag in "" "--tree-walk"; do
        out=$(cd "$W" && timeout 30 "$NAAB" "$1" $flag 2>&1)
        [[ "$out" == *"$2"* ]] || why="$why ${flag:-vm}:[$(printf '%s' "$out" | grep -v Loaded | tail -1 | cut -c1-90)]"
    done
    printf '%s' "$why"
}

# --- SQ-00 / SQ-01 -------------------------------------------------------------------
cat > "$W/s01.naab" <<'EOF'
main {
    let r = <<sql
SELECT 7 AS i, 'txt' AS s, 2.5 AS f, NULL AS n
>>
    let row = r[0]
    print("ROW:" + string(row["i"] + 1) + "|" + row["s"] + "|" + string(row["f"] * 2) + "|" + string(row["n"] == null))
}
EOF
w=$(both s01.naab "ROW:")
if [ -z "$w" ]; then ok "SQ-00" "CONTROL: a SELECT returns its row on both engines"
else bad "SQ-00" "the SQL executor did not return a row -- every arm below is unmeasured" "$w"; fi
w=$(both s01.naab "ROW:8|txt|5|true")
if [ -z "$w" ]; then ok "SQ-01" "int, string, float and null map to NAAb values"
else bad "SQ-01" "a value did not map by type" "$w"; fi

# --- SQ-02 ------------------------------------------------------------------------------
cat > "$W/s02.naab" <<'EOF'
main {
    let a = <<sql
CREATE TABLE t (id INTEGER, name TEXT);
INSERT INTO t VALUES (1, 'ann'), (2, 'bob');
>>
    let b = <<sqlite
SELECT count(*) AS c FROM t
>>
    print("SHARED:" + string(b[0]["c"]))
}
EOF
w=$(both s02.naab "SHARED:2")
if [ -z "$w" ]; then ok "SQ-02" "a table created in one block is visible to the next, across the sql/sqlite tags"
else bad "SQ-02" "state did not carry across blocks or tags" "$w"; fi

# --- SQ-03 / SQ-04 ----------------------------------------------------------------------
# One block, both variables, inline data -- written while the tree-walker
# could not see a variable declared AFTER an earlier polyglot block in the same
# function (its polyglot grouping ran the second block before the `let`;
# fixed, tests/robustness/test_polyglot_group_order.sh). A single block still
# keeps this arm about parameter binding alone.
cat > "$W/s03.naab" <<'EOF'
main {
    let who = "bob"
    let evil = "x' OR '1'='1"
    let r = <<sql[who, evil]
WITH u(name) AS (VALUES ('ann'), ('bob'))
SELECT (SELECT count(*) FROM u WHERE name = :who) AS hit,
       (SELECT count(*) FROM u WHERE name = :evil) AS miss
>>
    print("BIND:" + string(r[0]["hit"]) + "/" + string(r[0]["miss"]))
}
EOF
w=$(both s03.naab "BIND:1/0")
if [ -z "$w" ]; then ok "SQ-03" "bound variables bind as parameters; the injection string matched nothing"
else bad "SQ-03" "binding or injection behaviour wrong (want BIND:1/0)" "$w"; fi
cat > "$W/s04.naab" <<'EOF'
main {
    let r = <<sql
SELECT :nobody AS v
>>
    print("UNBOUND_RAN")
}
EOF
w=$(both s04.naab "has no bound variable")
if [ -z "$w" ]; then ok "SQ-04" "a parameter with no bound variable is refused"
else bad "SQ-04" "an unbound parameter was not refused" "$w"; fi

# --- SQ-05..SQ-07: containment ----------------------------------------------------------
refused() {  # <id> <sql> <file it must not create, or ""> <description>
    printf 'main {\n    let r = <<sql\n%s\n>>\n    print("ESCAPED")\n}\n' "$2" > "$W/c.naab"
    local why="" out flag
    for flag in "" "--tree-walk"; do
        out=$(cd "$W" && timeout 30 "$NAAB" c.naab $flag 2>&1)
        case "$out" in *ESCAPED*) why="$why ${flag:-vm}:ran" ;; esac
        case "$out" in *"not authorized"*|*"denied"*) ;; *) why="$why ${flag:-vm}:no-refusal" ;; esac
    done
    [ -n "$3" ] && [ -e "$3" ] && why="$why file-created"
    if [ -z "$why" ]; then ok "$1" "$4"; else bad "$1" "$4" "$why"; fi
}
refused SQ-05 "ATTACH DATABASE '$W/attached.db' AS x" "$W/attached.db" "ATTACH is refused and creates no file"
refused SQ-06 "VACUUM INTO '$W/vacuumed.db'" "$W/vacuumed.db" "VACUUM INTO is refused and writes no file"
refused SQ-07 "SELECT load_extension('$W/nonexistent')" "" "load_extension is refused"

# --- SQ-08 ------------------------------------------------------------------------------
cat > "$W/s08.naab" <<'EOF'
main {
    let r = <<sql
WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) SELECT count(*) AS n FROM c
>>
    print("NEVER")
}
EOF
start=$(date +%s)
out=$(cd "$W" && timeout 30 "$NAAB" s08.naab --timeout 2 2>&1); rc=$?
el=$(( $(date +%s) - start ))
if [ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] && [ "$el" -le 8 ] && [[ "$out" != *NEVER* ]] && [[ "$out" == *imeout* ]]; then
    ok "SQ-08" "--timeout 2 stopped a runaway recursive query (${el}s)"
else
    bad "SQ-08" "the runaway query was not stopped by --timeout" "rc=$rc ${el}s: $(printf '%s' "$out" | tail -1 | cut -c1-100)"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
