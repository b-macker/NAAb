#!/usr/bin/env bash
# Bundle 12 PR 2: the static information-flow report, `naab-gov taint FILE`.
#
# Part A  parity with the run-time semantics: every row of
#         tests/governance_v4/taint_semantics.psv (PR 1's table, the run's
#         verdict per expression kind), under both of its origins, must get the
#         same verdict from the report: a VALUE flow at the sink where the run
#         judges the value tainted, no flow there where it judges it clean.
# Part B  statements, from tests/governance_v4/taint_flow.psv: R1 the report
#         finds exactly the row's flows; R2 the VM's own taint check refuses
#         exactly the row's `run` lines (pinned); R3 every line the run refuses
#         is a VALUE flow in the report (else the analysis misses explicit
#         flows the run sees).
# Part C  the command: exit codes 0/1/2/4, UNMEASURABLE with no config, taint
#         off, no source; a partial analysis that found nothing is not clean;
#         --config; --json.
#
# Kill table: 41 mutants, each applied alone (src/interpreter/taint_flow.cpp,
# src/cli/gov_taint.cpp, and for M36-M41 src/vm/vm.cpp), built and run; the
# first arm it turned red. 40 killed:
#   M01 if-statement branch gives no control      B/c_if_assign (and 7 more)
#   M02 while condition gives no control          B/c_while_sink, c_while_count
#   M03 for over a value gives no control         B/c_for_sink
#   M04 loop element is not data                  A/yield_gen, B/v_for_elem
#   M05 return under control is not sticky        B/c_return
#   M06 break under control is not sticky         B/c_break
#   M07 && / || right side not under control      B/c_short
#   M08 match subject gives no control            B/c_match_arm
#   M09 catch block not under the throw's control B/c_catch_sink
#   M10 throw does not mark the try               B/c_try_throw, c_try_expr_outcome
#   M11 rest of a try not under its control       B/c_try_rest
#   M12 callee ignores the caller's control       B/c_callee_control, c_higher_order
#   M13 user call drops its return value          A/call_userfn_ret, A/await_ret
#   M14 stdlib callback parameters clean          B/v_callback
#   M15 mutating method changes nothing           B/v_push
#   M16 control data reported as value            B/c_if_assign (and more)
#   M18 assignment ignores control                B/c_if_assign (and 5 more)
#   M19 if-statement drops the else state         B/v_strong_under
#   M20 a loop runs once (no fixed point)         B/v_loop_carried
#   M21 `use <stdlib>` treated as an import       C-09, C-12
#   M22 no reachability                           A/if_cond_unwind (and more)
#   M23 try expression's outcome ignored          B/c_try_expr_outcome
#   M24 catch binding loses the thrown value      A/throw_stmt_catch, B/v_catch_value
#   M25 closure captures nothing                  B/v_higher_order
#   M26 call through a name not resolved          A/lambda_call, B/v_higher_order
#   M27 a kept lambda throws where it is made     A/if_cond_unwind (and more)
#   M28 if-expression branch gives no control     B/c_ifexpr_sink
#   M29 polyglot block is not a sink              B/v_shell_bound, c_shell_under
#   M30 a partial analysis reads clean (exit 0)   C-07
#   M31 taint off is not reported UNMEASURABLE    C-02
#   M32 a lambda clobbers the statement's control B/c_try_expr_then_lambda
#   M33 a call clobbers the statement's control   B/c_try_expr_then_call
#   M34 nested fn sees no enclosing names         B/v_nested_capture, _async, _late
#   M35 nested fn sees declaration-time names     B/v_nested_late
# The run's half (A120, src/vm/vm.cpp), each applied alone, built and run:
#   M36 a struct field store marks nothing         B/v_struct_field, v_struct_field_read (R2)
#   M37 the array evaluator gives callbacks no taint B/v_callback, v_callback_restore (R2)
#   M38 a method's callback gets no taint          B/v_callback_find, _for_each, _sorted (R2)
#   M39 the scope does not put the outer value back B/v_callback_restore (R2)
#   M40 the scope inherits the outer value         B/n_callback_nested_clean (R2, R3)
#   M41 a method's callback ignores the receiver   B/v_callback_find, _for_each, _sorted (R2)
# Red first: on the pre-change build (a06f5b2d) exactly those seven v_ rows fail
# R2 and every n_ control passes (observed 2026-11-05).
# Survived, equivalent: M17 (a `let` ignores the control it is declared under).
# A `let` is scoped to its block, so every way its value leaves (assignment,
# return, push, a function value made there and called later) already joins
# that control; B/c_lambda_made_under is the closest probe and stays green.
# Measured against no earlier binary: the command is new.
#
# Usage: bash tests/governance_v4/test_taint_flow.sh [path/to/naab-lang]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${1:-${NAAB:-$REPO/build/naab-lang}}"
GOV="${NAAB_GOV:-$(dirname "$NAAB")/naab-gov}"
SEM="$SCRIPT_DIR/taint_semantics.psv"
TABLE="$SCRIPT_DIR/taint_flow.psv"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -14 | sed 's/^/       /'; }
skip() { SKIP=$((SKIP+1)); echo "  UNMEASURABLE [$1] $2"; }

