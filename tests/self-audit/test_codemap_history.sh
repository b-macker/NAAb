#!/usr/bin/env bash
# ============================================================
# test_codemap_history.sh -- `codemap history` gathers the history pass
#
# tools/codemap/githist.py answers, for one function, which commits changed its
# lines (git log -L), where its name came and went (git log -S, with the lines
# added and removed), who wrote the lines as they are (blame), which PRs, and
# which records cite it. Every answer is checked on a git repository this test
# builds, so the expected commits are known by construction:
#
#   C1  add sink, gate, helper, tail_fn, tail2        (src/a.cpp)
#   C2  tighten gate (#12)       gate's body; adds the A7 row and a plan note
#   C3  sink doubles (#13)       sink's body only
#   C4  remove helper (#14)      deletes helper
#   C5  add other above gate (#15)   three new lines directly above gate
#   C5b edit mover (#19), C5c move mover to the end (#20)   (below gate)
#   C5d donor computes a checksum (#22), C5e move the checksum into blend (#21)
#   C6  script calls helper (#16)    scripts/run.sh, outside the default paths
#   C7  add gatekeeper (#17)     a longer identifier containing "gate"
#   then, UNCOMMITTED: four comment lines at the top of src/a.cpp
#
#   H-01  gate's lines: C2 and C1, and NOT C3 (sink) or C5 (inserted above it);
#         control H-01c: sink's lines do list C3, so its absence above is real
#   H-02  the working-tree range is mapped onto HEAD: four uncommitted lines
#         above gate do not shift it onto tail_fn (whose history is C1 alone);
#         a range of only uncommitted lines has no history, said as such (b)
#   H-03  `--name helper`: added in C1, REMOVED in C4 (#14); control: gate is
#         never reported removed
#   H-04  PRs are read from the commits found: gate -> #12 only
#   H-05  blame: gate's three lines come from two commits, one line from C2
#   H-06  records: the A7 table row and the heading of the plan note are cited
#         for gate; tail2, mentioned nowhere, has none
#   H-07  a shallow clone is UNMEASURABLE in part and its cut is labelled, not
#         read as where the lines began (control: the full repo says neither)
#   H-08  outside a git checkout: UNMEASURABLE, not "no history"
#   H-09  the default paths skip scripts/ (C6 not listed); --all-paths finds it
#   H-10  a commit that changed "gate" only inside "gatekeeper" (C7) is counted
#         apart, not listed as a change to gate
#   H-11  the indexed symbol (`history gate`) gives the same line history as
#         its range -- needs libclang, else UNMEASURABLE
#   H-12  a pickaxe that returns nothing for a name HEAD still holds is a
#         broken probe: UNMEASURABLE, not NONE (the git call is forced empty)
#   H-13  a name absent from HEAD and from every commit reads NONE, scoped --
#         the control that NONE is reachable at all
#   H-14  git log -L stops at a block MOVE (C5c), which then looks like the
#         origin; move-aware blame names the commits it lost (C1, C5b #19) and
#         the output says MOVED (control: gate, never moved, does not)
#   H-16  one line moved into blend from donor (C5d, via C5e) is reported as
#         1 of 6 lines moved in -- NOT as the function having arrived from
#         elsewhere, which H-14's mover (5 of 5) is
#   H-15  --limit caps the name search only: gate's line history with
#         --limit 1 still lists both commits and reports no move (a capped
#         -L list made every commit past the cap look lost to a move -- found
#         on agentSend, 2,508 of 3,927 lines, before the cap was removed)
#   H-07b in the shallow clone, `--name helper` (removed before the cut) is
#         UNMEASURABLE, where the full repository shows it REMOVED (H-03)
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

echo "=== codemap history gathers the history pass ==="

if ! command -v python3 >/dev/null 2>&1 || ! command -v git >/dev/null 2>&1; then
    skip H-ALL "python3 or git not found"
    finish
