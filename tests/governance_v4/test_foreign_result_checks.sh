#!/usr/bin/env bash
# ============================================================
# test_foreign_result_checks.sh -- restrictions.polyglot_output "json" and the
# output baselines apply to what foreign code hands back, on both engines
# (A52, A56)
#
# WHAT WAS WRONG (observed at dd7744fd)
#
# A52. checkPolyglotOutput() arrived in bcf647a0 with no caller; 2055ed39 wired
# it into the tree-walker's parallel-group path only, which only javascript
# takes. A block returning "not json" under polyglot_output "json" ran on the
# VM (the default engine) in every language, and on the tree-walker in python.
#
# A56. Output baselines did not work end to end. (a) 84034d0b saved the
# baselines file on every record; 58c7bc97 deferred that to the engine's
# destructor, which the CLI's _exit() skips, so a CLI run never saved a
# baseline. (b) checkBaseline() has one caller, the tree-walker's sequential
# polyglot path, so the VM never recorded or checked one, and neither did the
# tree-walker's parallel path.
#
# ARMS (on the VM, under enforce, unless marked). Under enforce --tree-walk is
# refused before anything runs (P-TW), so the tree-walker's routes are measured
# under audit, where a finding is recorded ("AUDIT ...") and the run goes on:
# PO-01a (javascript, its parallel group), PO-01pa (python, its sequential
# path), PO-06a (a block-library call, which only it can load), and BL-05.
#   P-00   CONTROL  a plain program runs under the json config
#   PO-00  CONTROL  no polyglot_output: a block returning "not json" runs
#   PO-01           javascript "not json" under "json": refused, exit 3
#   PO-01p          python "not json": refused (python only; the tree-walker
#                   runs python through its SEQUENTIAL path, javascript through
#                   the parallel one, so the two arms cover both)
#   PO-02  CONTROL  a valid JSON string runs
#   PO-03  CONTROL  a non-string result (a number) runs: only strings are
#                   judged, as on the one path that checked before
#   PO-04           a persistent runtime's .exec() result is checked
#   PO-04c CONTROL  ... a valid one runs
#   PO-05           codegen.run()'s output is checked
#   PO-05c CONTROL  ... a valid one runs
#   PO-06           a block-library call's result is checked (the VM compiler
#                   refuses `use BLOCK-...`, so its arm is UNMEASURABLE; PO-06a
#                   is the tree-walker under audit)
#   PO-07           try/catch cannot swallow the refusal (HARD)
#   PO-08           the object form {"format": "json"} refuses too
#   BL-01           a baseline recorded on a CLI run is saved (.naab/baselines.json)
#   BL-01f          the same through --governance-record-baselines
#   BL-02           a later run with a changed output is refused by
#                   --governance-check-baselines (record -> save -> load -> check)
#   BL-02c CONTROL  ... the unchanged output runs
#   BL-03  CONTROL  without the check flag a changed output runs
#   BL-04           a baseline PLANTED in the file is checked (isolates the
#                   check from the save)
#   BL-05           recorded on one engine, checked on the other (one key):
#                   VM under enforce, the tree-walker under audit
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"; setup_isolated_trust