for b in "$NAAB" "$GOV"; do
    if [ ! -x "$b" ] && [ ! -x "$b.exe" ]; then echo "UNMEASURABLE: $b not built"; exit 1; fi
done
for f in "$SEM" "$TABLE"; do [ -f "$f" ] || { echo "FAIL: $f missing"; exit 1; }; done

T="$(mktemp -d)"
cleanup() { teardown_isolated_trust; rm -rf "$T"; }
trap cleanup EXIT
setup_isolated_trust   # unsigned configs, no key: nothing is raised or refused for signing

cfg() {  # cfg SINKS_JSON -> a govern.json on stdout
    printf '{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},"taint_tracking":{"enabled":true,"level":"advisory","sources":["env.get"],"sinks":%s,"sanitizers":["sanitize_"]}}\n' "$1"
}

# Every naab-gov call is bounded. The report runs nothing of the program (and,
# since A122(c), none of its config's prerequisites; C-13), and an unbounded call
# that hung would hold this suite's output pipe open after the suite's own
# timeout killed bash -- a stalled step with no log, instead of a FAIL naming
# the call. 124 is timeout's exit status.
gov() { timeout "${TF_GOV_TIMEOUT:-60}" "$GOV" "$@"; }

# report DIR FILE -> REPORT: "kind line" per flow (from --json), RC
report() {
    local out
    out="$(cd "$1" && gov taint "$2" --json 2>&1)"; RC=$?
    [ "$RC" -eq 124 ] && echo "  (naab-gov taint $1/$2 did not finish in ${TF_GOV_TIMEOUT:-60} s)"
    REPORT="$(python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except Exception:
    print("PARSE-ERROR"); sys.exit(0)
for f in d.get("flows", []):
    print(f["kind"], f["line"])
' <<<"$out" | tr -d '\r')"   # print() writes CRLF on Windows
    RAW="$out"
}

# --------------------------------------------------------------------------
echo "=== Part A: the report agrees with the run-time semantics, per expression kind ==="
mkdir -p "$T/a"
cfg '["print"]' > "$T/a/govern.json"
ORIGIN_EXPR_source='env.get("HOME")'
ORIGIN_EXPR_callback='array.map_fn([1], fn(v) { return env.get("HOME") })[0]'
NA=0
while IFS='|' read -r id kind prelude setup expr context expect why; do
    case "$id" in ''|\#*) continue ;; esac
    [ "$prelude" = "-" ] && prelude=""
    [ "$setup" = "-" ] && setup=""
    expr="${expr//@@/$'\n'}"
    for origin in source callback; do
        ov="ORIGIN_EXPR_$origin"
        if [ "$context" = "sink" ]; then body="print(${expr})"
        else body="let x = ${expr}
    print(x)"; fi
        printf 'use env\nuse string\nuse array\n%s\nmain {\n    let t = %s\n    %s\n    %s\n    print("DONE")\n}\n' \
            "$prelude" "${!ov}" "$setup" "$body" > "$T/a/p.naab"
        # The sink's line: print(x) for assign rows, the body's print( for sink rows.
        if [ "$context" = "sink" ]; then
            target=$(awk '/^    let t = /{seen=1; next} seen && /^    print\(/ && !/print\("DONE"\)/ {print NR; exit}' "$T/a/p.naab")
        else
            target=$(awk '/^    print\(x\)$/ {print NR; exit}' "$T/a/p.naab")
        fi
        report "$T/a" p.naab
        NA=$((NA+1))
        if [ "$REPORT" = "PARSE-ERROR" ] || [ -z "$target" ]; then
            bad "A/$id/$origin" "the report did not run (rc=$RC, target=$target)" "$RAW"; continue
        fi
        got=$(awk -v L="$target" '$2 == L {print $1}' <<<"$REPORT" | sort -u | tr '\n' ' ')
        got="${got% }"
        if [ "$expect" = "tainted" ]; then want="value"; else want=""; fi
        # Known over-reports, measured and pinned (the report is static; the run
        # knows what actually ran). If one stops over-reporting, this fails too.
        case "$id" in
            if_cond_unwind_cb) want="control" ;;  # the callback throws on every path, but whether
                                                  # map_fn calls it at all depends on the list
        esac
        if [ "$got" = "$want" ]; then ok "A/$id/$origin" "$expect at the sink -- $why"
        else bad "A/$id/$origin" "run says $expect; report at line $target: [${got:-nothing}] -- $why" "$REPORT"; fi
    done
