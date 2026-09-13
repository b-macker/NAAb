#!/usr/bin/env bash
# ============================================================
# test_path_policy_reach.sh -- does the path policy cover the program?
#
# WHY THIS EXISTS. capabilities.filesystem.allowed_paths / blocked_paths are
# enforced by checkPathAccess(), which NAAb's standard library calls. A polyglot
# block reaches the filesystem through its own language runtime and never passes
# through it. Measured on fb1e4bd: file.read() on a blocked path is a HARD block
# while a <<python>> block in the SAME program reads that file and governance
# reports PASS.
#
# Nothing in govern.json says so. The file reads as though the rules cover the
# program, and that is the defect CONTRA-013 addresses -- by reporting the
# boundary, not by moving it.
#
# WHAT THIS SUITE IS FOR, and it is not "does a string appear". An advisory is
# worth shipping only if its claim is TRUE and its remedy WORKS, so:
#
#   PR-01..04  the advisory fires where the bypass is reachable
#   PR-05..06  controls: it stays silent where it would be false
#   PR-07      the CLAIM is true -- the bypass really happens
#   PR-08      the positive control for PR-07: the same path IS blocked for
#              NAAb code, so PR-07 is measuring the boundary and not a config
#              that permits everything
#   PR-09      the REMEDY works -- "restricted" really refuses polyglot
#   PR-10      it is ADVISORY: the run still exits 0
#
# Without PR-07 and PR-09 this suite would certify a warning that reads well and
# might be wrong in either direction. Without PR-08, PR-07 passes on a build
# where blocked_paths does nothing at all.
#
# WHY "standard" IS IN THE FIRING SET. Two mechanisms shape the real table. The
# #222 registry gate refuses every language at "restricted". SubprocessContainment
# then blocks fork and exec at "standard", which stops the subprocess languages.
# The embedded Python executor runs IN-PROCESS when the build carries pybind11,
# so it never forks and containment has nothing to contain -- which is why the
# default enforce posture still leaks. PR-07 therefore uses python, and its skip
# condition is the absence of that executor rather than a platform name.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
LAST_OUTPUT=""
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  {
    echo "  FAIL [$1] $2"
    [ -n "${LAST_OUTPUT:-}" ] && echo "$LAST_OUTPUT" | sed 's/^/        | /' | head -12
    FAIL=$((FAIL+1))
}
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }
report() { echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

echo "=== Path policy reach (CONTRA-013) ==="

if [ ! -x "$NAAB" ]; then
    skip "PR-00" "naab-lang not built -- UNMEASURABLE, not a pass"; report; exit 0
fi

# Unsigned govern.json in every fixture; with a real trust store present each
# run is an INTEGRITY BLOCK before any of this is consulted, and every "silent"
# assertion would pass for the wrong reason.
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust

W="$(mktemp -d)"
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT
# The target sits in $W, and every reference to it below is RELATIVE, because
# the program, the polyglot block and the config all resolve against the same
# working directory. An absolute path here would be in the SHELL's vocabulary:
# under MSYS2 `mktemp -d` yields /tmp/xxx, the embedded Python is a native
# Windows build, and it answered this fixture with
# "FileNotFoundError: /tmp/tmp.../secret.txt". PR-07 then read as "the block ran
# but did not read the blocked path", i.e. as evidence that CONTRA-013 had
# become false, when the truth was that the fixture could not reach the file.
echo "POLICY_REACH_SECRET" > "$W/secret.txt"

# $1 = sandbox_level ("" leaves the key out entirely, which enforce mode
#      resolves to "standard" -- a distinct condition from setting it)
# $2 = filesystem body fragment
cfg() {
    local lvl_json=""
    [ -n "$1" ] && lvl_json="\"sandbox_level\": \"$1\""
    cat > "$W/govern.json" <<EOF
{
  "version": "4.0",
  "meta": { "mode": "enforce" },
  "security": { $lvl_json },
  "capabilities": {
    "filesystem": { "mode": "readwrite", $2 },
    "shell": { "enabled": true }
  }
}
EOF
    if command -v python3 >/dev/null 2>&1; then
        python3 -c "import json,sys; json.load(sys.stdin)" < "$W/govern.json" 2>/dev/null \
            || bad "PR-CFG" "generated govern.json is not valid JSON -- fixture is broken"
    fi
}

# Runs a trivial program and reports whether CONTRA-013 was raised.
advisory() {
    printf 'main {\n  print("ran")\n}\n' > "$W/p.naab"
    LAST_OUTPUT=$( (cd "$W" && timeout 60s "$NAAB" p.naab) 2>&1 )
    case "$LAST_OUTPUT" in
        *CONTRA-013*) echo "fires" ;;
        *)            echo "silent" ;;
    esac
}

