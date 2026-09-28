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

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
