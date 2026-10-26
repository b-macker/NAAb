#!/usr/bin/env bash
# test_taint_array_transform.sh — Taint propagation through array.map/filter/reduce
# Verifies that tainted data flowing through functional array transforms
# remains tainted and is blocked at sinks.

set -euo pipefail
NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORKDIR="${HOME}/.naab/test_taint_array_$$"
mkdir -p "$WORKDIR"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "=== test_taint_array_transform.sh ==="
echo ""

# Governance config: taint tracking with file.write as sink.
# The blocked arms match the taint violation itself (both report formats). They
# used to match "taint|blocked|governance|sink|denied", which "[governance]
# Loaded:" -- printed by every run -- also matches, so no arm could fail.
cat > "$WORKDIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "taint_tracking": {
    "enabled": true,
    "sources": ["polyglot_output"],
    "sinks": ["file.write", "file.append"],
    "sanitizers": ["validate_", "sanitize_"]
  }
}
EOF

# ---------------------------------------------------------------------------
# T1: array.map_fn on tainted data → result must remain tainted
# ---------------------------------------------------------------------------
echo "[T1] array.map_fn propagates taint from polyglot output"
cat > "$WORKDIR/test_map.naab" << 'EOF'
use array
use string
use file
main {
  let raw = <<python
"alpha\nbeta\ngamma"
>>
  let items = string.split(raw, "\n")
  let upper = array.map_fn(items, fn(x) { return string.upper(x) })
  file.write("./data/out.txt", array.join(upper, "\n"))
}
EOF

out=$("$NAAB" "$WORKDIR/test_map.naab" 2>&1) || true
if grep <<<"$out" -qiE "Taint tracking violation|sink_violation"; then
    ok "file.write blocked — taint propagated through array.map_fn"
else
    fail "file.write NOT blocked — taint lost through array.map_fn: ${out:0:200}"
fi
echo ""

# ---------------------------------------------------------------------------
# T2: array.filter_fn on tainted data → result must remain tainted
# ---------------------------------------------------------------------------
echo "[T2] array.filter_fn propagates taint from polyglot output"
cat > "$WORKDIR/test_filter.naab" << 'EOF'
use array
use string
use file
main {
  let raw = <<python
"10\n20\n30"
>>
  let items = string.split(raw, "\n")
  let filtered = array.filter_fn(items, fn(x) { return string.length(x) > 1 })
  file.write("./data/out.txt", array.join(filtered, "\n"))
}
EOF

out=$("$NAAB" "$WORKDIR/test_filter.naab" 2>&1) || true
if grep <<<"$out" -qiE "Taint tracking violation|sink_violation"; then
    ok "file.write blocked — taint propagated through array.filter_fn"
else
    fail "file.write NOT blocked — taint lost through array.filter_fn: ${out:0:200}"
fi
echo ""

# ---------------------------------------------------------------------------
# T3: array.reduce_fn on tainted data → result must remain tainted
# ---------------------------------------------------------------------------
echo "[T3] array.reduce_fn propagates taint from polyglot output"
cat > "$WORKDIR/test_reduce.naab" << 'EOF'
use array
use string
use file
main {
  let raw = <<python
"a\nb\nc"
>>
  let items = string.split(raw, "\n")
  let joined = array.reduce_fn(items, fn(acc, x) { return acc + "," + x }, "")
  file.write("./data/out.txt", joined)
}
EOF

out=$("$NAAB" "$WORKDIR/test_reduce.naab" 2>&1) || true
if grep <<<"$out" -qiE "Taint tracking violation|sink_violation"; then
    ok "file.write blocked — taint propagated through array.reduce_fn"
else
    fail "file.write NOT blocked — taint lost through array.reduce_fn: ${out:0:200}"
fi
echo ""

# ---------------------------------------------------------------------------
# T4: Sanitized data through array.map_fn → should NOT be blocked
# ---------------------------------------------------------------------------
echo "[T4] Sanitized tainted data through array.map_fn is not blocked"
cat > "$WORKDIR/test_sanitized.naab" << 'EOF'
use array
use string
use file

fn validate_line(line) {
  if string.length(line) > 100 {
    return string.slice(line, 0, 100)
  }
  if string.contains(line, "<") {
    return ""
  }
  return line
}

main {
  let raw = <<python
"safe\ndata\nhere"
>>
  let items = string.split(raw, "\n")
  let clean = array.map_fn(items, fn(x) { return validate_line(x) })
  file.write("./data/out.txt", array.join(clean, "\n"))
}
EOF

out=$("$NAAB" "$WORKDIR/test_sanitized.naab" 2>&1) || ec=$?
ec=${ec:-0}
# After sanitization, file.write should succeed (or fail for unrelated reasons like missing dir)
if grep <<<"$out" -qi "taint.*block\|tainted.*sink"; then
    fail "file.write blocked despite sanitization — false positive"
else
    ok "sanitized data passes through without taint block"
fi
echo ""

# ---------------------------------------------------------------------------
# T5: array.map_fn with --tree-walk mode
# ---------------------------------------------------------------------------
# Under enforce --tree-walk is refused before it runs; the tree-walker's own
# taint propagation is measured beside an audit copy of the config.
echo "[T5] array.map_fn taint propagation works in tree-walk mode"
. "$(dirname "$0")/../helpers/tree_walk_refusal.sh"
rc=0; out=$("$NAAB" --tree-walk "$WORKDIR/test_map.naab" 2>&1) || rc=$?
if tw_refused "$rc" "$out"; then
    ok "under the enforce config --tree-walk is refused before it runs"
else
    fail "--tree-walk ran under an enforce config (rc=$rc): ${out:0:200}"
fi
mkdir -p "$WORKDIR/aud/data"; cp "$WORKDIR/test_map.naab" "$WORKDIR/aud/"
sed 's/"mode": "enforce"/"mode": "audit"/' "$WORKDIR/govern.json" > "$WORKDIR/aud/govern.json"
NAAB_ABS="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"   # NAAB may be relative
out=$(cd "$WORKDIR/aud" && "$NAAB_ABS" --tree-walk test_map.naab 2>&1) || true
# (the tree-walker's expression-path check ends the run even under audit,
# docs/open-investigations.md A83(e): the violation, recorded or not, is the claim)
if grep <<<"$out" -qiE "Taint tracking violation|sink_violation"; then
    ok "tree-walk (audit): taint propagated through array.map_fn is recorded at the sink"
else
    fail "tree-walk (audit): taint lost through array.map_fn: ${out:0:200}"
fi
echo ""

TOTAL=$((PASS + FAIL))
echo "Results: ${PASS}/${TOTAL} passed"
[[ "$FAIL" -eq 0 ]]
