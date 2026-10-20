#!/usr/bin/env bash
# ============================================================
# test_declaration_source.sh -- the per-function checks read the program the
# engine PARSED (docs/open-investigations.md A45, and three defects found
# while fixing it)
#
# The per-function checks (placeholders, contracts, oversimplification,
# complexity floor, intent) and the tree-walker's whole-source check get their
# text by re-reading the engine's current file from disk and counting braces
# from the AST line. Four hand-written copies did that, and a fix reached one
# copy at a time:
#
#   247e229d (EVA-8) put the re-read in the tree-walker; d4f3fa89 copied it to
#   the VM compiler for function bodies (not main); 11146ceb taught only the
#   VM to skip braces in strings and comments; 7d4fda0f gave only the VM a
#   backward search for "fn NAME(" -- which, when that exact text is absent,
#   starts five lines early anyway. cc975e33 (Finding C) named every REST
#   request "<cwd>/api-request.naab" so config discovery would run, a file that
#   does not exist. None of them records a decision to skip any check.
#
#   DS-R*  REST /execute (the tree-walker, in the server process)
#     R-00     CONTROL: a clean program runs through REST
#     R-01..05 a placeholder, an incomplete-logic sentinel, an oversimplified
#              stub, a must_call breach and a placeholder in main{} are each
#              refused (exit_code 3) -- each ran, exit 0, before
#     R-01c..05c CONTROL: the same file is refused by `naab-lang run
#              --tree-walk` with the rule name, so the fixture trips the check
#     R-06     a secret is refused. REST used to call the secret and PII
#              checks itself; that call is gone and visit(Program) runs them on
#              the submitted text, so this arm pins the removal (it goes red
#              with the parsed-text record removed)
#     R-07     a TODO file planted as api-request.naab in the server's
#              directory does not refuse a clean submission (it did)
#     R-08     a clean file planted there does not let a TODO submission run
#              (it did)
#     R-09     the server is still up
#   DS-V*  VM: the declaration is found by its line, not by exact text
#     V-01     `fn scale (x)` with a TODO, after another function, is refused
#              on the VM (the checks read the previous function, exit 0)
#     V-01c    CONTROL: `fn scale(x)` is refused on the VM
#     V-01t    the tree-walker refuses it too (it always did, starting at the
#              AST line; it now shares the VM's search)
#     V-02     a must_call contract that `fn needs_sqrt (x)` SATISFIES runs on
#              the VM -- the other direction of the same defect: the checks
#              read a body that does not call math.sqrt and refused it
#   DS-T*  tree-walker: braces inside strings and comments do not count
#     T-01     `let s = "}"` before a TODO does not hide it (exit 0 before)
#     T-02     `// }` before a TODO does not hide it (exit 0 before)
#     T-01c    the VM refuses it too (it always did; both engines share the counter now)
#     T-01n    CONTROL: without the brace the tree-walker refuses it
#   DS-M*  VM: the main block's body is checked
#     M-01     a TODO in main{} is refused on the VM (exit 0 before; the
#              tree-walker always checked main, 247e229d)
#     M-01t    CONTROL: the tree-walker refuses it
#     M-01c    CONTROL: a clean main{} runs on the VM
#   DS-L*  the checks' location does not outlive them (VM; the tree-walker
#          emits no BSD events for stdlib calls, open-investigations B11)
#     L-01     a runtime rule that fires twice after an entry-file function and
#              main{} were checked still gives two rows under
#              deduplicate_checks. The entry-file check left its location set
#              (only #300's module path was scoped), so every runtime row after
#              it looked like a located static one and was collapsed -- before
#              this change only in programs with functions, then in every
#              program once main{} was checked (test_dedup_runtime.sh DC-01)
#     L-01c    CONTROL: with dedup off the rule gives the same two rows (four
#              reads, two non-overlapping matches), so L-01's two is the rule
#              firing twice and a collapse shows as one
#   DS-W*  a refusal during VM compilation still writes the reports
#     W-01     a TODO in a function body, refused while the VM compiles, leaves
#              the --governance-sarif report written. The VM's write-reports-
#              then-rethrow wrapped only execute(), so a block from the
#              per-function checks (which run while compiling) wrote none --
#              already true for function bodies; checking main{} made it the
#              case for tests/cli/test_report_formats.sh's fixture
#     W-01t    CONTROL: the tree-walker, which runs these checks while
#              executing, writes it for the same file
#
# Against an interpreter that does nothing every refusal arm fails, and the
# REST arms report UNMEASURABLE when the server cannot be started.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

