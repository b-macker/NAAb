#!/usr/bin/env bash
# ============================================================
# test_project_context_integrity.sh -- what CLAUDE.md and the setup files add
# to governance holds, and the program cannot take it away (A212, A213, A214;
# Bundle 21 PR 1)
#
# THE DEFECTS (observed on master 2981add6 / 31c90c23, project_context on)
#
# A212: "- Never use eval()" in CLAUDE.md was reported
#   `"ban eval" -> *.banned_functions [APPLIED]`, and an eval in a Python block
#   ran under "Governance: PASS": a ban NAAb cannot tie to one language was
#   written to no rule. indent_size, indent_style and max_line_length were
#   labelled APPLIED too, and no rule reads them.
# A213: a program could rewrite CLAUDE.md, and the next run lost its ban
#   (import pickle refused; file.write over CLAUDE.md, exit 0; then allowed).
#   A file the program CREATES could lift a refusal too: language preferences
#   and manifests feed the language-choice check, and a CLAUDE.md preference or
#   a pyproject.toml let a block that check refused at hard run.
# A214: that check's HARD refusal was an ordinary error: a NAAb try caught it
#   (exit 0, "Governance: PASS (1 refused ...)") and uncaught it exited 1.
#
# THE FIX
#
# An unattributed ban is added to every language a block can be written in;
# the three style settings are reported NOT ENFORCED. The files whose text NAAb
# turned into a rule (a ban, no_console_log, strict_mode) are refused to program
# writes, deletes and renames while project_context is on (rule
# governance.project_context_files, HARD; reads allowed). Manifests and files
# not yet present stay writable: instead the language-choice check decides with
# and without project preferences and refuses when either refuses, so a project
# file can only ADD a refusal. Its HARD refusal goes through enforce().
#
#   PC-00   CONTROL: Python blocks run here, and a govern.json ban on eval
#           refuses one (the matcher sees eval). On a build without embedded
#           Python (every Windows build) PC-00 and every arm that needs a
#           Python block read UNMEASURABLE and the rest still run; Python
#           present but not running is a failure
#   PC-01   CLAUDE.md "Never use eval()" refuses eval in a Python block
#   PC-01jc CONTROL: without that line a JavaScript eval block runs
#   PC-01j  ... and the line refuses it (every language)
#   PC-01c  CONTROL: without that line the Python block runs
#   PC-02   "Use 4 spaces for indentation" is reported NOT ENFORCED, not APPLIED
#   PC-03b  CONTROL: CLAUDE.md "Never use pickle" refuses import pickle
#   PC-03   file.write over that CLAUDE.md is refused (exit 3) and it is unchanged
#   PC-03r  CONTROL: file.read of it is allowed
#   PC-03a  its absolute path is refused too
#   PC-03d  file.delete of it is refused (PC-03dc: an ordinary delete works)
#   PC-03p  embedded Python open(it, "w") is refused (UNMEASURABLE where Python
#           is not embedded)
#   PC-04   CONTROL: with project_context off the same write lands
#   PC-04n  CONTROL: a CLAUDE.md that adds no rule stays writable
#   PC-05   an .eslintrc.json that switches no_console_log on is protected
#   PC-06   CONTROL: a manifest stays writable (pyproject.toml)
#   PC-09c  CONTROL: a CLAUDE.md with CRLF line endings is read (its ban)
#   PC-09r  ... and its "Prefer go for concurrency" line is applied (A232: the
#           CR stayed on the task name and the line was dropped)
#   PC-07c  CONTROL: the configured score table refuses a Python JSON block at
#           hard with no project files
#   PC-07   ... and still refuses it with a pyproject.toml beside it
#   PC-07p  ... and with a CLAUDE.md preference for Python (table at 62)
#   PC-07t  CONTROL: a project preference still ADDS a refusal (a block the
#           table allows is refused once CLAUDE.md prefers another language)
#   PC-09p  ... and so does the same preference in a CRLF file with nothing
#           after the task (A232)
#   PC-08   the HARD refusal exits 3
#   PC-08t  ... and a NAAb try does not catch it
#   PC-08s  CONTROL: at soft the same block runs (soft stays advisory)
#   PC-08h  with helper_errors off (a setting for the hint text) the HARD
#           refusal still happens
#
# Kill table (mutant applied alone, naab-lang rebuilt, this suite run;
# observed 2026-11-23 on the default build, rerun after the suite gained
# PC-01jc and PC-09):
#   M1  an unattributed ban added to no list               -> PC-01 PC-01j
#   M2  an unread style setting labelled APPLIED            -> PC-02
#   M3  the files that added a rule not recorded            -> PC-03 PC-03a PC-03d PC-03p PC-05
#   M4  the write gate removed                              -> PC-03 PC-03a PC-03d PC-03p PC-05
#   M5  every file with an extraction recorded              -> PC-04n PC-06
#   M6  decided with the project's preferences only         -> PC-07 PC-07p
#   M7  decided without them only                           -> PC-07t PC-09p
#   M8  the HARD refusal returned, not enforced             -> PC-08 PC-08t PC-08h
#   M9  the HARD refusal gated on helper_errors             -> PC-08h
#   M10 an unattributed ban added to Python only            -> PC-01j
#   M11 the CR kept by the Markdown reader                  -> PC-09r PC-09p
# Not mutated: the same CR drop in the .editorconfig and go.mod readers, whose
# values reach no rule (labels and a version string).
# Against a stand-in for a Python-less build (the program run with its Python
# blocks replaced by null): 14 pass, 14 UNMEASURABLE, 0 fail, exit 0.
# On master 31c90c23: the 12 subject arms then written red, the 11 controls
# green; PC-08h was added with A214(b), red on master too (observed by hand).
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
source "$SCRIPT_DIR/../helpers/native_path.sh"
setup_isolated_trust
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_pcint_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2${3:+ -- $3}"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

