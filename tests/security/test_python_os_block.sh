#!/usr/bin/env bash
# A123 (Bundle 16): under a `standard` sandbox, embedded Python cannot start a
# command -- os.system and subprocess are refused by the audit hook, the
# refusal is catchable, and a compute-only block still runs.
#
# This replaces tests/security/test_python_os_block.naab. That test claimed
# os.system was blocked "in default (standard) mode", but its own commit
# (88442367, "opt-in sandbox with unrestricted default") made the default
# unrestricted, and the suite ran it with no govern.json and no sandbox flag:
# os.system executed, the test printed "FAIL: os.system() executed" and exited
# 0 (A123). run-all-tests.sh adds a sandbox flag only to tests named *sandbox*,
# and at `restricted` Python is refused before it runs ("python execution
# denied by sandbox"), an outer gate that would mask the hook. So the level
# here is `standard`, set on the command line, as the old test meant.
#
# Each block asks the shell to create a marker file. An arm credits a refusal
# only when the marker is absent AND the output carries the hook's refusal
# (an absent marker alone could be a command that failed to resolve).
#
#   PO-00  CONTROL: unrestricted, os.system runs -- the marker appears, so the
#          probe can observe the action
#   PO-01  standard: os.system is refused (marker absent, refusal printed), the
#          refusal is caught and the program goes on
#   PO-02  standard: subprocess.run is refused the same way
#   PO-03  CONTROL: standard, a compute-only block returns its value -- the
#          refusal is not a blanket refusal of Python
#   PO-04  pinned, not endorsed: elevated lets os.system run (elevated allows
#          exec; observed 2026-11-06) -- the level is what decides
#
# Skipped (exit 0, SKIP printed) where embedded Python is unavailable, as
# test_python_path_policy.sh does: there <<python>> is a separate process,
# contained by SubprocessContainment, which this suite does not measure.
#
# Red first: none -- this pins existing behaviour (observed 2026-11-06,
# 01fb3552). A dead interpreter SKIPs at the probe (exit 0 with SKIP printed,
# which the dead-interpreter gate does not count as clean). Run at a level
# where the command runs (the two `standard` runs swapped for `elevated`),
# PO-01 and PO-02 fail (marker present) and the controls still pass (observed).
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
case "$NAAB" in /*) ;; *) NAAB="$PWD/$NAAB" ;; esac

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       /'; }

echo "=== Embedded Python cannot start a command under a standard sandbox ==="
if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then
    echo "  FAIL naab-lang not built (UNMEASURABLE, not a pass)"; exit 1
fi
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-pyos.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
trap 'rm -rf "$W"' EXIT

printf 'main { let v = <<python\n1+1\n>>\nprint(string(v)) }\n' > "$W/probe.naab"
if [ "$(cd "$W" && "$NAAB" --no-governance probe.naab </dev/null 2>/dev/null | tail -1)" != "2" ]; then
    echo "  SKIP: embedded Python executor unavailable"; exit 0
fi

# prog NAME PYTHON-LINE: a program running one Python line in a try, then a
# compute-only block.
prog() {
    cat > "$W/$1.naab" <<EOF
main {
    try {
        let r = <<python
import os, subprocess
$2
"EXECUTED"
        >>
        print("RAN=" + r)
    } catch (e) {
        print("CAUGHT")
    }
    let c = <<python
6 * 7
    >>
    print("COMPUTE=" + string(c))
}
EOF
}
prog sys 'os.system("touch MARKER")'
prog sub 'subprocess.run(["/bin/sh", "-c", "touch MARKER"])'

OUT=""; RC=0
run() {  # run LEVEL NAME -> OUT, RC; MARKER says whether the command ran
    rm -f "$W/MARKER"
    OUT=$(cd "$W" && timeout 30 "$NAAB" --no-governance --sandbox-level "$1" "$2.naab" </dev/null 2>&1); RC=$?
}
marker() { [ -e "$W/MARKER" ]; }

run unrestricted sys
if [ "$RC" -eq 0 ] && marker && grep -q 'RAN=EXECUTED' <<<"$OUT"; then
    ok PO-00 "CONTROL: unrestricted, os.system runs and the marker appears"
else
    bad PO-00 "CONTROL: unrestricted, os.system should run (rc=$RC)" "$OUT"
fi

run standard sys
if [ "$RC" -eq 0 ] && ! marker && grep -q 'os.system denied by sandbox policy' <<<"$OUT" \
   && grep -q '^CAUGHT' <<<"$OUT" && ! grep -q 'RAN=' <<<"$OUT"; then
    ok PO-01 "standard: os.system refused by the hook, caught, the program goes on"
else
    bad PO-01 "standard: os.system should be refused (rc=$RC, marker=$(marker && echo yes || echo no))" "$OUT"
fi
if grep -q '^COMPUTE=42' <<<"$OUT"; then
    ok PO-03 "CONTROL: standard, a compute-only block still returns 42"
else
    bad PO-03 "CONTROL: standard, the compute-only block should return 42" "$OUT"
fi

run standard sub
if [ "$RC" -eq 0 ] && ! marker && grep -q 'subprocess.Popen denied by sandbox policy' <<<"$OUT" \
   && grep -q '^CAUGHT' <<<"$OUT" && ! grep -q 'RAN=' <<<"$OUT"; then
    ok PO-02 "standard: subprocess.run refused by the hook, caught"
else
    bad PO-02 "standard: subprocess.run should be refused (rc=$RC, marker=$(marker && echo yes || echo no))" "$OUT"
fi

run elevated sys
if [ "$RC" -eq 0 ] && marker && grep -q 'RAN=EXECUTED' <<<"$OUT"; then
    ok PO-04 "pinned: elevated lets os.system run (the level decides)"
else
    bad PO-04 "pinned: elevated was observed to let os.system run (rc=$RC)" "$OUT"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
