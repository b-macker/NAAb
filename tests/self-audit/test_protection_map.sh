#!/usr/bin/env bash
# ============================================================
# test_protection_map.sh -- which layer stops each forbidden action, per
# language, cannot change unnoticed
#
# tools/protmap/protmap.py runs one block per (language, action, column) and
# OBSERVES the action -- a marker file a child process created, a random token
# read from a blocked file or env var, a connection a local listener accepted
# -- never an exit code. Each cell is CONTAINED (the runtime stops it with
# every text check silenced), TEXT-ONLY (only a source-text check stops the
# plain form -- an evasion is a real escape), OPEN (nothing stops it), or
# NO_API. A control run with no policy at all must perform the action, or the
# cell is UNMEASURABLE rather than "contained". docs/protection-map.md is the
# rendered map; tools/protmap/baseline.json is what this suite pins.
#
#   PM-00  the instrument runs: the python row is measurable (python is the
#          embedded executor every Linux build has). A dead interpreter makes
#          every control fail, so this is what fails rather than passing on an
#          empty map.
#   PM-01  every runtime the binary registers is probed or listed in
#          tools/protmap/unprobed.txt with a reason -- a new executor cannot
#          arrive without a row, or a decision not to have one
#   PM-02  the measured map equals the committed baseline. Weaker cells are
#          regressions; stronger ones mean the committed map is out of date.
#          Cells this platform cannot measure (toolchain absent) are skipped
#          and counted. The baseline has a root and a nonroot section:
#          RLIMIT_NPROC does not bind root, so the same build contains less
#          when run as root (9 cells differ); the section follows the uid.
#   PM-03  the instrument can report every verdict: the measurement holds at
#          least one CONTAINED, one TEXT-ONLY and one OPEN cell. A probe that
#          collapsed to one answer would otherwise pass PM-02 only while the
#          baseline happened to agree.
#   PM-04  CONTROL: the comparator reports planted changes in BOTH directions
#          (a CONTAINED cell rewritten OPEN reads as STRONGER, a TEXT-ONLY cell
#          rewritten CONTAINED as WEAKER) and exits non-zero for each
#
# Linux only: the containment being mapped (rlimits, fork/exec gating) is the
# POSIX implementation, and the baseline was measured there.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$REPO/build/naab-lang"
GOV="$REPO/build/naab-gov"
TOOL="$REPO/tools/protmap/protmap.py"
BASE="$REPO/tools/protmap/baseline.json"
UNPROBED="$REPO/tools/protmap/unprobed.txt"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -12 | sed 's/^/       | /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }
report() { echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

echo "=== protection map ==="
if [ "$(uname -s)" != "Linux" ]; then
    skip PM-00 "not Linux -- the baseline maps the POSIX containment; UNMEASURABLE here"
    report; exit 0
fi
if [ ! -x "$NAAB" ] || [ ! -x "$GOV" ] || ! command -v python3 >/dev/null 2>&1; then
    skip PM-00 "naab-lang, naab-gov or python3 missing -- UNMEASURABLE, not a pass"
    report; exit 0
fi

W="$(mktemp -d)"
trap 'rm -rf "$W"' EXIT
JOBS="$(nproc 2>/dev/null || echo 4)"
# The baseline has a section per privilege (RLIMIT_NPROC does not bind root);
# protmap.py picks it from the effective uid the same way.
if [ "$(id -u)" = 0 ]; then PROFILE=root; else PROFILE=nonroot; fi

# One measurement; every arm below reads it.
mout="$(python3 "$TOOL" --naab "$NAAB" --jobs "$JOBS" --json "$W/cells.json" 2>&1)"; mrc=$?
if [ $mrc -ne 0 ] || [ ! -s "$W/cells.json" ]; then
    bad PM-00 "the map tool did not complete (exit $mrc)" "$mout"
    report; exit 1
fi

# --- PM-00 ---
py="$(python3 -c "
import json,sys
c=json.load(open(sys.argv[1]))
row=[v['verdict'] for k,v in c.items() if k.startswith('python/')]
print(sum(1 for v in row if v=='UNMEASURABLE'), len(row))" "$W/cells.json")"
read -r py_unm py_all <<<"$py"
if [ "${py_all:-0}" -gt 0 ] && [ "${py_unm:-1}" -eq 0 ]; then
    ok PM-00 "python row measurable ($py_all cells)"
else
    bad PM-00 "python row not measurable ($py_unm of $py_all UNMEASURABLE) -- instrument or interpreter broken"
fi

# --- PM-01 ---
rts="$(python3 "$TOOL" --naab "$NAAB" --list-runtimes 2>&1)"; rrc=$?
if [ $rrc -ne 0 ]; then
    skip PM-01 "could not ask the binary for its runtimes -- UNMEASURABLE"
else
    # The runtimes go in as ARGUMENTS: stdin is the script itself, and a
    # here-string beside it was silently discarded -- this arm compared
    # against an empty list and passed with a runtime removed from the list.
    # shellcheck disable=SC2086
    missing="$(python3 - "$TOOL" "$UNPROBED" $rts <<'EOF'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("protmap", sys.argv[1])
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
listed = set()
for ln in open(sys.argv[2], encoding="utf-8"):
    ln = ln.strip()
    if ln and not ln.startswith("#"):
        parts = ln.split(None, 1)
        if len(parts) == 2:          # a name with no reason does not count
            listed.add(parts[0])
runtimes = sys.argv[3:]
if not runtimes:
    print("<no runtimes received>")
for rt in runtimes:
    if rt not in m.SNIPPETS and rt not in listed:
        print(rt)
EOF
)"
    if [ -z "$missing" ]; then
        ok PM-01 "every registered runtime is probed or listed with a reason ($(echo "$rts" | wc -l) runtimes)"
    else
        bad PM-01 "registered runtimes with no probes and no reason in unprobed.txt: $(echo $missing)"
    fi
