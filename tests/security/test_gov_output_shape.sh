#!/usr/bin/env bash
# ============================================================
# test_gov_output_shape.sh -- `naab-gov check` emits JSON on BOTH verdicts
#
# WHAT THIS PINS. `cmdCheck` builds a structured JSON verdict, but
# `checkPolyglotBlock()` THROWS GovernanceHardError on a HARD block. That throw
# used to unwind past the JSON builder to main()'s handler, which prints
# e.what() as plain prose and _exit(3)s -- so the JSON was reachable only on a
# PASS. The contract was: structured output when nothing is wrong, unformatted
# text exactly when something is.
#
# WHY IT MATTERS, AND WHAT IT IS NOT. A caller parsing stdout for
# `"blocked": true` never saw a block. The deleted tests/security/
# test_alias_bypass.sh did exactly that and reported 86 false failures; with
# this fixed it reports 3. It is NOT fail-open: the exit code was always 3 on a
# block, so `naab-gov check ... || fail` was correct throughout. This is an
# output-shape defect. OS-04/OS-05 pin the exit codes precisely so a future
# change cannot "simplify" the JSON by dropping the exit-code distinction.
#
# THE LOAD-BEARING ARM IS OS-01, NOT OS-02. Every other arm asserts "this is
# valid JSON". If the parser itself were broken -- wrong binary, empty output,
# a python that cannot start -- every such arm fails loudly, but a test built
# only of them cannot tell "the fix regressed" from "the instrument died".
# OS-01 is the PASS case, which emitted JSON before this fix and must keep
# doing so; if OS-01 fails, believe the instrument is broken, not the fix.
# OS-03 is the one that fails if someone makes the block path emit JSON whose
# `blocked` field is false -- shape alone is not the claim.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
GOV="${NAAB_GOV:-$REPO/build/naab-gov}"
PASS=0; FAIL=0
ok()  { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL [$1] $2"; [ -n "${LAST:-}" ] && printf '%s\n' "$LAST" | head -6 | sed 's/^/        | /'; FAIL=$((FAIL+1)); }

echo "=== naab-gov check: one output shape for both verdicts ==="

# Usability checks, separate from any result. Two identical JSON documents are
# a match whatever produced them, so a stand-in emitting {} would pass every
# shape arm having checked nothing.
if [ ! -x "$GOV" ]; then
    echo "  SKIP [OS-00] naab-gov not built -- UNMEASURABLE, not a pass"; exit 0
fi
if ! "$GOV" --version 2>/dev/null | grep -qi naab-gov; then
    echo "  FAIL [OS-00] $GOV does not identify as naab-gov -- comparison would be vacuous"; exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "  SKIP [OS-00] python3 absent -- cannot validate JSON, UNMEASURABLE"; exit 0
fi

# An unsigned govern.json is an INTEGRITY BLOCK (exit 3) whenever the ambient
# trust store holds any key -- so without this, every "allowed" arm below would
# read as BLOCKED depending on what else had run first. Isolate the store.
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust

W="$(mktemp -d)"
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

cat > "$W/govern.json" <<'EOF'
{ "version": "3.0", "mode": "enforce",
  "restrictions": { "dangerous_calls": { "level": "hard" } } }
EOF
# Validate the fixture before trusting a single verdict from it. Feed BYTES,
# not a path: the shell does the open, so no filename crosses into a helper
# that may not share the shell's path vocabulary.
python3 -c "import json,sys; json.load(sys.stdin)" < "$W/govern.json" 2>/dev/null \
    || { echo "  FAIL [OS-00] generated govern.json is not valid JSON"; exit 1; }

printf 'x = 1 + 1\n'                 > "$W/ok.py"
printf 'import os; os.system("rm")\n' > "$W/bad.py"

# Captures into a variable and matches with `case` -- never `| grep -q`, which
# under `set -o pipefail` reports FAILURE exactly when the pattern IS present.
run() {  # $1 = fixture, $2.. = extra flags; sets LAST and RC
    LAST=$( (cd "$W" && "$GOV" check --language python --config govern.json --file "$1" "${@:2}") 2>/dev/null )
    RC=$?
}

# Prints "VALID <blocked>" or "INVALID". Reads stdin as bytes.
parse() { python3 -c "
import json,sys
try:
    d = json.load(sys.stdin)
    print('VALID %s' % d.get('blocked'))
except Exception:
    print('INVALID')
"; }

# --- OS-01: CONTROL -- the pass path emits JSON (it always did) ------------
run ok.py
V=$(printf '%s' "$LAST" | parse)
case "$V" in
    "VALID False") ok "OS-01" "CONTROL: pass emits valid JSON, blocked=false (instrument works)" ;;
    *) bad "OS-01" "pass path is not valid JSON ($V) -- instrument broken, later arms unreliable" ;;
esac

# --- OS-02: THE FIX -- the block path emits JSON too -----------------------
run bad.py
V=$(printf '%s' "$LAST" | parse)
case "$V" in
    VALID*) ok "OS-02" "block emits valid JSON (was plain prose from main()'s handler)" ;;
    *) bad "OS-02" "block path is not valid JSON -- the throw is escaping cmdCheck again" ;;
esac

# --- OS-03: shape is not the claim; the verdict is -------------------------
case "$V" in
    "VALID True") ok "OS-03" "blocked=true on a HARD block" ;;
    *) bad "OS-03" "block emitted JSON but blocked is not true ($V) -- a caller would read it as allowed" ;;
esac

# --- OS-04/OS-05: exit codes UNCHANGED -------------------------------------
# These were always correct. Pinned so a later change cannot regress the exit
# code while keeping the JSON tidy.
run bad.py
[ "$RC" -eq 3 ] && ok "OS-04" "block still exits 3" \
                || bad "OS-04" "block exit code is $RC, expected 3"
run ok.py
[ "$RC" -eq 0 ] && ok "OS-05" "CONTROL: pass still exits 0" \
                || bad "OS-05" "pass exit code is $RC, expected 0"

# --- OS-06: --sarif was unreachable on a block for the same reason ---------
run bad.py --sarif
V=$(printf '%s' "$LAST" | python3 -c "
import json,sys
try:
    d = json.load(sys.stdin); print('VALID %d' % len(d['runs'][0]['results']))
except Exception:
    print('INVALID')
")
case "$V" in
    "VALID 0") bad "OS-06" "SARIF on a block carries zero results -- the finding was dropped" ;;
    VALID*)    ok "OS-06" "--sarif also reaches the block path ($V result(s))" ;;
    *)         bad "OS-06" "SARIF on a block is not valid JSON" ;;
esac

echo "  Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
