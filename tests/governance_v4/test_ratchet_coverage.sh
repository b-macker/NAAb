#!/usr/bin/env bash
# ============================================================
# test_ratchet_coverage.sh -- no govern.json key reaches the mid-run reload
# path without someone deciding what a loosening of it means
#
# WHY THIS EXISTS. reloadIfChanged() installs a reloaded rule set when it passes
# checkRatchetViolation(), and every check reads the new values from then on.
# The ratchet is a hand-written list of comparisons, written in 596dd798 for
# ~50 scalar fields and extended one feature at a time. Keys nobody added were
# loosenable by a signed reload with no violation and no notice. Measured: the
# allow/block lists (#303), taint sinks / sanitizers / level, the import
# blocklists -- and the top-level `mode`: a reload from enforce to off was
# accepted and a blocked_read variable was read straight after it. That last
# one was found by THIS guard's first run, which is the case for having it.
#
# tools/ratchet_coverage.py classifies every leaf loadFromJson() reads as named
# by the ratchet, classified in ratchet_classification.txt, or UNCLASSIFIED.
# The unclassified set is pinned to ratchet_unclassified_baseline.txt -- debt
# that may only shrink.
#
#   RC-01  the tool's self-test passes (positive controls per credit path:
#          a plain literal, an agent "agent.{}.X" name, a table token, a
#          startup claim; >= 500 loader paths; >= 100 ratchet literals)
#   RC-02  the unclassified set equals the baseline: a NEW loader key fails
#          until classified, and a baseline line that is no longer
#          unclassified fails until removed (so the list only shrinks)
#   RC-03  no table entry is hollow (its token appears in the ratchet code)
#   RC-04  NEGATIVE CONTROL: a key planted in the loader is reported
#          unclassified and fails RC-02's comparison
#   RC-05  NEGATIVE CONTROL: stripping a name the ratchet uses demotes the
#          key it credits (named credit is load-bearing), and stripping a
#          table token makes that entry hollow
#   RC-06  output is ASCII
#
# WHAT THIS DOES NOT CHECK: that a named or tabled comparison is in the right
# DIRECTION, or that a reload reaches the key at runtime. "Named" means the
# ratchet mentions a key; the reload suites (test_capability_list_ratchet.sh
# and the per-feature ones) are the behavioural evidence. An inert / startup /
# neutral line is a claim carried by its reason.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOOL="$REPO/tools/ratchet_coverage.py"
BASELINE="$SCRIPT_DIR/ratchet_unclassified_baseline.txt"
CONFIG_SRC="$REPO/src/runtime/governance_config.cpp"
. "$REPO/tests/helpers/encoding_controls.sh"

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); [ -n "${3:-}" ] && printf '%s\n' "$3" | head -12 | sed 's/^/       | /'; }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }

echo ""
echo "=== Ratchet coverage: every reload-reachable key is classified ==="
if ! command -v python3 >/dev/null 2>&1; then
    skip "RC-00" "python3 missing -- UNMEASURABLE"
    echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

# The tool reads governance_config.cpp on stdin in every run, so a native
# Windows python never opens an MSYS path.
run_tool() { python3 "$TOOL" --config-stdin "$@" < "$CONFIG_SRC"; }

TMPD="$(mktemp -d)"; trap 'rm -rf "$TMPD"' EXIT
run_tool > "$TMPD/out.txt" 2>&1; RC=$?
OUT="$(enc_strip_cr < "$TMPD/out.txt")"

# ---- RC-01 -----------------------------------------------------------------
case "$OUT" in
    *"self-test failures: 0"*) if [ $RC -eq 0 ]; then ok "RC-01" "tool self-test passes"; else bad "RC-01" "self-test text clean but exit $RC" "$OUT"; fi ;;
    *) bad "RC-01" "tool self-test failed -- the list below is not a result" "$OUT" ;;
esac

