#!/usr/bin/env bash
# ============================================================
# test_code_injection_imports.sh -- `from os import <inert>` is not injection
#
# WHAT CHANGED. restrictions.code_injection blocked `\bfrom\s+os\b` outright, so
# `from os import path` was a HARD block described as "Code injection pattern",
# with help text about eval/exec. `import os.path` was blocked too, leaving NO
# way to reach Python's path helpers from a governed block -- and no config
# escape short of block_command_injection:false, which switches off os.system
# and subprocess detection with it.
#
# THIS TEST IS MOSTLY ABOUT WHAT MUST STILL BLOCK. The change turns BLOCK into
# PASS, which is a LOOSENING, so the arms that carry the security claim are the
# CI-* ones. A build that blocked nothing at all would sail through every
# "allowed" arm; CI-01..CI-12 are what stop that.
#
# THE ONE THAT MATTERS MOST IS CI-05/CI-06, THE COMMA TRAP. A narrowing that
# checks only the FIRST imported name lets `from os import path, system` pass:
# `path` satisfies it while `system` comes in beside it. The implementation
# requires the ENTIRE list to match to end-of-line for exactly this reason, and
# both orders are pinned because a first-name-only regex passes one of them.
#
# WHY AN ALLOWLIST. Omitting a safe name leaves it blocked -- a false positive.
# Omitting a dangerous name from a blocklist would permit it -- a hole. The
# inert set is deliberately small and fails in the first direction. CI-13/CI-14
# pin that unsupported forms (alias, parenthesised) stay BLOCKED rather than
# being quietly admitted.
#
# NOT A CLAIM THIS MAKES: that `from os import path` is harmless in general.
# Python introspection can reach a great deal from any object. The claim is
# narrower and is the one the rule is about: it does not BIND a name that
# executes, which `import os`, `import os.path` and `from os import *` all do.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
GOV="${NAAB_GOV:-$REPO/build/naab-gov}"
PASS=0; FAIL=0
ok()  { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); }

echo "=== code_injection: os imports that bind nothing executable ==="

if [ ! -x "$GOV" ]; then
    echo "  SKIP [CI-00] naab-gov not built -- UNMEASURABLE, not a pass"; exit 0
fi
if ! "$GOV" --version 2>/dev/null | grep -qi naab-gov; then
    echo "  FAIL [CI-00] $GOV does not identify as naab-gov"; exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "  SKIP [CI-00] python3 absent -- cannot parse verdicts, UNMEASURABLE"; exit 0
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
  "restrictions": { "code_injection": { "level": "hard" } } }
EOF
python3 -c "import json,sys; json.load(sys.stdin)" < "$W/govern.json" 2>/dev/null \
    || { echo "  FAIL [CI-00] generated govern.json is not valid JSON"; exit 1; }

# Verdict via the JSON contract, captured into a variable -- never `| grep -q`,
# which under pipefail reports failure exactly when the pattern IS present.
verdict() {
    printf '%s\n' "$1" | (cd "$W" && "$GOV" check --language python --config govern.json) 2>/dev/null \
      | python3 -c "
import json,sys
try: print('BLOCKED' if json.load(sys.stdin)['blocked'] else 'allowed')
except Exception: print('UNPARSEABLE')
"
}
expect() {  # $1 = id, $2 = expected, $3 = code, $4 = why
    V=$(verdict "$3")
    if [ "$V" = "$2" ]; then ok "$1" "$2: $3${4:+  ($4)}"
    else bad "$1" "expected $2, got $V: $3${4:+  ($4)}"; fi
}

echo "--- must stay BLOCKED (this is the security claim) ---"
expect CI-01 BLOCKED 'import os'                        'binds os'
expect CI-02 BLOCKED 'import os.path'                   'ALSO binds os'
expect CI-03 BLOCKED 'from os import system'
expect CI-04 BLOCKED 'from os import *'                 'binds every public name'
expect CI-05 BLOCKED 'from os import path, system'      'COMMA TRAP'
expect CI-06 BLOCKED 'from os import system, path'      'COMMA TRAP, reversed'
expect CI-07 BLOCKED 'from os import popen'
expect CI-08 BLOCKED 'from os import execv'
expect CI-09 BLOCKED 'from os import fork'
expect CI-10 BLOCKED 'from subprocess import run'       'other modules unchanged'
expect CI-11 BLOCKED 'from ctypes import CDLL'          'other modules unchanged'
expect CI-12 BLOCKED 'os.system("rm -rf /")'            'call detection unchanged'

echo "--- unsupported forms fail CLOSED ---"
expect CI-13 BLOCKED 'from os import path as p'         'alias not covered by the form'
expect CI-14 BLOCKED 'from os import (path, sep)'       'parenthesised not covered'

echo "--- the false positives, now allowed ---"
expect CI-15 allowed 'from os import path'
expect CI-16 allowed 'from os import getcwd'
expect CI-17 allowed 'from os import sep'
expect CI-18 allowed 'from os import path, sep'         'every name inert'
expect CI-19 allowed 'from os.path import join'         'submodule binds no os'

echo "--- CONTROL: the check is live and discriminating ---"
# Without this, a build where code_injection never fires passes CI-15..CI-19
# for free. CI-12 above is the paired positive; this is the negative.
expect CI-20 allowed 'x = 1 + 1'                        'ordinary code untouched'
expect CI-21 allowed 'import json'                      'undangerous module untouched'

echo "  Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
