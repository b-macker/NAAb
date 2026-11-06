#!/usr/bin/env bash
# A122 (Bundle 16): the engine's own child processes do not read the run's
# stdin, and prerequisite commands run only where a program runs.
#
# (a) A `command` prerequisite was started with the run's stdin, so an
#     interpreter named as a prerequisite read -- and ran -- whatever was piped
#     to the program, and the program then read nothing. (b) A hook inherited
#     the run's stdin the same way. (c) Every govern.json load ran the
#     prerequisites, so `naab-gov lint`, `taint`, `ratchet`, `manifest check`
#     and `naab-lang --lint-only` started the commands a project's govern.json
#     names. Decided (the owner, 2026-11-06): the engine's children start with
#     the null device as stdin (ChildStdin::Null, fireHook); a child the PROGRAM
#     starts (process.run, polyglot executors) still inherits it; prerequisites
#     run only under `naab-lang run` (and its workers) and `naab-lang --attest`,
#     and other loads print that they were not run.
#
# A piped line, PIPED_INPUT, is the probe. A child that reads stdin writes what
# it read to a file; each arm requires that file to EXIST (the child ran) and
# to be empty (it read nothing), and the program's own io.read_line() to still
# return PIPED_INPUT (the input was left for the program).
#
#   ES-00  CONTROL: with no prerequisite or hook, the program reads PIPED_INPUT
#          -- the pipe reaches the program at all
#   ES-01  a `command` prerequisite (sh -c 'cat > file') runs, reads nothing,
#          and the program still reads PIPED_INPUT
#   ES-02  a pre_check hook (sh -c 'cat > file') runs, reads nothing, and the
#          program still reads PIPED_INPUT
#   ES-03  CONTROL: a child the PROGRAM starts (process.run) still inherits
#          stdin and reads PIPED_INPUT -- the fix is not wider than decided
#   ES-04  under `naab-lang run` a prerequisite that appends a line per run
#          runs exactly once -- the control for ES-05
#   ES-05  under each static command -- naab-gov lint / taint / ratchet /
#          manifest check / check, naab-lang --lint-only -- the marker is NOT
#          made, and the command prints that prerequisites were not run
#   ES-06  `naab-lang --attest` runs them exactly once and reports the result
#          (it ran each twice: at the load, then again to print)
#   ES-07  pinned, not decided: `naab-gov lint` on a program with a violation
#          fires an on_violation hook (enforce() fires hooks; lint compiles the
#          program) -- recorded as A126 for the owner
#
# Red first (observed 2026-11-06, 01fb3552): ES-01 and ES-02 fail (the file
# holds PIPED_INPUT, the program read an empty line); every ES-05 command
# makes the marker; ES-06 reads runs=2 (--attest ran each check twice).
#
# Kill table (each mutant alone, both binaries rebuilt; observed 2026-11-06):
#   M55 ChildStdin::Null not dup2'd onto fd 0          -> ES-01
#   M56 fireHook leaves stdin inherited                -> ES-02
#   M57 prerequisitesAllowed() always true             -> ES-05 (every command)
#   M58 `naab-lang run` does not allow them            -> ES-01, ES-04
#   M59 `--attest` does not allow them                 -> ES-06
#   M65 `--attest` runs them again to print            -> ES-06 (runs=2)
# The first run of M58/M59 also read ES-05 red: the harness had rebuilt only
# naab-lang, so naab-gov still carried M57. It now rebuilds both (one sample
# each; traced, the rerun with a fresh naab-gov showed none).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
GOV="${2:-$(dirname "$NAAB")/naab-gov}"
case "$NAAB" in /*) ;; *) NAAB="$PWD/$NAAB" ;; esac
case "$GOV" in /*) ;; *) GOV="$PWD/$GOV" ;; esac

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -10 | sed 's/^/       /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }

echo "=== The engine's own children do not read the run's stdin (A122) ==="
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) echo "  SKIP: POSIX-only (sh -c, /dev/null); Windows is traced, not run"; exit 0 ;;
esac
for b in "$NAAB" "$GOV"; do
    [ -x "$b" ] || { echo "  FAIL $(basename "$b") not built (UNMEASURABLE, not a pass)"; exit 1; }
done
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-childstdin.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
trap 'rm -rf "$W"' EXIT
case "$W" in *[[:space:]]*) echo "  SKIP: the work dir has a space (the prerequisite argv is split on spaces)"; exit 0 ;; esac
# An isolated trust store: these configs are unsigned.
export HOME="$W/home"; mkdir -p "$HOME"

# A program that reads one line and prints it; optionally starts a child too.
cat > "$W/read.naab" <<'EOF'
main {
    let line = io.read_line()
    print("PROGRAM_READ=" + line)
}
EOF
cat > "$W/proc.naab" <<EOF
use process

main {
    let r = process.run("/bin/sh", ["-c", "cat > $W/d3/got_proc.txt"])
    print("PROC_DONE")
}
EOF

# cfg DIR EXTRA_JSON: an enforce config at elevated (so commands may start)
cfg() {
    mkdir -p "$1"
    cat > "$1/govern.json" <<EOF
{
  "version": "3.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" }$2
}
EOF
    python3 -c 'import json,sys; json.load(sys.stdin)' < "$1/govern.json" \
        || { echo "FATAL: fixture $1/govern.json is not JSON" >&2; exit 1; }
}

# runread DIR -> OUT, RC: pipe PIPED_INPUT to read.naab run from DIR
OUT=""; RC=0
runread() {
    cp "$W/read.naab" "$1/read.naab"
    OUT=$(cd "$1" && printf 'PIPED_INPUT\n' | timeout 30 "$NAAB" run read.naab 2>&1); RC=$?
}

cfg "$W/d0" ""
runread "$W/d0"
if [ "$RC" -eq 0 ] && grep -q '^PROGRAM_READ=PIPED_INPUT$' <<<"$OUT"; then
    ok ES-00 "CONTROL: the program reads PIPED_INPUT"
else
    bad ES-00 "CONTROL: the program should read PIPED_INPUT (rc=$RC)" "$OUT"
fi

# stdin_arm ID FILE WHY: the child wrote FILE; it must exist and be empty, and
# the program must still have read the line
stdin_arm() {
    local id="$1" f="$2" why="$3"
    if [ "$RC" -eq 0 ] && [ -f "$f" ] && [ ! -s "$f" ] && grep -q '^PROGRAM_READ=PIPED_INPUT$' <<<"$OUT"; then
        ok "$id" "$why"
    else
        local st="absent"; [ -f "$f" ] && st="size=$(wc -c < "$f") content=$(head -c 40 "$f")"
        bad "$id" "$why (rc=$RC, child's file: $st)" "$OUT"
    fi
}

cfg "$W/d1" ',
  "prerequisites": { "enabled": true, "checks": [
    { "type": "command", "name": "/bin/sh -c cat>'"$W"'/d1/got_prereq.txt", "required": "exists", "level": "advisory" }
  ] }'
runread "$W/d1"
stdin_arm ES-01 "$W/d1/got_prereq.txt" "a command prerequisite reads nothing; the program reads PIPED_INPUT"

cfg "$W/d2" ',
  "hooks": { "pre_check": { "command": "/bin/sh", "args": ["-c", "cat > '"$W"'/d2/got_hook.txt"], "timeout": 5 } }'
runread "$W/d2"
stdin_arm ES-02 "$W/d2/got_hook.txt" "a pre_check hook reads nothing; the program reads PIPED_INPUT"

cfg "$W/d3" ""
cp "$W/proc.naab" "$W/d3/proc.naab"
OUT=$(cd "$W/d3" && printf 'PIPED_INPUT\n' | timeout 30 "$NAAB" run proc.naab 2>&1); RC=$?
if [ "$RC" -eq 0 ] && grep -q '^PIPED_INPUT$' "$W/d3/got_proc.txt" 2>/dev/null; then
    ok ES-03 "CONTROL: a child the program starts still inherits stdin"
else
    bad ES-03 "CONTROL: process.run's child should read PIPED_INPUT (rc=$RC)" "$OUT"
fi

# --- (c) where prerequisites run --------------------------------------------
D="$W/d4"
cfg "$D" ',
  "prerequisites": { "enabled": true, "checks": [
    { "type": "command", "name": "/bin/sh -c echo>>'"$D"'/MARKER", "required": "exists", "level": "advisory" }
  ] }'
printf 'main {\n    print("RAN")\n}\n' > "$D/p.naab"
printf 'x = 1\n' > "$D/x.py"
# The prerequisite appends one line per run, so the marker counts runs.
marker() { [ -e "$D/MARKER" ]; }
runs() { if [ -e "$D/MARKER" ]; then wc -l < "$D/MARKER" | tr -d ' '; else echo 0; fi; }

rm -f "$D/MARKER"
OUT=$(cd "$D" && timeout 30 "$NAAB" run p.naab </dev/null 2>&1); RC=$?
if [ "$RC" -eq 0 ] && [ "$(runs)" -eq 1 ] && grep -q '^RAN$' <<<"$OUT"; then
    ok ES-04 "naab-lang run runs the prerequisite once"
else
    bad ES-04 "naab-lang run should run the prerequisite once (runs=$(runs), rc=$RC)" "$OUT"
fi

static() {  # static NAME CMD...: no marker, the not-run note printed
    local name="$1"; shift
    rm -f "$D/MARKER"
    OUT=$(cd "$D" && timeout 60 "$@" </dev/null 2>&1); RC=$?
    if ! marker && grep -q 'environment prerequisite check(s) not run' <<<"$OUT"; then
        ok ES-05 "$name: prerequisite not run, and it says so (rc=$RC)"
    else
        bad ES-05 "$name: the prerequisite should not run (marker=$(marker && echo yes || echo no), rc=$RC)" "$OUT"
    fi
}
static "naab-gov lint"           "$GOV" lint p.naab
static "naab-gov taint"          "$GOV" taint p.naab
static "naab-gov ratchet"        "$GOV" ratchet govern.json govern.json
static "naab-gov manifest check" "$GOV" manifest check p.naab
static "naab-gov check"          "$GOV" check --language python --file x.py
static "naab-lang --lint-only"   "$NAAB" run --lint-only p.naab

rm -f "$D/MARKER"
OUT=$(cd "$D" && timeout 30 "$NAAB" --attest </dev/null 2>&1); RC=$?
if [ "$(runs)" -eq 1 ] && grep -q '\[PASS\] command:' <<<"$OUT"; then
    ok ES-06 "naab-lang --attest runs the prerequisite once and reports it (rc=$RC)"
else
    bad ES-06 "naab-lang --attest should run the prerequisite once and report it (runs=$(runs), rc=$RC)" "$OUT"
fi

# --- ES-07: pinned observation, A126 -----------------------------------------
E="$W/d7"
cfg "$E" ',
  "code_quality": { "no_placeholders": { "enabled": true, "level": "hard" } },
  "hooks": { "on_violation": { "command": "/usr/bin/env", "args": ["touch", "'"$W"'/d7/HOOK_FIRED"], "timeout": 5 } }'
printf 'fn f() -> int {\n    // TODO: finish this\n    return 1\n}\nmain {\n    print(f())\n}\n' > "$E/p.naab"
rm -f "$E/HOOK_FIRED"
OUT=$(cd "$E" && timeout 60 "$GOV" lint p.naab </dev/null 2>&1); RC=$?
if [ -e "$E/HOOK_FIRED" ]; then
    ok ES-07 "pinned (A126, undecided): naab-gov lint fires an on_violation hook (rc=$RC)"
elif ! grep -qi 'placeholder\|TODO' <<<"$OUT"; then
    skip ES-07 "the lint reported no violation, so the hook had nothing to fire on (rc=$RC)"
else
    bad ES-07 "pinned: naab-gov lint was observed to fire the on_violation hook; it no longer does -- update A126 (rc=$RC)" "$OUT"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
