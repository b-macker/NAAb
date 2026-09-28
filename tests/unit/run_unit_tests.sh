#!/usr/bin/env bash
# run_unit_tests.sh -- run naab_unit_tests minus the documented exclusions.
#
# naab_unit_tests was built and run by nothing for long enough that five of its
# nineteen files stopped compiling unnoticed. This is what CI runs so that
# cannot happen again. Exclusions live in known_failures.txt, each tied to a
# finding in docs/unit-test-findings.md.
#
#   1. Every test NOT in the list must pass.
#   2. Every `fails` entry is run on its own and must still FAIL: a test that
#      starts passing means something was fixed and the entry must go, so the
#      list cannot silently outlive its reasons.
#   3. `hang` entries are not run (they deadlock or crash the process).
#
# Usage: bash tests/unit/run_unit_tests.sh [path/to/naab_unit_tests]

set -uo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="${1:-$REPO/build/naab_unit_tests}"
LIST="$REPO/tests/unit/known_failures.txt"

if [ ! -x "$BIN" ]; then
    echo "FAIL: $BIN not built (UNMEASURABLE, not a pass)"
    exit 1
fi

# Run from the repo root: several tests use repo-relative fixtures. The
# tamper-evident logger tests leave their logs in the working directory.
cd "$REPO" || exit 1
trap 'rm -f "$REPO"/test_tamper_evident_*.log "$REPO"/test_tamper_evident_*.log.tamper_evident' EXIT

all=()
fails=()
while read -r name kind _; do
    [[ -z "$name" || "$name" == \#* ]] && continue
    case "$kind" in
        fails) fails+=("$name") ;;
        hang) ;;
        *) echo "FAIL: bad kind '$kind' for $name in $LIST"; exit 1 ;;
    esac
    all+=("$name")
done < "$LIST"

# Guard against a typo that excludes nothing: every entry must name a real test.
listed="$("$BIN" --gtest_list_tests | awk '/^[^ ]/{s=$1} /^  /{print s $1}')"
bad=0
for t in "${all[@]}"; do
    if ! grep -qxF "$t" <<<"$listed"; then
        echo "FAIL: known_failures.txt names a test that does not exist: $t"
        bad=1
    fi
done
[ "$bad" -eq 0 ] || exit 1

filter="-$(IFS=:; echo "${all[*]}")"
echo "=== naab_unit_tests, ${#all[@]} documented exclusions ==="
timeout 900 "$BIN" --gtest_filter="$filter" --gtest_brief=1
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "FAIL: naab_unit_tests exited $rc"
    exit 1
fi

echo "=== excluded tests must still fail ==="
fixed=0
for t in "${fails[@]}"; do
    if timeout 120 "$BIN" --gtest_filter="$t" >/dev/null 2>&1; then
        echo "  NOW PASSES: $t -- remove it from known_failures.txt"
        fixed=1
    fi
done
if [ "$fixed" -ne 0 ]; then
    echo "FAIL: known_failures.txt lists tests that now pass"
    exit 1
fi
echo "  all ${#fails[@]} still fail for their recorded reason"
echo "PASS"
