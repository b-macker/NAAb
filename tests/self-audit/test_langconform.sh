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
#   LC-04  CONTROL: groups reports a planted alias disagreement (alias
#          groups come from the binary's own language table)
#   LC-05  probes GENERATED from the binary's language table all pass: every
#          registered name has an entry, and every comment form an entry
#          declares is honoured (a new language or form is covered with no
#          edit here)
#   LC-06  CONTROL: a registered language missing from the table FAILS
#   LC-07  CONTROL: a declared comment form the engine does not honour FAILS
#          (keyword family: comments hidden from code checks)
#   LC-08  CONTROL: the same, for the marker family (markers in comments
#          visible to the comment checks): a fake Python block form <<< >>>
#   LC-09  verify: every comment form the table declares is ACCEPTED as a
#          comment by the language's own installed parser (languages whose
#          toolchain is absent report UNMEASURABLE; at least one must measure)
#   LC-10  CONTROL: a planted wrong fact (Python "--" comments) is WRONG, exit 1
#   LC-11  CONTROL: a removed real fact (Python "#") is rediscovered as proposed
#   LC-12  CONTROL: a string literal posing as a comment (Ruby %{ %}) is WRONG --
#          the verifier cannot be fooled by a form that only starts a string
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

# --- LC-05 ---
c5="$(python3 "$TOOL" conform --gov build/naab-gov --naab build/naab-lang 2>&1)"; c5rc=$?
if [ $c5rc -eq 0 ]; then
    ok LC-05 "$(printf '%s' "$c5" | tr -d '\r' | tail -1 | sed 's/^langconform conform: //')"
else
    bad LC-05 "the binary's language table and its governance disagree" "$c5"
fi

# --- LC-06 / LC-07: planted tables ---
build/naab-gov languages > "$W/table.json"
python3 - "$W/table.json" "$W/no_zig.json" "$W/bogus.json" "$W/fakeblock.json" <<'PY'
import json, sys
t = json.load(open(sys.argv[1], encoding="utf-8", errors="strict"))
open(sys.argv[2], "w", encoding="ascii", newline="\n").write(
    json.dumps([d for d in t if d["canonical"] != "zig"]))
for d in t:
    if d["canonical"] == "python":
        d["line_comments"].append("//")   # not a Python comment; the engine must disagree
open(sys.argv[3], "w", encoding="ascii", newline="\n").write(json.dumps(t))
t = json.load(open(sys.argv[1], encoding="utf-8", errors="strict"))
for d in t:
    if d["canonical"] == "python":
        d["block_comments"].append({"open": "<<<", "close": ">>>", "line_start_only": False})
open(sys.argv[4], "w", encoding="ascii", newline="\n").write(json.dumps(t))
PY
c6="$(python3 "$TOOL" conform --gov build/naab-gov --naab build/naab-lang --table "$W/no_zig.json" 2>&1)"; c6rc=$?
case "$c6" in
  *"FAIL zig: registered, but the language table has no entry"*)
      [ $c6rc -ne 0 ] && ok LC-06 "a registered language with no table entry fails (exit $c6rc)" \
                      || bad LC-06 "reported, but exit 0" "$c6" ;;
  *) bad LC-06 "a missing table entry was not reported" "$c6" ;;
esac
c7="$(python3 "$TOOL" conform --gov build/naab-gov --naab build/naab-lang --table "$W/bogus.json" 2>&1)"; c7rc=$?
case "$c7" in
  *"FAIL python line //: code_quality.no_hallucinated_apis reported"*)
      [ $c7rc -ne 0 ] && ok LC-07 "a declared comment form the engine does not honour fails (exit $c7rc)" \
                      || bad LC-07 "reported, but exit 0" "$c7" ;;
  *) bad LC-07 "a dishonoured comment form was not reported" "$c7" ;;
esac

