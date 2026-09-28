#!/usr/bin/env bash
# test_dogfood_hints.sh -- diagnostics found noisy or missing while building a
# real project (repo-sentinel) in NAAb.
#
# Group D: dict.get() miss hints. The hint is for typos, but building a map
#          from data (counting files from `git log`) misses on every new key,
#          and one hint per distinct key printed hundreds of lines over the
#          program's own output. Now capped per run, with one line saying how
#          to mark an expected miss. D-02 is the control that a lone typo still
#          gets its "did you mean" -- a cap of zero would pass D-01 alone.
# Group T: the ternary hint. It existed but fired only when '?' started an
#          expression; inside a dict literal or parentheses expect() reported
#          "Expected '}'" (plus a missing-brace diagnosis) or "Expected ')'"
#          instead. T-01 is the path that always worked, kept as the control.

set -uo pipefail
PASS=0
FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="$REPO/build/naab-lang"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/naab_hints.XXXXXX")"
[ -n "$WORK" ] && [ -d "$WORK" ] || { echo "FATAL: could not create work dir" >&2; exit 1; }
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'rm -rf "$WORK"; teardown_isolated_trust' EXIT

if [ ! -x "$NAAB" ]; then
    echo "FAIL: naab-lang not built at $NAAB (UNMEASURABLE, not a pass)"
    exit 1
fi
echo '{ "version": "4.0", "mode": "off" }' > "$WORK/govern.json"

pass() { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
fail() { echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "         $3"; FAIL=$((FAIL+1)); }

run() {  # run FLAG FILE -> stdout+stderr
    (cd "$WORK" && timeout 30 "$NAAB" $1 "$WORK/$2" 2>&1)
}

echo "=== D: dict.get miss hints ==="
cat > "$WORK/count.naab" <<'EOF'
main {
    let counts = {}
    let i = 0
    while i < 200 {
        let f = "src/dir/file_" + string(i % 50) + ".cpp"
        let n = counts.get(f)
        if n == null { counts[f] = 1 } else { counts[f] = n + 1 }
        i = i + 1
    }
    print("FILES:" + string(counts.size()))
}
EOF
cat > "$WORK/typo.naab" <<'EOF'
main {
    let cfg = {"service": "api", "port": 8080}
    let s = cfg.get("servce")
    print("DONE")
}
EOF
cat > "$WORK/default.naab" <<'EOF'
main {
    let counts = {"a.cpp": 1, "b.cpp": 2}
    let n = counts.get("c.cpp", 0)
    print("N:" + string(n))
}
EOF
for eng in "" "--tree-walk"; do
    tag="${eng:-vm}"; tag="${tag#--}"
    out="$(run "$eng" count.naab)"
    hints=$(grep -c '^\[hint\] dict.get' <<<"$out")
    if [[ "$out" == *"FILES:50"* ]] && [ "$hints" -le 3 ] && [[ "$out" == *"further hints suppressed"* ]]; then
        pass "D-01/$tag" "50 distinct expected misses: $hints hints, then one suppression line"
    else
        fail "D-01/$tag" "miss hints not capped ($hints hint lines)" "$(grep '^\[hint\]' <<<"$out" | head -2)"
    fi
    out="$(run "$eng" typo.naab)"
    case "$out" in
        *'did you mean "service"'*) pass "D-02/$tag" "control: a lone typo still gets its did-you-mean" ;;
        *) fail "D-02/$tag" "typo hint lost" "$(head -3 <<<"$out")" ;;
    esac
    out="$(run "$eng" default.naab)"
    if [[ "$out" == *"N:0"* ]] && ! grep -q '^\[hint\]' <<<"$out"; then
        pass "D-03/$tag" "a miss with a default prints no hint"
    else
        fail "D-03/$tag" "default did not silence the hint" "$(head -3 <<<"$out")"
    fi
done

echo "=== T: ternary hint ==="
printf 'main {\n    let c = true\n    let x = c ? "a" : "b"\n    print(x)\n}\n' > "$WORK/t_let.naab"
printf 'main {\n    let n = 3\n    let d = {"k": n > 2 ? "big" : "small"}\n    print(d)\n}\n' > "$WORK/t_dict.naab"
printf 'main {\n    let n = 3\n    print("v=" + (n > 2 ? "big" : "small"))\n}\n' > "$WORK/t_paren.naab"
printf 'fn f(n) {\n    return g(n > 2 ? 1 : 0)\n}\nfn g(x) { return x }\nmain { print(f(3)) }\n' > "$WORK/t_call.naab"
for case_ in "T-01:t_let:let (control)" "T-02:t_dict:dict literal value" "T-03:t_paren:parenthesised" "T-04:t_call:call argument"; do
    id="${case_%%:*}"; rest="${case_#*:}"; f="${rest%%:*}"; label="${rest#*:}"
    out="$(run "" "$f.naab")"
    if [[ "$out" == *"ternary operator"* ]] && [[ "$out" == *"if condition { value_a } else { value_b }"* ]] \
       && [[ "$out" != *"missing"*"closing"* ]]; then
        pass "$id" "ternary in a $label: hint shown"
    else
        fail "$id" "ternary in a $label: no hint" "$(head -3 <<<"$out")"
    fi
