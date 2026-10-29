#!/usr/bin/env bash
# test_typescript_memory_ceiling.sh -- TypeScript runs under the child memory budget (D13)
#
# A child started under a memory budget gets RLIMIT_DATA at the budget and an
# address-space ceiling of 4x, at least 2 GB (subprocess_helpers.cpp, L7). V8
# reserves far more address space than it uses: each WebAssembly instance
# reserves an ~8 GB guard region, and tsx instantiates a WebAssembly lexer
# while it transpiles, so every <<typescript>> block failed at `elevated` with
# "WebAssembly.instantiate(): Out of memory: Cannot allocate Wasm memory"
# (observed on master 6055ac3c). Measured with prlimit on an uncached tsx run:
# 32-48 GB of address space needed as is, 16 GB with --disable-wasm-trap-handler,
# under 2 GB with --jitless. The executor now starts tsx with --jitless (and
# --no-expose-wasm, which --jitless implies) whenever the child has a memory
# budget. The ceiling itself is not raised: it is the backstop for shared
# memory that RLIMIT_DATA does not count (tests/security/test_child_memory_limit.sh).
#
#   TM-00  CONTROL: a TypeScript block returns its value at `unrestricted`
#          (no budget) -- the toolchain works
#   TM-01  the same kind of block returns its value at `elevated` (each run's
#          content is new, so tsx transpiles rather than reading its cache)
#   TM-02  the budget still binds: at `elevated` a block that touches 1.5 GB
#          fails; TM-02c CONTROL: one that touches 64 MB succeeds
#   TM-03  the ceiling is unchanged: the child's /proc/self/limits reads an
#          address-space limit of max(4 x its data budget, 2 GB)
#   TM-04  the flag is applied only under a budget: the child's node sees
#          --jitless at `elevated` and not at `unrestricted`
#
# Kill table (each mutant rebuilt, suite re-run; observed 2026-10-29):
#   no --jitless for tsx (the pre-fix shape)   -> TM-01, TM-02c, TM-03, TM-04 red
#   --jitless whatever the budget (no budget check)       -> TM-04 red
# Red first: on master TM-01, TM-02c, TM-03 and TM-04 fail -- no TypeScript
# block runs at `elevated` there -- and TM-00 and TM-02 pass (TM-02 vacuously,
# for the same reason; on the fix it is paired with TM-02c).
# Linux only (the rlimits are Linux's); needs tsx on PATH, else SKIP.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

echo "=== TypeScript under the child memory budget (D13) ==="
if [ "$(uname -s)" != "Linux" ] || ! command -v tsx >/dev/null 2>&1; then
    for a in TM-00 TM-01 TM-02 TM-02c TM-03 TM-04; do
        echo "  SKIP [$a] needs Linux and tsx on PATH -- UNMEASURABLE"
    done
    exit 0
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_tsmem_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

# run ID LEVEL TS_BODY -> OUT, RC (the block's value is printed as VALUE=)
run() {
    local d="$W/$1"; mkdir -p "$d"
    printf '{"mode":"enforce","security":{"sandbox_level":"%s"}}\n' "$2" > "$d/govern.json"
    printf 'main {\n    let r = <<typescript\n%s\n>>\n    print("VALUE=" + string(r))\n}\n' "$3" > "$d/p.naab"
    RC=0; OUT=$(cd "$d" && timeout 180s "$NAAB" run p.naab 2>&1) || RC=$?
}
has() { grep -q -F -- "$1" <<<"$OUT"; }
brief() { grep -v '^\[governance\]' <<<"$OUT" | head -c 300 | tr '\n' ' '; }
fresh() { echo "$RANDOM$RANDOM"; }   # new content each run: tsx cannot use its cache

N=$(fresh); run tm00 unrestricted "const n: number = $N + 1
n"
if [ "$RC" -eq 0 ] && has "VALUE=$((N + 1))"; then
    pass TM-00 "CONTROL: a TypeScript block returns its value without a budget"
else
    fail TM-00 "a TypeScript block should run at unrestricted" "rc=$RC $(brief)"
fi

N=$(fresh); run tm01 elevated "const n: number = $N + 1
n"
if [ "$RC" -eq 0 ] && has "VALUE=$((N + 1))"; then
    pass TM-01 "a TypeScript block returns its value at elevated"
else
    fail TM-01 "a TypeScript block should run under the elevated budget" "rc=$RC $(brief)"
fi

N=$(fresh); run tm02 elevated "const tag: number = $N
const b = new Uint8Array(1536 * 1024 * 1024)
b.fill(1)
b.length"
if [ "$RC" -ne 0 ] && ! has "VALUE=1610612736"; then
    pass TM-02 "the budget still binds: touching 1.5 GB fails at elevated"
else
    fail TM-02 "a block touching 1.5 GB must not succeed under the elevated budget" "rc=$RC $(brief)"
fi
N=$(fresh); run tm02c elevated "const tag: number = $N
const b = new Uint8Array(64 * 1024 * 1024)
b.fill(1)
b.length"
if [ "$RC" -eq 0 ] && has "VALUE=67108864"; then
    pass TM-02c "CONTROL: touching 64 MB succeeds at elevated"
else
    fail TM-02c "a block touching 64 MB should succeed" "rc=$RC $(brief)"
fi

N=$(fresh); run tm03 elevated "const tag: number = $N
const fs = require('fs')
const lines = fs.readFileSync('/proc/self/limits', 'utf8').split('\\n')
const soft = (name: string) => lines.find((l: string) => l.startsWith(name)).split(/ {2,}/)[1]
soft('Max data size') + ':' + soft('Max address space')"
LIM=$(grep -o 'VALUE=[0-9]*:[0-9]*' <<<"$OUT" | head -1); LIM=${LIM#VALUE=}
DATA=${LIM%%:*}; AS=${LIM##*:}
WANT=0; if [ -n "$DATA" ] && [ "$DATA" -gt 0 ] 2>/dev/null; then WANT=$((DATA * 4)); [ "$WANT" -lt 2147483648 ] && WANT=2147483648; fi
if [ "$RC" -eq 0 ] && [ "$WANT" -gt 0 ] && [ "$AS" = "$WANT" ]; then
    pass TM-03 "the address-space ceiling is unchanged: max(4 x data, 2 GB) = $AS (data $DATA)"
else
    fail TM-03 "the child's ceiling should be max(4 x its data budget, 2 GB)" "rc=$RC data=$DATA as=$AS $(brief)"
fi

N=$(fresh); run tm04e elevated "const tag: number = $N
process.execArgv.includes('--jitless') ? 'JITLESS' : 'JIT'"
E_OUT="$OUT"; E_RC=$RC
N=$(fresh); run tm04u unrestricted "const tag: number = $N
process.execArgv.includes('--jitless') ? 'JITLESS' : 'JIT'"
if [ "$E_RC" -eq 0 ] && grep -q 'VALUE=JITLESS' <<<"$E_OUT" && [ "$RC" -eq 0 ] && has "VALUE=JIT" && ! has "VALUE=JITLESS"; then
    pass TM-04 "--jitless only under a budget (elevated yes, unrestricted no)"
else
    fail TM-04 "the flag should follow the budget" "elevated rc=$E_RC $(grep -o 'VALUE=[A-Z]*' <<<"$E_OUT") / unrestricted rc=$RC $(grep -o 'VALUE=[A-Z]*' <<<"$OUT")"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
