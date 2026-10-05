#!/usr/bin/env bash
# ============================================================
# test_nested_unknown_keys.sh -- a nested govern.json key the engine never reads is reported
#
# validateSchema() checked top-level keys only. A nested key the loader never
# reads was accepted in silence, and the silent default was usually the weaker
# one: agent_dispatch.hard_stop.max_calls (the loader reads max_calls_per_run)
# left a run with no call budget; api.auth (REST auth is api.keys) left
# /api/v1/execute unauthenticated. The schema is GENERATED from the loader by
# tools/config_known_keys.py into src/runtime/governance_known_keys.inc.
#
#   NK-00  the committed schema matches what the generator produces now -- a
#          loader change cannot leave it stale (UNMEASURABLE without python3)
#   NK-01  api.auth                           is reported
#   NK-02  telemetry.forwarding               is reported
#   NK-03  agent_dispatch.hard_stop.max_calls is reported
#   NK-04  agents.<name>.timeout_seconds      is reported (agents read "timeout")
#   NK-05  CONTROL: the spellings the loader DOES read, annotations, and a
#          contract's fixture data are all silent, and the program ran --
#          without it NK-01..04 pass for a validator that reports every key
#   NK-06  CONTROL: both shipped templates load, run, and report nothing
#   NK-07  more than 25 findings are summarised, not printed one per line
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$REPO/build/naab-lang"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== nested govern.json keys the engine never reads ==="

# --- NK-00: the committed schema is current ---------------------------------
# Compared as bytes the generator writes itself (ASCII, no CRLF translation),
# against the committed file read by the shell -- no path crosses into Python.
# Line endings are not part of the schema: a Windows checkout (core.autocrlf)
# hands the shell the committed file with CRLF, and the first build-windows run
# reported "stale" for a file whose paths were identical. Both sides are
# compared with CR removed, and a real mismatch prints the first differing line
# with control characters visible (tests/helpers/encoding_controls.sh), so a
# platform difference can never again pass for a content difference.
. "$REPO/tests/helpers/encoding_controls.sh"
if command -v python3 >/dev/null 2>&1; then
    fresh="$(cd "$REPO" && python3 tools/config_known_keys.py 2>&1)"; prc=$?
    committed="$(cat "$REPO/src/runtime/governance_known_keys.inc")"
    fresh_n="$(printf '%s' "$fresh" | enc_strip_cr)"
    committed_n="$(printf '%s' "$committed" | enc_strip_cr)"
    if [ "$prc" -ne 0 ]; then
        bad NK-00 "the generator failed -- the schema cannot be checked" "$(printf '%s' "$fresh" | tail -1)"
    elif [ -z "$fresh_n" ]; then
        bad NK-00 "the generator printed nothing -- the schema cannot be checked"
    elif [ "$fresh_n" == "$committed_n" ]; then
        crnote=""
        [ "$committed" != "$committed_n" ] && crnote="; checkout has CRLF line endings"
        ok NK-00 "the committed schema matches the loader ($(printf '%s\n' "$committed_n" | grep -c '^"') paths$crnote)"
    else
        bad NK-00 "src/runtime/governance_known_keys.inc is stale" \
            "run: python3 tools/config_known_keys.py --write"
        i=0
        while IFS= read -r a <&3 && IFS= read -r b <&4; do
            i=$((i+1))
            if [ "$a" != "$b" ]; then
                echo "       first difference at line $i:"
                enc_escaped_diff "generator" "$a" "committed" "$b" | sed 's/^/       /'
                break
            fi
        done 3< <(printf '%s\n' "$fresh") 4< <(printf '%s\n' "$committed")
        echo "       lines: generator $(printf '%s\n' "$fresh_n" | wc -l), committed $(printf '%s\n' "$committed_n" | wc -l)"
    fi
else
    skip NK-00 "python3 unavailable -- schema freshness UNMEASURABLE"
fi

if [ ! -x "$NAAB" ]; then
    for id in NK-01 NK-02 NK-03 NK-04 NK-05 NK-06 NK-07; do skip "$id" "naab-lang not built (UNMEASURABLE)"; done
    echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; [ "$FAIL" -eq 0 ]; exit
fi

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-nested-keys.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust   # the unsigned govern.json below must not meet a populated trust store
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT
printf 'main {\n    print("PROGRAM_RAN")\n}\n' > "$W/p.naab"

NOT_READ='is not a setting the engine reads'
run() { printf '%s\n' "$1" > "$W/govern.json"; (cd "$W" && timeout 30 "$NAAB" p.naab 2>&1); }
BASE='"version": "4.0", "mode": "audit"'

