#!/usr/bin/env bash
# A99 (Bundle 16): embedded Python's allowlist scrub passes the same essential
# names as every other child's -- essentialChildEnvVars(), one list.
#
# Under `subprocess_scrub_mode: "allowlist"` a child keeps only the essential
# system names and the allowed ones. Every child NAAb starts gets that rule from
# subprocessListsScrub(), which reads essentialChildEnvVars() -- a list that
# leaves LD_LIBRARY_PATH out on purpose (a loader hijack vector). Embedded
# Python's os.environ scrub (python_c_executor.cpp) wrote its own copy of the
# names into the Python it generates, and that copy kept LD_LIBRARY_PATH, so a
# <<python>> block, and anything it started, saw the parent's value. The copy is
# now generated from essentialChildEnvVars() (the owner's decision, 2026-11-06).
#
# The parent's LD_LIBRARY_PATH is set to a directory that does not exist, so
# it changes nothing for the loader and is recognisable in output.
#
#   PE-00  CONTROL: no env policy -- os.environ has the probe LD_LIBRARY_PATH
#          (the probe reaches embedded Python at all)
#   PE-01  allowlist mode: os.environ has no LD_LIBRARY_PATH
#   PE-02  CONTROL: the same run keeps HOME (an essential name) and an allowed
#          name, and drops another name -- an allowlist, not a blanket clear
#   PE-03  CONTROL: a process.run child under the same config does not see the
#          probe value either -- the two routes agree
#
# Skipped (exit 0, SKIP printed) where embedded Python is unavailable.
# Red first (observed 2026-11-06, 01fb3552): PE-01 fails (os.environ holds the
# probe value); the controls pass.
# Kill table: M60, LD_LIBRARY_PATH added back to the generated list -> PE-01.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
case "$NAAB" in /*) ;; *) NAAB="$PWD/$NAAB" ;; esac

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       /'; }

echo "=== Embedded Python's allowlist scrub uses the shared essential names (A99) ==="
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) echo "  SKIP: POSIX-only (LD_LIBRARY_PATH)"; exit 0 ;;
esac
[ -x "$NAAB" ] || { echo "  FAIL naab-lang not built (UNMEASURABLE, not a pass)"; exit 1; }
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-pyenv.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
# These configs are unsigned: an isolated trust store (NAAB_TRUST_STORE_DIR,
# which wins over HOME), and a scratch HOME for everything else.
# shellcheck source=../helpers/trust_setup.sh
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT
export HOME="$W/home"; mkdir -p "$HOME"

printf 'main { let v = <<python\n1+1\n>>\nprint(string(v)) }\n' > "$W/probe.naab"
if [ "$(cd "$W" && "$NAAB" --no-governance probe.naab </dev/null 2>/dev/null | tail -1)" != "2" ]; then
    echo "  SKIP: embedded Python executor unavailable"; exit 0
fi

PROBE_LD="/nonexistent-naab-pe-probe"
PE="$(command -v printenv || true)"
cat > "$W/py.naab" <<EOF
use process

main {
    let ld = <<python
import os
os.environ.get("LD_LIBRARY_PATH", "ABSENT")
    >>
    let home = <<python
import os
"HOME_SET" if os.environ.get("HOME") else "HOME_ABSENT"
    >>
    let ok = <<python
import os
os.environ.get("NAAB_PE_OK", "ABSENT")
    >>
    let other = <<python
import os
os.environ.get("NAAB_PE_OTHER", "ABSENT")
    >>
    print("PY_LD=" + ld)
    print("PY_HOME=" + home)
    print("PY_OK=" + ok)
    print("PY_OTHER=" + other)
    let r = process.run("$PE", ["LD_LIBRARY_PATH"])
    print("CHILD_LD=[" + string.trim(r["stdout"]) + "]")
}
EOF

cfg() {  # cfg DIR ENV_VARS_JSON
    mkdir -p "$1"; cp "$W/py.naab" "$1/py.naab"
    printf '{"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"},"capabilities":{"env_vars":%s}}\n' "$2" > "$1/govern.json"
}
OUT=""; RC=0
run() {
    OUT=$(cd "$1" && LD_LIBRARY_PATH="$PROBE_LD" NAAB_PE_OK=yes NAAB_PE_OTHER=yes \
          timeout 30 "$NAAB" run py.naab </dev/null 2>&1); RC=$?
}

cfg "$W/none" '{}'
run "$W/none"
if [ "$RC" -eq 0 ] && grep -q "^PY_LD=$PROBE_LD\$" <<<"$OUT"; then
    ok PE-00 "CONTROL: with no env policy, os.environ has the probe LD_LIBRARY_PATH"
else
    bad PE-00 "CONTROL: os.environ should carry the probe value (rc=$RC)" "$OUT"
fi

cfg "$W/allow" '{"subprocess_scrub_mode":"allowlist","allowed_subprocess_vars":["NAAB_PE_OK"]}'
run "$W/allow"
if [ "$RC" -eq 0 ] && grep -q '^PY_LD=ABSENT$' <<<"$OUT"; then
    ok PE-01 "allowlist mode: os.environ has no LD_LIBRARY_PATH"
else
    bad PE-01 "allowlist mode: LD_LIBRARY_PATH should be withheld from os.environ (rc=$RC)" "$(grep -E '^PY_|^CHILD_' <<<"$OUT")"$'\n'"$OUT"
fi
if [ "$RC" -eq 0 ] && grep -q '^PY_HOME=HOME_SET$' <<<"$OUT" && grep -q '^PY_OK=yes$' <<<"$OUT" \
   && grep -q '^PY_OTHER=ABSENT$' <<<"$OUT"; then
    ok PE-02 "CONTROL: HOME and the allowed name pass, another name is dropped"
else
    bad PE-02 "CONTROL: expected HOME and NAAB_PE_OK kept, NAAB_PE_OTHER dropped" "$(grep -E '^PY_' <<<"$OUT")"
fi
if [ -z "$PE" ]; then
    echo "  SKIP [PE-03] UNMEASURABLE: no printenv on this machine"
elif [ "$RC" -eq 0 ] && grep -q '^CHILD_LD=' <<<"$OUT" && ! grep -q "^CHILD_LD=\[$PROBE_LD\]" <<<"$OUT"; then
    ok PE-03 "CONTROL: a process.run child does not see the probe value either"
else
    bad PE-03 "CONTROL: the child should not see the probe LD_LIBRARY_PATH" "$(grep -E '^CHILD_' <<<"$OUT")"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