PASS=0; FAIL=0; SKIP=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }

echo "=== The per-function checks read the parsed program ==="

if [ ! -x "$NAAB" ]; then
    echo "FAIL: naab-lang not built at $NAAB (UNMEASURABLE, not a pass)"
    exit 1
fi

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-declsrc.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
API_PID=""
cleanup() {
    [ -n "$API_PID" ] && { kill "$API_PID" 2>/dev/null; wait "$API_PID" 2>/dev/null; }
    rm -rf "$W"
    teardown_isolated_trust
}
trap cleanup EXIT

CONFIG='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "code_quality": { "no_secrets": { "enabled": true, "level": "hard" },
    "no_placeholders": { "enabled": true, "level": "hard" },
    "no_incomplete_logic": { "enabled": true, "level": "hard" },
    "no_oversimplification": { "enabled": true, "level": "hard" } },
  "contracts": { "level": "hard", "functions": { "needs_sqrt": { "must_call": ["math.sqrt"] } } } }'
PH_CONFIG='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "code_quality": { "no_placeholders": { "enabled": true, "level": "hard" } } }'
CT_CONFIG='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "contracts": { "level": "hard", "functions": { "needs_sqrt": { "must_call": ["math.sqrt"] } } } }'

P_CLEAN='fn compute(x) {
    let y = x * 2
    return y + 1
}
main {
    print("RAN_" + string(compute(3)))
}'
P_PLACEHOLDER='fn compute(x) {
    // TODO: handle negative input
    let y = x * 2
    return y + 1
}
main {
    print("RAN_" + string(compute(3)))
}'
# The sentinel check keys on a validator's NAME (test_module_source_checks.sh).
P_INCOMPLETE='fn validate_id(id) {
    if id == null {
        return "invalid-id"
    }
    return id
}
main {
    print("RAN_" + validate_id("a"))
}'
P_OVERSIMPLE='fn compute(x) {
    return []
}
main {
    print("RAN_" + string(compute(3)))
}'
P_CONTRACT='use math
fn needs_sqrt(x) {
    let y = x * 2
    return y + 1
}
main {
    print("RAN_" + string(needs_sqrt(4)))
}'
P_MAINPH='main {
    // TODO: finish this
    let y = 2
    print("RAN_" + string(y))
}'
P_SECRET='fn compute(x) {
    let k = "AKIAIOSFODNN7EXAMPLE"
    return k
}
main {
    print("RAN_" + compute(3))
}'

ran() { case "$OUT" in *RAN_*) return 0 ;; *) return 1 ;; esac; }
# cli <dir> <program> <engine flag>: run it as a file; sets RC and OUT
cli() {
    printf '%s\n' "$2" > "$W/$1/main.naab"
    OUT="$(cd "$W/$1" && timeout 60 "$NAAB" run $3 main.naab 2>&1)"; RC=$?
}
refused_by() { [ "$RC" -eq 3 ] && ! ran && case "$OUT" in *"$1"*) true ;; *) false ;; esac; }
newcase() { rm -rf "$W/$1"; mkdir -p "$W/$1"; printf '%s\n' "$2" > "$W/$1/govern.json"; }

# ------------------------------------------------------------
echo ""
echo "--- DS-R: REST /execute ---"
SRV="$W/server"; mkdir -p "$SRV"; printf '%s\n' "$CONFIG" > "$SRV/govern.json"
rest_ok=1
if ! command -v curl >/dev/null 2>&1; then
    rest_ok=0; why="curl not available"
else
    PORT=$((19000 + RANDOM % 900))
    ( cd "$SRV" && exec "$NAAB" api "$PORT" --api-key "k-ds" >"$W/server.log" 2>&1 ) &
    API_PID=$!
    up=0
    for _ in $(seq 1 100); do
        if curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then up=1; break; fi
        sleep 0.1
    done
    [ "$up" -eq 1 ] || { rest_ok=0; why="API server did not start: $(head -c 200 "$W/server.log")"; }
fi

