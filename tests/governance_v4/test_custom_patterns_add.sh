#!/usr/bin/env bash
# test_custom_patterns_add.sh -- custom_patterns ADD to a simple code_quality
# check's patterns (A105, the owner's decision 16).
#
# loadSimpleCheck() pushed `custom_patterns` into `patterns`, and each of seven
# checks uses `patterns.empty() ? BUILT_IN : patterns`, so a non-empty
# custom_patterns REPLACED the built-in list: "no_temporary_code": {"level":
# "hard"} refused a block carrying `# for now`, and adding "custom_patterns":
# ["zz_never_matches_zz"] let it run (observed on d32f8603, CA-RUN). Since the
# v3.0 engine (bcf647a0), when the struct fields and the merge came in
# together; the two checks written beside them (no_oversimplification,
# no_incomplete_logic) always appended theirs. Now the seven append too.
#
# `patterns` still REPLACES the built-in list (CA-R pins it: the decision
# names custom_patterns only). no_hardcoded_urls and no_hardcoded_ips read no
# pattern list at all -- one fixed regex each -- so neither key does anything
# there (recorded, not changed).
#
#   CA-<chk>.0  CONTROL: the built-in pattern fires on its snippet (no custom).
#   CA-<chk>.1  with a custom pattern that never matches, the built-in still
#               fires (the A105 arm; red on d32f8603 for every check).
#   CA-<chk>.2  CONTROL: a custom pattern fires on a snippet the built-ins miss.
#   CA-C        CONTROL: a clean snippet is not flagged under the same config.
#   CA-R        PINNED: `patterns` replaces the built-in list.
#   CA-RUN      the record's reproduction through a real run: exit 3, the block
#               never runs.
#
# Kill table (mutants of src/runtime/governance_config.cpp and
# governance_checks.cpp, run by hand):
#   A1 custom_patterns pushed into patterns again   -> every CA-<chk>.1, CA-RUN
#   A2 withCustomPatterns() ignores the extras      -> every CA-<chk>.2
#   A3 withCustomPatterns() keeps only the extras   -> every CA-<chk>.1, CA-RUN
#      when there are any (the old behaviour, moved)

set -uo pipefail
PASS=0; FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
GOV="${GOV:-$(dirname "$NAAB")/naab-gov}"
W="$(mktemp -d "${TMPDIR:-/tmp}/naab_cpadd.XXXXXX")"
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT
ok()  { echo "  PASS $1: $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL $1: $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/      /'; FAIL=$((FAIL+1)); }
[ -x "$NAAB" ] && [ -x "$GOV" ] || { echo "UNMEASURABLE: naab-lang or naab-gov not built"; exit 1; }

# flagged CHECK LANG EXTRA CODE -> 0 flagged, 1 not, 2 UNMEASURABLE (no JSON).
N=0
flagged() {
    N=$((N+1))
    printf '{"version":"3.0","mode":"audit","code_quality":{"%s":{"level":"hard"%s}}}\n' "$1" "$3" > "$W/c$N.json"
    python3 -c 'import json,sys; json.load(sys.stdin)' < "$W/c$N.json" || return 2
    # Captured first: naab-gov exits non-zero when it blocks, and under
    # pipefail that status would replace the filter's (every finding read as
    # "not flagged" in the first version of this suite).
    local out
    out=$(printf '%s\n' "$4" | "$GOV" check --language "$2" --config "$W/c$N.json" 2>/dev/null)
    printf '%s' "$out" | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except ValueError:
    sys.exit(2)
sys.exit(0 if any(v.get("rule") == "code_quality." + sys.argv[1] for v in d.get("violations", [])) else 1)' "$1"
}
# expect ID WANT CHECK LANG EXTRA CODE DESC
expect() {
    flagged "$3" "$4" "$5" "$6"; local r=$?
    if [ "$r" -eq 2 ]; then bad "$1" "UNMEASURABLE: no JSON from naab-gov check (or a bad fixture)"; return; fi
    if { [ "$2" = yes ] && [ "$r" -eq 0 ]; } || { [ "$2" = no ] && [ "$r" -eq 1 ]; }; then ok "$1" "$7"
    else bad "$1" "$7 (wanted flagged=$2)" "$6"; fi
}

NEVER=', "custom_patterns": ["zz_never_matches_zz"]'
while IFS='|' read -r chk lang code cust ccode; do
    code=$(printf '%b' "$code"); ccode=$(printf '%b' "$ccode")
    expect "CA-$chk.0" yes "$chk" "$lang" "" "$code" "CONTROL: $chk's built-in pattern fires"
    expect "CA-$chk.1" yes "$chk" "$lang" "$NEVER" "$code" "$chk: a never-matching custom pattern leaves the built-ins on"
    expect "CA-$chk.2" yes "$chk" "$lang" ", \"custom_patterns\": [\"$cust\"]" "$ccode" "CONTROL: $chk's custom pattern fires"
done <<'EOF'
no_temporary_code|python|x = 1  # for now|zzcustomtemp|x = 1  # zzcustomtemp
no_simulation_markers|python|x = 1  # simulated response|frobnicat|x = 1  # frobnicated
no_dead_code|python|if True:\n    x = 1|zzdeadmark|zzdeadmark = 1
no_debug_artifacts|python|import pdb|zzdebugmark|zzdebugmark()
no_unsafe_deserialization|python|import pickle\nv = pickle.loads(b)|zzunsafe_load|v = zzunsafe_load(b)
no_sql_injection|python|q = "SELECT * FROM t WHERE id = '" + uid|ZZSQLMARK|q = "ZZSQLMARK"
no_path_traversal|python|x = base/../secret|zzpathmark|x = zzpathmark
EOF

expect CA-C no no_temporary_code python "$NEVER" $'def add(a, b):\n    return a + b' "CONTROL: a clean snippet is not flagged"
expect CA-R no no_temporary_code python ', "patterns": ["zz_never_matches_zz"]' 'x = 1  # for now' "PINNED: patterns replaces the built-in list"

mkdir -p "$W/run"
printf '{"version":"3.0","mode":"enforce","code_quality":{"no_temporary_code":{"level":"hard","custom_patterns":["zz_never_matches_zz"]}}}\n' > "$W/run/govern.json"
cat > "$W/run/p.naab" <<'EOF'
main {
    let r = <<python
x = 41  # for now
x + 1
>>
    print("RAN=" + string(r))
}
EOF
OUT=$(cd "$W/run" && "$NAAB" run p.naab 2>&1); RC=$?
if [ "$RC" -eq 3 ] && ! grep -q '^RAN=' <<<"$OUT" && grep -q 'code_quality.no_temporary_code' <<<"$OUT"; then
    ok CA-RUN "the record's reproduction is refused (exit 3) with a custom pattern set"
else bad CA-RUN "the block ran (rc=$RC)" "$OUT"; fi

echo
echo "custom_patterns add: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
