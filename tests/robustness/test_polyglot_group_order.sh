#!/usr/bin/env bash
# ============================================================
# test_polyglot_group_order.sh -- polyglot blocks keep source order with the statements around them
#
# The tree-walker runs polyglot blocks in parallel GROUPS
# (polyglot_dependency_analyzer.cpp), and visit(CompoundStmt) runs the ordinary
# statements lying between a group's blocks only after the whole group. The
# analyzer kept two blocks in one batch across a gap of ONE statement ("could
# just be a print"), so that statement ran out of order:
#
#   PG-01  `let later = 10` between two blocks: the second block, binding
#          `later`, ran first and failed -- "Variable 'later' not found in scope
#          for inline code binding"
#   PG-02  `x = x + 1` between two blocks: the second block bound the stale x
#          (10 instead of 20)
#   PG-03  CONTROL: adjacent, independent blocks still run and return their
#          values -- a fix that stopped grouping (or running) blocks altogether
#          would pass PG-01/02 only if their outputs were also wrong, so this
#          pins the plain case.
#
# The VM does not group blocks; every arm runs on BOTH engines and expects the
# same answer, so the VM is the oracle. PG-01/PG-02 fail on the pre-fix build
# on the tree-walker only.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }
skip_all() { for id in PG-01 PG-02 PG-03; do skip "$id" "$1"; done
             echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0; }

echo "=== polyglot blocks keep source order (both engines) ==="

[ -x "$NAAB" ] || skip_all "naab-lang not built (UNMEASURABLE)"

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-pgorder.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust   # the unsigned govern.json below must not meet a populated trust store
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT
echo '{ "version": "4.0", "mode": "off" }' > "$W/govern.json"

cat > "$W/adjacent.naab" <<'EOF'
main {
    let a = <<python
1
>>
    let b = <<python
2
>>
    print("ADJ=" + string(a) + "," + string(b))
}
EOF
cat > "$W/let_between.naab" <<'EOF'
main {
    let first = 1
    let r1 = <<python[first]
first + 1
>>
    let later = 10
    let r2 = <<python[later]
later + 1
>>
    print("LET=" + string(r1) + "," + string(r2))
}
EOF
cat > "$W/reassign_between.naab" <<'EOF'
main {
    let x = 1
    let r1 = <<python[x]
x * 10
>>
    x = x + 1
    let r2 = <<python[x]
x * 10
>>
    print("RE=" + string(r1) + "," + string(r2))
}
EOF

# A build without the embedded Python executor says so, and its blocks return
# no value -- every arm would be unobservable. Only that self-declared
# limitation skips; any other failure of the plain case is PG-03's FAIL.
probe="$(cd "$W" && timeout 60 "$NAAB" adjacent.naab 2>&1)"
case "$probe" in
    *"ADJ=1,2"*) ;;
    *"Python support not available"*) skip_all "this build has no embedded Python executor (UNMEASURABLE)" ;;
esac

# check <id> <file> <expected line> <description>
check() {
    local why="" out flag
    for flag in "" "--tree-walk"; do
        out="$(cd "$W" && timeout 60 "$NAAB" "$2" $flag 2>&1)"
        case "$out" in
            *"$3"*) ;;
            *) why="$why ${flag:-vm}:[$(printf '%s' "$out" | grep -v Loaded | tail -1 | cut -c1-90)]" ;;
        esac
    done
    if [ -z "$why" ]; then ok "$1" "$4"; else bad "$1" "$4" "$why"; fi
}

check PG-01 let_between.naab      "LET=2,11" "a block binding a variable declared after the previous block sees it"
check PG-02 reassign_between.naab "RE=10,20" "a block binding a variable reassigned after the previous block sees the new value"
check PG-03 adjacent.naab         "ADJ=1,2"  "CONTROL: adjacent independent blocks still run and return their values"

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