# execute <source>: sets REXIT (exit_code, or "?") and RRAN (yes/no)
execute() {
    local resp
    resp="$(python3 -c 'import json,sys; print(json.dumps({"code": sys.stdin.read()}))' <<<"$1" |
        curl -s -X POST "http://127.0.0.1:$PORT/api/v1/execute" \
            -H "Authorization: Bearer k-ds" -H "Content-Type: application/json" \
            --data-binary @- 2>&1)"
    # Bytes, not print(): a native Windows python3 (MSYS2) writes print()'s
    # newline as CRLF, and `read` keeps the CR on the last field -- every
    # REST arm read "yes\r" there and failed (build-windows, 0b7bd6d5).
    read -r REXIT RRAN < <(python3 -c 'import json,sys
try:
    d = json.loads(sys.stdin.read())
    out = "%s %s\n" % (d.get("exit_code", "?"), "yes" if "RAN_" in (d.get("output") or "") else "no")
except Exception:
    out = "? no\n"
sys.stdout.buffer.write(out.encode())' <<<"$resp")
    REXIT="${REXIT%$'\r'}"; RRAN="${RRAN%$'\r'}"
    RRESP="$resp"
}

rest_pair() {  # <id> <rule> <program> <label>
    local id="$1" rule="$2" prog="$3" label="$4"
    newcase "$id-cli" "$CONFIG"; cli "$id-cli" "$prog" --tree-walk
    if refused_by "$rule"; then
        pass "${id}c" "CONTROL: run --tree-walk refuses $label with $rule"
    else
        fail "${id}c" "CONTROL: the fixture does not trip $rule on the CLI" "exit=$RC"
    fi
    if [ "$rest_ok" -ne 1 ]; then skip "$id" "$why"; return; fi
    execute "$prog"
    if [ "$REXIT" = "3" ] && [ "$RRAN" = "no" ]; then
        pass "$id" "REST refuses $label"
    else
        fail "$id" "REST ran $label" "exit_code=$REXIT ran=$RRAN"
    fi
}

if [ "$rest_ok" -eq 1 ]; then
    execute "$P_CLEAN"
    if [ "$REXIT" = "0" ] && [ "$RRAN" = "yes" ]; then
        pass R-00 "CONTROL: a clean program runs through REST"
    else
        fail R-00 "a clean program did not run through REST" "exit_code=$REXIT $(head -c 200 <<<"$RRESP")"
    fi
else
    skip R-00 "$why"
fi
rest_pair R-01 code_quality.no_placeholders "$P_PLACEHOLDER" "a TODO placeholder"
rest_pair R-02 code_quality.no_incomplete_logic "$P_INCOMPLETE" "an incomplete-logic sentinel"
rest_pair R-03 code_quality.no_oversimplification "$P_OVERSIMPLE" "an oversimplified stub"
rest_pair R-04 contracts "$P_CONTRACT" "a must_call breach"
rest_pair R-05 code_quality.no_placeholders "$P_MAINPH" "a TODO in main{}"

if [ "$rest_ok" -eq 1 ]; then
    execute "$P_SECRET"
    if [ "$REXIT" = "3" ] && [ "$RRAN" = "no" ]; then
        pass R-06 "REST refuses a secret (whole-source check on the submitted text)"
    else
        fail R-06 "REST ran a secret" "exit_code=$REXIT"
    fi

    printf '%s\n' "$P_PLACEHOLDER" > "$SRV/api-request.naab"
    execute "$P_CLEAN"
    if [ "$REXIT" = "0" ] && [ "$RRAN" = "yes" ]; then
        pass R-07 "a TODO file planted as api-request.naab does not refuse a clean submission"
    else
        fail R-07 "the planted file decided the verdict on a clean submission" "exit_code=$REXIT ran=$RRAN"
    fi
    printf '%s\n' "$P_CLEAN" > "$SRV/api-request.naab"
    execute "$P_PLACEHOLDER"
    if [ "$REXIT" = "3" ] && [ "$RRAN" = "no" ]; then
        pass R-08 "a clean planted api-request.naab does not let a TODO submission run"
    else
        fail R-08 "the planted file let a TODO submission run" "exit_code=$REXIT ran=$RRAN"
    fi
    rm -f "$SRV/api-request.naab"

    if curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && kill -0 "$API_PID" 2>/dev/null; then
        pass R-09 "the server is still up"
    else
        fail R-09 "the server died" "$(tail -3 "$W/server.log")"
    fi
else
    skip R-06 "$why"; skip R-07 "$why"; skip R-08 "$why"; skip R-09 "$why"
