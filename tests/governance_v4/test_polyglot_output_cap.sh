#!/usr/bin/env bash
# ============================================================
# test_polyglot_output_cap.sh -- limits.data.output_size and
# languages.per_language.<lang>.max_output_size bound what foreign code hands
# back, on both engines, as an uncatchable HARD block (A18, A3a)
#
# WHAT WAS WRONG (observed at a1252222)
#
# The cap arrived in cc975e33 (Finding G, "FFI output size overflow",
# 2026-04-04) in the tree-walker's polyglot.cpp only. The VM had been the
# default engine since 94058845 (2026-03-28), and src/vm/ has never mentioned
# output_size, so on the default engine the cap did nothing: a block returning
# 100 bytes under output_size 20 ran, Governance: PASS, exit 0. Where it did run
# it threw a plain std::runtime_error -- it predates GovernanceHardError
# (fd97d0ae) -- so a NAAb try/catch swallowed it and the script continued, and
# it left no governance finding in any report. It measured only a STRING
# result: a list holding the same 100 bytes passed on both engines. An
# enforce()-based HARD version, GovernanceEngine::checkOutputSize(), existed from
# bcf647a0 and had no caller.
#
# Every result of foreign code a program receives now goes through one check,
# checkOutputSize() -- since A52 inside checkForeignResult(), which adds the
# restrictions.polyglot_output format check: inline blocks and persistent runtimes' .exec() on both
# engines, codegen.run(), and block-library method calls. Like every other
# limits.* check it is HARD with no level key.
#
# ARMS (each on the VM and the tree-walker unless marked)
#   OC-P    CONTROL  a plain program runs under the capped config (FAILS, not
#                    skips, when it does not: that is a broken interpreter)
#   OC-00   CONTROL  no cap: the block returns 100 bytes and the script ends
#                    (viability -- an engine where this fails is UNMEASURABLE)
#   OC-00z  CONTROL  output_size 0 means no limit
#   OC-01            output_size 20, a 100-byte string: refused, exit 3
#   OC-01c  CONTROL  output_size 200: the same block runs (the cap compares)
#   OC-02            inside try/catch: still refused, the catch never runs
#   OC-03            a LIST holding 100 bytes is refused (any result type)
#   OC-03c  CONTROL  a list holding 10 bytes under output_size 200 runs
#   OC-04            per-language javascript.max_output_size 20: refused
#   OC-04c  CONTROL  a per-language cap for ANOTHER language does not apply
#   OC-05            the refusal is a governance finding in the JSON report
#   OC-06            a persistent runtime's .exec() is capped
#   OC-06c  CONTROL  ... and runs under a cap it fits
#   OC-07            codegen.run() is capped
#   OC-07c  CONTROL  ... and runs under a cap it fits
#   OC-08            a block-library method call is capped (tree-walker; the VM
#                    compiler refuses `use BLOCK-...`, so its arm is UNMEASURABLE)
#   OC-08c  CONTROL  ... and runs under a cap it fits
#   OC-09            an embedded <<python>> block is capped (python only)
#
# On the tree-walker javascript runs through the parallel-group path
# (executePolyglotGroupParallel) and python through the sequential one, so
# OC-01..05/tw and OC-09/tw between them cover both. The parallel path had no
# cap at all: a javascript block escaped it on the tree-walker too, which the
# python-only control of A18's first measurement could not show.
#
# A refusal arm passes only on exit 3 AND the cap's own message, with the line
# after the block unreached: a broken fixture (exit 4, parse error, missing
# executor) is a different exit and fails the arm instead of passing it.
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"; setup_isolated_trust