fi

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
R="$W/repo"
mkdir -p "$R/src" "$R/docs" "$R/scripts"
cd "$R" || exit 1
CM="$(native_path "$REPO/tools/codemap/codemap.py")"
cm() { python3 "$CM" --repo . --cache .cache/codemap "$@" 2>&1; }
g() { git -c user.name=fixture -c user.email=fixture@example.invalid -c commit.gpgsign=false "$@"; }

git init -q . || { skip H-ALL "git init failed"; finish; }
git config core.autocrlf false        # line numbers are the subject; no conversion
printf '.cache/\n' > .gitignore
cat > src/a.cpp <<'EOF'
int sink(int x) {
    return x;
}   // sink
int gate(int x) {
    return x > 0;
}   // gate
int helper(int x) {
    return x * 2;
}   // helper
int mover(int x) {
    int doubled = x * 2;
    int shifted = doubled + 7;
    return shifted - 3;
}   // mover
int tail_fn(int x) {
    return x;
}   // tail_fn
int tail2(int x) {
    return x;
}   // tail2
int donor(int x) {
    return x + 5;
}   // donor
int blend(int x) {
    int base = x + 1;
    int scaled = base * 3;
    return scaled;
}   // blend
EOF
printf '# Plan\n\n## Sink notes\n\nNothing yet.\n' > docs/plan.md
g add -A && g commit -q -m "add sink, gate, helper, tail_fn, tail2"

sed -i.bak 's/return x > 0;/return x > 1;/' src/a.cpp && rm -f src/a.cpp.bak
printf '# Open investigations\n\n| ID | Finding |\n|----|---------|\n| A7 | gate() lets zero through |\n' > docs/open-investigations.md
printf '\n## Gate design\n\nThe gate function runs before anything reaches the sink.\n' >> docs/plan.md
g add -A && g commit -q -m "tighten gate (#12)"

sed -i.bak '2s/return x;/return x * 2;/' src/a.cpp && rm -f src/a.cpp.bak
g add -A && g commit -q -m "sink doubles (#13)"

python3 - <<'EOF'
p = "src/a.cpp"
s = open(p).read()
s = s.replace("int helper(int x) {\n    return x * 2;\n}   // helper\n", "")
s = s.replace("int gate(int x) {", "int other(int x) {\n    return x - 1;\n}   // other\nint gate(int x) {", 1)
open(p, "w").write(s)
EOF
# C4 and C5 as separate commits: undo the insertion, commit the removal, redo it
python3 - <<'EOF'
p = "src/a.cpp"
s = open(p).read()
open(p + ".c5", "w").write(s)
open(p, "w").write(s.replace("int other(int x) {\n    return x - 1;\n}   // other\n", ""))
EOF
g add src/a.cpp && g commit -q -m "remove helper (#14)"
mv src/a.cpp.c5 src/a.cpp
g add src/a.cpp && g commit -q -m "add other above gate (#15)"

sed -i.bak 's/doubled + 7/doubled + 9/' src/a.cpp && rm -f src/a.cpp.bak
g add -A && g commit -q -m "edit mover (#19)"
python3 - <<'EOF'
p = "src/a.cpp"
s = open(p).read()
blk = s[s.index("int mover(int x) {"):s.index("}   // mover\n") + len("}   // mover\n")]
open(p, "w").write(s.replace(blk, "") + blk)
EOF
g add -A && g commit -q -m "move mover to the end (#20)"

python3 - <<'EOF'
p = "src/a.cpp"
s = open(p).read()
open(p, "w").write(s.replace("int donor(int x) {\n", "int donor(int x) {\n    long checksum_value = (x * 31 + 17) ^ (x >> 3) + 424242;\n", 1))
EOF
g add -A && g commit -q -m "donor computes a checksum (#22)"
python3 - <<'EOF'
p = "src/a.cpp"
s = open(p).read()
line = "    long checksum_value = (x * 31 + 17) ^ (x >> 3) + 424242;\n"
s = s.replace(line, "", 1)
open(p, "w").write(s.replace("    int scaled = base * 3;\n", "    int scaled = base * 3;\n" + line, 1))
EOF
g add -A && g commit -q -m "move the checksum into blend (#21)"