done < <(grep -v '^#' "$SEM" | tr -d '\r')
[ "$NA" -ge 150 ] || bad "A-N" "only $NA semantics checks ran (expected 2 per row of $SEM)"

# --------------------------------------------------------------------------
echo "=== Part B: statements (R1 report, R2 run pinned, R3 run within report) ==="
# pos LINE P S -> sN / pN / done for a file line, given prelude and setup sizes
pos() {
    local line="$1" p="$2" s="$3"
    if [ "$line" -le $((3 + p)) ]; then echo "p$((line - 3))"
    elif [ "$line" -eq $((3 + p + 2 + s + 1)) ]; then echo "done"
    else echo "s$((line - 3 - p - 2))"; fi
}
canon() { tr ' ' '\n' <<<"$1" | grep -v '^$' | sort -u | tr '\n' ' ' | sed 's/ $//'; }
NB=0
while IFS='|' read -r id prelude setup expect run why; do
    case "$id" in ''|\#*) continue ;; esac
    NB=$((NB+1))
    d="$T/b/$id"; mkdir -p "$d"
    cfg '["print", "shell_exec", "javascript_exec"]' > "$d/govern.json"
    [ "$prelude" = "-" ] && prelude=""
    pre="${prelude//@@/$'\n'}"
    set_="${setup//@@/$'\n    '}"
    if [ -z "$pre" ]; then P=0; else P=$(printf '%s\n' "$pre" | wc -l | tr -d ' '); fi
    S=$(printf '%s\n' "$set_" | wc -l | tr -d ' ')
    { printf 'use env\nuse string\nuse array\n'
      [ -n "$pre" ] && printf '%s\n' "$pre"
      printf 'main {\n    let t = env.get("HOME")\n    %s\n    print("DONE")\n}\n' "$set_"; } > "$d/p.naab"
    report "$d" p.naab
    if [ "$REPORT" = "PARSE-ERROR" ]; then bad "B/$id" "the report did not run (rc=$RC)" "$RAW"; continue; fi
    got=""
    while read -r k l; do [ -n "$k" ] && got="$got $k@$(pos "$l" "$P" "$S")"; done <<<"$REPORT"
    got="$(canon "$got")"; [ -z "$got" ] && got="none"
    want="$(canon "$expect")"
    if [ "$got" = "$want" ]; then ok "B/$id/R1" "$why"
    else bad "B/$id/R1" "report [$got], expected [$want] -- $why" "$RAW"; fi

    [ "$run" = "-" ] && continue
    # R2 under both origins of t (A121): the source call itself, which the VM
    # compiler's names see, and a callback's result, which only the run-time
    # taint carries. A row's pinned lines hold for both. R3 compares the
    # source origin's run with the report.
    for origin in source callback; do
    ov="ORIGIN_EXPR_$origin"
    { printf 'use env\nuse string\nuse array\n'
      [ -n "$pre" ] && printf '%s\n' "$pre"
      printf 'main {\n    let t = %s\n    %s\n    print("DONE")\n}\n' "${!ov}" "$set_"; } > "$d/p.naab"
    rid="B/$id/R2"; [ "$origin" = callback ] && rid="B/$id/R2cb"
    rm -f "$d/rep.json"
    (cd "$d" && timeout 30 "$NAAB" p.naab --governance-report rep.json >"$d/run.out" 2>&1)
    if ! grep -qx "DONE" "$d/run.out" || [ ! -s "$d/rep.json" ]; then
        bad "$rid" "the program did not run to its end, or wrote no report" "$(tail -5 "$d/run.out")"; continue
    fi
    flagged=""
    while read -r l; do [ -n "$l" ] && flagged="$flagged s$(( l - 3 - P - 2 ))"; done < <(python3 -c '
import json, re, sys
d = json.load(sys.stdin)
for r in d.get("results", []):
    if r.get("rule") == "taint_tracking.sink_violation" and not r.get("passed", True):
        for m in re.findall(r"p\.naab:(\d+)", json.dumps(r)):
            print(m)
' < "$d/rep.json" | tr -d '\r')
    # a refusal inside the prelude is a prelude position
    fixed=""
    for f in $flagged; do
        n="${f#s}"
        if [ "$n" -le 0 ]; then fixed="$fixed p$((n + P + 2))"; else fixed="$fixed $f"; fi
    done
    flagged="$(canon "$fixed")"; [ -z "$flagged" ] && flagged="none"
    if [ "${TF_MEASURE:-}" = 1 ]; then echo "  MEASURE [$id/$origin] run=$flagged"; fi
    # `run` may pin the callback origin apart: "X cb:Y" (an over-report only the
    # compiler's names make, which a callback's result does not reach).
    pin="${run% cb:*}"
    if [ "$origin" = callback ] && [ "$pin" != "$run" ]; then pin="${run##* cb:}"; fi
    if [ "$flagged" = "$(canon "$pin")" ]; then ok "$rid" "the run refuses [$flagged] (pinned, t from $origin)"
    else bad "$rid" "the run refuses [$flagged], pinned [$pin] (t from $origin)" "$(grep -i violation "$d/run.out" | head -4)"; fi
    [ "$origin" = callback ] && continue
    if [ "$flagged" != "none" ]; then
        miss=""
        for f in $flagged; do grep -qw "value@$f" <<<"$got" || miss="$miss $f"; done
        # Accepted over-reports of the run (the owner's decision), pinned so a
        # change in either direction shows: o_ rows only, and only these lines.
        case "$id" in
            o_nested_reassigned) [ "$(canon "$miss")" = "s6" ] && miss="" ;;  # A121(c)
            o_nested_reassigned_after_param) [ "$(canon "$miss")" = "s9" ] && miss="" ;;  # A121(c)
        esac
        if [ -z "$miss" ]; then ok "B/$id/R3" "every line the run refuses is a value flow in the report"
        else bad "B/$id/R3" "the run refuses [$miss] but the report has no value flow there: [$got]"; fi
    fi
    done
