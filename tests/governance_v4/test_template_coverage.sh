#!/usr/bin/env bash
# ============================================================
# test_template_coverage.sh — govern-template.json must show what the loader reads
#
# THE FAILURE THIS CATCHES
#
# govern-template.json, and its copy docs/govern-template.json, are what users
# read to learn what govern.json can do (`naab-lang init` points at it as the
# "Full reference"). A measured pass found 147 settings loadFromJson() reads
# missing from it -- the whole circuit_breaker.output_admissibility block,
# deescalate_sustained, the S20-S23 signals, telemetry.decision_snapshots,
# agents.<name>.api_base -- and found keys it DID show sitting where the loader
# never looks: the four subprocess-scrub keys at the top level instead of under
# capabilities.env_vars, project_intent/function_intents beside intent_validation
# instead of inside it. Placed there by a "sync with parser" commit, so even the
# sync was unchecked. And the two copies had drifted from EACH OTHER: the docs
# copy shipped semantic_stability/mandate_alignment/response_quality/
# thinking_collapse as false, disabling four signals the engine enables by
# default for anyone who copied it.
#
#   TC-01  the extractor's own self-test passes. It carries a positive control
#          per access shape the walker must follow (alias chain, lambda with a
#          parameter key, literal key list, .items() map, static helper) and a
#          negative one (a path the loader never reads must not come back read).
#          Without them, a walker that stopped following aliases would report
#          hundreds of keys "missing" -- or, filtered, nothing at all.
#   TC-02  the two template copies are byte-identical.
#   TC-03  the loader leaves absent from the template match
#          template_coverage_baseline.txt, the deliberate exclusions (aliases,
#          object forms, entry shapes, read-but-inert keys, warn-only keys).
#   TC-04  NEGATIVE CONTROL for TC-03: the same template with one known setting
#          removed must report exactly that setting. Without it TC-03 passes for
#          a comparison that can never report anything.
#   TC-05  the tool's output is pure ASCII (see tests/helpers/encoding_controls.sh).
#
# WHAT THIS DOES NOT CHECK: that a key the template shows does anything, or is
# read at all (the reverse direction), or that the template's VALUES are the
# engine defaults. A key being read is not a key having an effect; the
# read-but-inert entries in the baseline were each traced past the loader.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOOL="$REPO/tools/template_coverage.py"
BASELINE="$SCRIPT_DIR/template_coverage_baseline.txt"
. "$REPO/tests/helpers/encoding_controls.sh"

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }

echo ""
echo "+==============================================================+"
echo "|  govern-template.json: every setting the loader reads         |"
echo "+==============================================================+"
echo ""

if ! command -v python3 >/dev/null 2>&1; then
    skip "TC-00" "python3 not available -- UNMEASURABLE, not a pass"
    echo "  Results: 0 passed, 0 failed, 1 skipped"
    exit 0
fi
for f in "$TOOL" "$BASELINE" "$REPO/govern-template.json" "$REPO/docs/govern-template.json"; do
    if [ ! -s "$f" ]; then
        bad "TC-00" "missing or empty: ${f#$REPO/}"
        exit 1
    fi
done

OUT="$(mktemp)"; trap 'rm -f "$OUT" "$OUT".*' EXIT
# The template is handed over as BYTES on stdin, never as a path: under MSYS2,
# python3 is a native Windows build that cannot open an MSYS /tmp path
# (test_path_precedence.sh). The real template also goes through stdin so TC-03
# and TC-04 exercise the same input path.
python3 "$TOOL" --template - < "$REPO/govern-template.json" > "$OUT" 2>&1
TOOL_RC=$?

# --- TC-01: the instrument's own controls ---------------------------------
if [ "$TOOL_RC" -eq 0 ] && grep -q '^self-test failures: 0' "$OUT"; then
    ok "TC-01" "extractor self-test passes ($(grep -m1 '^loader-paths:' "$OUT" | enc_strip_cr))"
else
    bad "TC-01" "extractor exited $TOOL_RC / self-test not clean -- the list below is NOT a result"
    grep -E '^!! SELF-TEST FAIL' "$OUT" | head -20 | sed 's/^/       /'