# ---- RC-02 -----------------------------------------------------------------
current="$(sed -n 's/^unclassified: //p' <<<"$OUT" | sort -u)"
pinned="$(grep -v '^#' "$BASELINE" | grep -v '^[[:space:]]*$' | sort -u)"
new_keys="$(comm -23 <(printf '%s\n' "$current") <(printf '%s\n' "$pinned") | grep -v '^$')"
gone_keys="$(comm -13 <(printf '%s\n' "$current") <(printf '%s\n' "$pinned") | grep -v '^$')"
if [ -z "$current" ]; then
    bad "RC-02" "the tool reported no unclassified keys at all -- broken probe, not a result" "$OUT"
elif [ -z "$new_keys" ] && [ -z "$gone_keys" ]; then
    ok "RC-02" "unclassified keys match the baseline ($(printf '%s\n' "$current" | wc -l | tr -d ' ') keys of debt)"
else
    msg=""
    [ -n "$new_keys" ] && msg+="NEW loader key(s) with no ratchet classification -- decide what a loosening means and classify in ratchet_classification.txt:"$'\n'"$new_keys"$'\n'
    [ -n "$gone_keys" ] && msg+="baseline line(s) no longer unclassified -- remove them from ratchet_unclassified_baseline.txt:"$'\n'"$gone_keys"
    bad "RC-02" "unclassified set differs from the baseline" "$msg"
fi

# ---- RC-03 -----------------------------------------------------------------
hollow="$(sed -n 's/^hollow: //p' <<<"$OUT")"
if [ -z "$hollow" ]; then ok "RC-03" "no table entry is hollow"
else bad "RC-03" "table entries whose token the ratchet code does not contain" "$hollow"; fi

# ---- RC-04: a planted loader key ------------------------------------------
# Insert a read of a fresh key at the top of the loader body; it must come
# back unclassified (and so fail RC-02's comparison).
PLANT='zz_ratchet_probe_key'
planted="$(python3 -c '
import sys
src = sys.stdin.buffer.read().decode("utf-8", "replace")
anchor = "static void loadFromJsonImpl(const nlohmann::json& j, GovernanceRules& rules_) {"
if anchor not in src:
    anchor = "static void loadFromJson(const nlohmann::json& j, GovernanceRules& rules_) {"
if anchor not in src:
    sys.stdout.buffer.write(b"NO_ANCHOR"); sys.exit(0)
plant = "\n    if (j.contains(\"'"$PLANT"'\") && j[\"'"$PLANT"'\"].is_boolean()) rules_.version = \"x\";\n"
sys.stdout.buffer.write(src.replace(anchor, anchor + plant, 1).encode("utf-8"))
' < "$CONFIG_SRC")"
if [ "$planted" = "NO_ANCHOR" ]; then
    skip "RC-04" "loader entry point not found for the plant -- UNMEASURABLE"
else
    POUT="$(python3 "$TOOL" --config-stdin <<<"$planted" 2>&1)"
    if grep -q "^unclassified: $PLANT\$" <<<"$POUT"; then
        if grep -qx "$PLANT" <<<"$pinned"; then
            bad "RC-04" "the planted key is in the baseline -- the comparison could not fail" ""
        else
            ok "RC-04" "control: a key planted in the loader is reported unclassified"
        fi
    else
        bad "RC-04" "control failed: the planted key was not reported" "$POUT"
    fi
fi

# ---- RC-05: credit is load-bearing ------------------------------------------
SOUT="$(run_tool --strip-token taint_tracking.lineage 2>&1)"
if grep -q '^unclassified: taint_tracking.lineage$' <<<"$SOUT"; then
    ok "RC-05a" "control: stripping a ratchet name demotes the key it credits"
else
    bad "RC-05a" "control failed: taint_tracking.lineage kept its credit with its name stripped" "$SOUT"
fi
SOUT="$(run_tool --strip-token 'capabilities.filesystem paths' 2>&1)"
if grep -q '^hollow: capabilities.filesystem.blocked_paths$' <<<"$SOUT"; then
    ok "RC-05b" "control: stripping a table token makes its entry hollow"
else
    bad "RC-05b" "control failed: capabilities.filesystem.blocked_paths not reported hollow" "$SOUT"
fi

# ---- RC-06 -------------------------------------------------------------------
if enc_has_non_ascii "$TMPD/out.txt"; then bad "RC-06" "tool output is not ASCII" ""
else ok "RC-06" "tool output is ASCII"; fi

echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ] || exit 1
exit 0