printf '#!/bin/sh\n# calls helper from the old build\n' > scripts/run.sh
g add -A && g commit -q -m "script calls helper (#16)"

printf 'int gatekeeper(int x) {\n    return x;\n}   // gatekeeper\n' >> src/a.cpp
g add -A && g commit -q -m "add gatekeeper (#17)"

# uncommitted: four lines above everything
python3 - <<'EOF'
p = "src/a.cpp"
s = open(p).read()
open(p, "w").write("// uncommitted 1\n// uncommitted 2\n// uncommitted 3\n// uncommitted 4\n" + s)
EOF

ln_of() { grep -n "$1" src/a.cpp | head -1 | cut -d: -f1; }
GS="$(ln_of '^int gate(')"; GE=$((GS + 2))
SS="$(ln_of '^int sink(')"; SE=$((SS + 2))
TS="$(ln_of '^int tail_fn(')"
NCOMMITS="$(git rev-list --count HEAD)"
if [ "$NCOMMITS" -ne 11 ] || [ "$GS" -ne 11 ]; then
    bad H-00 "fixture repository not as designed (commits=$NCOMMITS, gate at $GS)" "$(git log --oneline)"
    finish
fi

# ---------------------------------------------------------------- H-01 / H-02 / H-04 / H-05
GATE="$(cm history --range "src/a.cpp:$GS-$GE")"
SINK="$(cm history --range "src/a.cpp:$SS-$SE")"
if grep -q "tighten gate (#12)" <<<"$GATE" && grep -q "add sink, gate, helper" <<<"$GATE" \
   && ! grep -q "sink doubles" <<<"$GATE" && ! grep -q "add other above gate" <<<"$GATE"; then
    ok H-01 "gate's lines were changed by C2 and C1 only -- not by the sink edit or the insertion above it"
else
    bad H-01 "gate's line history is wrong" "$GATE"
fi
if grep -q "sink doubles (#13)" <<<"$SINK"; then
    ok H-01c "control: sink's lines list C3, so its absence from gate's history is real"
else
    bad H-01c "control: sink's history does not show C3; H-01 cannot fail" "$SINK"
fi
if grep -q "(lines 7-9 at HEAD)" <<<"$GATE" && grep -q "tighten gate" <<<"$GATE"; then
    ok H-02 "four uncommitted lines above gate do not shift its range: lines $GS-$GE are 7-9 at HEAD"
else
    bad H-02 "the working-tree range was not mapped onto HEAD" "$GATE"
fi
UNC="$(cm history --range "src/a.cpp:1-4")"
if grep -q "all 4 lines are uncommitted" <<<"$UNC"; then
    ok H-02b "a range of only uncommitted lines is said to have no history yet"
else
    bad H-02b "uncommitted lines were given a history" "$UNC"
fi
if grep -q "PRs: #12$" <<<"$GATE"; then
    ok H-04 "the PRs are those of the commits found: #12 for gate"
else
    bad H-04 "PR list wrong" "$(grep 'PRs:' <<<"$GATE")"
fi
if grep -q "the 3 lines at HEAD were written by 2 commits" <<<"$GATE" \
   && grep -qE "^ +1 lines .*tighten gate" <<<"$GATE"; then
    ok H-05 "blame: gate's three lines come from two commits, one line from C2"
else
    bad H-05 "blame summary wrong" "$(grep -A3 'blame:' <<<"$GATE")"
fi

# ---------------------------------------------------------------- H-03 / H-06 / H-09 / H-10
HELP="$(cm history --name helper)"
GN="$(cm history --name gate)"
if grep -qE "\+[0-9]+ +-0 +add sink, gate, helper" <<<"$HELP" \
   && grep -qE "\+0 +-[1-9][0-9]* +remove helper \(#14\)  REMOVED here" <<<"$HELP" \
   && ! grep -q "REMOVED here" <<<"$GN"; then
    ok H-03 "helper: added in C1, REMOVED in C4 (#14); gate is never reported removed"
else
    bad H-03 "add/remove not read from the name's history" "$HELP
