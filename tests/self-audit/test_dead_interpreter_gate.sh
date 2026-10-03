#!/usr/bin/env bash
# ============================================================
# test_dead_interpreter_gate.sh -- the dead-interpreter gate decides correctly
#
# tools/testrunner/dead_gate.py runs every suite against an interpreter that
# prints nothing and exits 0, and fails if a suite passes CLEAN without an
# allowlisted reason (CI job "Dead-interpreter gate"). That full run takes
# minutes and is not repeated here. This suite checks the DECISION, on authored
# results, so a change to the classifier cannot quietly turn the gate into one
# that always passes.
#
#   DG-01  an unlisted unit that passes clean is FLAGGED and fails the gate
#   DG-01c CONTROL: the same results with that unit allowlisted pass, so DG-01
#          fails on the missing entry and not on something else
#   DG-02  a unit that fails, and one that passes reporting skips, both pass
#          the gate (they can fail / they said they could not measure)
#   DG-03  an allowlisted unit that no longer passes clean is reported STALE
#          but does not fail the gate (skips depend on the machine's tools)
#   DG-04  an incomplete run fails the gate even with nothing flagged
#   DG-05  an allowlist line without a valid kind or a reason is refused
#   DG-06  every entry in the real allowlist names a REGISTERED unit: listed now,
#          or its script exists and run-all-tests.sh names it. Some units are
#          registered conditionally (the prescan canaries only when src/ is
#          clean), so "listed on this machine right now" is the wrong test --
#          it failed build-windows and would fail any developer with
#          uncommitted src/ changes.
#   DG-06c CONTROL: an entry naming a suite that does not exist IS reported
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
GATE="$REPO/tools/testrunner/dead_gate.py"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== The dead-interpreter gate decides correctly ==="

if ! command -v python3 >/dev/null 2>&1; then
    for id in DG-01 DG-01c DG-02 DG-03 DG-04 DG-05 DG-06 DG-06c; do skip "$id" "python3 unavailable (UNMEASURABLE)"; done
    echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-deadgate.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
trap 'rm -rf "$W"' EXIT
T=$'\t'

# results <file> <listed> <rows...>; row = key:verdict:skip_markers
results() {
    local f="$1" listed="$2"; shift 2
    python3 - "$listed" "$@" > "$f" <<'PY'
import json, sys
listed = int(sys.argv[1])
rows = [a.rsplit(":", 2) for a in sys.argv[2:]]
print(json.dumps({"units_listed": listed, "units_run": len(rows),
                  "results": [{"key": k, "verdict": v, "skip_markers": int(s)} for k, v, s in rows]}))
PY
}
classify() { python3 "$GATE" classify "$1" --allowlist "$2" > "$W/out" 2>&1; }

printf '# test allowlist\n' > "$W/empty.txt"
printf 'u/tool.sh%stool%stests another binary\n' "$T" "$T" > "$W/tool.txt"

# --- DG-01 / DG-01c ----------------------------------------------------------------
results "$W/r1.json" 1 "u/tool.sh:PASS:0"
classify "$W/r1.json" "$W/empty.txt"; rc=$?
case "$(cat "$W/out")" in
    *FLAGGED*"u/tool.sh"*) [ "$rc" -ne 0 ] && ok "DG-01" "an unlisted clean pass is flagged and fails the gate" \
                                          || bad "DG-01" "flagged but the gate exited 0" ;;
    *) bad "DG-01" "an unlisted clean pass was not flagged" "rc=$rc; $(tail -3 "$W/out")" ;;
esac
classify "$W/r1.json" "$W/tool.txt"; rc=$?
if [ "$rc" -eq 0 ] && ! grep -q FLAGGED "$W/out"; then
    ok "DG-01c" "CONTROL: the same unit, allowlisted with a reason, passes"
else bad "DG-01c" "an allowlisted unit still failed the gate" "rc=$rc"; fi

# --- DG-02 ---------------------------------------------------------------------------
results "$W/r2.json" 3 "u/strong.sh:FAIL:0" "u/honest.sh:PASS:2" "u/slow.sh:TIMEOUT:0"
classify "$W/r2.json" "$W/empty.txt"; rc=$?
if [ "$rc" -eq 0 ] && grep -q "failed (they can fail) *2" "$W/out" && grep -q "reporting skips (honest) *1" "$W/out"; then
    ok "DG-02" "failing, timing-out and skip-reporting units pass the gate"