# expect_reported <id> <config body> <dotted path> <description>
expect_reported() {
    local out; out="$(run "{ $BASE, $2 }")"
    case "$out" in *PROGRAM_RAN*) ;; *) bad "$1" "$4" "program did not run: $(printf '%s' "$out" | tail -1)"; return ;; esac
    case "$out" in
        *"\"$3\" $NOT_READ"*) ok "$1" "$4" ;;
        *) bad "$1" "$4" "\"$3\" was not reported" ;;
    esac
}

expect_reported NK-01 '"api": { "auth": { "keys": [ { "id": "a", "key": "k", "permissions": ["write"] } ] } }' \
    api.auth "api.auth is reported (REST auth is api.keys)"
expect_reported NK-02 '"telemetry": { "forwarding": { "webhook_url": "https://example.invalid/h" } }' \
    telemetry.forwarding "telemetry.forwarding is reported (the keys are flat under telemetry)"
expect_reported NK-03 '"agent_dispatch": { "hard_stop": { "max_calls": 5 } }' \
    agent_dispatch.hard_stop.max_calls "hard_stop.max_calls is reported (the budget key is max_calls_per_run)"
expect_reported NK-04 '"agents": { "worker": { "provider": "gemini", "model": "m", "timeout_seconds": 5 } }' \
    agents.worker.timeout_seconds "agents.<name>.timeout_seconds is reported (agents read timeout)"

# --- NK-05: what the loader does read stays silent --------------------------
out="$(run "{ $BASE,
  \"api\": { \"keys\": [ { \"name\": \"a\", \"key\": \"k\", \"scopes\": [\"execute\"] } ] },
  \"telemetry\": { \"webhook_url\": \"https://example.invalid/h\" },
  \"agent_dispatch\": { \"hard_stop\": { \"max_calls_per_run\": 5 } },
  \"agents\": { \"worker\": { \"provider\": \"gemini\", \"model\": \"m\", \"timeout\": 5,
                              \"rationale\": \"why\", \"_note\": \"annotation\" } },
  \"contracts\": { \"functions\": { \"f\": { \"must_handle_case\": [
      { \"inputs\": [ { \"any_field\": 1 } ], \"expect\": 2 } ] } } } }")"
case "$out" in
    *PROGRAM_RAN*) case "$out" in
        *"$NOT_READ"*) bad NK-05 "a key the loader reads was reported" "$(printf '%s' "$out" | grep "$NOT_READ" | head -2)" ;;
        *) ok NK-05 "CONTROL: read spellings, annotations and contract fixture data are silent" ;; esac ;;
    *) bad NK-05 "program did not run -- NK-01..04 are unmeasured" "$(printf '%s' "$out" | tail -1)" ;;
esac

# --- NK-06: the shipped templates are clean ---------------------------------
# Their "extends" names a file that is not shipped, so it is removed first.
# The program must run (Loaded + PROGRAM_RAN): zero findings from a config that
# never loaded would be a broken probe, not a result.
nk6=""
for t in "$REPO/govern-template.json" "$REPO/docs/govern-template.json"; do
    sed 's#"extends"[[:space:]]*:[[:space:]]*"[^"]*",##' "$t" > "$W/govern.json"
    out="$(cd "$W" && timeout 60 "$NAAB" p.naab 2>&1)"
    case "$out" in *PROGRAM_RAN*) ;; *) nk6="$nk6 ${t#$REPO/}:did-not-run"; continue ;; esac
    n="$(printf '%s\n' "$out" | grep -c "$NOT_READ")"
    [ "$n" -eq 0 ] || nk6="$nk6 ${t#$REPO/}:$n-findings"
done
if [ -z "$nk6" ]; then ok NK-06 "CONTROL: both templates load, run and report nothing"
else bad NK-06 "a shipped template draws findings or did not run" "$nk6"; fi

# --- NK-07: a long list is capped -------------------------------------------
many=""
for i in $(seq 1 30); do many="$many\"bogus_$i\": 1,"; done
out="$(run "{ $BASE, \"telemetry\": { ${many%,} } }")"
listed="$(printf '%s\n' "$out" | grep -c "$NOT_READ")"
case "$out" in
    *"5 more settings the engine does not read were not listed"*)
        if [ "$listed" -eq 25 ]; then ok NK-07 "30 findings: 25 listed, the rest summarised"
        else bad NK-07 "the summary appeared but $listed findings were listed (want 25)"; fi ;;
    *) bad NK-07 "30 findings were not summarised" "listed=$listed" ;;
esac

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