fi

# ------------------------------------------------------------
echo ""
echo "--- DS-V: VM finds the declaration by its line ---"
# The function is NOT called "stub": the placeholder patterns match that word
# in main{}'s call to it, and V-01 then passes on main{} instead of on the
# TODO (the A47 revert check caught it).
P_SPACE='fn helper() {
    return 1
}
fn scale (x) {
    // TODO: implement
    return x
}
main {
    print("RAN_" + string(scale(helper())))
}'
P_NOSPACE="${P_SPACE/fn scale (x)/fn scale(x)}"

newcase V-01 "$PH_CONFIG"; cli V-01 "$P_SPACE" ""
if refused_by code_quality.no_placeholders; then
    pass V-01 "VM refuses a TODO in \`fn scale (x)\`"
else
    fail V-01 "VM ran a TODO in \`fn scale (x)\` -- the checks read another function" "exit=$RC"
fi
newcase V-01c "$PH_CONFIG"; cli V-01c "$P_NOSPACE" ""
if refused_by code_quality.no_placeholders; then
    pass V-01c "CONTROL: VM refuses the TODO in \`fn scale(x)\`"
else
    fail V-01c "CONTROL: VM ran the TODO in \`fn scale(x)\`" "exit=$RC"
fi
newcase V-01t "$PH_CONFIG"; cli V-01t "$P_SPACE" --tree-walk
if refused_by code_quality.no_placeholders; then
    pass V-01t "the tree-walker refuses V-01's file too"
else
    fail V-01t "the tree-walker ran V-01's file" "exit=$RC"
fi

P_SQRT_SPACE='use math
fn helper() {
    return 1
}
fn needs_sqrt (x) {
    let r = math.sqrt(x)
    return r + 1
}
main {
    print("RAN_" + string(needs_sqrt(4) + helper()))
}'
newcase V-02 "$CT_CONFIG"; cli V-02 "$P_SQRT_SPACE" ""
if [ "$RC" -eq 0 ] && ran; then
    pass V-02 "VM runs \`fn needs_sqrt (x)\`, which satisfies its must_call contract"
else
    fail V-02 "VM refused a function that satisfies its contract -- it checked another body" "exit=$RC $(grep -m1 -i contract <<<"$OUT")"
fi

# ------------------------------------------------------------
echo ""
echo "--- DS-T: tree-walker skips braces in strings and comments ---"
P_STRBRACE='fn compute(x) {
    let s = "}"
    // TODO: implement
    return x
}
main {
    print("RAN_" + string(compute(1)))
}'
P_COMMENTBRACE='fn compute(x) {
    // a closing brace } in a comment
    // TODO: implement
    return x
}
main {
    print("RAN_" + string(compute(1)))
}'
P_NOBRACE="${P_STRBRACE/let s = \"\}\"/let s = \"x\"}"

newcase T-01 "$PH_CONFIG"; cli T-01 "$P_STRBRACE" --tree-walk
if refused_by code_quality.no_placeholders; then
    pass T-01 "tree-walker: a \"}\" string does not hide the TODO after it"
else
    fail T-01 "tree-walker: a \"}\" string hid the TODO after it" "exit=$RC"
fi
newcase T-02 "$PH_CONFIG"; cli T-02 "$P_COMMENTBRACE" --tree-walk
if refused_by code_quality.no_placeholders; then
    pass T-02 "tree-walker: a } in a comment does not hide the TODO after it"
else
    fail T-02 "tree-walker: a } in a comment hid the TODO after it" "exit=$RC"
fi
newcase T-01c "$PH_CONFIG"; cli T-01c "$P_STRBRACE" ""
if refused_by code_quality.no_placeholders; then
    pass T-01c "the VM refuses T-01's file too"
else
    fail T-01c "the VM ran T-01's file" "exit=$RC"
fi
newcase T-01n "$PH_CONFIG"; cli T-01n "$P_NOBRACE" --tree-walk
if refused_by code_quality.no_placeholders; then
    pass T-01n "CONTROL: without the brace the tree-walker refuses it"
else
    fail T-01n "CONTROL: the tree-walker ran the TODO without any brace" "exit=$RC"
fi

# ------------------------------------------------------------
echo ""
echo "--- DS-M: VM checks the main block ---"
newcase M-01 "$PH_CONFIG"; cli M-01 "$P_MAINPH" ""
if refused_by code_quality.no_placeholders; then
    pass M-01 "VM refuses a TODO in main{}"
