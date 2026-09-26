#!/usr/bin/env bash
# test_string_interp_escape.sh -- literal ${ in strings, and errors that teach.
#
# NAAb evaluates every ${...} inside a string literal. Before this suite there
# was no way to write a literal "${": \${ and \x24{ both still interpolated.
# Embedding JavaScript template literals or shell ${VAR} in a string was a
# trap, and falling into it produced "Parse error at line 1, column 11" -- the
# position inside the extracted expression, with no file -- followed by a hint
# about multi-line operators, which had nothing to do with it. Found while an
# LLM built a real NAAb project; it cost a long bisection.
#
# Group A: \${ is a literal ${, in both engines (A-07: an interpolated literal
#          concatenated with another literal -- the VM constant-folded the raw
#          text and never interpolated it); ordinary ${expr} still
#          interpolates (the control -- without it "no interpolation happened"
#          would pass every escape arm); \$ NOT followed by { is unchanged, so
#          existing shell text like "echo \$HOME" keeps its meaning.
# Group B: the errors an LLM hits now name the file and line and the fix.
# Group C: the formatter round-trips \${ -- it writes string values back out,
#          and writing the lexer's internal marker would turn a literal ${ into
#          live interpolation on the next run.
# Taint agreement for \${ is in governance_v4/test_taint_engine_parity.sh
# (PARITY-interp_escaped).

set -uo pipefail
PASS=0
FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="$REPO/build/naab-lang"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/naab_interp.XXXXXX")"
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

run() {  # run ENGINE_FLAG FILE -> stdout+stderr without governance chatter
    (cd "$WORK" && timeout 30 "$NAAB" $1 "$WORK/$2" 2>&1) | grep -v '^\[governance\]'
}

# The control and the compatibility arm run as their OWN program: in the
# escape program below, one bad line on an older build aborts the whole run,
# and every other arm would fail with it -- a false FAIL on exactly the arm
# (A-04) that claims behaviour is UNCHANGED from older builds.
cat > "$WORK/base.naab" <<'EOF'
main {
    let n = 5
    print("A2:${n}")
    print("A4:keep \$PATH")
    print("A8:regex a\{2\}")
}
EOF

cat > "$WORK/esc.naab" <<'EOF'
main {
    let n = 5
    print("A1:" + "js `\${row.churn/max}%`")
    print("A3:mix \${x} and ${n}")
    print(f"A5:{n} and \{literal\}")
    print("A6:sh echo \${HOME}/x")
    print("A7:n=${n}" + "!")
}
EOF

echo "=== A: escape semantics ==="
for eng in "" "--tree-walk"; do
    tag="${eng:-vm}"
    out="$(run "$eng" base.naab)"
    expect_line() {  # ID LABEL EXPECTED
        if grep -qxF -- "$3" <<<"$out"; then pass "$1/$tag" "$2"
        else fail "$1/$tag" "$2" "want: $3 | got: $(grep "^${3%%:*}:" <<<"$out")"; fi
    }
    expect_line A-02 "control: \${expr} still interpolates"     'A2:5'
    expect_line A-04 "\\\$ without { is unchanged (backslash kept)" 'A4:keep \$PATH'
    expect_line A-08 "\\{ outside f-strings is unchanged (regex text)" 'A8:regex a\{2\}'
    out="$(run "$eng" esc.naab)"
    expect_line A-01 "\\\${ is a literal \${ (JS template literal)" 'A1:js `${row.churn/max}%`'
    expect_line A-03 "escaped and live \${ in one string"          'A3:mix ${x} and 5'
    expect_line A-05 "f-string: \\{ and \\} are literal braces"     'A5:5 and {literal}'
    expect_line A-06 "shell \${VAR} text via \\\${"                'A6:sh echo ${HOME}/x'
    # The VM folded "lit" + "lit" from RAW source text, so an interpolated
    # literal on either side of + printed "n=${n}!" on the VM and "n=5!" on the
    # tree-walker -- a pre-existing engine divergence, found by A-01.
    expect_line A-07 "\"\${n}\" + \"!\" interpolates (VM used to fold raw text)" 'A7:n=5!'
done

echo "=== B: errors that teach ==="
cat > "$WORK/b_js.naab" <<'EOF'
main {
    let js = "const w = ${x.replace(/a/g, '')};"
}
EOF
cat > "$WORK/b_sh.naab" <<'EOF'
main {
    let cmd = "echo ${HOME}/x"
    print(cmd)
}
EOF
cat > "$WORK/b_eq.naab" <<'EOF'
main {
    if 1 === 1 { print("y") }
}
EOF
for eng in "" "--tree-walk"; do
    tag="${eng:-vm}"
    out="$(run "$eng" b_js.naab)"
    case "$out" in
        *"String interpolation error"*"b_js.naab:2"*"looks like JavaScript"*'\${'*)
            pass "B-01/$tag" "JS in \${...}: names file:line, recognises JS, shows \\\${" ;;
        *) fail "B-01/$tag" "JS-in-interpolation error does not teach" "$(head -4 <<<"$out")" ;;
    esac
    case "$out" in
        *"Operator '/' at start of expression"*) fail "B-02/$tag" "still shows the unrelated multi-line-operator hint" ;;
        *) pass "B-02/$tag" "the unrelated multi-line-operator hint is gone" ;;
    esac
    out="$(run "$eng" b_sh.naab)"
    case "$out" in
        *"Undefined variable"*"HOME"*"shell or environment variable"*'\${HOME}'*'env.get("HOME")'*)
            pass "B-03/$tag" "\${HOME}: explains interpolation, offers \\\${HOME} and env.get" ;;
        *) fail "B-03/$tag" "undefined ALL_CAPS name gives no escape hint" "$(head -4 <<<"$out")" ;;
    esac
    out="$(run "$eng" b_eq.naab)"
    case "$out" in
        *"no '===' operator"*"a == b"*) pass "B-04/$tag" "=== points to ==" ;;
        *) fail "B-04/$tag" "=== gives no hint" "$(head -4 <<<"$out")" ;;
    esac
done

echo "=== C: formatter round trip ==="
cp "$WORK/esc.naab" "$WORK/fmt.naab"
before="$(run "" esc.naab)"
(cd "$WORK" && "$NAAB" fmt "$WORK/fmt.naab" >/dev/null 2>&1)
after="$(run "" fmt.naab)"
if grep -qF 'js `\${row.churn/max}%`' "$WORK/fmt.naab" && [ "$before" = "$after" ]; then
    pass C-01 "formatted file keeps \\\${ and runs identically"
else
    fail C-01 "formatter changed \\\${ or the program's output" "$(grep -n 'A1' "$WORK/fmt.naab")"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