done < <(grep -v '^#' "$TABLE" | tr -d '\r')
[ "$NB" -ge 30 ] || bad "B-N" "only $NB statement rows read from $TABLE"

# --------------------------------------------------------------------------
echo "=== Part C: the command ==="
C="$T/c"; mkdir -p "$C/none" "$C/off" "$C/nosrc" "$C/ok" "$C/other"
flow='use env
main {
    let t = env.get("HOME")
    if t == "/x" {
        print("bit")
    }
}'
clean='use env
main {
    let c = "k"
    print(c)
}'
kept='use env
fn apply(f) {
    return f(1)
}
main {
    print("x")
}'
printf '%s\n' "$flow" > "$C/none/p.naab"
out="$(cd "$C/none" && gov taint p.naab 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && grep -q "UNMEASURABLE" <<<"$out"; then ok "C-01" "no govern.json: reported unmeasurable, exit 2"
else bad "C-01" "expected exit 2 and UNMEASURABLE, got rc=$rc" "$out"; fi

printf '%s\n' '{"version":"5.0","mode":"enforce","taint_tracking":{"enabled":false,"sources":["env.get"],"sinks":["print"]}}' > "$C/off/govern.json"
printf '%s\n' "$flow" > "$C/off/p.naab"
out="$(cd "$C/off" && gov taint p.naab 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && grep -q "not enabled" <<<"$out"; then ok "C-02" "taint_tracking off: reported unmeasurable, exit 2"
else bad "C-02" "expected exit 2, not enabled; got rc=$rc" "$out"; fi

printf '%s\n' '{"version":"5.0","mode":"enforce","taint_tracking":{"enabled":true,"sources":[],"sinks":["print"]}}' > "$C/nosrc/govern.json"
printf '%s\n' "$flow" > "$C/nosrc/p.naab"
out="$(cd "$C/nosrc" && gov taint p.naab 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && grep -q "no source" <<<"$out"; then ok "C-03" "no source listed: reported unmeasurable, exit 2"
else bad "C-03" "expected exit 2, no source; got rc=$rc" "$out"; fi

cfg '["print"]' > "$C/ok/govern.json"
printf 'main {\n    let = \n}\n' > "$C/ok/bad.naab"
out="$(cd "$C/ok" && gov taint bad.naab 2>&1)"; rc=$?
if [ "$rc" -eq 4 ]; then ok "C-04" "a file that does not parse: exit 4"
else bad "C-04" "expected exit 4, got rc=$rc" "$out"; fi

printf '%s\n' "$clean" > "$C/ok/clean.naab"
out="$(cd "$C/ok" && gov taint clean.naab 2>&1)"; rc=$?
if [ "$rc" -eq 0 ] && grep -q "no flow found" <<<"$out"; then ok "C-05" "a clean program: no flow found, exit 0"
else bad "C-05" "expected exit 0, no flow found; got rc=$rc" "$out"; fi

