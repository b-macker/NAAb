#!/usr/bin/env bash
# ============================================================
# test_langconform.sh -- what governance sees, per language, cannot change
# unnoticed
#
# tools/langconform/langconform.py places each payload in each syntactic
# position (code, every common comment and string form) for every language the
# binary registers, and records which rules fired. tools/langconform/baseline.json
# is that matrix for the current engine. Consolidating per-language knowledge
# (one descriptor table instead of 540 scattered comparisons) changes the
# matrix in both directions; this test makes every change show up as a
# reviewed edit to the baseline in the same commit, so nothing loosens
# silently.
#
#   LC-00  the binary lists its languages (UNMEASURABLE without python3 or
#          naab-gov)
#   LC-01  POSITIVE CONTROL: every payload fires a real rule somewhere, and
#          the keyword payload fires as active code in EVERY language -- a
#          probe that cannot fire measures nothing
#   LC-02  the current binary's matrix equals the committed baseline
#   LC-03  CONTROL: diff reports a planted lost finding, and exits non-zero
#   LC-04  CONTROL: groups reports a planted alias disagreement
#
# To accept a deliberate change: run
#   python3 tools/langconform/langconform.py snapshot --gov build/naab-gov \
#       --naab build/naab-lang --out tools/langconform/baseline.json
# and say in the commit which findings appeared or disappeared, and why.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
# Relative paths from the repo root: under MSYS2 a native python cannot open
# an MSYS absolute path, and the tool resolves these against its own cwd.
cd "$REPO" || exit 1
TOOL=tools/langconform/langconform.py

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -40 | sed 's/^/       | /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== per-language governance matrix ==="

if ! command -v python3 >/dev/null 2>&1; then
    skip LC-00 "python3 unavailable -- the matrix is UNMEASURABLE"
    echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi
if [ ! -x build/naab-gov ] && [ ! -x build/naab-gov.exe ]; then
    skip LC-00 "build/naab-gov not built -- the matrix is UNMEASURABLE"
    echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

# Scratch files live under the repo's build dir and are named relatively, so
# no MSYS path crosses into the native python.
W="build/langconform-test.$$"
mkdir -p "$W"
trap 'rm -rf "$W"' EXIT

# --- LC-00 ---
langs="$(python3 "$TOOL" languages --naab build/naab-lang 2>&1)"; rc=$?
n=$(printf '%s\n' "$langs" | tr -d '\r' | grep -c .)
if [ $rc -eq 0 ] && [ "$n" -ge 2 ]; then
    ok LC-00 "the binary lists $n languages"
else
    bad LC-00 "could not list languages (rc=$rc)" "$langs"
fi

# --- snapshot of the current binary ---
snap_out="$(python3 "$TOOL" snapshot --gov build/naab-gov --naab build/naab-lang --out "$W/now.json" 2>&1)"; src=$?

# --- LC-01 ---
if [ $src -ne 0 ]; then
    bad LC-01 "the snapshot did not complete (rc=$src)" "$snap_out"
else
    ctl="$(python3 - "$W/now.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1], encoding="ascii", errors="strict"))
C, bad = s["cells"], []
for pay in s["payloads"]:
    if not any(k.endswith("|" + pay) and any(r != "languages.allowed" for r in v) for k, v in C.items()):
        bad.append("payload %s fires no rule anywhere" % pay)
for lang in s["languages"]:
    if "code_quality.no_hallucinated_apis" not in C.get("%s|code|keyword" % lang, []):
        bad.append("keyword as active code is not reported in %s" % lang)
sys.stdout.write("\n".join(bad))
PY
)"
    if [ -z "$ctl" ]; then
        ok LC-01 "every payload fires; the keyword is caught as code in every language"
    else
        bad LC-01 "a probe cannot fire, so the matrix measures nothing there" "$ctl"
    fi
fi

# --- LC-02 ---
if [ $src -eq 0 ]; then
    d="$(python3 "$TOOL" diff tools/langconform/baseline.json "$W/now.json" 2>&1)"; drc=$?
    if [ $drc -eq 0 ]; then
        ok LC-02 "the matrix matches the committed baseline ($(printf '%s' "$snap_out" | tr -d '\r' | sed -n 's/.*: \([0-9]* cells\).*/\1/p'))"
    else
        bad LC-02 "what governance sees per language CHANGED -- review, then regenerate the baseline (see header)" "$d"
    fi
else
    bad LC-02 "no snapshot to compare"
fi

# --- LC-03 ---
python3 - tools/langconform/baseline.json "$W/planted.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1], encoding="ascii", errors="strict"))
k = "python|code|keyword"
s["cells"][k] = [r for r in s["cells"][k] if r != "code_quality.no_hallucinated_apis"]
open(sys.argv[2], "wb").write((json.dumps(s, sort_keys=True, indent=1) + "\n").encode("ascii"))
PY
d3="$(python3 "$TOOL" diff tools/langconform/baseline.json "$W/planted.json" 2>&1)"; d3rc=$?
case "$d3" in
  *"- python|code|keyword"*"code_quality.no_hallucinated_apis"*"1 finding(s) disappeared"*)
      [ $d3rc -ne 0 ] && ok LC-03 "a planted lost finding is reported, exit $d3rc" \
                      || bad LC-03 "reported, but exit 0" "$d3" ;;
  *) bad LC-03 "a planted lost finding was not reported" "$d3" ;;
esac

# --- LC-04 ---
python3 - tools/langconform/baseline.json "$W/group.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1], encoding="ascii", errors="strict"))
s["cells"]["golang|code|keyword"] = []
open(sys.argv[2], "wb").write((json.dumps(s, sort_keys=True, indent=1) + "\n").encode("ascii"))
PY
g="$(python3 "$TOOL" groups "$W/group.json" 2>&1)"
case "$g" in
  *"go/golang code|keyword"*) ok LC-04 "a planted go/golang disagreement is reported" ;;
  *) bad LC-04 "a planted alias disagreement was not reported" "$g" ;;
esac

# Report only: alias groups that disagree today (sql/sqlite on 2026-10-05).
echo "  info: $(python3 "$TOOL" groups tools/langconform/baseline.json 2>&1 | tail -1)"

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