done

echo "=== I: interpolation parse errors report the column as a column ==="
# #258 shifted the inner tokens to the string's FILE line (so runtime errors
# inside ${...} point at the right line), which moved the inner parse error
# off "line 1" -- and the rewrite to "character N of the expression" matched
# only "line 1", so round 2 of repo-sentinel printed "Parser said: Parse error
# at line 2, column 11": a file line paired with an offset inside ${...}.
printf 'main {\n    let js = "x ${c.replace(/a/g, 1)} y"\n}\n' > "$WORK/i_line2.naab"
out="$(run "" i_line2.naab)"
case "$out" in
    *"Parser said:  at character"*"of the expression"*)
        if [[ "$out" == *"Parser said:  Parse error at line"* ]]; then
            fail I-01 "still reports the inner parse error as a file line"
        else
            pass I-01 "string on line 2: the inner column is reported as a character offset"
        fi ;;
    *) fail I-01 "inner parse error not rewritten" "$(grep 'Parser said' <<<"$out")" ;;
esac

echo "=== S: string names from other languages point at the NAAb one ==="
# F-007: `string.slice` suggested split() (edit distance). These say the
# equivalent directly. S-00 is the control that a real function still works.
# Match the suggestion itself ("Did you mean: string.X"): the old message
# listed every function after "Available:", so a loose match on the name
# passed S-01 on the build that suggested split().
# (S-01 was slice -> substring; string.slice now exists, see S-06. substr keeps
# a hint because JavaScript's substr takes a LENGTH, not an end index.)
for case_ in "S-01:substr:string.substring" "S-02:includes:string.contains" \
             "S-03:padStart:string.pad_left" "S-04:replaceAll:string.replace" \
             "S-05:trimStart:string.trim"; do
    id="${case_%%:*}"; rest="${case_#*:}"; fn="${rest%%:*}"; want="${rest#*:}"
    printf 'use string\nmain {\n    print(string.%s("abc", 1, 2))\n}\n' "$fn" > "$WORK/s_$fn.naab"
    out="$(run "" "s_$fn.naab")"
    case "$out" in
        *"Did you mean: $want"*) pass "$id" "string.$fn -> $want" ;;
        *) fail "$id" "string.$fn gives no useful suggestion" "$(grep -m2 'Did you mean\|Error' <<<"$out")" ;;
    esac
done
printf 'use string\nmain {\n    print(string.substring("hello", 1, 3))\n}\n' > "$WORK/s_ok.naab"
out="$(run "" s_ok.naab)"
if grep -qx 'el' <<<"$out"; then pass S-00 "control: string.substring still works"
else fail S-00 "control: string.substring broke" "$(head -2 <<<"$out")"; fi

# S-06 (round 3): `s.slice(...)` worked as a METHOD while `string.slice(...)`
# was an unknown function. It is now a module function with the method's
# JavaScript semantics, in both engines.
printf 'use string\nmain {\n    print(string.slice("hello", -3))\n    print(string.slice("hello", 1, -1))\n}\n' > "$WORK/s_slice.naab"
for eng in "" "--tree-walk"; do
    tag="${eng:-vm}"; tag="${tag#--}"
    out="$(run "$eng" s_slice.naab)"
    if [ "$(grep -xE 'llo|ell' <<<"$out" | tr '\n' ' ')" = "llo ell " ]; then
        pass "S-06/$tag" "string.slice works, negative indices count from the end"
    else
        fail "S-06/$tag" "string.slice missing or wrong" "$(head -3 <<<"$out")"
    fi
done

echo "=== E: an empty pattern cannot hang replace ==="
# The tree-walker's s.replace had no empty-pattern guard: find("") matches at
# every position, so "ab".replace("", "+") inserted forever with growing
# memory, and --timeout could not stop it (the loop never returns to the
# interpreter). The REST API runs the tree-walker. The expected output is the
# string unchanged, as string.replace already did.
printf 'main {\n    print("ab".replace("", "+"))\n    print("E_DONE")\n}\n' > "$WORK/e_empty.naab"
for eng in "" "--tree-walk"; do
    tag="${eng:-vm}"; tag="${tag#--}"
    out="$( (cd "$WORK" && timeout 10 "$NAAB" $eng "$WORK/e_empty.naab" 2>&1) )"
    rc=$?
    if [ "$rc" -eq 124 ]; then
        fail "E-01/$tag" "s.replace(\"\", ...) hung (killed after 10s)"
    elif grep -qx 'ab' <<<"$out" && grep -qx 'E_DONE' <<<"$out"; then
        pass "E-01/$tag" "empty-pattern replace returns the string unchanged"
    else
        fail "E-01/$tag" "empty-pattern replace gave the wrong result" "$(head -3 <<<"$out")"
    fi
done

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
