#!/usr/bin/env bash
# ============================================================
# test_codemap_usage.sh -- tools/codemap/usage.py (roadmap item 10)
#
# usage.py counts code-map invocations in Claude Code transcripts, per
# subcommand: calls, errors, immediate repeats, and calls followed by a grep for
# the same name. A synthetic transcript holds one case per rule.
#
#   CU-01  callers counted twice; the second is an immediate repeat; both are
#          followed (within 3 tool calls) by a Grep for their argument
#   CU-02  an errored result counts as an error; control: history's clean
#          result does not
#   CU-03  a name bound in the same command (C="python3 .../codemap.py";
#          $C explain x) is an invocation, and so is a wrapper function
#   CU-04  the word "codemap" in a commit message, an echo or a grep pattern is
#          NOT an invocation (the first version counted "the", "is", "item")
#   CU-05  a grep for ANOTHER name is not grep-after (history's control)
#   CU-06  the report carries no command text: neither an argument nor a token
#          from the command line appears in it
#   CU-07  a malformed transcript line is counted, not fatal
#
# Needs python3; missing it is UNMEASURABLE.
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

echo "=== codemap usage counts invocations from transcripts ==="
command -v python3 >/dev/null 2>&1 || { skip CU-ALL "python3 not found"; finish; }

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
T="$W/t.jsonl"
python3 - "$T" <<'PYEOF'
import json, sys
lines = []
def use(i, name, inp):
    lines.append({"type": "assistant", "message": {"content": [{"type": "tool_use", "id": i, "name": name, "input": inp}]}})
def res(i, text, err=False):
    lines.append({"type": "user", "message": {"content": [{"type": "tool_result", "tool_use_id": i,
                                                         "content": text, "is_error": err}]}})
CM = "python3 tools/codemap/codemap.py"
use("t1", "Bash", {"command": CM + " callers alpha_target --depth 2"}); res("t1", "ok")
use("t2", "Bash", {"command": CM + " callers alpha_target --depth 2"}); res("t2", "ok")
use("t3", "Grep", {"pattern": "alpha_target", "path": "src"}); res("t3", "x")
use("t4", "Bash", {"command": CM + " --json status"}); res("t4", "boom", err=True)
use("t5", "Bash", {"command": "cd x && " + CM + " --repo . --cache c history beta_target && echo SECRETTOKEN"}); res("t5", "fine")
use("t6", "Bash", {"command": "grep -rn other_name src"}); res("t6", "x")
use("t7", "Bash", {"command": 'C="python3 tools/codemap/codemap.py --repo /r"; $C explain gamma_target; echo done'}); res("t7", "ok")
use("t8", "Bash", {"command": 'cm() { python3 tools/codemap/codemap.py --cache c "$@"; }; cm types delta_target'}); res("t8", "ok")
use("t9", "Bash", {"command": 'git commit -m "codemap item 1: the codemap is fine" && echo codemap is here && grep -n "codemap" x'}); res("t9", "ok")
use("t10", "Bash", {"command": CM + " tested epsilon_target"}); res("t10", "Traceback (most recent call last):\n  boom")
with open(sys.argv[1], "w") as fh:
    for d in lines:
        fh.write(json.dumps(d) + "\n")
    fh.write("{not json\n")
PYEOF

U="$(native_path "$REPO/tools/codemap/usage.py")"
OUT="$(python3 "$U" "$T" 2>&1)"
J="$(python3 "$U" --json "$T" 2>&1)"
row() { python3 -c 'import json, sys
d = json.load(sys.stdin)["subcommands"].get(sys.argv[1])
print("none" if d is None else "%(calls)d %(errors)d %(repeats)d %(grep_after)d" % d)' "$1" <<<"$J" 2>&1; }
if ! python3 -c 'import json, sys; json.load(sys.stdin)' <<<"$J" >/dev/null 2>&1; then
    bad CU-00 "usage.py did not produce JSON" "$J"; finish
fi

[ "$(row callers)" = "2 0 1 2" ] && ok CU-01 "callers: 2 calls, 1 immediate repeat, both followed by a grep for their argument" \
    || bad CU-01 "callers counts wrong" "$(row callers)"
if [ "$(row status)" = "1 1 0 0" ] && [ "$(row tested)" = "1 1 0 0" ] && [ "$(row history)" = "1 0 0 0" ]; then
    ok CU-02 "an errored result and a traceback count as errors; history's clean result does not"
else
    bad CU-02 "error counts wrong" "status $(row status) tested $(row tested) history $(row history)"
fi
if [ "$(row explain)" = "1 0 0 0" ] && [ "$(row types)" = "1 0 0 0" ]; then
    ok CU-03 "an invocation through a variable or a wrapper function bound in the same command is counted"
else
    bad CU-03 "an aliased invocation was missed" "explain $(row explain) types $(row types)"
fi
TOTAL="$(python3 -c 'import json, sys; print(json.load(sys.stdin)["codemap_calls"])' <<<"$J")"
SUBS="$(python3 -c 'import json, sys; print(",".join(sorted(json.load(sys.stdin)["subcommands"])))' <<<"$J")"
if [ "$TOTAL" = "7" ] && [ "$SUBS" = "callers,explain,history,status,tested,types" ]; then
    ok CU-04 "the word codemap in a commit message, an echo or a grep pattern is not an invocation"
else
    bad CU-04 "non-invocations were counted" "$TOTAL $SUBS"
fi
[ "$(row history)" = "1 0 0 0" ] && ok CU-05 "a grep for another name is not grep-after" || bad CU-05 "history grep-after wrong" "$(row history)"
if ! grep -qE "alpha_target|beta_target|gamma_target|SECRETTOKEN|--depth|other_name" <<<"$OUT$J"; then
    ok CU-06 "the report carries no argument or command text"
else
    bad CU-06 "command text leaked into the report" "$OUT"
fi
if grep -q "(1 unreadable lines)" <<<"$OUT" && grep -q "LIMIT: transcripts written by the session" <<<"$OUT"; then
    ok CU-07 "a malformed line is counted, not fatal; the report states its limit"
else
    bad CU-07 "malformed line or limit line wrong" "$OUT"
fi
finish