$GN"
fi
T2="$(cm history --name tail2)"
if grep -q "docs/open-investigations.md:5 \[A7\]" <<<"$GN" && grep -q "docs/plan.md:[0-9]* (Gate design)" <<<"$GN" \
   && grep -q "records: 0 mentions" <<<"$T2"; then
    ok H-06 "records: gate's A7 row and its plan heading are cited; tail2 has none"
else
    bad H-06 "records not found or not attributed" "$(grep -A4 'records:' <<<"$GN")
$(grep 'records:' <<<"$T2")"
fi
ALLP="$(cm history --name helper --all-paths)"
if ! grep -q "script calls helper" <<<"$HELP" && grep -q "script calls helper (#16)" <<<"$ALLP"; then
    ok H-09 "the default paths leave out scripts/; --all-paths finds C6 there"
else
    bad H-09 "the search paths were not applied" "$HELP
$ALLP"
fi
if ! grep -q "add gatekeeper" <<<"$GN" && grep -q "1 more changed it only inside longer names" <<<"$GN"; then
    ok H-10 "a commit that changed 'gate' only inside 'gatekeeper' is counted apart, not listed"
else
    bad H-10 "a longer identifier was read as the name" "$GN"
fi

# ---------------------------------------------------------------- H-07
if grep -q "shallow clone" <<<"$GATE" || grep -q "SHALLOW CUT" <<<"$GATE"; then
    bad H-07 "control: the full repository was reported shallow" "$GATE"
elif git clone -q --depth 1 "file://$(native_path "$R")" "$W/shallow" >/dev/null 2>&1; then
    (cd "$W/shallow" && printf '// uncommitted 1\n// uncommitted 2\n// uncommitted 3\n// uncommitted 4\n' \
        | cat - src/a.cpp > src/a.tmp && mv src/a.tmp src/a.cpp)
    SH="$(cd "$W/shallow" && python3 "$CM" --repo . --cache .cache/codemap history --range "src/a.cpp:$GS-$GE" 2>&1)"
    if grep -q "UNMEASURABLE in part: this is a shallow clone" <<<"$SH" \
       && grep -q "add gatekeeper (#17).*SHALLOW CUT" <<<"$SH"; then
        ok H-07 "a shallow clone is UNMEASURABLE in part and its cut is labelled, not taken as the origin"
    else
        bad H-07 "a shallow clone's cut was read as history" "$SH"
    fi
    SHN="$(cd "$W/shallow" && python3 "$CM" --repo . --cache .cache/codemap history --name helper 2>&1)"
    if grep -q "name: UNMEASURABLE -- 'helper': history is truncated" <<<"$SHN" && ! grep -q "name: NONE" <<<"$SHN"; then
        ok H-07b "in a shallow clone a removed name is UNMEASURABLE, not NONE"
    else
        bad H-07b "a shallow clone's empty pickaxe was read as NONE" "$SHN"
    fi
else
    skip H-07 "could not make a shallow clone here"
fi

# ---------------------------------------------------------------- H-12 / H-13
BROKEN="$(python3 - "$(native_path "$REPO/tools/codemap")" <<'EOF' 2>&1
import sys
sys.path.insert(0, sys.argv[1])
import githist, codemap
real = githist.git
def git(repo, *args):
    if args and args[0] == "log" and any(a.startswith("-S") for a in args):
        return ""                          # the probe fails silently
    return real(repo, *args)
githist.git = git
codemap.main(["--repo", ".", "--cache", ".cache/codemap", "history", "--name", "gate"])
EOF
)"
if grep -q "name: UNMEASURABLE -- 'gate': present at HEAD in 3 file(s) but no commit changed its count" <<<"$BROKEN"; then
    ok H-12 "an empty pickaxe for a name HEAD still holds is reported as a broken probe"
else
    bad H-12 "a failed probe was reported as a finding" "$BROKEN"