fi

# --- PM-02 ---
cout="$(python3 "$TOOL" --cells "$W/cells.json" --baseline "$BASE" 2>&1)"; crc=$?
summary="$(printf '%s\n' "$cout" | grep '^BASELINE' || true)"
if [ $crc -eq 0 ] && [ -n "$summary" ]; then
    ok PM-02 "map equals the committed $PROFILE baseline: ${summary#BASELINE }"
else
    bad PM-02 "map differs from tools/protmap/baseline.json (regenerate it AND docs/protection-map.md if the change is intended)" \
        "$(printf '%s\n' "$cout" | grep -E '^(DIFF|BASELINE)')"
fi

# --- PM-03 ---
kinds="$(python3 -c "
import json,sys
c=json.load(open(sys.argv[1]))
print(' '.join(sorted({v['verdict'] for v in c.values()})))" "$W/cells.json")"
miss=""
for v in CONTAINED TEXT-ONLY OPEN; do
    case " $kinds " in *" $v "*) ;; *) miss="$miss $v" ;; esac
done
if [ -z "$miss" ]; then ok PM-03 "instrument reports every verdict ($kinds)"
else bad PM-03 "measurement never produced:$miss -- the instrument may have collapsed"; fi

# --- PM-04 ---
python3 - "$BASE" "$W/planted.json" "$PROFILE" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
b = d[sys.argv[3]]
k = next(k for k, v in sorted(b.items()) if v == "CONTAINED")
b[k] = "OPEN"
json.dump(d, open(sys.argv[2], "w"))
EOF
pout="$(python3 "$TOOL" --cells "$W/cells.json" --baseline "$W/planted.json" 2>&1)"; prc=$?
python3 - "$BASE" "$W/planted2.json" "$PROFILE" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
b = d[sys.argv[3]]
k = next(k for k, v in sorted(b.items()) if v == "TEXT-ONLY")
b[k] = "CONTAINED"
json.dump(d, open(sys.argv[2], "w"))
EOF
pout2="$(python3 "$TOOL" --cells "$W/cells.json" --baseline "$W/planted2.json" 2>&1)"; prc2=$?
if [ $prc -ne 0 ] && [[ "$pout" == *"(STRONGER)"* ]] && [ $prc2 -ne 0 ] && [[ "$pout2" == *"(WEAKER)"* ]]; then
    ok PM-04 "comparator reports planted changes in both directions and fails"
else
    bad PM-04 "comparator missed a planted change (exit $prc/$prc2)" "$pout
$pout2"
fi

report
[ "$FAIL" -eq 0 ]
