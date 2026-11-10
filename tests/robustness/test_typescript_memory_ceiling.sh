#!/usr/bin/env bash
# test_typescript_memory_ceiling.sh -- TypeScript runs under the child memory budget (D13, A145, A146)
#
# A child started under a memory budget gets RLIMIT_DATA at the budget and an
# address-space ceiling of 4x, at least 2 GB (subprocess_helpers.cpp, L7). V8
# reserves far more address space than it uses: each WebAssembly instance
# reserves an ~8 GB guard region, and tsx instantiates a WebAssembly lexer
# while it transpiles, so every <<typescript>> block failed at `elevated` with
# "WebAssembly.instantiate(): Out of memory: Cannot allocate Wasm memory"
# (observed on master 1c2a8765). Measured with prlimit on an uncached tsx run:
# 32-48 GB of address space needed as is, 16 GB with --disable-wasm-trap-handler,
# under 2 GB with --jitless. The executor starts tsx with --jitless whenever
# the child has a memory budget. The ceiling itself is not raised: it is the
# backstop for shared memory that RLIMIT_DATA does not count
# (tests/security/test_child_memory_limit.sh).
#
# Node 24 changed two things (A145, A146; observed with node 24.22.2 and tsx
# 4.23.15). Its V8 refuses --no-expose-wasm, which the executor used to pass
# beside --jitless, so every block under a budget failed with "bad option"
# (A146); the executor now passes --jitless alone and removes node 22's
# "disabling flag --expose_wasm" warning from the child's stderr. And node 24
# maps its ~512 MB JIT code range writable, which RLIMIT_DATA counts, so tsx's
# own launcher -- a plain node, started without the flag -- needs a data
# budget of about 600 MB and does not start under the 512 MB default (A145;
# kept by the owner's decision, 2026-11-10: a project sets runtime.memory_limit).
# The arms therefore run at elevated with runtime.memory_limit 1024, which
# both node versions fit; TM-06 measures the default.
#
#   TM-00  CONTROL: a TypeScript block returns its value at `unrestricted`
#          (no budget) -- the toolchain works
#   TM-01  the same kind of block returns its value at `elevated` with a
#          1024 MB budget (each run's content is new, so tsx transpiles rather
#          than reading its cache)
#   TM-02  the budget still binds: a block that touches 1.5 GB fails;
#          TM-02c CONTROL: one that touches 64 MB succeeds
#   TM-03  the ceiling is unchanged: the child's /proc/self/limits reads an
#          address-space limit of max(4 x its data budget, 2 GB)
#   TM-04  the flag is applied only under a budget: the child's node sees
#          --jitless at `elevated` and not at `unrestricted`
#   TM-05  node 22's --jitless warning stays out of the block's output; a
#          node that never prints it (node 24) reads UNMEASURABLE
#   TM-06  at the DEFAULT budget a block runs on node < 24, and node >= 24 is
#          refused with V8's CodeRange message (the documented limit), never
#          some other way
#
# Kill table (each mutant rebuilt, suite re-run):
#   no --jitless for tsx (the pre-fix shape, 2026-10-29)  -> TM-01, TM-02c, TM-03, TM-04 red
#   --jitless whatever the budget (no budget check)       -> TM-04 red
#   --no-expose-wasm back beside --jitless (node 24)      -> TM-01, TM-02c, TM-03, TM-04 red
#   warning not removed (node 22)                         -> TM-05 red
# Red first (node 24): TM-01, TM-02c, TM-03 and TM-04 fail on master.
# Linux only (the rlimits are Linux's); needs tsx on PATH, else SKIP.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

echo "=== TypeScript under the child memory budget (D13) ==="
if [ "$(uname -s)" != "Linux" ] || ! command -v tsx >/dev/null 2>&1; then
    for a in TM-00 TM-01 TM-02 TM-02c TM-03 TM-04 TM-05 TM-06; do
        echo "  SKIP [$a] needs Linux and tsx on PATH -- UNMEASURABLE"
    done
    exit 0
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_tsmem_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm()  { UNM=$((UNM+1)); echo "  UNMEASURABLE [$1] $2"; }

# The budget the arms run under (MB): one both node 22 and node 24 fit (A145).
BUDGET=1024
# run ID LEVEL TS_BODY [BUDGET_MB] -> OUT, RC (the block's value is printed as
# VALUE=). BUDGET_MB defaults to $BUDGET at a level with a budget; "default"
# writes no runtime.memory_limit, so the run's default budget applies.
run() {
    local d="$W/$1"; mkdir -p "$d"
    local mb="${4:-$BUDGET}" rt=""
    [ "$2" != "unrestricted" ] && [ "$mb" != "default" ] && rt=",\"runtime\":{\"memory_limit\":$mb}"
    printf '{"mode":"enforce","security":{"sandbox_level":"%s"}%s}\n' "$2" "$rt" > "$d/govern.json"
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
    pass TM-01 "a TypeScript block returns its value at elevated (budget $BUDGET MB)"
else
    fail TM-01 "a TypeScript block should run under a $BUDGET MB budget at elevated" "rc=$RC $(brief)"
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

# TM-05: node 22 prints "Warning: disabling flag --expose_wasm due to
# conflicting flags" under --jitless; the executor removes it. Load-bearing
# only on a node that prints it, which is probed directly first.
NOTICE="disabling flag --expose_wasm"
NODE_NOTICE=$(node --jitless -e 0 2>&1)
N=$(fresh); run tm05 elevated "const n: number = $N + 1
n"
if ! grep -q -F -- "$NOTICE" <<<"$NODE_NOTICE"; then
    unm TM-05 "this node ($(node --version 2>/dev/null)) prints no --expose_wasm warning under --jitless; nothing to remove"
elif [ "$RC" -eq 0 ] && has "VALUE=$((N + 1))" && ! has "$NOTICE"; then
    pass TM-05 "node's --jitless warning is kept out of the block's output"
else
    fail TM-05 "the block's output should carry neither an error nor node's --jitless warning" "rc=$RC $(brief)"
fi

# TM-06: the default budget (no runtime.memory_limit). Node 24 does not start
# under it (A145, kept by decision); anything else must run.
NODE_MAJOR=$(node --version 2>/dev/null | sed -n 's/^v\([0-9]*\)\..*/\1/p')
N=$(fresh); run tm06 elevated "const n: number = $N + 1
n" default
if [ -z "$NODE_MAJOR" ]; then
    unm TM-06 "node --version gave no major version"
elif [ "$NODE_MAJOR" -lt 24 ]; then
    if [ "$RC" -eq 0 ] && has "VALUE=$((N + 1))"; then
        pass TM-06 "node $NODE_MAJOR runs TypeScript at the default budget"
    else
        fail TM-06 "node $NODE_MAJOR should run TypeScript at the default budget" "rc=$RC $(brief)"
    fi
elif [ "$RC" -ne 0 ] && has "Failed to reserve virtual memory for CodeRange"; then
    pass TM-06 "node $NODE_MAJOR is refused at the default budget with V8's CodeRange message (documented, A145)"
elif [ "$RC" -eq 0 ] && has "VALUE=$((N + 1))"; then
    fail TM-06 "node $NODE_MAJOR now runs at the default budget: A145's documented limit is stale, update it" "rc=$RC"
else
    fail TM-06 "node $NODE_MAJOR failed at the default budget some other way than A145 documents" "rc=$RC $(brief)"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ]