fi

# --- TC-02: the two copies are one file -----------------------------------
if cmp -s "$REPO/govern-template.json" "$REPO/docs/govern-template.json"; then
    ok "TC-02" "govern-template.json and docs/govern-template.json are identical"
else
    bad "TC-02" "the two template copies differ -- edit govern-template.json and copy it over"
    diff "$REPO/govern-template.json" "$REPO/docs/govern-template.json" | head -20 | sed 's/^/       /'
fi

# --- TC-03: absent settings match the pinned exclusions -------------------
EXPECTED="$(grep -v '^#' "$BASELINE" | grep -v '^[[:space:]]*$' | enc_strip_cr | sort)"
ACTUAL="$(sed -n 's/^missing: //p' "$OUT" | enc_strip_cr | sort)"
N_ACTUAL=$(printf '%s\n' "$ACTUAL" | grep -c . || true)
if [ "$TOOL_RC" -ne 0 ]; then
    bad "TC-03" "not evaluated: the extractor failed its self-test (TC-01)"
elif [ "$ACTUAL" = "$EXPECTED" ]; then
    ok "TC-03" "every loader setting is in the template or a pinned exclusion ($N_ACTUAL excluded)"
else
    bad "TC-03" "loader settings absent from the template changed"
    ADDED=$(comm -13 <(printf '%s\n' "$EXPECTED") <(printf '%s\n' "$ACTUAL"))
    REMOVED=$(comm -23 <(printf '%s\n' "$EXPECTED") <(printf '%s\n' "$ACTUAL"))
    if [ -n "$ADDED" ]; then
        echo "       Read by the loader, absent from the template and not excluded:"
        printf '%s\n' "$ADDED" | sed 's/^/         + /'
        echo "       Add each to BOTH template copies with its real default and a _comment,"
        echo "       or -- only if one of the baseline's categories applies -- to the baseline."
    fi
    if [ -n "$REMOVED" ]; then
        echo "       Pinned as excluded but no longer absent (added to the template, or no"
        echo "       longer read by the loader). Confirm which, then drop from the baseline:"
        printf '%s\n' "$REMOVED" | sed 's/^/         - /'
    fi
    enc_escaped_diff expected "$EXPECTED" actual "$ACTUAL" | head -10 | sed 's/^/       /'
fi

# --- TC-04: negative control -- a removed setting must be reported ---------
# Remove circuit_breaker.deescalate_sustained from an in-memory copy and expect
# exactly that path to appear on top of the baseline.
python3 -c '
import json, sys
t = json.loads(sys.stdin.buffer.read().decode("utf-8"))
del t["circuit_breaker"]["deescalate_sustained"]
sys.stdout.buffer.write(json.dumps(t).encode("utf-8"))
' < "$REPO/govern-template.json" > "$OUT.mut" 2>"$OUT.muterr"
MUT_RC=$?
if [ "$MUT_RC" -ne 0 ]; then
    bad "TC-04" "could not build the mutated template (the setting is no longer in the template?)"
    sed 's/^/       /' "$OUT.muterr" | head -5
else
    python3 "$TOOL" --template - < "$OUT.mut" > "$OUT.neg" 2>&1
    NEG_EXTRA="$(comm -13 <(printf '%s\n' "$ACTUAL") <(sed -n 's/^missing: //p' "$OUT.neg" | enc_strip_cr | sort))"
    if [ "$NEG_EXTRA" = "circuit_breaker.deescalate_sustained" ]; then
        ok "TC-04" "removing a setting from the template is reported (negative control)"
    else
        bad "TC-04" "removing circuit_breaker.deescalate_sustained reported [${NEG_EXTRA:-nothing}]"
    fi
fi

# --- TC-05: ASCII-only output ---------------------------------------------
if OFFENDERS="$(enc_has_non_ascii "$OUT")"; then
    bad "TC-05" "tool printed non-ASCII bytes (breaks cp1252/C-locale round-trip)"
    echo "$OFFENDERS" | sed 's/^/       /'
else
    ok "TC-05" "tool output is ASCII-only"
fi

echo ""
echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
