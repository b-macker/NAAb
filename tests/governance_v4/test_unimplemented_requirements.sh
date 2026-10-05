#!/usr/bin/env bash
# ============================================================
# test_unimplemented_requirements.sh -- requirements blocks whose check was never built
#
# requirements.error_handling and requirements.naming_conventions are parsed
# (ratchet, inheritance, explicitly_set) and consulted by nothing:
# requiresErrorHandling() has no caller and no naming checker exists
# (docs/findings/inert-config-keys.md). They used to draw the enable/level
# mismatch warnings -- "the check is ON" / "the check is OFF" -- which both
# imply a check exists. The loader now says what is true.
#
#   RU-01  error_handling with a level warns that nothing enforces it
#   RU-02  error_handling as the legacy string form warns too
#   RU-03  naming_conventions warns that nothing enforces it
#   RU-04  TRUTH: under HARD error_handling, code with no try/catch still runs
#          -- the warning describes the engine, it is not a guess
#   RU-05  CONTROL: {enabled:false} with no level asks for nothing -- silent
#   RU-06  CONTROL: requirements.main_block, whose check exists, keeps its own
#          enable/level warning -- a change that silenced the whole section
#          would pass RU-05 and fail here
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== requirements blocks with no check behind them ==="

if [ ! -x "$NAAB" ]; then
    for id in RU-01 RU-02 RU-03 RU-04 RU-05 RU-06; do skip "$id" "naab-lang not built (UNMEASURABLE)"; done
    echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-unimpl-req.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust   # the unsigned govern.json below must not meet a populated trust store
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT

printf 'main {\n    print("PROGRAM_RAN")\n}\n' > "$W/p.naab"

# run <requirements JSON> -> combined output of one run
run() {
    printf '{ "version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },\n  "requirements": %s }\n' "$1" > "$W/govern.json"
    (cd "$W" && timeout 30 "$NAAB" p.naab 2>&1)
}
UNIMPL='is accepted but no check enforces it'

# expect_warn <id> <requirements JSON> <name> <description>
expect_warn() {
    local out; out="$(run "$2")"
    case "$out" in *PROGRAM_RAN*) ;; *) bad "$1" "$4" "program did not run: $(printf '%s' "$out" | tail -1)"; return ;; esac
    case "$out" in
        *"\"requirements.$3\" $UNIMPL"*) ok "$1" "$4" ;;
        *) bad "$1" "$4" "no unimplemented-check warning for requirements.$3" ;;
    esac
}

expect_warn RU-01 '{ "error_handling": { "level": "hard" } }'           error_handling     "error_handling with a level warns that nothing enforces it"
expect_warn RU-02 '{ "error_handling": "hard" }'                         error_handling     "the legacy string form warns too"
expect_warn RU-03 '{ "naming_conventions": { "variables": "snake_case" } }' naming_conventions "naming_conventions warns that nothing enforces it"

# RU-04: the program has no try/catch; a working HARD error_handling check
# would refuse it. It runs.
out="$(run '{ "error_handling": { "level": "hard" } }')"
case "$out" in
    *PROGRAM_RAN*) ok RU-04 "TRUTH: under HARD error_handling, code with no try/catch still runs" ;;
    *) bad RU-04 "the program was refused -- a check DOES exist and the warning is false" "$(printf '%s' "$out" | tail -2)" ;;
esac

out="$(run '{ "error_handling": { "enabled": false } }')"
case "$out" in
    *PROGRAM_RAN*) case "$out" in
        *"$UNIMPL"*) bad RU-05 "{enabled:false} warned although it asks for nothing" ;;
        *) ok RU-05 "CONTROL: {enabled:false} asks for nothing and stays silent" ;; esac ;;
    *) bad RU-05 "program did not run" "$(printf '%s' "$out" | tail -1)" ;;
esac

out="$(run '{ "main_block": { "enabled": true } }')"
case "$out" in
    *'"requirements.main_block.enabled": true does not enable this check'*)
        ok RU-06 "CONTROL: main_block, whose check exists, keeps its enable/level warning" ;;
    *) bad RU-06 "main_block's enable/level warning disappeared" "$(printf '%s' "$out" | grep -i warning | head -2)" ;;
esac

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
