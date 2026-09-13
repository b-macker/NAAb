#!/usr/bin/env bash
# ============================================================
# test_shell_path_handoff.sh — a shell path must not cross into native python
#
# THE DEFECT CLASS
#
# A shell variable holding a path, interpolated into python source, is opened
# by PYTHON rather than by the shell. Under MSYS2 `python3` is a NATIVE Windows
# build and cannot open an MSYS "/tmp/..." path, so the call fails on Windows
# and nowhere else.
#
#     python3 -c "... open('$W/fixture.json','w') ..."     <- defect
#     python3 -c "... json.dump(d, sys.stdout)" > "$W/f"   <- safe
#
# The second form is safe because the SHELL performs the open and no path
# crosses the boundary. Same variable, same file, different vocabulary.
#
# WHY A LINT AND NOT A NOTE. This rule is already written down in CLAUDE.md,
# and the fifth instance of it landed ONE COMMIT after the rule was written.
# Prose did not prevent it. The failure is also silent in the worst direction:
# `test_path_precedence.sh` reported "fixture is broken" eleven times while all
# eleven real assertions passed in the same run — the guard became the broken
# probe, and a reader would have chased the fixture instead of trusting it.
#
# WHY awk AND grep, NOT python3. This lint runs in CI including build-windows.
# A python3 implementation handed a list of file PATHS would break under MSYS2
# in exactly the way it exists to detect — the lint would be its own first
# defect. MSYS2's own awk and grep understand MSYS paths; a native python3 does
# not. If you rewrite this in python, feed it bytes on stdin, never filenames.
#
# BASELINE, NOT ZERO. 56 instances exist today across 10 files. A gate that
# fails on the day it lands gets switched off on the day it lands, so this
# fails when the count GROWS. Lowering the baseline is progress and is
# expected; raising it needs a reason in the commit.
#
#   SP-01  the instance count has not grown past the pinned baseline
#   SP-02  POSITIVE CONTROL: the detector fires on a freshly written instance.
#          Without this, a detector broken into silence reports a clean tree
#          and the baseline never moves again
#   SP-03  NEGATIVE CONTROL: a path inside a QUOTED heredoc (<<'PY') is NOT
#          flagged — the shell does not expand there, so there is no handoff.
#          Without this arm, "grep for open(" passes SP-01 and SP-02 while
#          flagging code that is correct
#   SP-04  NEGATIVE CONTROL: the safe remedy (shell redirect) is NOT flagged.
#          This is the form the fix produces, so a detector that flags it makes
#          the defect unfixable
#   SP-05  NEGATIVE CONTROL: a COMMENTED example is not flagged. This file
#          flagged its own header on the first run — a lint that cannot
#          tolerate its own documentation makes the rule unwritable
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO" || exit 1

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; }

# Raise ONLY with a reason. Lowering is progress.
BASELINE_HANDOFFS=56

