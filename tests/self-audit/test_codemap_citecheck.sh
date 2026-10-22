#!/usr/bin/env bash
# ============================================================
# test_codemap_citecheck.sh -- `codemap cite-check DOC` (roadmap item 4)
#
# tools/codemap/citecheck.py judges each `path:line` citation in a doc by the
# backticked names beside it, never by the number. HOLDS is the reassuring answer,
# so the arms that could make it lie get the most controls. A fixture repo and doc
# put one citation per outcome on its own line; each arm reads the outcome at that
# line (--json), beside a control that must come out the other way.
#
#   CC-01  HOLDS: alpha_fn cited at its line; control (c) the same name cited at a
#          stale line reads MOVED, not HOLDS
#   CC-02  MOVED to the one line that holds the name
#   CC-03  GONE: the name occurs nowhere in the file
#   CC-04  AMBIGUOUS: the name occurs twice, neither near the cited line
#   CC-05  FILE GONE: no tracked file has the path
#   CC-06  UNCHECKABLE: no backticked name beside the citation; control: the same
#          citation with a name is checked (CC-01)
#   CC-07  a name that occurs more than COMMON_IN_FILE times places nothing
#          (AMBIGUOUS, "too common"), even next to the cited line; control (c): a
#          name as near but rare HOLDS -- found when `code_quality` (137 times in
#          governance_checks.cpp) confirmed a stale citation
#   CC-08  each citation is judged by the names in its own segment: in one
#          sentence, rare_one (once, line 8) beside a.cpp:50 and stale_name
#          (nowhere) beside a.cpp:60 -- a.cpp:60 reads GONE, not MOVED to 8
#   CC-09  a `:N` continuation is judged in the file named before it; a table cell
#          with a pipe inside a code span keeps its backticks paired
#   CC-10  a citation in a code block is not a citation; control: the same outside
#   CC-11  a URL is not a citation
#   CC-12  the "`file`, ~line N" form; a backticked file name is an anchor for a
#          citation of ANOTHER file (a suite's registration line), and a directory
#          span gives no names
#   CC-13  a bare file name two tracked files share is judged in the one that
#          holds the name
#   CC-14  the doc is never edited
#
# Needs python3 and git; missing either is UNMEASURABLE.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$REPO/tests/helpers/native_path.sh"
export PYTHONDONTWRITEBYTECODE=1

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | sed 's/^/       | /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }
finish() { echo "Result: $PASS passed, $FAIL failed, $SKIP skipped"; [ "$FAIL" -eq 0 ]; exit $?; }

echo "=== codemap cite-check judges citations by name, not number ==="
if ! command -v python3 >/dev/null 2>&1 || ! command -v git >/dev/null 2>&1; then
    skip CC-ALL "python3 or git not found"; finish
fi

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
cd "$W" || exit 1
git init -q . && git config core.autocrlf false
mkdir -p src other tests/dir
# a.cpp: alpha_fn at 3, beta_fn at 10, delta_fn at 12 and 20, rare_one at 8,
# epsilon_fn at 40, common_name on 12 lines (21-32), near_rare at 33
{
    printf '// a.cpp\n\nint alpha_fn() { return 1; }\n\n\n\n\nint rare_one = 0;\n\nint beta_fn() { return 2; }\n\n'
    printf 'int delta_fn(int);\n'                                  # 12
    for i in $(seq 13 19); do printf '//\n'; done
    printf 'int delta_fn(int x) { return x; }\n'                   # 20
    for i in $(seq 21 32); do printf 'int v%d = common_name;\n' "$i"; done   # 21..32: 12 lines
    printf 'int near_rare = 0;\n'                                 # 33
    for i in $(seq 34 39); do printf '//\n'; done
    printf 'int epsilon_fn() { return 5; }\n'                      # 40
} > src/a.cpp
printf '// b.cpp\n\nint b_one() { return 1; }\n\n\n\nint b_two() { return 2; }\n' > src/b.cpp     # b_one 3, b_two 7
printf '#!/bin/sh\n# runner\n\n\n\n\n\n\nbash tests/dir/test_x.sh\n' > run.sh                 # test_x.sh at 9
printf 'int dup_name() { return 1; }\n' > src/dup.cpp
printf '// another dup.cpp without the name\nint other() { return 0; }\n' > other/dup.cpp
cat > DOC.md <<'EOF'
# Fixture doc