fi
NONE="$(cm history --name noSuchIdentifierEverWritten)"
if grep -q "name: NONE -- no commit in" <<<"$NONE" && grep -q "scoped NONE covers only these paths" <<<"$NONE"; then
    ok H-13 "a name in no commit and not at HEAD reads NONE, with its scope"
else
    bad H-13 "control: NONE is not reachable" "$NONE"
fi

# ---------------------------------------------------------------- H-14
MS="$(ln_of '^int mover(')"
MOV="$(cm history --range "src/a.cpp:$MS-$((MS + 4))")"
if grep -q "move mover to the end (#20)" <<<"$MOV" \
   && grep -q "MOVED IN: 5 of 5 lines came from elsewhere.*the oldest commit above is where it arrived" <<<"$MOV" \
   && grep -A3 "MOVED IN:" <<<"$MOV" | grep -q "edit mover (#19)" && grep -q "PRs: .*#19" <<<"$MOV" \
   && ! grep -q "MOVED" <<<"$GATE"; then
    ok H-14 "a block move stops -L; move-aware blame names the lost commits (#19) and the output says MOVED"
else
    bad H-14 "the history before a move was lost, or gate was called moved" "$MOV"
fi

BS="$(ln_of '^int blend(')"
BL="$(cm history --range "src/a.cpp:$BS-$((BS + 5))")"
if grep -q "MOVED IN: 1 of 6 lines came from elsewhere" <<<"$BL" && ! grep -q "where it arrived" <<<"$BL" \
   && grep -A2 "MOVED IN:" <<<"$BL" | grep -q "donor computes a checksum (#22)"; then
    ok H-16 "one line moved in is reported as one line, not as the function arriving from elsewhere"
else
    bad H-16 "a partial move was misreported" "$BL"
fi
LIM="$(cm history --range "src/a.cpp:$GS-$GE" --limit 1)"
if grep -q "lines: 2 commits changed these lines" <<<"$LIM" && ! grep -q "MOVED" <<<"$LIM"; then
    ok H-15 "--limit 1 does not cap the line history or invent a move"
else
    bad H-15 "a capped line history was compared with blame" "$LIM"
fi

# ---------------------------------------------------------------- H-08
mkdir -p "$W/plain"
OUT="$(cd "$W/plain" && python3 "$CM" --repo . --cache .cache/codemap history --name gate 2>&1)"; rc=$?
if [ "$rc" -ne 0 ] && grep -q "UNMEASURABLE" <<<"$OUT" && grep -q "not a git checkout" <<<"$OUT"; then
    ok H-08 "outside a git checkout the history is UNMEASURABLE, not empty"
else
    bad H-08 "a directory without git was answered (rc=$rc)" "$OUT"
fi

# ---------------------------------------------------------------- H-11
if ! grep -q LIBCLANG_OK <<<"$(python3 -c "import clang.cindex as ci; ci.Index.create(); print('LIBCLANG_OK')" 2>&1)"; then
    skip H-11 "libclang Python bindings unavailable (python3-clang); the symbol form is not measured"
else
    mkdir -p build
    python3 - <<'EOF'
import json, os
w = os.getcwd()
json.dump([{"directory": os.path.join(w, "build"), "file": os.path.join(w, "src", "a.cpp"),
            "arguments": ["c++", "-std=c++17", "-c", os.path.join(w, "src", "a.cpp"), "-o", "a.o"]}],
          open(os.path.join(w, "build", "compile_commands.json"), "w"))
EOF
    IDX="$(cm index --jobs 1)"; irc=$?
    SYM="$(cm history gate)"
    if [ "$irc" -ne 0 ]; then
        skip H-11 "the fixture did not index here (rc=$irc)"
    elif grep -q "history of gate  (src/a.cpp:$GS-" <<<"$SYM" && grep -q "tighten gate (#12)" <<<"$SYM" \
         && ! grep -q "sink doubles" <<<"$SYM" && grep -q "records: .* mentions" <<<"$SYM"; then
        ok H-11 "the indexed symbol gives the same line history as its range, plus its name and records"
    else
        bad H-11 "the symbol form disagrees with the range form" "$SYM"
    fi
fi

finish
