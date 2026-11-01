#!/usr/bin/env bash
# test_config_fuzz.sh -- a govern.json loads as written or refuses to load (Bundle 9)
#
# tests/fuzzing/config/fuzz_config.py mutates a restrictive config (a blocked
# env variable, a blocked path, shell disabled) without changing what it
# restricts -- junk keys, retyped values, the same restriction written another
# way, malformed text -- and runs a probe for each restricted action. Per case
# and probe: no hang, no signal, and either exit 4 or the action refused.
# The tool's own controls run first (each probe's action happens unrestricted
# and is refused by the unmutated config).
#
#   CF-00   CONTROL: with a planted mutation that really drops the
#           restrictions, every case fails ("loaded weaker than written" fires)
#   CF-01   seeds 1-40 pass
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
FUZZ="$SCRIPT_DIR/config/fuzz_config.py"
PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -25 | sed 's/^/       | /'; }

echo "=== Config fuzzer (Bundle 9) ==="
if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then
    echo "  UNMEASURABLE [CF-01] naab-lang not built"; echo "Results: 0 passed, 1 failed"; exit 1
fi
out="$(python3 "$FUZZ" --naab "$NAAB" --seeds 1-6 --plant drop 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then pass CF-00 "CONTROL: a mutation that drops the restrictions fails every case"
else fail CF-00 "CONTROL: dropped restrictions were not caught on every case" "$out"; fi
out="$(python3 "$FUZZ" --naab "$NAAB" --seeds 1-40 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then pass CF-01 "40 mutated configs: each refused or held its restrictions ($(tail -1 <<<"$out"))"
else fail CF-01 "a mutated config crashed, hung or loaded weaker than written" "$out"; fi
echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
