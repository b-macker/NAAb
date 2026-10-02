#!/usr/bin/env bash
# ============================================================
# test_relative_new_path.sh — a bare relative path to a NEW file is resolved
#
# THE FAILURE THIS CATCHES
#
# checkPathAccess() canonicalized the target with weakly_canonical(), which
# returns a relative path UNCHANGED when its first component does not exist.
# A bare name for a file not yet on disk ("out.txt") therefore stayed relative
# while every policy entry is absolute, and no prefix ever matched it. Both
# directions were wrong, and only the bare spelling -- "./out.txt" resolved:
#
#   * allowed_paths ["."] refused file.write("out.txt") (fail-closed)
#   * an UNSIGNED project's govern.json.sig -- auto-protected, but not on
#     disk -- was writable as "govern.json.sig" (fail-OPEN: a forged
#     signature planted by the program it is meant to govern)
#
# Found converting the mid-run reload tests to an external swap operator: a
# Python block could not write a marker file in its own cwd.
#
#   RN-01  file.write of a bare new name under allowed_paths ["."] passes
#   RN-02  same for a <<python>> open(..., "w") (the hook asks the same function)
#   RN-03  a bare govern.json.sig in an unsigned project is blocked (VM)
#   RN-04  same on the tree-walker
#   RN-05  same for a <<python>> write
#   RN-06  CONTROL: a path OUTSIDE allowed_paths is still refused -- RN-01
#          cannot pass by the allowlist going quiet
#   RN-07  CONTROL: the "./" spelling keeps its verdict (it was always right)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

W="${TMPDIR:-/tmp}/naab-relnew-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W/proj" "$W/outside"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }
P="$W/proj"

echo "=== Bare relative paths to files that do not exist yet ==="

cfg() {  # $1 = filesystem object JSON
    printf '{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},"languages":{"allowed":["python"]},"capabilities":{"filesystem":%s}}\n' "$1" > "$P/govern.json"
}
run() { ( cd "$P" && timeout 60 "$NAAB" ${2:-} "$1" 2>&1 ); }
naab_write() {  # $1 = path literal -> program file
    printf 'use file\nmain {\n  file.write("%s", "x")\n  print("WROTE")\n}\n' "$1" > "$P/w.naab"
}
py_write() {  # $1 = path literal -> program file
    cat > "$P/p.naab" <<EOF
main {
  let r = <<python
try:
    target = "$1"
    open(target, "w").write("x")
    r = "WROTE"
except PermissionError:
    r = "DENIED"
r
>>
  print("R=" + string(r))
}
EOF
}

cfg '{"mode":"write","allowed_paths":["."]}'
rm -f "$P/new1.txt"; naab_write new1.txt; out=$(run w.naab); rc=$?
if [ $rc -eq 0 ] && [ -f "$P/new1.txt" ]; then ok "RN-01" "file.write(\"new1.txt\") under allowed_paths [\".\"] passes"
else bad "RN-01" "bare new file refused (rc=$rc)" "$(grep -m1 -E 'rror' <<<"$out")"; fi

printf 'main { let v = <<python\n1+1\n>>\nprint(string(v)) }\n' > "$P/probe.naab"
probe_out=$(run probe.naab 2>/dev/null)
if ! grep -qx 2 <<<"$probe_out"; then
    skip "RN-02" "embedded Python executor unavailable (UNMEASURABLE)"
    PY=0
else
    PY=1
    rm -f "$P/new2.txt"; py_write new2.txt; out=$(run p.naab)
    if grep -q 'R=WROTE' <<<"$out" && [ -f "$P/new2.txt" ]; then ok "RN-02" "a <<python>> write of a bare new name passes too"
    else bad "RN-02" "python bare new-file write refused" "$(grep -m1 -E 'R=|rror' <<<"$out")"; fi
fi

# Unsigned project: govern.json.sig is auto-protected but does not exist.
cfg '{"mode":"write"}'
rm -f "$P/govern.json.sig"; naab_write govern.json.sig
out=$(run w.naab); rc=$?
if [ $rc -eq 3 ] && [ ! -e "$P/govern.json.sig" ]; then ok "RN-03" "a bare govern.json.sig is blocked in an unsigned project (VM)"
else bad "RN-03" "planted a signature file (rc=$rc exists=$([ -e "$P/govern.json.sig" ] && echo y || echo n))"; fi
rm -f "$P/govern.json.sig"

out=$(run w.naab --tree-walk); rc=$?
if [ $rc -eq 3 ] && [ ! -e "$P/govern.json.sig" ]; then ok "RN-04" "same on the tree-walker"
else bad "RN-04" "planted a signature file on the tree-walker (rc=$rc)"; fi
rm -f "$P/govern.json.sig"

if [ "$PY" = 1 ]; then
    py_write govern.json.sig; out=$(run p.naab)
    if grep -q 'R=DENIED' <<<"$out" && [ ! -e "$P/govern.json.sig" ]; then ok "RN-05" "a <<python>> write of a bare govern.json.sig is denied"
    else bad "RN-05" "python planted a signature file" "$(grep -m1 -E 'R=' <<<"$out")"; fi
    rm -f "$P/govern.json.sig"
else
    skip "RN-05" "embedded Python executor unavailable (UNMEASURABLE)"
fi

cfg '{"mode":"write","allowed_paths":["."]}'
naab_write ../outside/new3.txt; out=$(run w.naab); rc=$?
if [ $rc -eq 3 ] && [ ! -e "$W/outside/new3.txt" ]; then ok "RN-06" "CONTROL: a new file outside allowed_paths is still refused"
else bad "RN-06" "write outside allowed_paths went through (rc=$rc)"; fi

cfg '{"mode":"write"}'
naab_write ./govern.json.sig; out=$(run w.naab); rc=$?
if [ $rc -eq 3 ] && [ ! -e "$P/govern.json.sig" ]; then ok "RN-07" "CONTROL: ./govern.json.sig keeps its block"
else bad "RN-07" "the ./ spelling changed verdict (rc=$rc)"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