c8="$(python3 "$TOOL" conform --gov build/naab-gov --naab build/naab-lang --table "$W/fakeblock.json" 2>&1)"; c8rc=$?
case "$c8" in
  *"FAIL python marker in block <<< >>>: code_quality.no_temporary_code not reported"*)
      [ $c8rc -ne 0 ] && ok LC-08 "a declared block form whose markers the engine cannot see fails (exit $c8rc)" \
                      || bad LC-08 "reported, but exit 0" "$c8" ;;
  *) bad LC-08 "a fake block form's invisible marker was not reported" "$c8" ;;
esac

# --- LC-09..12: verify against the real toolchains ---
v9="$(python3 "$TOOL" verify --gov build/naab-gov 2>&1)"; v9rc=$?
measured=$(printf '%s' "$v9" | tr -d '\r' | sed -n 's/^langconform verify: \([0-9]*\) of.*/\1/p')
if [ "${measured:-0}" -eq 0 ]; then
    skip LC-09 "no language toolchain installed -- the table's facts are UNMEASURABLE here"
elif [ $v9rc -eq 0 ]; then
    ok LC-09 "$(printf '%s' "$v9" | tr -d '\r' | tail -1 | sed 's/^langconform verify: //')"
else
    bad LC-09 "the table declares a comment form a language's own parser rejects" "$v9"
fi
if ! printf '%s' "$v9" | grep -q "python .*\[python3\] confirmed"; then
    skip LC-10 "python3 cannot parse here -- the verifier controls are UNMEASURABLE"
    skip LC-11 "python3 cannot parse here"
    skip LC-12 "ruby not installed or python3 cannot parse here"
else
    python3 - "$W/table.json" "$W/v_wrong.json" "$W/v_missing.json" "$W/v_string.json" <<'PY'
import json, sys
def load():
    return json.load(open(sys.argv[1], encoding="utf-8", errors="strict"))
def save(t, i):
    open(sys.argv[i], "w", encoding="ascii", newline="\n").write(json.dumps(t))
t = load()
for d in t:
    if d["canonical"] == "python":
        d["line_comments"].append("--")
save(t, 2)
t = load()
for d in t:
    if d["canonical"] == "python":
        d["line_comments"] = []
save(t, 3)
t = load()
for d in t:
    if d["canonical"] == "ruby":
        d["block_comments"].append({"open": "%{", "close": "%}", "line_start_only": False})
save(t, 4)
PY
    v10="$(python3 "$TOOL" verify --gov build/naab-gov --table "$W/v_wrong.json" 2>&1)"; v10rc=$?
    case "$v10" in
      *"WRONG: line -- is declared, but python3 rejects it"*)
          [ $v10rc -ne 0 ] && ok LC-10 "a planted wrong fact is caught by the real parser (exit $v10rc)" \
                           || bad LC-10 "caught, but exit 0" "$v10" ;;
      *) bad LC-10 "a planted wrong fact was not caught" "$v10" ;;
    esac
    v11="$(python3 "$TOOL" verify --gov build/naab-gov --table "$W/v_missing.json" 2>&1)"
    case "$v11" in
      *"proposed: line # (accepted by python3, not declared)"*)
          ok LC-11 "a removed real fact is rediscovered from the parser" ;;
      *) bad LC-11 "a removed real fact was not rediscovered" "$v11" ;;
    esac
    if command -v ruby >/dev/null 2>&1; then
        v12="$(python3 "$TOOL" verify --gov build/naab-gov --table "$W/v_string.json" 2>&1)"
        case "$v12" in
          *"WRONG: block %{ %} is declared, but ruby rejects it"*)
              ok LC-12 "a string literal posing as a comment is rejected" ;;
          *) bad LC-12 "a string literal passed for a comment" "$v12" ;;
        esac
    else
        skip LC-12 "ruby not installed -- the string-literal control is UNMEASURABLE"
    fi
fi

# Report only: alias groups that disagree today (sql/sqlite until the language table).
echo "  info: $(python3 "$TOOL" groups tools/langconform/baseline.json 2>&1 | tail -1)"

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
