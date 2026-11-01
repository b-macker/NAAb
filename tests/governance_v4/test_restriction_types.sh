#!/usr/bin/env bash
# test_restriction_types.sh -- a restriction the loader cannot read refuses the config (A111)
#
# The capability parsers read a switch only when it is a boolean and a list
# entry only when it is a string, and drop anything else without a word. So a
# restriction written in another type loaded WITHOUT the restriction: found by
# the config fuzzer (tests/fuzzing/config), which saw shell run under
# `"shell": {"enabled": "no"}` / `0` / `"off"` / `"false"`, and a blocked file
# and variable read under `"blocked_paths": [["vault/"]]` and
# `"blocked_read": [["SECRET"]]`, all under mode enforce. Such a config is now
# refused at load (exit 4, the key named), as a config that does not parse is.
#
#   TY-00   CONTROL: the well-typed config refuses each action (exit 3, nothing
#           read, no shell marker) -- the probes can see a restriction hold
#   TY-00c  CONTROL: with no restriction each action happens -- the probes can
#           see the action
#   TY-01   shell.enabled "no"          -> exit 4, shell did not run
#   TY-02   shell.enabled 0             -> exit 4, shell did not run
#   TY-03   blocked_paths [["vault/"]]  -> exit 4, the file was not read
#   TY-04   blocked_read [["SECRET"]]   -> exit 4, the variable was not read
#   TY-05   languages.allowed "python" (read only as a list) -> exit 4
#   TY-06   an agent role's blocked_paths "vault/" (read only as a list) -> exit 4
#   TY-07   CONTROL: the forms the loader does apply still load and restrict:
#           blocked_read as one string, the bare "shell": false, and
#           "filesystem": true (restricts nothing) loads
#   TY-08   a mid-run reload to an ill-typed config is a config error (naab-gov
#           ratchet exit 4), not an accepted reload
#
# Kill table (observed 2026-11-01): checkRestrictionTypes() not called ->
# TY-01..TY-06, TY-08; the strict array rule dropped -> TY-05, TY-06.
# Red first (observed): on master fc9b8099 TY-01..TY-06 fail (each loads; shell
# runs, the file and the variable are read) and every control passes. TY-08 was
# UNMEASURABLE there (that build had no naab-gov); it fails under the first
# mutant above, where the reload is refused as a loosening, not as a config error.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
GOV="$(dirname "$NAAB")/naab-gov"
[ -x "$GOV" ] || [ ! -x "$GOV.exe" ] || GOV="$GOV.exe"
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_rtypes_XXXXXX")
trap 'rm -rf "$TEST_TMP"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -6 | sed 's/^/       | /'; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

export NAAB_TY_SECRET="tok-ty-$$"
CAPS_OK='"env_vars": {"blocked_read": ["NAAB_TY_SECRET"]}, "filesystem": {"mode": "write", "blocked_paths": ["vault/"]}, "shell": {"enabled": false}'

# run ID CAPS_JSON PROBE [EXTRA_TOP_LEVEL] -> RC, OUT, HAPPENED (yes/no)
run() {
    local d="$TEST_TMP/$1"; mkdir -p "$d/vault"; printf 'tok-file-%s' "$$" > "$d/vault/s.txt"
    printf '{"version": "3.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}, "capabilities": {%s}%s}\n' "$2" "${4:-}" > "$d/govern.json"
    case "$3" in
        env)   printf 'use env\nmain {\n    print("VAL=" + string(env.get("NAAB_TY_SECRET")))\n}\n' > "$d/p.naab" ;;
        file)  printf 'use file\nmain {\n    print("FILE=" + file.read("vault/s.txt"))\n}\n' > "$d/p.naab" ;;
        shell) printf 'main {\n    let r = <<sh\necho made > marker.txt\n>>\n    print("SHELL-RAN")\n}\n' > "$d/p.naab" ;;
        plain) printf 'main {\n    print("PLAIN-RAN")\n}\n' > "$d/p.naab" ;;
    esac
    RC=0; OUT=$(cd "$d" && timeout 60s "$NAAB" p.naab 2>&1) || RC=$?
    HAPPENED=no
    case "$3" in
        env)   grep -q "VAL=$NAAB_TY_SECRET" <<<"$OUT" && HAPPENED=yes ;;
        file)  grep -q "FILE=tok-file-$$" <<<"$OUT" && HAPPENED=yes ;;
        shell) [ -f "$d/marker.txt" ] && HAPPENED=yes ;;
        plain) grep -q 'PLAIN-RAN' <<<"$OUT" && HAPPENED=yes ;;
    esac
}

