#!/usr/bin/env bash
# ============================================================
# test_async_engine_semantics.sh -- what `async fn` does on each engine (ASYNC-001)
#
# The VM runs every async fn call on its own thread and returns a future. The
# tree-walker runs the body synchronously at the call site and returns the
# value: `await` on a non-future passes it through, so programs compute the
# same results, but not concurrently. See docs/engine-divergences.md ASYNC-001
# for why the tree-walker's async branch stays unreachable for now.
#
#   AS-01  both engines compute the same awaited result
#   AS-02  the VM runs four 1 s async calls concurrently (well under 4 s)
#   AS-03  PINNED, not endorsed: the tree-walker runs them in sequence
#          (about 4 s, and the call returns a plain value). If this goes red
#          because the tree-walker became concurrent, ASYNC-001's blockers
#          (imports not visible in the worker, a crash when several worker
#          interpreters start at once, and the worker's _exit(3)) must be
#          fixed in the same change -- then update this arm and the doc.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== async fn semantics on each engine ==="

if [ ! -x "$NAAB" ]; then
    for id in AS-01 AS-02 AS-03; do skip "$id" "naab-lang not built (UNMEASURABLE)"; done
    echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-async-sem.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust   # the unsigned govern.json below must not meet a populated trust store
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT
echo '{ "version": "4.0", "mode": "off" }' > "$W/govern.json"
cat > "$W/conc.naab" <<'EOF'
use time
async fn w(i) {
    time.sleep(1)
    return i
}
main {
    let t0 = time.now_millis()
    let fs = []
    let i = 0
    while i < 4 {
        fs.push(w(i))
        i = i + 1
    }
    let kind = typeof(fs[0])
    let s = 0
    for f in fs {
        s = s + await f
    }
    print("RESULT sum=" + string(s) + " kind=" + kind + " ms=" + string(time.now_millis() - t0))
}
EOF

# run <engine flag> -> sets LINE (the RESULT line) and MS
run() {
    local out
    out=$(cd "$W" && timeout 30 "$NAAB" conc.naab $1 2>&1)
    LINE=$(printf '%s\n' "$out" | grep '^RESULT' | head -1)
    MS=$(printf '%s\n' "$LINE" | sed -n 's/.*ms=\([0-9]*\).*/\1/p')
    [ -n "$LINE" ] || LINE="(no result) $(printf '%s\n' "$out" | tail -2 | tr '\n' ' ')"
}

run ""; VM_LINE="$LINE"; VM_MS="${MS:-0}"
run "--tree-walk"; TW_LINE="$LINE"; TW_MS="${MS:-0}"

case "$VM_LINE|$TW_LINE" in
    *"sum=6 "*"|"*"sum=6 "*) ok "AS-01" "both engines await the same result (sum=6)" ;;
    *) bad "AS-01" "the engines disagree on the awaited result" "vm: $VM_LINE / tw: $TW_LINE" ;;
esac

if [[ "$VM_LINE" == *"kind=future"* ]] && [ "$VM_MS" -ge 900 ] && [ "$VM_MS" -lt 2500 ]; then
    ok "AS-02" "the VM ran four 1 s calls concurrently (${VM_MS} ms, futures)"
else
    bad "AS-02" "the VM did not run the calls concurrently" "$VM_LINE"
fi

if [[ "$TW_LINE" == *"kind=int"* ]] && [ "$TW_MS" -ge 3500 ]; then
    ok "AS-03" "PINNED, not endorsed: the tree-walker runs async fn synchronously (${TW_MS} ms, plain values) -- ASYNC-001"
else
    bad "AS-03" "the tree-walker's async behaviour changed -- see ASYNC-001 before accepting it" "$TW_LINE"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