else
    fail M-01 "VM ran a TODO in main{}" "exit=$RC"
fi
newcase M-01t "$PH_CONFIG"; cli M-01t "$P_MAINPH" --tree-walk
if refused_by code_quality.no_placeholders; then
    pass M-01t "CONTROL: the tree-walker refuses it"
else
    fail M-01t "CONTROL: the tree-walker ran it" "exit=$RC"
fi
newcase M-01c "$PH_CONFIG"; cli M-01c "$P_CLEAN" ""
if [ "$RC" -eq 0 ] && ran; then
    pass M-01c "CONTROL: a clean main{} runs on the VM"
else
    fail M-01c "CONTROL: a clean main{} was refused on the VM" "exit=$RC $(head -c 200 <<<"$OUT")"
fi

# ------------------------------------------------------------
echo ""
echo "--- DS-L: the checks' location does not outlive them ---"
bsd_config() {  # <dedup true|false>
    printf '{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "code_quality": { "no_placeholders": { "enabled": true, "level": "hard" } },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl", "deduplicate_checks": %s },
  "behavioral_sequences": { "enabled": true, "patterns": [
    { "name": "repeat_read", "sequence": ["file.read","file.read"], "max_gap": 20, "level": "advisory" } ] } }\n' "$1"
}
P_READS='use file
fn compute(x) {
    let y = x * 2
    return y + 1
}
main {
    file.write("a.txt", "x")
    let a = file.read("a.txt")
    let b = file.read("a.txt")
    let c = file.read("a.txt")
    let d = file.read("a.txt")
    print("RAN_" + string(compute(1)))
}'
rows() {  # telemetry file -> repeat_read rows
    python3 -c '
import json, sys
n = 0
for line in sys.stdin:
    try: e = json.loads(line)
    except Exception: continue
    d = e.get("fields", e)
    if d.get("event_type") in ("RuleViolation", "GovernanceCheck") and "repeat_read" in (d.get("rule_name") or ""): n += 1
sys.stdout.buffer.write(b"%d\n" % n)' < "$1"
}
newcase L-01 "$(bsd_config true)"; cli L-01 "$P_READS" ""
n="$( [ -f "$W/L-01/tele.jsonl" ] && rows "$W/L-01/tele.jsonl" || echo none)"; n="${n%$'\r'}"
if [ "$n" = "2" ] && [ "$RC" -eq 0 ] && ran; then
    pass L-01 "two runtime rows after the entry file's checks, under dedup"
else
    fail L-01 "runtime rows after the entry file's checks were collapsed or lost" "rows=$n exit=$RC"
fi
newcase L-01c "$(bsd_config false)"; cli L-01c "$P_READS" ""
n="$( [ -f "$W/L-01c/tele.jsonl" ] && rows "$W/L-01c/tele.jsonl" || echo none)"; n="${n%$'\r'}"
if [ "$n" = "2" ] && [ "$RC" -eq 0 ] && ran; then
    pass L-01c "CONTROL: without dedup the rule gives the same two rows"
else
    fail L-01c "CONTROL: the rule does not fire as the fixture expects" "rows=$n exit=$RC"
fi

# ------------------------------------------------------------
echo ""
echo "--- DS-W: a refusal during VM compilation writes the reports ---"
for e in vm tw; do
    id="W-01"; [ "$e" = tw ] && id="W-01t"
    newcase "$id" "$PH_CONFIG"
    printf '%s\n' "$P_PLACEHOLDER" > "$W/$id/main.naab"
    flagarg=""; [ "$e" = tw ] && flagarg="--tree-walk"
    OUT="$(cd "$W/$id" && timeout 60 "$NAAB" run $flagarg main.naab --governance-sarif report.sarif 2>&1)"; RC=$?
    if [ "$RC" -eq 3 ] && [ -s "$W/$id/report.sarif" ] && grep -q no_placeholders "$W/$id/report.sarif"; then
        pass "$id" "$e: refused (exit 3) and the SARIF report names the rule"
    else
        fail "$id" "$e: no report after the refusal" "exit=$RC sarif=$([ -s "$W/$id/report.sarif" ] && echo present || echo missing)"
    fi
done

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP unmeasurable"
[ "$FAIL" -eq 0 ]