echo "=== A restriction the loader cannot read refuses the config (A111) ==="

ok=1; why=""
for p in env file shell; do
    run "c-$p" "$CAPS_OK" "$p"
    if [ "$HAPPENED" = yes ] || [ "$RC" -eq 4 ]; then ok=0; why="$p: rc=$RC happened=$HAPPENED"; fi
done
if [ "$ok" -eq 1 ]; then pass TY-00 "CONTROL: the well-typed config refuses env, file and shell"
else fail TY-00 "CONTROL: the well-typed config should refuse each action" "$why"; fi

open_ok=1; why=""
for p in env file shell; do
    run "o-$p" '"filesystem": {"mode": "write"}' "$p"
    [ "$HAPPENED" = yes ] || { open_ok=0; why="$why $p:rc=$RC"; }
done
if [ "$open_ok" -eq 1 ]; then pass TY-00c "CONTROL: with no restriction env, file and shell all happen"
else unm TY-00c "an action does not happen even unrestricted here ($why) -- its arms cannot measure"; fi

# arm ID DESC CAPS PROBE [EXTRA]
arm() {
    run "$1" "$3" "$4" "${5:-}"
    if [ "$RC" -eq 4 ] && [ "$HAPPENED" = no ] && grep -q 'must be' <<<"$OUT"; then
        pass "$1" "$2: refused at load (exit 4), nothing happened"
    else fail "$1" "$2 should refuse the config" "rc=$RC happened=$HAPPENED $(grep -m1 -iE 'error|must be' <<<"$OUT")"; fi
}
arm TY-01 'shell.enabled "no"' '"shell": {"enabled": "no"}' shell
arm TY-02 'shell.enabled 0' '"shell": {"enabled": 0}' shell
arm TY-03 'blocked_paths [["vault/"]]' '"filesystem": {"mode": "write", "blocked_paths": [["vault/"]]}' file
arm TY-04 'blocked_read [["NAAB_TY_SECRET"]]' '"env_vars": {"blocked_read": [["NAAB_TY_SECRET"]]}' env
arm TY-05 'languages.allowed "python"' '"shell": {"enabled": false}' plain ', "languages": {"allowed": "python"}'
arm TY-06 "an agent role's blocked_paths \"vault/\"" '"shell": {"enabled": false}' plain ', "agents": {"r": {"provider": "gemini", "blocked_paths": "vault/"}}'

ok=1; why=""
run t7a '"env_vars": {"blocked_read": "NAAB_TY_SECRET"}' env
{ [ "$RC" -ne 4 ] && [ "$HAPPENED" = no ]; } || { ok=0; why="blocked_read string: rc=$RC happened=$HAPPENED"; }
run t7b '"shell": false' shell
{ [ "$RC" -ne 4 ] && [ "$HAPPENED" = no ]; } || { ok=0; why="$why; bare shell false: rc=$RC happened=$HAPPENED"; }
run t7c '"filesystem": true' plain
{ [ "$RC" -eq 0 ] && [ "$HAPPENED" = yes ]; } || { ok=0; why="$why; filesystem true: rc=$RC"; }
if [ "$ok" -eq 1 ]; then pass TY-07 "CONTROL: a one-string blocked_read, bare \"shell\": false and \"filesystem\": true load and behave as before"
else fail TY-07 "CONTROL: an accepted form changed behaviour" "$why"; fi

if [ -x "$GOV" ]; then
    d="$TEST_TMP/t8"; mkdir -p "$d"
    printf '{"version": "3.0", "mode": "enforce", "capabilities": {"shell": {"enabled": false}}}\n' > "$d/old.json"
    printf '{"version": "3.0", "mode": "enforce", "capabilities": {"shell": {"enabled": "no"}}}\n' > "$d/new.json"
    touch -d '+2 seconds' "$d/new.json" 2>/dev/null || true
    RC=0; OUT=$(cd "$d" && "$GOV" ratchet old.json new.json --ratchet-only 2>&1) || RC=$?
    if [ "$RC" -eq 4 ] && grep -q 'must be' <<<"$OUT"; then pass TY-08 "a reload to an ill-typed config is a config error (exit 4), not installed"
    else fail TY-08 "a reload to an ill-typed config should be a config error" "rc=$RC $(head -2 <<<"$OUT")"; fi
else
    unm TY-08 "naab-gov not built -- UNMEASURABLE"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ] || exit 1
exit 0