else bad "DG-02" "a unit that can fail was held against the gate" "rc=$rc; $(head -6 "$W/out" | tail -4)"; fi

# --- DG-03 ---------------------------------------------------------------------------
results "$W/r3.json" 1 "u/tool.sh:FAIL:0"
classify "$W/r3.json" "$W/tool.txt"; rc=$?
if [ "$rc" -eq 0 ] && grep -q "STALE" "$W/out" && grep -q "u/tool.sh: no longer passes clean" "$W/out"; then
    ok "DG-03" "a stale allowlist entry is reported without failing the gate"
else bad "DG-03" "stale entry mishandled" "rc=$rc; $(tail -3 "$W/out")"; fi

# --- DG-04 ---------------------------------------------------------------------------
results "$W/r4.json" 2 "u/strong.sh:FAIL:0"
classify "$W/r4.json" "$W/empty.txt"; rc=$?
if [ "$rc" -ne 0 ] && grep -q "INCOMPLETE" "$W/out"; then
    ok "DG-04" "a run with units missing fails the gate"
else bad "DG-04" "an incomplete run passed" "rc=$rc"; fi

# --- DG-05 ---------------------------------------------------------------------------
printf 'u/tool.sh%sfine%sreason\n' "$T" "$T" > "$W/badkind.txt"
printf 'u/tool.sh%stool%s \n' "$T" "$T" > "$W/noreason.txt"
classify "$W/r1.json" "$W/badkind.txt"; r1=$?
classify "$W/r1.json" "$W/noreason.txt"; r2=$?
if [ "$r1" -ne 0 ] && [ "$r2" -ne 0 ]; then ok "DG-05" "an entry with an unknown kind or no reason is refused"
else bad "DG-05" "a malformed allowlist line was accepted" "bad kind rc=$r1, no reason rc=$r2"; fi

# --- DG-06 ---------------------------------------------------------------------------
if [ ! -f "$REPO/build/naab-lang" ]; then
    skip "DG-06" "no build/naab-lang -- run-all-tests.sh list mode refuses to start (UNMEASURABLE)"
else
    ( cd "$REPO" && NAAB_TEST_PHASE=shell NAAB_TEST_LIST="$W/plan.tsv" bash run-all-tests.sh > "$W/list.out" 2>&1 )
    # unregistered <allowlist> -> entries naming no registered unit. Bytes in on
    # stdin and paths relative to the repo, never a shell path handed to python.
    unregistered() {
        ( cd "$REPO" && python3 -c '
import os, sys
plan, allow = sys.argv[1], sys.argv[2]
units = {" ".join(l.split("\t")[2:]) for l in open(plan, encoding="utf-8").read().splitlines()}
ras = open("run-all-tests.sh", encoding="utf-8", errors="replace").read()
bad = []
for l in open(allow, encoding="utf-8").read().splitlines():
    if not l.strip() or l.startswith("#"):
        continue
    key = l.split("\t")[0]
    path = key.split(" ")[0]
    if key in units or (os.path.isfile(path) and os.path.basename(path) in ras):
        continue
    bad.append(key)
print(" ".join(bad))' "$1" "$2" )
    }
    missing=$(unregistered "$W/plan.tsv" "tests/self-audit/dead_interpreter_allowlist.txt")
    if [ ! -s "$W/plan.tsv" ]; then bad "DG-06" "list mode produced no units -- cannot check the allowlist"
    elif [ -z "$missing" ]; then ok "DG-06" "every allowlist entry names a registered unit"
    else bad "DG-06" "allowlist names units that are not registered" "$missing"; fi
    printf 'tests/no/such_suite.sh\ttool\tcontrol entry\n' > "$W/ghost.txt"
    ghost=$(unregistered "$W/plan.tsv" "$W/ghost.txt")
    case "$ghost" in
        *no/such_suite.sh*) ok "DG-06c" "CONTROL: an entry naming a non-existent suite is reported" ;;
        *) bad "DG-06c" "a non-existent suite passed the registration check -- DG-06 cannot fail" "got: '$ghost'" ;;
    esac
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
