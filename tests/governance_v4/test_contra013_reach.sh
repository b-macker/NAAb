#!/usr/bin/env bash
# test_contra013_reach.sh -- CONTRA-013 fires only while a separate-process
# language can run (F-03; Bundle 17)
#
# CONTRA-013 tells the operator that capabilities.filesystem path rules do not
# reach a separate-process language. It was written (#226) before the
# shell-off gate (#298) and fired whenever a path rule was set below
# `restricted` -- under configs that refuse every separate-process language,
# and with its own remedy ("block the subprocess languages in
# languages.blocked") applied (observed on master 314bccfd). A warning that
# cannot be silenced by doing what it says is noise operators learn to ignore.
# It now asks the gates' own rules: shell disabled refuses every
# separate-process executor; otherwise a runtime runs when the language lists
# admit it. In-process: QuickJS, SQLite, and Python where the build embeds it.
#
#   CT-00c  CONTROL a path rule and default languages: fires
#   CT-01   shell disabled: does not fire
#   CT-02   every separate-process language blocked: does not fire
#   CT-03   allowed only javascript and sql: does not fire
#   CT-04c  CONTROL allowed javascript and node (node is a subprocess runtime): fires
#   CT-05   allowed python: fires exactly when this build runs Python as a
#           subprocess (measured first: <<python>> under shell off)
#   CT-06c  CONTROL sandbox restricted: does not fire (unchanged)
#
# Kill table (each mutant applied alone, rebuilt, this suite run; observed
# 2026-11-09 on a build that embeds Python):
#   reachability ignored (master's condition)    CT-01, CT-02, CT-03, CT-05
#   node treated as in-process                   CT-04c
#   python never in-process                      CT-05
#   python always in-process                     CT-05 on a build without pybind11
#                                                (not run: this build embeds it)
# Red first (observed, master 314bccfd, a build that embeds Python): CT-01,
# CT-02, CT-03 and CT-05 fail; the controls pass.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_contra13_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -6 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then echo "UNMEASURABLE: naab-lang not built"; exit 1; fi

# run ID SANDBOX SHELL LANGUAGES_JSON
run() {
    local d="$W/$1"; mkdir -p "$d"
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "%s"},
 "capabilities": {"shell": {"enabled": %s}, "filesystem": {"mode": "read", "blocked_paths": ["vault/"]}},
 "languages": %s}\n' "$2" "$3" "$4" > "$d/govern.json"
    printf 'main {\n    print("RAN=ok")\n}\n' > "$d/p.naab"
    RC=0; OUT=$(cd "$d" && timeout 60s "$NAAB" p.naab 2>&1) || RC=$?
}
fires() { grep -q "CONTRA-013" <<<"$OUT"; }
ran()   { grep -q "RAN=ok" <<<"$OUT"; }

SUBPROC='["shell", "node", "typescript", "ruby", "go", "cpp", "csharp", "rust", "nim", "php", "julia", "zig", "python"]'
echo "=== CONTRA-013 fires only while a separate-process language can run ==="

run ct00 elevated true '{}'
if ran && fires; then pass CT-00c "CONTROL: a path rule with default languages fires"
else fail CT-00c "the control config did not fire CONTRA-013 (rc=$RC)" "$OUT"
     echo ""; echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$UNM"; exit 1; fi

run ct01 elevated false '{}'
if ran && ! fires; then pass CT-01 "shell disabled: does not fire"
else fail CT-01 "fired with shell disabled (rc=$RC)" "$OUT"; fi

run ct02 elevated true "{\"blocked\": $SUBPROC}"
if ran && ! fires; then pass CT-02 "every separate-process language blocked: does not fire"
else fail CT-02 "fired with every separate-process language blocked (rc=$RC)" "$OUT"; fi

run ct03 elevated true '{"allowed": ["javascript", "sql"]}'
if ran && ! fires; then pass CT-03 "allowed only javascript and sql: does not fire"
else fail CT-03 "fired with only in-process languages allowed (rc=$RC)" "$OUT"; fi

run ct04 elevated true '{"allowed": ["javascript", "node"]}'
if ran && fires; then pass CT-04c "CONTROL: allowed node (a subprocess runtime): fires"
else fail CT-04c "did not fire with node allowed (rc=$RC)" "$OUT"; fi

# Does this build run Python in-process? Under shell off, only an in-process
# executor runs at all.
pd="$W/pyprobe"; mkdir -p "$pd"
printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}, "capabilities": {"shell": {"enabled": false}}}\n' > "$pd/govern.json"
printf 'main {\n    let r = <<python\n6 * 7\n>>\n    print("PY=" + string(r))\n}\n' > "$pd/p.naab"
PYOUT=$(cd "$pd" && timeout 60s "$NAAB" p.naab 2>&1) || true
if grep -q "PY=42" <<<"$PYOUT"; then PY_INPROC=1
elif grep -qiE "separate process|shell is disabled|not available|refus" <<<"$PYOUT"; then PY_INPROC=0
else PY_INPROC=""; fi
run ct05 elevated true '{"allowed": ["python"]}'
if [ -z "$PY_INPROC" ]; then unm CT-05 "could not tell whether this build embeds Python"
elif [ "$PY_INPROC" = 1 ] && ran && ! fires; then pass CT-05 "python embedded here, so allowing it does not fire"
elif [ "$PY_INPROC" = 0 ] && ran && fires; then pass CT-05 "python is a subprocess here, so allowing it fires"
else fail CT-05 "CONTRA-013 disagreed with how this build runs Python (inproc=$PY_INPROC, rc=$RC)" "$OUT"; fi

run ct06 restricted true '{}'
if ran && ! fires; then pass CT-06c "CONTROL: sandbox restricted: does not fire"
else fail CT-06c "fired under restricted (rc=$RC)" "$OUT"; fi

echo ""
echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$UNM"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