`alpha_fn()` is defined at `src/a.cpp:3`.

`alpha_fn()` is cited stale at `src/a.cpp:30`.

`beta_fn()` sits at `src/a.cpp:1`.

`gamma_fn()` was at `src/a.cpp:5`.

`delta_fn` is at `src/a.cpp:1`.

`zeta_fn()` is at `src/missing.cpp:4`.

See src/a.cpp:3 for details.

`common_name` is used at `src/a.cpp:28`.

`near_rare` is used at `src/a.cpp:28`.

`rare_one` (`src/a.cpp:50`) and `stale_name` (`src/a.cpp:60`) are named together.

`b_one()` and `b_two()` are at `src/b.cpp:3`, `:7`.

| Id | Note |
|---|---|
| T1 | `x|y` then `alpha_fn` at `src/a.cpp:3` |

```
`beta_fn()` in an example at `src/a.cpp:99`
```

`epsilon_fn()` outside the example at `src/a.cpp:99`.

See https://example.com/src/a.cpp:3 for the upstream copy.

`epsilon_fn` in `a.cpp`, ~line 40.

`test_x.sh` in `tests/dir/` is registered at `run.sh:2`.

`dup_name()` is at `dup.cpp:1`.
EOF
git add -A && git -c user.name=f -c user.email=f@example.invalid -c commit.gpgsign=false commit -q -m fixture
BEFORE="$(git hash-object DOC.md)"