echo "=== test_project_context_integrity.sh ==="
if [ ! -x "$NAAB" ]; then echo "  UNMEASURABLE: naab-lang not built"; echo "Result: 0 passed, 0 failed, 1 unmeasurable"; exit 1; fi

# cfg PC_ENABLED SANDBOX [EXTRA_TOP_LEVEL_JSON]
cfg() {
    printf '{"mode": "enforce", "security": {"sandbox_level": "%s"},
 "capabilities": {"filesystem": {"mode": "write"}},
 "project_context": {"enabled": %s, "show_extractions": true}%s}\n' "${2:-elevated}" "$1" "${3:-}"
}
# a language-choice section: LEVEL, the table's Python score, JavaScript's,
# [helper_errors.enabled]
po() {
    printf ', "polyglot_optimization": {"enabled": true, "enforcement_level": "%s",
   "calibration": {"enabled": false}, "helper_errors": {"enabled": %s},
   "task_language_matrix": {"json_processing": {"python": %s, "javascript": %s}}}' "$1" "${4:-true}" "$2" "$3"
}
# fresh ID PC_ENABLED [SANDBOX] [EXTRA]: a project directory; sets D
fresh() { D="$TEST_TMP/$1"; rm -rf "$D"; mkdir -p "$D"; cfg "$2" "${3:-elevated}" "${4:-}" > "$D/govern.json"; }
run() { RC=0; OUT=$(cd "$D" && timeout 120s "$NAAB" run t.naab 2>&1) || RC=$?; }
sum() { cksum < "$1" 2>/dev/null; }

PY_EVAL='main {
    let r = <<python
x = eval("2+3")
x
>>
    print("RAN " + r)
}'
JS_EVAL='main {
    let r = <<javascript
eval("2+3")
>>
    print("RAN " + r)
}'
PY_PICKLE='main {
    let r = <<python
import pickle
len(pickle.dumps(1))
>>
    print("RAN " + r)
}'
JSON_BODY='import json
raw = '"'"'{"items": [{"name": "a", "price": 12}, {"name": "b", "price": 3}]}'"'"'
data = json.loads(raw)
items = [i for i in data["items"] if i["price"] > 10]
out = json.dumps({"names": [i["name"] for i in items]})
parsed = json.loads(out)
len(parsed["names"])'
PY_JSON="main {
    let r = <<python