# --- PR-01..04: fires wherever the bypass is reachable --------------------
for spec in \
    "standard|\"blocked_paths\": [\"./secret.txt\"]|PR-01|a blocked path at \"standard\"" \
    "elevated|\"blocked_paths\": [\"./secret.txt\"]|PR-02|a blocked path at \"elevated\"" \
    "elevated|\"allowed_paths\": [\"./data\"]|PR-03|an allowlist at \"elevated\"" \
    "|\"blocked_paths\": [\"./secret.txt\"]|PR-04|an unset level (enforce resolves it to \"standard\")"
do
    IFS='|' read -r lvl body id desc <<< "$spec"
    cfg "$lvl" "$body"
    R=$(advisory)
    if [ "$R" = "fires" ]; then ok "$id" "reported for $desc"
    else bad "$id" "no CONTRA-013 for $desc"; fi
done

# --- PR-05/06: controls -- silent where the claim would be false ----------
cfg "restricted" "\"blocked_paths\": [\"./secret.txt\"]"
R=$(advisory)
if [ "$R" = "silent" ]; then
    ok "PR-05" "silent at \"restricted\", where polyglot is refused (control)"
else
    bad "PR-05" "fired at \"restricted\" -- the advisory would be false there"
fi

cfg "elevated" "\"allowed_paths\": [], \"blocked_paths\": []"
R=$(advisory)
if [ "$R" = "silent" ]; then
    ok "PR-06" "silent with no path policy at all (control)"
else
    bad "PR-06" "fired without a path policy -- nothing to warn about"
fi

# --- PR-07: the claim is true --------------------------------------------
# The block WRITES A FILE rather than printing: a polyglot block's stdout is
# captured, not forwarded, so a printed token is invisible even on success.
cfg "elevated" "\"blocked_paths\": [\"./secret.txt\"]"
rm -f "$W/leaked.txt"
cat > "$W/bypass.naab" <<EOF
main {
  <<python
with open("secret.txt") as src:
    open("leaked.txt", "w").write(src.read())
>>
  print("block ran")
}
EOF
LAST_OUTPUT=$( (cd "$W" && timeout 60s "$NAAB" bypass.naab) 2>&1 )
if grep -q POLICY_REACH_SECRET "$W/leaked.txt" 2>/dev/null; then
    ok "PR-07" "the advisory tells the truth: <<python>> read a blocked path"
elif ! echo "$LAST_OUTPUT" | grep -q "block ran"; then
    skip "PR-07" "the python block did not execute here -- UNMEASURABLE, not a pass"
else
    bad "PR-07" "the block ran but did not read the blocked path -- CONTRA-013 may now be false"
fi

# --- PR-08: positive control for PR-07 -----------------------------------
# Without this, PR-07 passes on a build where blocked_paths enforces nothing,
# and the suite would be reporting a bypass of a gate that was never closed.
cat > "$W/direct.naab" <<EOF
main {
  let c = file.read("secret.txt")
  print("NAAB_READ_OK")
}
EOF
LAST_OUTPUT=$( (cd "$W" && timeout 60s "$NAAB" direct.naab) 2>&1 )
case "$LAST_OUTPUT" in
    *NAAB_READ_OK*) bad "PR-08" "file.read reached a blocked path -- PR-07 proves nothing" ;;
    *)              ok   "PR-08" "the same path IS blocked for NAAb code (control for PR-07)" ;;
esac

# --- PR-09: the recommended remedy works ---------------------------------
cfg "restricted" "\"blocked_paths\": [\"./secret.txt\"]"
rm -f "$W/leaked.txt"
LAST_OUTPUT=$( (cd "$W" && timeout 60s "$NAAB" bypass.naab) 2>&1 )
if [ ! -s "$W/leaked.txt" ]; then
    ok "PR-09" "the resolution holds: \"restricted\" refuses the polyglot block"
else
    bad "PR-09" "\"restricted\" did not stop the bypass -- the advisory's remedy is wrong"
fi

# --- PR-10: advisory means advisory --------------------------------------
cfg "elevated" "\"blocked_paths\": [\"./secret.txt\"]"
printf 'main {\n  print("ran")\n}\n' > "$W/p.naab"
( cd "$W" && timeout 60s "$NAAB" p.naab >/dev/null 2>&1 )
RC=$?
if [ "$RC" -eq 0 ]; then
    ok "PR-10" "reporting it does not block the run (exit 0)"
else
    bad "PR-10" "the advisory blocked a run that passed before it existed (exit $RC)"
fi

report
[ "$FAIL" -eq 0 ] || exit 1
exit 0