printf '%s\n' "$flow" > "$C/ok/flow.naab"
out="$(cd "$C/ok" && gov taint flow.naab 2>&1)"; rc=$?
if [ "$rc" -eq 1 ] && grep -q "control line 5: print" <<<"$out"; then ok "C-06" "a control flow: reported with its line, exit 1"
else bad "C-06" "expected exit 1 and a control flow at line 5; got rc=$rc" "$out"; fi

printf '%s\n' "$kept" > "$C/ok/kept.naab"
out="$(cd "$C/ok" && gov taint kept.naab 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && grep -q "not analysed" <<<"$out"; then ok "C-07" "nothing found but a construct not analysed: reported unmeasurable, exit 2 (not clean)"
else bad "C-07" "expected exit 2 with a not-analysed line; got rc=$rc" "$out"; fi

printf '%s\n' "$flow" > "$C/other/p.naab"   # no govern.json beside it
out="$(cd "$C/other" && gov taint p.naab --config "$C/ok/govern.json" 2>&1)"; rc=$?
if [ "$rc" -eq 1 ] && grep -q "control line 5" <<<"$out"; then ok "C-08" "--config names the policy where none is discovered"
else bad "C-08" "expected the flow under --config; got rc=$rc" "$out"; fi

cfg '["file.write"]' > "$C/other/sinks.json"
out="$(cd "$C/other" && gov taint p.naab --config "$C/other/sinks.json" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then ok "C-09" "the sink list is read from the config: print is not a sink here, so no flow (pairs with C-06)"
else bad "C-09" "expected exit 0 with print not a sink; got rc=$rc" "$out"; fi

out="$(cd "$C/ok" && gov taint flow.naab --json 2>/dev/null)"
if python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["result"]=="flows" and d["flows"][0]["kind"]=="control" and d["flows"][0]["line"]==5' <<<"$out" 2>/dev/null; then
    ok "C-10" "--json: result, kind and line"
else bad "C-10" "--json did not carry the flow" "$out"; fi

out="$(cd "$C/ok" && gov taint flow.naab 2>&1)"
if ! grep -q "not analysed" <<<"$out" && grep -q "control line 5" <<<"$out"; then
    ok "C-12" "a standard-library module (use env) is not an imported module the report skips"
else bad "C-12" "a stdlib call was reported as not analysed" "$out"; fi

out="$(cd "$C/ok" && gov taint flow.naab --bogus 2>&1)"; rc=$?
if [ "$rc" -eq 4 ]; then ok "C-11" "an unknown option: exit 4"
else bad "C-11" "expected exit 4, got rc=$rc" "$out"; fi

# C-13: a `command` prerequisite is not started by `naab-gov taint` at all
# (A122(c), Bundle 16: prerequisites run only where a program runs), and the
# load says so. Before, every load ran them, with this process's stdin. `tee
# got.txt` would copy its stdin to got.txt, so got.txt not existing is the
# claim -- meaningful only with tee on PATH (else UNMEASURABLE); its being run
# under `naab-lang run` is test_engine_child_stdin.sh ES-04's control.
mkdir -p "$C/pre"
python3 -c '
import json, sys
d = json.load(sys.stdin)
d["prerequisites"] = {"enabled": True, "checks": [
    {"type": "command", "name": "tee got.txt", "required": "exists", "level": "advisory"}]}
print(json.dumps(d))' < "$C/ok/govern.json" > "$C/pre/govern.json"
printf '%s\n' "$flow" > "$C/pre/p.naab"
out="$(cd "$C/pre" && gov taint p.naab <<<"PIPED_INPUT_C13" 2>&1)"; rc=$?
if [ "$rc" -eq 124 ]; then
    bad "C-13" "naab-gov did not finish in ${TF_GOV_TIMEOUT:-60} s with a prerequisite command configured" "$out"
elif ! command -v tee >/dev/null 2>&1; then
    skip "C-13" "no tee on PATH -- a prerequisite that did not run cannot be told from one that could not"
elif [ "$rc" -eq 1 ] && [ ! -e "$C/pre/got.txt" ] && grep -q 'environment prerequisite check(s) not run' <<<"$out"; then
    ok "C-13" "naab-gov taint starts no prerequisite command, and says so (A122(c))"
else bad "C-13" "rc=$rc; got.txt $([ -e "$C/pre/got.txt" ] && echo "exists: [$(head -c 80 "$C/pre/got.txt")]" || echo absent)" "$out"; fi

echo ""
echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$SKIP"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