$JSON_BODY
>>
    print(\"RAN \" + r)
}"
PY_JSON_TRY="main {
    try {
        let r = <<python
$JSON_BODY
>>
        print(\"RAN \" + r)
    } catch (e) {
        print(\"CAUGHT\")
    }
    print(\"AFTER\")
}"

# ---- PC-00 control -----------------------------------------------------------
# A build without embedded Python (every Windows build) prints this warning on
# every run and runs no Python block: the arms that need one read UNMEASURABLE
# there (py_arm), and the rest -- the file protection, the labels, CRLF -- are
# still measured. Python present but not running is a failure, not a skip.
PY=1
fresh pc00 false elevated ', "languages": {"per_language": {"python": {"banned_functions": ["eval"]}}}'
printf 'main {\n    let r = <<python\n2 + 3\n>>\n    print("RAN " + r)\n}\n' > "$D/t.naab"; run
if grep -q 'Python support not available' <<<"$OUT"; then
    PY=0
    unm PC-00 "Python is not embedded in this build: the arms that need a Python block are UNMEASURABLE"
elif [ "$RC" != 0 ] || ! grep -q 'RAN 5' <<<"$OUT"; then
    fail PC-00 "a Python block does not run here; nothing below can be read" "rc=$RC"
    echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
else
    printf '%s\n' "$PY_EVAL" > "$D/t.naab"; run
    if [ "$RC" = 3 ] && grep -q 'Banned function' <<<"$OUT" && ! grep -q 'RAN' <<<"$OUT"; then
        pass PC-00 "CONTROL: Python runs, and a govern.json ban on eval refuses the eval block (exit 3)"
    else
        fail PC-00 "the ban matcher did not refuse eval; nothing below can be read" "rc=$RC"
        echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
    fi
fi
# py_arm ID TEXT: true when the arm can run here; otherwise it is UNMEASURABLE
py_arm() { [ "$PY" = 1 ] && return 0; unm "$1" "$2 (no embedded Python in this build)"; return 1; }

# ---- PC-01 an unattributed ban applies ----------------------------------------
fresh pc01 true
printf '# Rules\n\n## Restrictions\n- Never use eval()\n' > "$D/CLAUDE.md"
if py_arm PC-01 "CLAUDE.md 'Never use eval()' refuses eval in a Python block"; then
    printf '%s\n' "$PY_EVAL" > "$D/t.naab"; run
    if [ "$RC" = 3 ] && grep -q 'Banned function' <<<"$OUT" && ! grep -q 'RAN' <<<"$OUT"; then
        pass PC-01 "CLAUDE.md 'Never use eval()' refuses eval in a Python block"
    else
        fail PC-01 "the CLAUDE.md ban did not reach the Python block" "rc=$RC $(grep -m1 -E 'ban eval|RAN' <<<"$OUT")"
    fi
fi
fresh pc01c true
printf '# Rules\n' > "$D/CLAUDE.md"
printf '%s\n' "$JS_EVAL" > "$D/t.naab"; run
JS_OK=0
if [ "$RC" = 0 ] && grep -q 'RAN 5' <<<"$OUT"; then
    JS_OK=1
    pass PC-01jc "CONTROL: without the line the JavaScript eval block runs"
else
    unm PC-01jc "a JavaScript block does not run here (rc=$RC); PC-01j cannot be read"
fi
if py_arm PC-01c "CONTROL: without the line the Python eval block runs"; then
    printf '%s\n' "$PY_EVAL" > "$D/t.naab"; run
    if [ "$RC" = 0 ] && grep -q 'RAN 5' <<<"$OUT"; then
        pass PC-01c "CONTROL: without the line the Python eval block runs"
    else
        fail PC-01c "the eval block is refused without any ban" "rc=$RC"
    fi
fi
if [ "$JS_OK" = 1 ]; then
    D="$TEST_TMP/pc01"
    printf '%s\n' "$JS_EVAL" > "$D/t.naab"; run
    if [ "$RC" = 3 ] && grep -q 'Banned function' <<<"$OUT" && ! grep -q 'RAN' <<<"$OUT"; then
        pass PC-01j "the same CLAUDE.md ban refuses eval in a JavaScript block"
    else
        fail PC-01j "the CLAUDE.md ban did not reach the JavaScript block" "rc=$RC $(grep -m1 -E 'RAN|rror' <<<"$OUT")"
    fi
else
    unm PC-01j "no JavaScript control (PC-01jc)"
fi

# ---- PC-02 labels ---------------------------------------------------------------
fresh pc02 true
printf '# Rules\n\n## Style\n- Use 4 spaces for indentation\n' > "$D/CLAUDE.md"
printf 'main {\n    print("OK")\n}\n' > "$D/t.naab"; run
IND=$(grep -m1 -i 'indent' <<<"$OUT")
if grep -q 'NOT ENFORCED' <<<"$IND" && ! grep -q 'APPLIED' <<<"$IND"; then
    pass PC-02 "an indentation preference is reported NOT ENFORCED"
else
    fail PC-02 "the indentation line does not say it is not enforced" "${IND:-no indentation line}"
fi

# ---- PC-03 a file that added a rule is protected -------------------------------
fresh pc03 true
printf '# Rules\n\n## Restrictions\n- Never use pickle\n' > "$D/CLAUDE.md"
CL_SUM=$(sum "$D/CLAUDE.md")
if py_arm PC-03b "CONTROL: the CLAUDE.md ban refuses import pickle"; then
    printf '%s\n' "$PY_PICKLE" > "$D/t.naab"; run
    if [ "$RC" = 3 ] && ! grep -q 'RAN' <<<"$OUT"; then
        pass PC-03b "CONTROL: the CLAUDE.md ban refuses import pickle"
    else
        fail PC-03b "the CLAUDE.md pickle ban did not refuse; the protection arms cannot be read" "rc=$RC"
    fi
fi
printf 'main {\n    file.write("CLAUDE.md", "# Rules\\n")\n    print("WROTE")\n}\n' > "$D/t.naab"; run
if [ "$RC" = 3 ] && grep -q 'governance.project_context_files' <<<"$OUT" && [ "$(sum "$D/CLAUDE.md")" = "$CL_SUM" ]; then
    pass PC-03 "file.write over CLAUDE.md is refused (exit 3) and it is unchanged"
else
    fail PC-03 "the program rewrote the CLAUDE.md that added a ban" "rc=$RC $(grep -m1 -E 'WROTE|project_context_files' <<<"$OUT")"
fi
printf 'main {\n    let t = file.read("CLAUDE.md")\n    print("READ " + string(len(t) > 0))\n}\n' > "$D/t.naab"; run
if [ "$RC" = 0 ] && grep -q 'READ true' <<<"$OUT"; then
    pass PC-03r "CONTROL: file.read of it is allowed"
else
    fail PC-03r "reading the file was refused" "rc=$RC"
fi
ABS=$(native_path "$D/CLAUDE.md")
printf 'main {\n    file.write("%s", "# Rules\\n")\n    print("WROTE")\n}\n' "$ABS" > "$D/t.naab"; run
if [ "$RC" = 3 ] && grep -q 'governance.project_context_files' <<<"$OUT" && [ "$(sum "$D/CLAUDE.md")" = "$CL_SUM" ]; then
    pass PC-03a "its absolute path is refused too"
else
    fail PC-03a "the absolute path was written" "rc=$RC"
fi
fresh pc03d true unrestricted
printf '# Rules\n\n## Restrictions\n- Never use pickle\n' > "$D/CLAUDE.md"
printf 'main {\n    file.write("junk.txt", "x")\n    file.delete("junk.txt")\n    print("DELETED=" + string(file.exists("junk.txt") == false))\n}\n' > "$D/t.naab"; run
if [ "$RC" = 0 ] && grep -q 'DELETED=true' <<<"$OUT"; then
    pass PC-03dc "CONTROL: an ordinary file.delete works under this config"
    printf 'main {\n    file.delete("CLAUDE.md")\n    print("DELETED")\n}\n' > "$D/t.naab"; run
    if [ "$RC" = 3 ] && grep -q 'governance.project_context_files' <<<"$OUT" && [ -f "$D/CLAUDE.md" ]; then
        pass PC-03d "file.delete of it is refused and it is present"
    else
        fail PC-03d "the program deleted the CLAUDE.md that added a ban" "rc=$RC"
    fi
else
    unm PC-03d "an ordinary delete does not work under this config (rc=$RC)"
fi
fresh pc03p true
printf '# Rules\n\n## Restrictions\n- Never use pickle\n' > "$D/CLAUDE.md"
printf 'main {\n    let r = <<python\nopen("CLAUDE.md", "w").write("# Rules\\n")\n"WROTE"\n>>\n    print("PY=" + r)\n}\n' > "$D/t.naab"; run
if grep -q 'Python support not available' <<<"$OUT"; then
    unm PC-03p "Python is not embedded in this build"
elif [ "$(sum "$D/CLAUDE.md")" = "$CL_SUM" ] && ! grep -q 'PY=WROTE' <<<"$OUT"; then
    pass PC-03p "embedded Python cannot rewrite it"
else
    fail PC-03p "embedded Python rewrote it" "rc=$RC"
fi

# ---- PC-04 protection follows what the engine turned into a rule --------------
fresh pc04 false
printf '# Rules\n\n## Restrictions\n- Never use pickle\n' > "$D/CLAUDE.md"
printf 'main {\n    file.write("CLAUDE.md", "# Rules\\n")\n    print("WROTE")\n}\n' > "$D/t.naab"; run
if [ "$RC" = 0 ] && grep -q 'WROTE' <<<"$OUT"; then
    pass PC-04 "CONTROL: with project_context off the same write lands"
else
    fail PC-04 "CLAUDE.md was protected though project_context is off" "rc=$RC"
fi
fresh pc04n true
printf '# Rules\n\n## Style\n- Use 4 spaces for indentation\n' > "$D/CLAUDE.md"
printf 'main {\n    file.write("CLAUDE.md", "# Rules\\n")\n    print("WROTE")\n}\n' > "$D/t.naab"; run
if [ "$RC" = 0 ] && grep -q 'WROTE' <<<"$OUT"; then
    pass PC-04n "CONTROL: a CLAUDE.md that adds no rule stays writable"
else
    fail PC-04n "a CLAUDE.md that adds no rule was protected" "rc=$RC"
fi

# ---- PC-05 a linter file that switched a check on ------------------------------
fresh pc05 true
printf '{"rules": {"no-console": "error"}}\n' > "$D/.eslintrc.json"
ES_SUM=$(sum "$D/.eslintrc.json")
printf 'main {\n    file.write(".eslintrc.json", "{}")\n    print("WROTE")\n}\n' > "$D/t.naab"; run
if [ "$RC" = 3 ] && grep -q 'governance.project_context_files' <<<"$OUT" && [ "$(sum "$D/.eslintrc.json")" = "$ES_SUM" ]; then
    pass PC-05 "an .eslintrc.json that switched no_console_log on is protected"
else
    fail PC-05 "the program rewrote a linter file that added a rule" "rc=$RC"
fi

# ---- PC-06 manifests stay writable ----------------------------------------------
fresh pc06 true
printf '[project]\nname = "x"\n' > "$D/pyproject.toml"
printf 'main {\n    file.write("pyproject.toml", "[project]\\nname = \\"y\\"\\n")\n    print("WROTE")\n}\n' > "$D/t.naab"; run
if [ "$RC" = 0 ] && grep -q 'WROTE' <<<"$OUT"; then
    pass PC-06 "CONTROL: a manifest stays writable (a build tool may write pyproject.toml)"
else
    fail PC-06 "a manifest was protected" "rc=$RC"
fi

# ---- PC-09 a CLAUDE.md with Windows line endings (A232) ----------------------
# std::getline keeps the CR, and a directive whose capture runs to the end of
# the line then held it: "Prefer go for concurrency" named the task
# "concurrency\r" and was dropped. The ban survived only because its pattern
# does not run to the end of the line.
fresh pc09 true
printf '# Rules\r\n\r\n## Restrictions\r\n- Never use eval()\r\n- Prefer go for concurrency\r\n' > "$D/CLAUDE.md"
printf 'main {\n    print("OK")\n}\n' > "$D/t.naab"; run
if grep -q 'ban eval' <<<"$OUT"; then
    pass PC-09c "CONTROL: a CLAUDE.md with CRLF line endings is read (its ban is reported)"
    if grep -qi 'prefer go for concurrency.*APPLIED' <<<"$OUT"; then
        pass PC-09r "... and a preference that runs to the end of its line is applied too"
    else
        fail PC-09r "a CRLF line lost its preference" "$(grep -m3 -E 'Language|Summary' <<<"$OUT" | tr '\n' ' ')"
    fi
else
    fail PC-09c "the CRLF CLAUDE.md was not read; PC-09r cannot be read" "rc=$RC"
fi

if [ "$PY" = 1 ]; then
# ---- PC-07 project preferences cannot lift a language-choice refusal ----------
fresh pc07c true elevated "$(po hard 72 100)"
printf '%s\n' "$PY_JSON" > "$D/t.naab"; run
if [ "$RC" != 0 ] && grep -qi 'suboptimal language' <<<"$OUT" && ! grep -q 'RAN' <<<"$OUT"; then
    pass PC-07c "CONTROL: the table refuses the Python JSON block at hard with no project files"
    printf '[project]\nname = "x"\n' > "$D/pyproject.toml"; run
    if [ "$RC" != 0 ] && ! grep -q 'RAN' <<<"$OUT"; then
        pass PC-07 "a pyproject.toml beside it does not lift the refusal"
    else
        fail PC-07 "a manifest lifted the language-choice refusal" "rc=$RC"
    fi
else
    fail PC-07c "the table did not refuse the block; PC-07 cannot be read" "rc=$RC"
fi
fresh pc07p true elevated "$(po hard 62 100)"
printf '# Rules\n- Prefer python for json processing.\n' > "$D/CLAUDE.md"
printf '%s\n' "$PY_JSON" > "$D/t.naab"; run
if [ "$RC" != 0 ] && grep -qi 'suboptimal language' <<<"$OUT" && ! grep -q 'RAN' <<<"$OUT"; then
    pass PC-07p "a CLAUDE.md preference for Python does not lift the refusal"
else
    fail PC-07p "a CLAUDE.md preference lifted the language-choice refusal" "rc=$RC $(grep -m1 -E 'RAN|prefer' <<<"$OUT")"
fi
fresh pc07t true elevated "$(po hard 50 55)"
printf '%s\n' "$PY_JSON" > "$D/t.naab"; run
T07_OK=0
if [ "$RC" = 0 ] && grep -q 'RAN' <<<"$OUT"; then
    T07_OK=1
    printf '# Rules\n- Prefer javascript for json processing.\n' > "$D/CLAUDE.md"; run
    if [ "$RC" != 0 ] && grep -qi 'suboptimal language' <<<"$OUT" && ! grep -q 'RAN' <<<"$OUT"; then
        pass PC-07t "CONTROL: a project preference still adds a refusal"
    else
        fail PC-07t "a project preference no longer adds a refusal" "rc=$RC"
    fi
else
    fail PC-07t "the table refused the block before any preference; the control cannot be read" "rc=$RC"
fi

# PC-09p: the same preference in a CRLF file with nothing after the task, the
# shape A232 dropped; PC-07t's first half is its control (same table)
if [ "$T07_OK" = 1 ]; then
    fresh pc09p true elevated "$(po hard 50 55)"
    printf '%s\n' "$PY_JSON" > "$D/t.naab"
    printf '# Rules\r\n- Prefer javascript for json processing\r\n' > "$D/CLAUDE.md"; run
    if [ "$RC" != 0 ] && grep -qi 'suboptimal language' <<<"$OUT" && ! grep -q 'RAN' <<<"$OUT"; then
        pass PC-09p "a CRLF preference still adds its refusal"
    else
        fail PC-09p "the CRLF preference was lost: the block the preference refuses ran" "rc=$RC"
    fi
else
    unm PC-09p "no control (PC-07t's first half did not run)"
fi

# ---- PC-08 the HARD refusal is HARD -------------------------------------------
fresh pc08 false elevated "$(po hard 72 100)"
printf '%s\n' "$PY_JSON" > "$D/t.naab"; run
if [ "$RC" = 3 ] && ! grep -q 'RAN' <<<"$OUT"; then
    pass PC-08 "the HARD language-choice refusal exits 3"
else
    fail PC-08 "the HARD refusal did not exit 3" "rc=$RC"
fi
printf '%s\n' "$PY_JSON_TRY" > "$D/t.naab"; run
if [ "$RC" = 3 ] && ! grep -q 'CAUGHT' <<<"$OUT" && ! grep -q 'AFTER' <<<"$OUT"; then
    pass PC-08t "a NAAb try does not catch it"
else
    fail PC-08t "the HARD refusal was caught" "rc=$RC $(grep -m1 -E 'CAUGHT|AFTER' <<<"$OUT")"
fi
fresh pc08s false elevated "$(po soft 72 100)"
printf '%s\n' "$PY_JSON" > "$D/t.naab"; run
if grep -q 'RAN' <<<"$OUT"; then
    pass PC-08s "CONTROL: at soft the same block runs (soft stays advisory)"
else
    fail PC-08s "the block did not run at soft" "rc=$RC"
fi

fresh pc08h false elevated "$(po hard 72 100 false)"
printf '%s\n' "$PY_JSON" > "$D/t.naab"; run
if [ "$RC" = 3 ] && ! grep -q 'RAN' <<<"$OUT"; then
    pass PC-08h "with helper_errors off the HARD refusal still happens"
else
    fail PC-08h "turning the hint text off removed the HARD refusal" "rc=$RC"
fi
else
    for a in PC-07c PC-07 PC-07p PC-07t PC-09p PC-08 PC-08t PC-08s PC-08h; do
        unm "$a" "needs a Python block (no embedded Python in this build)"
    done
fi

echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