PASS=0; FAIL=0; SKIP=0
ok(){ PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad(){ FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> ${3:0:400}"; }
skip(){ SKIP=$((SKIP+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }

echo "=== Foreign-result checks: polyglot_output json and output baselines (A52, A56) ==="
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }
T="$(mktemp -d)"
cleanup(){ teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "$T"; }
trap cleanup EXIT

JSON_MSG="must return valid JSON"
BL_MSG="Baseline mismatch"

# run DIR ENGINE EXTRA_JSON PROGRAM [naab args...] -> OUT, RC
#   DIR is reused when it exists (the baseline arms run several programs in one
#   directory); EXTRA_JSON is the members after the header, starting with ",".
run() {
    local d="$T/$1" eng=$2 extra=$3 prog=$4; shift 4
    mkdir -p "$d"
    printf '{"version":"4.0","mode":"%s","security":{"sandbox_level":"elevated"}%s}\n' "${MODE:-enforce}" "$extra" > "$d/govern.json"
    if ! python3 -c 'import json,sys; json.load(sys.stdin)' < "$d/govern.json" 2>/dev/null; then
        OUT="__BADJSON__"; RC=99; return
    fi
    printf '%s\n' "$prog" > "$d/p.naab"
    local a=("$NAAB" run); [ "$eng" = tw ] && a+=(--tree-walk)
    OUT="$(cd "$d" && timeout 60 "${a[@]}" "$@" p.naab 2>&1)"; RC=$?
}
refused_with() { [ "$RC" = 3 ] && grep -qF "$1" <<<"$OUT" && ! grep -q '^END$' <<<"$OUT"; }
ran() { [ "$RC" = 0 ] && grep -q '^END$' <<<"$OUT"; }
# audit: the finding is recorded and the program goes on to the end
recorded_with() { [ "$RC" = 0 ] && grep -q 'AUDIT ' <<<"$OUT" && grep -qF "$1" <<<"$OUT" && grep -q '^END$' <<<"$OUT"; }

JSON_ON=',"restrictions":{"polyglot_output":"json"}'
JSON_OBJ=',"restrictions":{"polyglot_output":{"format":"json"}}'
CG_ON=',"codegen":{"enabled":true}'
js_prog() { printf 'main {\n  let s = <<javascript\n%s\n>>\n  print("GOT=" + s)\n  print("END")\n}\n' "$1"; }
PY_BAD='main {
  let s = <<python
"not json"
>>
  print("GOT=" + s)
  print("END")
}'
JS_TRY='main {
  try {
    let s = <<javascript
"not json"
>>
    print("GOT=" + s)
  } catch (e) {
    print("SWALLOWED")
  }
  print("END")
}'
rt_prog() { printf 'runtime js = javascript.start()\nmain {\n  let s = js.exec("%s")\n  print("GOT=" + s)\n  print("END")\n}\n' "$1"; }
cg_prog() { printf 'use codegen\nmain {\n  let r = codegen.run("javascript", "%s")\n  print("GOT=" + r["output"])\n  print("END")\n}\n' "$1"; }

echo "-- polyglot_output: json (A52)"
source "$SCRIPT_DIR/../helpers/tree_walk_refusal.sh"
run ptw tw "$JSON_ON" "$(js_prog '"not json"')"
if tw_refused "$RC" "$OUT" && ! grep -q '^END$' <<<"$OUT"; then
    ok "P-TW" "under the enforce config --tree-walk is refused before anything runs"
else bad "P-TW" "--tree-walk ran under an enforce config" "rc=$RC $OUT"; fi
MODE=audit run po01a tw "$JSON_ON" "$(js_prog '"not json"')"
if recorded_with "$JSON_MSG"; then ok "PO-01a/tw" "audit: a javascript non-JSON result is recorded on the tree-walker (parallel group)"
else bad "PO-01a/tw" "audit: no finding for a javascript non-JSON result on the tree-walker" "rc=$RC $OUT"; fi
MODE=audit run po01pva tw "" "$PY_BAD"
if ran && grep -q 'GOT=not json' <<<"$OUT"; then
    MODE=audit run po01pa tw "$JSON_ON" "$PY_BAD"
    if recorded_with "$JSON_MSG"; then ok "PO-01pa/tw" "audit: a python non-JSON result is recorded on the tree-walker (sequential path)"
    else bad "PO-01pa/tw" "audit: no finding for a python non-JSON result on the tree-walker" "rc=$RC $OUT"; fi
else
    skip "PO-01pa/tw" "a <<python>> block does not return its value here (rc=$RC)"
fi

for eng in vm; do
    run p00.$eng $eng "$JSON_ON" 'main { print("END") }'
    if ran; then ok "P-00/$eng" "CONTROL: a plain program runs under the json config"
    else bad "P-00/$eng" "a plain program did not run" "rc=$RC $OUT"; continue; fi

    run po00.$eng $eng "" "$(js_prog '"not json"')"
    if ! ran || ! grep -q 'GOT=not json' <<<"$OUT"; then
        skip "PO-*/$eng" "a javascript block does not return its value on this engine here (rc=$RC)"; continue
    fi
    ok "PO-00/$eng" "CONTROL: with no polyglot_output a non-JSON result runs"

    run po01.$eng $eng "$JSON_ON" "$(js_prog '"not json"')"
    if refused_with "$JSON_MSG"; then ok "PO-01/$eng" "a javascript non-JSON result is refused, exit 3"
    else bad "PO-01/$eng" "a javascript non-JSON result ran" "rc=$RC $OUT"; fi

    run po01pv.$eng $eng "" "$PY_BAD"
    if ran && grep -q 'GOT=not json' <<<"$OUT"; then
        run po01p.$eng $eng "$JSON_ON" "$PY_BAD"
        if refused_with "$JSON_MSG"; then ok "PO-01p/$eng" "a python non-JSON result is refused, exit 3"
        else bad "PO-01p/$eng" "a python non-JSON result ran" "rc=$RC $OUT"; fi
    else
        skip "PO-01p/$eng" "a <<python>> block does not return its value here (rc=$RC)"
    fi

    run po02.$eng $eng "$JSON_ON" "$(js_prog 'JSON.stringify({a: 1})')"
    if ran && grep -qF 'GOT={"a":1}' <<<"$OUT"; then ok "PO-02/$eng" "CONTROL: a valid JSON string runs"
    else bad "PO-02/$eng" "a valid JSON string was refused" "rc=$RC $OUT"; fi

    run po03.$eng $eng "$JSON_ON" "$(js_prog '40 + 2')"
    if ran && grep -q 'GOT=42' <<<"$OUT"; then ok "PO-03/$eng" "CONTROL: a non-string result runs"
    else bad "PO-03/$eng" "a number result was refused" "rc=$RC $OUT"; fi

    run po04c.$eng $eng "$JSON_ON" "$(rt_prog 'JSON.stringify([1])')"
    if ran && grep -qF 'GOT=[1]' <<<"$OUT"; then
        ok "PO-04c/$eng" "CONTROL: a runtime .exec() returning JSON runs"
        run po04.$eng $eng "$JSON_ON" "$(rt_prog '\"not json\"')"
        if refused_with "$JSON_MSG"; then ok "PO-04/$eng" "a runtime .exec() non-JSON result is refused"
        else bad "PO-04/$eng" "a runtime .exec() non-JSON result ran" "rc=$RC $OUT"; fi
    else
        skip "PO-04/$eng" "a runtime .exec() does not run here (rc=$RC: ${OUT:0:160})"
    fi

    run po05c.$eng $eng "$JSON_ON$CG_ON" "$(cg_prog 'JSON.stringify([2])')"
    if ran && grep -qF 'GOT=[2]' <<<"$OUT"; then
        ok "PO-05c/$eng" "CONTROL: codegen.run() returning JSON runs"
        run po05.$eng $eng "$JSON_ON$CG_ON" "$(cg_prog '\"not json\"')"
        if refused_with "$JSON_MSG"; then ok "PO-05/$eng" "codegen.run() non-JSON output is refused"
        else bad "PO-05/$eng" "codegen.run() non-JSON output ran" "rc=$RC $OUT"; fi
    else
        skip "PO-05/$eng" "codegen.run() does not run here (rc=$RC: ${OUT:0:160})"
    fi

    run po07.$eng $eng "$JSON_ON" "$JS_TRY"
    if refused_with "$JSON_MSG" && ! grep -q SWALLOWED <<<"$OUT"; then ok "PO-07/$eng" "try/catch cannot swallow the refusal"
    else bad "PO-07/$eng" "the refusal was caught or did not happen" "rc=$RC $OUT"; fi

    run po08.$eng $eng "$JSON_OBJ" "$(js_prog '"not json"')"
    if refused_with "$JSON_MSG"; then ok "PO-08/$eng" "the object form {format: json} refuses too"
    else bad "PO-08/$eng" "the object form did not refuse" "rc=$RC $OUT"; fi
done

# A block-library block (the recipe of test_polyglot_output_cap.sh OC-08):
# javascript source in $HOME/.naab/language/blocks/library. The VM's compiler
# refuses `use BLOCK-...`, so its arm is UNMEASURABLE.
BLK_SRC='function val(ok) { return ok ? JSON.stringify([3]) : "not json"; }'
BLK_HASH=$(printf '%s' "$BLK_SRC" | sha256sum | awk '{print $1}')
blk_run() {  # NAME ENGINE OKFLAG
    local d="$T/$1.$2"; local L="$d/home/.naab/language/blocks/library/javascript"
    mkdir -p "$L"
    printf '%s' "$BLK_SRC" > "$L/fr_block.js"
    printf '{"id":"BLOCK-JS-FRCHK","name":"fr_chk","language":"javascript","code_file":"fr_block.js","code_hash":"%s","version":"1.0.0","is_active":true}\n' "$BLK_HASH" > "$L/fr_block.json"
    printf '{"version":"4.0","mode":"%s","security":{"sandbox_level":"elevated"}%s}\n' "${MODE:-enforce}" "$JSON_ON" > "$d/govern.json"
    printf 'use BLOCK-JS-FRCHK as b\nmain {\n  let s = b.val(%s)\n  print("GOT=" + s)\n  print("END")\n}\n' "$3" > "$d/p.naab"
    local a=("$NAAB" run); [ "$2" = tw ] && a+=(--tree-walk)
    OUT="$(cd "$d" && HOME="$d/home" timeout 60 "${a[@]}" p.naab 2>&1)"; RC=$?
}
blk_run po06c vm true
if ran && grep -qF 'GOT=[3]' <<<"$OUT"; then
    blk_run po06 vm false
    if refused_with "$JSON_MSG"; then ok "PO-06/vm" "a block-library call's non-JSON result is refused"
    else bad "PO-06/vm" "a block-library call's non-JSON result ran" "rc=$RC $OUT"; fi
else
    skip "PO-06/vm" "a block-library call does not run on this engine here (rc=$RC: ${OUT:0:120})"
fi
MODE=audit blk_run po06ac tw true
if ran && grep -qF 'GOT=[3]' <<<"$OUT"; then
    MODE=audit blk_run po06a tw false
    if recorded_with "$JSON_MSG"; then ok "PO-06a/tw" "audit: a block-library call's non-JSON result is recorded"
    else bad "PO-06a/tw" "audit: no finding for a block-library call's non-JSON result" "rc=$RC $OUT"; fi
else
    skip "PO-06a/tw" "a block-library call does not run on the tree-walker here (rc=$RC: ${OUT:0:120})"
fi

echo "-- output baselines (A56)"
# The block's TEXT never changes (it is the baseline key, with its line); its
# output follows val.txt through a bound variable.
BL_PROG='use file
main {
  let v = file.read("val.txt")
  let b = <<javascript[v]
v.trim()
>>
  print("BL=" + b)
  print("END")
}'
BL_REC=',"baselines":{"enabled":true,"auto_record":true}'
bl_entry() {  # DIR -> prints the recorded output values, one per line
    python3 -c 'import json,sys
d=json.load(sys.stdin)
for v in d.get("entries",{}).values(): print(v.get("output"))' < "$T/$1/.naab/baselines.json" 2>/dev/null
}
for eng in vm; do
    D=bl.$eng
    mkdir -p "$T/$D"; echo A > "$T/$D/val.txt"
    run $D $eng "" "$BL_PROG"
    if ! ran || ! grep -q '^BL=A$' <<<"$OUT"; then
        skip "BL-*/$eng" "the bound-variable javascript block does not run here (rc=$RC)"; continue
    fi
    run $D $eng "$BL_REC" "$BL_PROG"
    if ran && [ "$(bl_entry $D)" = "A" ]; then ok "BL-01/$eng" "a baseline recorded on a CLI run is saved"
    else bad "BL-01/$eng" "no saved baseline (entries: '$(bl_entry $D | tr '\n' ' ')')" "rc=$RC $OUT"; fi

    echo B > "$T/$D/val.txt"
    run $D $eng "" "$BL_PROG"
    if ran && grep -q '^BL=B$' <<<"$OUT"; then ok "BL-03/$eng" "CONTROL: without the check a changed output runs"
    else bad "BL-03/$eng" "a changed output was refused without the check" "rc=$RC $OUT"; fi
    run $D $eng "" "$BL_PROG" --governance-check-baselines
    if refused_with "$BL_MSG"; then ok "BL-02/$eng" "the check refuses a changed output against the saved baseline"
    else bad "BL-02/$eng" "the check did not refuse a changed output" "rc=$RC $OUT"; fi
    echo A > "$T/$D/val.txt"
    run $D $eng "" "$BL_PROG" --governance-check-baselines
    if ran && grep -q '^BL=A$' <<<"$OUT"; then ok "BL-02c/$eng" "CONTROL: the unchanged output runs under the check"
    else bad "BL-02c/$eng" "the unchanged output was refused" "rc=$RC $OUT"; fi

    D=blf.$eng; mkdir -p "$T/$D"; echo A > "$T/$D/val.txt"
    run $D $eng "" "$BL_PROG" --governance-record-baselines
    if ran && [ "$(bl_entry $D)" = "A" ]; then ok "BL-01f/$eng" "--governance-record-baselines saves the baseline"
    else bad "BL-01f/$eng" "the flag saved no baseline (entries: '$(bl_entry $D | tr '\n' ' ')')" "rc=$RC $OUT"; fi

    # Planted: hash_keys false makes the key "javascript::LINE", so the file can
    # be written by hand -- this arm does not depend on the save.
    D=blp.$eng; mkdir -p "$T/$D/.naab"; echo B > "$T/$D/val.txt"
    printf '%s\n' "$BL_PROG" > "$T/$D/p.naab"
    LINE="$(grep -n 'let b = <<javascript' "$T/$D/p.naab" | cut -d: -f1)"
    printf '{"entries":{"javascript::%s":{"output":"A","type":"string"}}}\n' "$LINE" > "$T/$D/.naab/baselines.json"
    run $D $eng ',"baselines":{"hash_keys":false}' "$BL_PROG" --governance-check-baselines
    if refused_with "$BL_MSG"; then ok "BL-04/$eng" "a planted baseline is checked"
    else bad "BL-04/$eng" "a planted baseline was not checked" "rc=$RC $OUT"; fi
done

# One key on both engines: record on one, check on the other. The VM under
# enforce (refused); the tree-walker under audit (recorded).
mode_of() { [ "$1" = tw ] && echo audit || echo enforce; }
for pair in "vm tw" "tw vm"; do
    set -- $pair
    D=blx.$1$2; mkdir -p "$T/$D"; echo A > "$T/$D/val.txt"
    MODE=$(mode_of $1) run $D $1 "$BL_REC" "$BL_PROG"
    if [ "$(bl_entry $D)" != "A" ]; then skip "BL-05/$1-$2" "nothing was recorded on $1 (rc=$RC)"; continue; fi
    echo B > "$T/$D/val.txt"
    MODE=$(mode_of $2) run $D $2 "" "$BL_PROG" --governance-check-baselines
    if { [ "$2" = vm ] && refused_with "$BL_MSG"; } || { [ "$2" = tw ] && recorded_with "$BL_MSG"; }; then
        ok "BL-05/$1-$2" "recorded on $1, checked on $2"
    else bad "BL-05/$1-$2" "a baseline recorded on $1 was not checked on $2" "rc=$RC $OUT"; fi
done

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