CM="$(native_path "$REPO/tools/codemap/codemap.py")"
J="$(python3 "$CM" --repo . --cache "$W/.cache" --json cite-check DOC.md 2>&1)"
# outcome[,detail] for each citation, keyed by the doc line it is on
at() { python3 -c 'import json, sys
d = json.load(sys.stdin)
for r in d["citations"]:
    if r["doc_line"] == int(sys.argv[1]):
        print("%s|%s|%s|%s" % (r["outcome"], r.get("new_line", ""), r["cited"], r["line"]))' "$1" <<<"$J" 2>&1; }
line_of() { grep -n -F -- "$1" DOC.md | head -1 | cut -d: -f1; }
if ! python3 -c 'import json, sys; json.load(sys.stdin)' <<<"$J" >/dev/null 2>&1; then
    bad CC-00 "cite-check did not produce JSON" "$J"; finish
fi

L1="$(line_of 'is defined at')"; L1c="$(line_of 'cited stale')"
if [ "$(at "$L1")" = "HOLDS||src/a.cpp|3" ] && [ "$(at "$L1c")" = "MOVED|3|src/a.cpp|30" ]; then
    ok CC-01 "a name at its cited line HOLDS; control: the same name cited at a stale line reads MOVED -> 3"
else
    bad CC-01 "HOLDS / its stale control wrong" "$(at "$L1") / $(at "$L1c")"
fi
R="$(at "$(line_of 'sits at')")"
[ "$R" = "MOVED|10|src/a.cpp|1" ] && ok CC-02 "MOVED to the one line holding beta_fn" || bad CC-02 "MOVED wrong" "$R"
R="$(at "$(line_of 'was at')")"
[ "$R" = "GONE||src/a.cpp|5" ] && ok CC-03 "a name nowhere in the file reads GONE" || bad CC-03 "GONE wrong" "$R"
R="$(at "$(line_of '`delta_fn` is at')")"
[ "$R" = "AMBIGUOUS||src/a.cpp|1" ] && ok CC-04 "a name in two places, neither near, reads AMBIGUOUS" || bad CC-04 "AMBIGUOUS wrong" "$R"
R="$(at "$(line_of 'missing.cpp')")"
[ "$R" = "FILE GONE||src/missing.cpp|4" ] && ok CC-05 "an untracked path reads FILE GONE" || bad CC-05 "FILE GONE wrong" "$R"
R="$(at "$(line_of 'for details')")"
[ "$R" = "UNCHECKABLE||src/a.cpp|3" ] && ok CC-06 "a citation with no name beside it is UNCHECKABLE, not passed" || bad CC-06 "UNCHECKABLE wrong" "$R"
R7="$(at "$(line_of '`common_name`')")"; R7c="$(at "$(line_of '`near_rare`')")"
if [ "$R7" = "AMBIGUOUS||src/a.cpp|28" ] && [ "$R7c" = "HOLDS||src/a.cpp|28" ]; then
    ok CC-07 "a name on 12 lines around the cited line places nothing; control: a rare one as near HOLDS"
else
    bad CC-07 "a common name confirmed a line (or the rare control failed)" "$R7 / $R7c"
fi
R8="$(at "$(line_of 'named together')")"
if [ "$R8" = "MOVED|8|src/a.cpp|50
GONE||src/a.cpp|60" ]; then
    ok CC-08 "each citation is judged by its own segment: a.cpp:50 by rare_one (MOVED 8), a.cpp:60 by stale_name (GONE)"
else
    bad CC-08 "names from one citation's segment decided another's" "$R8"
fi
R9="$(at "$(line_of 'b_two')")"; R9t="$(at "$(line_of '| T1 |')")"
if [ "$R9" = "HOLDS||src/b.cpp|3
HOLDS||src/b.cpp|7" ] && [ "$R9t" = "HOLDS||src/a.cpp|3" ]; then
    ok CC-09 "a :N continuation is judged in b.cpp; a pipe inside a code span keeps the cell whole"
else
    bad CC-09 "continuation or table-cell pairing wrong" "$R9 / $R9t"
fi
R10="$(at "$(line_of 'in an example')")"; R10c="$(at "$(line_of 'outside the example')")"
if [ -z "$R10" ] && [ "$R10c" = "MOVED|40|src/a.cpp|99" ]; then
    ok CC-10 "a citation in a code block is skipped; control: the same outside is checked"
else
    bad CC-10 "code-block citation handling wrong" "[$R10] / $R10c"
fi
R11="$(at "$(line_of 'example.com')")"
[ -z "$R11" ] && ok CC-11 "a URL is not a citation" || bad CC-11 "a URL was read as a citation" "$R11"
R12="$(at "$(line_of '~line 40')")"; R12r="$(at "$(line_of 'is registered')")"
if [ "$R12" = "HOLDS||a.cpp|40" ] && [ "$R12r" = "MOVED|9|run.sh|2" ]; then
    ok CC-12 "the ~line form HOLDS; a backticked file name anchors run.sh:2 (MOVED 9); the directory gave no names"
else
    bad CC-12 "~line form or file-name anchor wrong" "$R12 / $R12r"
fi
R13="$(at "$(line_of 'dup_name')")"
[ "$R13" = "HOLDS||dup.cpp|1" ] && ok CC-13 "a shared bare name is judged in the file that holds the name" || bad CC-13 "shared basename wrong" "$R13"
TXT="$(python3 "$CM" --repo . --cache "$W/.cache" cite-check DOC.md 2>&1)"
if [ "$(git hash-object DOC.md)" = "$BEFORE" ] && grep -q "Nothing is edited" <<<"$TXT" \
   && grep -qE "^cite-check DOC.md: [0-9]+ citations -- HOLDS [0-9]+, MOVED" <<<"$TXT"; then
    ok CC-14 "the doc is unchanged, and the text report gives the counts and its screened tier"
else
    bad CC-14 "the doc changed, or the report lost its counts" "$TXT"
fi
finish
