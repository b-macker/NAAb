#!/usr/bin/env bash
# ============================================================
# test_timeout_config.sh — govern.json's run time limit applies as written
# (docs/open-investigations.md A74)
#
# THE FAILURE THIS CATCHES
#
# main.cpp merged the run limit as
#     timeout = flag-or-30;  if (cfg != 30) timeout = max(timeout, cfg)
# 6c9426af wrote the max so that govern.json applies even when --timeout is
# passed ("--timeout can extend but not shrink the govern.json limit"). With
# NO flag the left side is the built-in 30 s default, so a configured limit
# below 30 never applied: observed before the fix (USER_GUIDE rewrite,
# 2026-10-25), limits.timeout.global: 10 stopped a `while true` loop at 30 s
# on both engines. Before 6c9426af the configured value applied when no flag
# was given; that half was a regression, not the decision.
#
#   TC-01  limits.timeout.global 2, no flag: the run stops at ~2 s, not 30
#   TC-02  --timeout 6 after the file does NOT extend a 2 s limit
#   TC-03  --timeout 6 before the file does not extend it either
#   TC-04  --timeout 2 lowers a 6 s limit (a flag may tighten)
#   TC-05  extends: a child with no limit gets its parent's 2 s
#   TC-06  extends, parent_wins: the parent's 2 s beats the child's 10
#   TC-C1  CONTROL: a program that finishes inside the limit runs to the end
#
# TC-02/03/04 flipped on 2026-10-25: 6c9426af had --timeout "extend but not
# shrink the govern.json limit" (max of the two); the owner reversed that
# ("govern.json is king"), so a flag may only lower a configured limit
# (applyConfiguredRunLimits(), main.cpp).
# TC-05/06: the merge set only limits.timeout.global, never runtime.timeout,
# the copy the run reads, so an inherited limit never reached the run. That
# was hidden by the 30 s floor (test_extends.sh T2's parent 30 equalled it).
# The merge now carries the limit over as written.
# Every arm but TC-05/06 runs on both engines: the tree-walker read neither
# govern.json limit before 2026-10-25.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2
    exit 1
fi

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-timeout-XXXXXX")"
cleanup() { teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT

# A 2 s run limit, and a loop limit high enough that only the clock stops it.
cat > "$W/govern.json" <<'EOF'
{"version":"5.0","mode":"enforce","limits":{"timeout":{"global":2},"execution":{"loop_iterations":2000000000}}}
EOF
cat > "$W/spin.naab" <<'EOF'
main {
    let n = 0
    while true {
        n = n + 1
    }
}
EOF
cat > "$W/quick.naab" <<'EOF'
main {
    let n = 0
    while n < 1000 {
        n = n + 1
    }
    print("DONE=" + string(n))
}
EOF

mkdir -p "$W/six"; cp "$W/spin.naab" "$W/six/"
echo '{"version":"5.0","mode":"enforce","limits":{"timeout":{"global":6},"execution":{"loop_iterations":2000000000}}}' > "$W/six/govern.json"

# timed ARGS... -> OUT, RC, SECS. The outer timeout is well past the old 30 s
# failure, so an unfixed build reads as slow rather than being killed silently.
timed() {
    local start=$SECONDS
    OUT=$(cd "$W" && timeout 45 "$NAAB" "$@" 2>&1); RC=$?
    SECS=$((SECONDS - start))
}
stopped_by_clock() { [ "$RC" != 0 ] && [ "$RC" != 124 ] && grep -qi 'timeout' <<<"$OUT"; }

for eng in "" "--tree-walk"; do
    tag="${eng:-vm}"; tag="${tag#--}"

    timed spin.naab $eng --timeout 6
    if stopped_by_clock && [ "$SECS" -ge 1 ] && [ "$SECS" -le 4 ]; then
        ok "TC-02/$tag" "--timeout 6 after the file does not extend govern.json's 2 s (${SECS}s)"
    else
        bad "TC-02/$tag" "--timeout 6 after the file should leave the 2 s limit" "rc=$RC ${SECS}s"
    fi

    timed --timeout 6 spin.naab $eng
    if stopped_by_clock && [ "$SECS" -ge 1 ] && [ "$SECS" -le 4 ]; then
        ok "TC-03/$tag" "--timeout 6 before the file does not extend it either (${SECS}s)"
    else
        bad "TC-03/$tag" "--timeout 6 before the file should leave the 2 s limit" "rc=$RC ${SECS}s"
    fi

    timed spin.naab $eng
    if stopped_by_clock && [ "$SECS" -ge 1 ] && [ "$SECS" -le 6 ]; then
        ok "TC-01/$tag" "a 2 s limit in govern.json stops the run (${SECS}s)"
    else
        bad "TC-01/$tag" "a 2 s limit in govern.json should stop the run near 2 s" "rc=$RC ${SECS}s $(grep -m1 -i 'timeout\|error' <<<"$OUT")"
    fi

    # TC-04: a 6 s limit, lowered by the flag.
    start=$SECONDS
    OUT=$(cd "$W/six" && timeout 45 "$NAAB" spin.naab $eng --timeout 2 2>&1); RC=$?
    SECS=$((SECONDS - start))
    if stopped_by_clock && [ "$SECS" -ge 1 ] && [ "$SECS" -le 4 ]; then
        ok "TC-04/$tag" "--timeout 2 lowers govern.json's 6 s limit (${SECS}s)"
    else
        bad "TC-04/$tag" "--timeout 2 should lower the 6 s limit" "rc=$RC ${SECS}s"
    fi

    timed quick.naab $eng
    if [ "$RC" = 0 ] && grep -qx 'DONE=1000' <<<"$OUT"; then
        ok "TC-C1/$tag" "control: a program inside the limit runs to the end"
    else
        bad "TC-C1/$tag" "a quick program should finish" "rc=$RC $(grep -m1 -i 'error' <<<"$OUT")"
    fi
done

# Extends: base/ holds the parent, kid/ and pw/ the children.
mkdir -p "$W/base" "$W/kid" "$W/pw"
echo '{"version":"5.0","mode":"enforce","limits":{"timeout":{"global":2},"execution":{"loop_iterations":2000000000}}}' > "$W/base/govern.json"
echo '{"version":"5.0","extends":"../base/govern.json","mode":"enforce"}' > "$W/kid/govern.json"
echo '{"version":"5.0","extends":"../base/govern.json","mode":"enforce","meta":{"inheritance":{"merge_strategy":"parent_wins"}},"limits":{"timeout":{"global":10}}}' > "$W/pw/govern.json"
cp "$W/spin.naab" "$W/kid/"; cp "$W/spin.naab" "$W/pw/"
for spec in "TC-05 kid a child with no limit gets its parent's 2 s" "TC-06 pw parent_wins: the parent's 2 s beats the child's 10"; do
    set -- $spec; id=$1; dir=$2; shift 2
    start=$SECONDS
    OUT=$(cd "$W/$dir" && timeout 45 "$NAAB" spin.naab 2>&1); RC=$?
    SECS=$((SECONDS - start))
    if stopped_by_clock && [ "$SECS" -ge 1 ] && [ "$SECS" -le 6 ]; then
        ok "$id/vm" "$* (${SECS}s)"
    else
        bad "$id/vm" "$* -- expected a stop near 2 s" "rc=$RC ${SECS}s $(grep -m1 -i 'timeout\|error\|extends' <<<"$OUT")"
    fi
done


echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