# Emits "<file>:<line>" for each shell-expanded path handed to python.
# Tracks heredoc quoting so <<'PY' (no expansion) is not counted.
detect() {
    awk '
      # A quoted heredoc introducer: <<'"'"'TAG'"'"' or <<"TAG" -- no expansion inside.
      /<<[[:space:]]*('"'"'[A-Za-z_][A-Za-z_0-9]*'"'"'|"[A-Za-z_][A-Za-z_0-9]*")/ {
          inq = 1
          tag = $0
          sub(/.*<<[[:space:]]*['"'"'"]/, "", tag)
          sub(/['"'"'"].*/, "", tag)
          next
      }
      inq && $0 ~ ("^[[:space:]]*" tag "[[:space:]]*$") { inq = 0; next }
      inq { next }
      # A comment cannot open anything. In shell a leading # is a comment; inside
      # a python -c block a leading # is a python comment. Excluding them is what
      # lets this rule be DOCUMENTED -- including in this file, which flagged its
      # own header example on the first run.
      /^[[:space:]]*#/ { next }
      # A path expanded by the shell and opened by python.
      /(open|Path)\(['"'"'"]?\$/ { print FILENAME ":" FNR }
    ' "$@"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  shell paths handed to native python                          |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

mapfile -t SHFILES < <(find tests examples tools .github -name '*.sh' -type f 2>/dev/null | sort)
if [ "${#SHFILES[@]}" -eq 0 ]; then
    bad "SP-00" "found no shell scripts to scan" "the glob is broken; every arm below would pass vacuously"
    echo ""; exit 1
fi
ok "SP-00" "scanning ${#SHFILES[@]} shell scripts"

COUNT=$(detect "${SHFILES[@]}" | wc -l | tr -d ' ')
echo "  shell-expanded paths handed to python: $COUNT (baseline $BASELINE_HANDOFFS)"
echo ""

if [ "$COUNT" -le "$BASELINE_HANDOFFS" ]; then
    if [ "$COUNT" -lt "$BASELINE_HANDOFFS" ]; then
        ok "SP-01" "count fell to $COUNT (baseline $BASELINE_HANDOFFS) — lower the baseline in this file"
    else
        ok "SP-01" "count has not grown ($COUNT <= $BASELINE_HANDOFFS)"
    fi
else
    bad "SP-01" "new shell-path handoffs introduced ($COUNT > $BASELINE_HANDOFFS)" \
        "$(detect "${SHFILES[@]}" | tail -n $((COUNT - BASELINE_HANDOFFS)))"
    echo -e "       ${YELLOW}Fix: have python write to sys.stdout and let the SHELL redirect:${NC}"
    echo -e "       ${YELLOW}  python3 -c \"...json.dump(d, sys.stdout)\" > \"\$W/file.json\"${NC}"
fi

# ---- controls -----------------------------------------------------------
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/positive.sh" <<'FIXTURE_EOF'
#!/usr/bin/env bash
W=/tmp/x
python3 -c "
import json
json.dump({'a':1}, open('$W/fixture.json','w'))"
FIXTURE_EOF
if [ "$(detect "$TMP/positive.sh" | wc -l | tr -d ' ')" -eq 1 ]; then
    ok "SP-02" "POSITIVE CONTROL: detector fires on a freshly written instance"
else
    bad "SP-02" "POSITIVE CONTROL: detector fires on a freshly written instance" \
        "detector is silent — a broken detector reports a clean tree forever"
fi

cat > "$TMP/quoted.sh" <<'FIXTURE_EOF'
#!/usr/bin/env bash
python3 <<'PY'
import json
json.dump({'a':1}, open('$W/fixture.json','w'))
PY
FIXTURE_EOF
if [ "$(detect "$TMP/quoted.sh" | wc -l | tr -d ' ')" -eq 0 ]; then
    ok "SP-03" "NEGATIVE CONTROL: a quoted heredoc is not flagged (no shell expansion)"
else
    bad "SP-03" "NEGATIVE CONTROL: a quoted heredoc is not flagged" \
        "flagged code the shell never expands — this is a plain grep, not a detector"
fi

cat > "$TMP/safe.sh" <<'FIXTURE_EOF'
#!/usr/bin/env bash
W=/tmp/x
python3 -c "
import json, sys
json.dump({'a':1}, sys.stdout)" > "$W/fixture.json"
FIXTURE_EOF
if [ "$(detect "$TMP/safe.sh" | wc -l | tr -d ' ')" -eq 0 ]; then
    ok "SP-04" "NEGATIVE CONTROL: the safe remedy is not flagged"
else
    bad "SP-04" "NEGATIVE CONTROL: the safe remedy is not flagged" \
        "the fix itself trips the lint — the defect would be unfixable"
fi

cat > "$TMP/commented.sh" <<'FIXTURE_EOF'
#!/usr/bin/env bash
# Documenting the defect must not BE the defect:
#     python3 -c "... open('$W/fixture.json','w') ..."
echo hi
FIXTURE_EOF
if [ "$(detect "$TMP/commented.sh" | wc -l | tr -d ' ')" -eq 0 ]; then
    ok "SP-05" "NEGATIVE CONTROL: a commented example is not flagged"
else
    bad "SP-05" "NEGATIVE CONTROL: a commented example is not flagged" \
        "the rule cannot be written down without tripping its own lint — this file flagged its own header on the first run"
fi

echo ""
echo -e "${CYAN}--------------------------------------------------------------${NC}"
echo -e "  Passed: ${GREEN}${PASS}${NC}   Failed: ${RED}${FAIL}${NC}"
[ "$FAIL" -gt 0 ] && { echo ""; exit 1; }
echo -e "  ${GREEN}ALL PASSED${NC}"; echo ""; exit 0