PASS=0; FAIL=0; SKIP=0
ok(){ PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad(){ FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> ${3:0:300}"; }
skip(){ SKIP=$((SKIP+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }

[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }
T="$(mktemp -d)"
cleanup(){ teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "$T"; }
trap cleanup EXIT

CAP_MSG="exceeds limit of"

# run NAME ENGINE LIMITS_JSON LANGS_JSON EXTRA_JSON PROGRAM [extra args...]
# -> OUT, RC. Each run gets its own directory, config and HOME.
run() {
    local name=$1 eng=$2 limits=$3 langs=$4 extra=$5 prog=$6; shift 6
    local d="$T/$name.$eng"; mkdir -p "$d/home"
    cat > "$d/govern.json" <<EOF
{"version":"4.0","mode":"enforce","security":{"sandbox_level":"elevated"},
 "limits":{"data":$limits},"languages":{"per_language":$langs}$extra}
EOF
    if ! python3 -c 'import json,sys; json.load(sys.stdin)' < "$d/govern.json" 2>/dev/null; then
        OUT="__BADJSON__"; RC=99; return
    fi
    printf '%s\n' "$prog" > "$d/p.naab"
    local a=("$NAAB" run); [ "$eng" = tw ] && a+=(--tree-walk)
    OUT="$(cd "$d" && timeout 60 "${a[@]}" "$@" p.naab 2>&1)"; RC=$?
}
refused() { [ "$RC" = 3 ] && grep -qF "$CAP_MSG" <<<"$OUT" && ! grep -q '^END$' <<<"$OUT"; }
ran() { [ "$RC" = 0 ] && grep -q '^END$' <<<"$OUT"; }

JS_STR='main {
  let s = <<javascript
"x".repeat(100)
>>
  print("LEN=" + string.length(s))
  print("END")
}'
JS_TRY='main {
  try {
    let s = <<javascript
"x".repeat(100)
>>
    print("LEN=" + string.length(s))
  } catch (e) {
    print("SWALLOWED")
  }
  print("END")
}'
js_list() { printf 'main {\n  let s = <<javascript\n["x".repeat(%s)]\n>>\n  print("N=" + string.length(s[0]))\n  print("END")\n}\n' "$1"; }
RT_PROG='runtime js = javascript.start()
main {
  let s = js.exec("\"x\".repeat(100)")
  print("LEN=" + string.length(s))
  print("END")
}'
CG_PROG='use codegen
main {
  let r = codegen.run("javascript", "\"x\".repeat(100)")
  print("LEN=" + string.length(r["output"]))
  print("END")
}'
CG_ON=',"codegen":{"enabled":true}'
PY_STR='main {
  let s = <<python
"x" * 100
>>
  print("LEN=" + string.length(s))
  print("END")
}'

for eng in vm tw; do
    echo "--- engine: $eng"
    # A plain program first: an engine that cannot run one is broken, not
    # missing an executor, so this FAILS instead of skipping the rest.
    run ocp $eng '{"output_size":20}' '{}' '' 'main { print("END") }'
    if ran; then ok "OC-P/$eng" "CONTROL: the interpreter runs a program under the capped config"
    else bad "OC-P/$eng" "a plain program did not run" "rc=$RC $OUT"; continue; fi
    run oc00 $eng '{}' '{}' '' "$JS_STR"
    if ! ran || ! grep -q 'LEN=100' <<<"$OUT"; then
        for id in OC-00 OC-00z OC-01 OC-01c OC-02 OC-03 OC-03c OC-04 OC-04c OC-05 OC-06 OC-06c OC-07 OC-07c; do
            skip "$id/$eng" "a javascript block does not run on this engine here (rc=$RC)"
        done
        continue
    fi
    ok "OC-00/$eng" "CONTROL: with no cap the block returns 100 bytes and the script ends"

    run oc00z $eng '{"output_size":0}' '{}' '' "$JS_STR"
    if ran && grep -q 'LEN=100' <<<"$OUT"; then ok "OC-00z/$eng" "CONTROL: output_size 0 is no limit"
    else bad "OC-00z/$eng" "output_size 0 refused a block" "rc=$RC $OUT"; fi

    run oc01 $eng '{"output_size":20}' '{}' '' "$JS_STR"
    if refused; then ok "OC-01/$eng" "a 100-byte result under output_size 20 is refused, exit 3"
    else bad "OC-01/$eng" "the cap did not refuse a 100-byte result" "rc=$RC $OUT"; fi

    run oc01c $eng '{"output_size":200}' '{}' '' "$JS_STR"
    if ran && grep -q 'LEN=100' <<<"$OUT"; then ok "OC-01c/$eng" "CONTROL: under output_size 200 the same block runs"
    else bad "OC-01c/$eng" "a cap the result fits refused it" "rc=$RC $OUT"; fi

    run oc02 $eng '{"output_size":20}' '{}' '' "$JS_TRY"
    if refused && ! grep -q SWALLOWED <<<"$OUT"; then ok "OC-02/$eng" "try/catch cannot swallow the refusal"
    else bad "OC-02/$eng" "the refusal was caught or did not happen" "rc=$RC $OUT"; fi

    run oc03 $eng '{"output_size":20}' '{}' '' "$(js_list 100)"
    if refused; then ok "OC-03/$eng" "a list holding 100 bytes is refused"
    else bad "OC-03/$eng" "a list result escaped the cap" "rc=$RC $OUT"; fi

    run oc03c $eng '{"output_size":200}' '{}' '' "$(js_list 10)"
    if ran && grep -q 'N=10' <<<"$OUT"; then ok "OC-03c/$eng" "CONTROL: a list it fits runs"
    else bad "OC-03c/$eng" "a list under the cap was refused" "rc=$RC $OUT"; fi

    run oc04 $eng '{}' '{"javascript":{"max_output_size":20}}' '' "$JS_STR"
    if refused; then ok "OC-04/$eng" "per-language javascript.max_output_size 20 refuses it"
    else bad "OC-04/$eng" "the per-language cap did not refuse it" "rc=$RC $OUT"; fi

    run oc04c $eng '{}' '{"python":{"max_output_size":20}}' '' "$JS_STR"
    if ran && grep -q 'LEN=100' <<<"$OUT"; then ok "OC-04c/$eng" "CONTROL: another language's cap does not apply"
    else bad "OC-04c/$eng" "python's cap refused a javascript block" "rc=$RC $OUT"; fi

    run oc05 $eng '{"output_size":20}' '{}' '' "$JS_STR" --governance-report r.json
    REP="$T/oc05.$eng/r.json"
    if refused && [ -f "$REP" ] && python3 -c '
import json,sys
r=json.load(sys.stdin)
def walk(o):
    if isinstance(o,dict):
        if "limits.data.output_size" in json.dumps(o) and str(o.get("passed","")).lower()=="false": return True
        return any(walk(v) for v in o.values())
    if isinstance(o,list): return any(walk(v) for v in o)
    return False
sys.exit(0 if walk(r) else 1)' < "$REP" 2>/dev/null; then
        ok "OC-05/$eng" "the refusal is a failed limits.data.output_size check in the report"
    else
        bad "OC-05/$eng" "no failed output_size check in the report" "rc=$RC report=$( [ -f "$REP" ] && head -c 300 "$REP" || echo missing)"
    fi

    run oc06 $eng '{"output_size":20}' '{}' '' "$RT_PROG"
    if refused; then ok "OC-06/$eng" "a persistent runtime's .exec() result is capped"
    else bad "OC-06/$eng" "the runtime .exec() result escaped the cap" "rc=$RC $OUT"; fi
    run oc06c $eng '{"output_size":200}' '{}' '' "$RT_PROG"
    if ran && grep -q 'LEN=100' <<<"$OUT"; then ok "OC-06c/$eng" "CONTROL: .exec() runs under a cap it fits"
    else bad "OC-06c/$eng" "the .exec() route does not run here" "rc=$RC $OUT"; fi

    run oc07 $eng '{"output_size":20}' '{}' "$CG_ON" "$CG_PROG"
    if refused; then ok "OC-07/$eng" "codegen.run() is capped"
    else bad "OC-07/$eng" "codegen.run() escaped the cap" "rc=$RC $OUT"; fi
    run oc07c $eng '{"output_size":200}' '{}' "$CG_ON" "$CG_PROG"
    if ran && grep -q 'LEN=100' <<<"$OUT"; then ok "OC-07c/$eng" "CONTROL: codegen.run() runs under a cap it fits"
    else bad "OC-07c/$eng" "codegen.run() does not run here" "rc=$RC $OUT"; fi
done

# --- python: embedded blocks and block-library calls -------------------------
echo "--- python"
run py00 vm '{}' '{}' '' "$PY_STR"
if ran && grep -q 'LEN=100' <<<"$OUT"; then
    for eng in vm tw; do
        run oc09 $eng '{"output_size":20}' '{}' '' "$PY_STR"
        if refused; then ok "OC-09/$eng" "a <<python>> result is capped"
        else bad "OC-09/$eng" "a <<python>> result escaped the cap" "rc=$RC $OUT"; fi
    done
else
    skip "OC-09" "a <<python>> block does not return its value here (rc=$RC)"
fi

# A block-library block: javascript source in
# $HOME/.naab/language/blocks/library, loaded by a top-level `use BLOCK-... as
# b` (the recipe of tests/security/test_block_integrity_rt004.sh). Its
# function's return value is foreign output like any block's. The VM's compiler
# refuses `use BLOCK-...`, so the VM arm reports UNMEASURABLE: its block-call
# sites are wired but unreachable today.
BLK_SRC='function big(n) { return "x".repeat(n); }'
BLK_HASH=$(printf '%s' "$BLK_SRC" | sha256sum | awk '{print $1}')
blk_run() {  # NAME ENGINE CAP N
    local d="$T/$1.$2"; mkdir -p "$d/home/.naab/language/blocks/library/javascript"
    local L="$d/home/.naab/language/blocks/library/javascript"
    printf '%s' "$BLK_SRC" > "$L/oc_cap_block.js"
    cat > "$L/oc_cap_block.json" <<EOF
{"id":"BLOCK-JS-OCCAP","name":"oc_cap","language":"javascript","code_file":"oc_cap_block.js",
 "code_hash":"$BLK_HASH","version":"1.0.0","is_active":true}
EOF
    printf '{"version":"4.0","mode":"enforce","security":{"sandbox_level":"elevated"},"limits":{"data":{"output_size":%s}}}\n' "$3" > "$d/govern.json"
    printf 'use BLOCK-JS-OCCAP as b\nmain {\n  let s = b.big(%s)\n  print("LEN=" + string.length(s))\n  print("END")\n}\n' "$4" > "$d/p.naab"
    local a=("$NAAB" run); [ "$2" = tw ] && a+=(--tree-walk)
    OUT="$(cd "$d" && HOME="$d/home" timeout 60 "${a[@]}" p.naab 2>&1)"; RC=$?
}
for eng in tw vm; do
    blk_run oc08c $eng 200 100
    if ran && grep -q 'LEN=100' <<<"$OUT"; then
        ok "OC-08c/$eng" "CONTROL: a block-library call runs under a cap it fits"
        blk_run oc08 $eng 20 100
        if refused; then ok "OC-08/$eng" "a block-library method result is capped"
        else bad "OC-08/$eng" "a block-library method result escaped the cap" "rc=$RC $OUT"; fi
    else
        skip "OC-08/$eng" "a block-library call does not run on this engine here (rc=$RC: ${OUT:0:120})"
    fi
done

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
