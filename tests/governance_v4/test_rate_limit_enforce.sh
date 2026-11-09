#!/usr/bin/env bash
# test_rate_limit_enforce.sh -- a rate-limit refusal is a HARD governance block
# (Bundle 17; the owner's decision of 2026-11-09: "HARD, like other limits")
#
# limits.rate.max_stdlib_calls_per_second, max_file_ops_per_second and
# max_polyglot_per_second refused a call with a plain runtime error (wired in
# 883a06c1 as runtimeError / std::runtime_error, never through enforce()).
# Observed on master 314bccfd: a loop of 40 string.trim() calls under a limit of
# 5 per second inside try/catch caught 35 refusals, and the run ended
# "Governance: PASS (0 static, 0 runtime, 0 violations)", exit 0 -- the calls
# were refused, but nothing recorded it and the verdict said PASS. Every other
# limit (array size, polyglot block count, loop iterations) blocks at HARD
# through enforce(): uncatchable, recorded, exit 3. The rate limits now do the
# same, at each call site on both engines and in codegen.run(), through
# GovernanceEngine::check{Stdlib,FileOps,Polyglot}RateLimit().
#
#   RL-00c  CONTROL under the limit: the calls run (exit 0)
#   RL-01   stdlib limit crossed inside try/catch: exit 3, the catch never runs,
#           and the rule is named
#   RL-02   stdlib limit crossed with no try/catch: exit 3 (was 1)
#   RL-03   file-op limit crossed: exit 3
#   RL-04   polyglot limit crossed (JavaScript block in a loop): exit 3
#   RL-05   codegen.run() shares the polyglot limit: exit 3
#   RL-06c  CONTROL no limit configured: 200 calls run (exit 0)
#
# Kill table (each mutant applied alone, rebuilt, this suite run; observed
# 2026-11-09):
#   the stdlib site back to a catchable runtime error      RL-01, RL-02
#   the file-op site back to a catchable runtime error     RL-03
#   the VM polyglot site back to a catchable runtime error RL-04
#   codegen back to its own catchable error                RL-05
# Red first (observed, master 314bccfd): RL-01..RL-05 fail; the controls pass.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_ratelim_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       -> /'; }

if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then echo "UNMEASURABLE: naab-lang not built"; exit 1; fi

# run ID RATE_JSON PROGRAM [EXTRA_CONFIG]
run() {
    local d="$W/$1"; mkdir -p "$d"
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}, "limits": {"rate": %s}%s}\n' "$2" "${4:-}" > "$d/govern.json"
    printf '%s\n' "$3" > "$d/p.naab"
    RC=0; OUT=$(cd "$d" && timeout 60s "$NAAB" p.naab 2>&1) || RC=$?
}
blocked() { [ "$RC" -eq 3 ] && grep -q "Rule: limits.rate.$1" <<<"$OUT" && ! grep -q "DONE" <<<"$OUT"; }

loop_prog() {   # loop_prog N BODY
    printf 'use string\nmain {\n    let s = " a "\n    let i = 0\n    while i < %s {\n        %s\n        i = i + 1\n    }\n    print("DONE i=" + string(i))\n}\n' "$1" "$2"
}
CATCH_PROG='use string
main {
    let s = " a "
    let i = 0
    let refused = 0
    while i < 40 {
        try {
            let t = string.trim(s)
        } catch (e) {
            refused = refused + 1
        }
        i = i + 1
    }
    print("DONE refused=" + string(refused))
}'

echo "=== rate-limit refusals are HARD governance blocks ==="

run rl00 '{"max_stdlib_calls_per_second": 1000}' "$(loop_prog 5 'let t = string.trim(s)')"
if [ "$RC" -eq 0 ] && grep -q "DONE i=5" <<<"$OUT"; then pass RL-00c "CONTROL: under the limit the calls run"
else fail RL-00c "calls under the limit did not run (rc=$RC)" "$OUT"
     echo ""; echo "PASS=$PASS FAIL=$FAIL"; exit 1; fi

run rl01 '{"max_stdlib_calls_per_second": 5}' "$CATCH_PROG"
if blocked max_stdlib_calls_per_second; then pass RL-01 "stdlib limit inside try/catch: exit 3, uncatchable, the rule named"
else fail RL-01 "a stdlib rate refusal was caught or not recorded (rc=$RC)" "$OUT"; fi

run rl02 '{"max_stdlib_calls_per_second": 5}' "$(loop_prog 40 'let t = string.trim(s)')"
if blocked max_stdlib_calls_per_second; then pass RL-02 "stdlib limit, no try/catch: exit 3"
else fail RL-02 "a stdlib rate refusal did not exit 3 (rc=$RC)" "$OUT"; fi

run rl03 '{"max_file_ops_per_second": 3}' "$(loop_prog 40 'let e = file.exists("nothing_here.txt")')"
if blocked max_file_ops_per_second; then pass RL-03 "file-op limit: exit 3"
else fail RL-03 "a file-op rate refusal did not exit 3 (rc=$RC)" "$OUT"; fi

run rl04 '{"max_polyglot_per_second": 2}' 'main {
    let i = 0
    while i < 10 {
        let r = <<javascript
1 + 1
>>
        i = i + 1
    }
    print("DONE i=" + string(i))
}'
if blocked max_polyglot_per_second; then pass RL-04 "polyglot limit: exit 3"
else fail RL-04 "a polyglot rate refusal did not exit 3 (rc=$RC)" "$OUT"; fi

run rl05 '{"max_polyglot_per_second": 2}' 'use codegen
main {
    let i = 0
    while i < 10 {
        let r = codegen.run("javascript", "1 + 1")
        i = i + 1
    }
    print("DONE i=" + string(i))
}' ', "codegen": {"enabled": true, "allowed_languages": ["javascript"]}'
if blocked max_polyglot_per_second; then pass RL-05 "codegen.run shares the polyglot limit: exit 3"
else fail RL-05 "a codegen rate refusal did not exit 3 (rc=$RC)" "$OUT"; fi

run rl06 '{}' "$(loop_prog 200 'let t = string.trim(s)')"
if [ "$RC" -eq 0 ] && grep -q "DONE i=200" <<<"$OUT"; then pass RL-06c "CONTROL: no limit configured, 200 calls run"
else fail RL-06c "unlimited calls did not run (rc=$RC)" "$OUT"; fi

echo ""
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
