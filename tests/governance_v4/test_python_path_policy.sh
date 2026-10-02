#!/usr/bin/env bash
# ============================================================
# test_python_path_policy.sh — the project path policy reaches <<python>> blocks
#
# THE FAILURE THIS CATCHES
#
# The embedded Python audit hook (#209, A7) asked only the SANDBOX about each
# open(). The sandbox is an allowlist with no notion of blocked_paths, so
# capabilities.filesystem.allowed_paths / blocked_paths -- and the
# auto-protected govern.json, its .sig and the trusted keys -- did not reach a
# Python block at all. Measured at sandbox `standard` (the enforce default):
# file.read("vault/s.txt") is a HARD block, while a <<python>> block in the same
# program read it and governance reported PASS. docs/plan-enforcement-boundaries.md
# recorded this as the hole stage 4 had to close; the design it states is one
# decision function called by every path, and the hook is an in-process call
# site, so it now asks that function (pathPolicyDenial -> the same code
# file.read runs, never throwing inside CPython).
#
# Found adding a Python step to examples/agent_harness, whose evidence files
# (telemetry, transcript, audit) are in blocked_paths: a Python block could
# have overwritten them.
#
#   PY-00  CONTROL: with NO path policy the block still reads the file -- the
#          hook decides on policy, it does not blanket-deny (and stdlib imports
#          still work under an allowlist: PY-06)
#   PY-01  CONTROL: NAAb's file.read of the same path is HARD-blocked, so the
#          path really is in the policy the Python arms are tested against
#   PY-02  a read of a blocked path is denied (VM)
#   PY-03  same on the tree-walker
#   PY-04  an OBFUSCATED read (no literal open(, no `os.` token) is denied --
#          the load-bearing arm: no source-text gate can see it
#   PY-05  a write outside allowed_paths is denied and the file is not created
#   PY-06  CONTROL: an allowed path reads, and stdlib imports load
#   PY-07  a block inside an async fn (worker thread) is denied on the VM -- the
#          engine pointer is thread_local and was not propagated to the worker
#   PY-08  the auto-protected govern.json is denied to Python too
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; FAILURES=""
ok()  { PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }

W="${TMPDIR:-/tmp}/naab-pypath-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W/data" "$W/vault"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }

echo "VAULT_CONTENT" > "$W/vault/s.txt"
echo "DATA_CONTENT" > "$W/data/ok.txt"

# The embedded executor is what this is about; skip where Python is absent.
printf 'main { let v = <<python\n1+1\n>>\nprint(string(v)) }\n' > "$W/probe.naab"
if [ "$(cd "$W" && "$NAAB" --no-governance probe.naab 2>/dev/null | tail -1)" != "2" ]; then
    echo "  SKIP: embedded Python executor unavailable"; exit 0
fi

policy() {  # $1 = filesystem object JSON
    printf '{"version":"5.0","mode":"enforce","security":{"sandbox_level":"standard"},"languages":{"allowed":["python"]},"capabilities":{"filesystem":%s}}\n' "$1" > "$W/govern.json"
}
POLICY='{"mode":"write","allowed_paths":["./data","./vault"],"blocked_paths":["./vault"]}'
run() { ( cd "$W" && timeout 60 "$NAAB" ${2:-} "$1" 2>&1 ); }

# A block that reports DENIED on PermissionError and the content otherwise.
read_block() {  # $1 = path expression (python)
    cat <<EOF
main {
  let r = <<python
target = $1
try:
    r = open(target).read().strip()
except PermissionError:
    r = "DENIED"
r
>>
  print("R=" + string(r))
}
EOF
}

echo -e "${CYAN}=== Group PY: path policy reaches embedded Python ===${NC}"

policy '{"mode":"write"}'
read_block '"vault/s.txt"' > "$W/t.naab"
out=$(run t.naab)
if grep -q 'R=VAULT_CONTENT' <<<"$out"; then ok "PY-00" "CONTROL: with no path policy the block reads the file"
else bad "PY-00" "block denied without any path policy -- the hook blanket-denies" "$(grep -m2 -E 'R=|rror' <<<"$out")"; fi

policy "$POLICY"
printf 'use file\nmain { print(file.read("vault/s.txt")) }\n' > "$W/n.naab"
out=$(run n.naab); rc=$?
if [ $rc -eq 3 ] && grep -q 'blocked by governance' <<<"$out"; then ok "PY-01" "CONTROL: NAAb file.read of the path is HARD-blocked"
else bad "PY-01" "file.read not blocked (rc=$rc) -- fixture does not put the path in policy"; fi

out=$(run t.naab)
if grep -q 'R=DENIED' <<<"$out" && ! grep -q 'VAULT_CONTENT' <<<"$out"; then ok "PY-02" "a <<python>> read of a blocked path is denied (VM)"
else bad "PY-02" "python read a blocked path (VM)" "$(grep -m2 -E 'R=' <<<"$out")"; fi

out=$(run t.naab --tree-walk)
if grep -q 'R=DENIED' <<<"$out" && ! grep -q 'VAULT_CONTENT' <<<"$out"; then ok "PY-03" "same on the tree-walker"
else bad "PY-03" "python read a blocked path (tree-walk)" "$(grep -m2 -E 'R=' <<<"$out")"; fi

cat > "$W/o.naab" <<'EOF'
main {
  let r = <<python
try:
    _sc = getattr("".__class__.__base__, "__subcla" + "sses__")()
    _imp = [c for c in _sc if c.__name__ == "BuiltinImporter"][0]
    _m = getattr(_imp, "load_" + "module")("o" + "s")
    _fd = getattr(_m, "op" + "en")("vault/" + "s.txt", 0)
    r = getattr(_m, "re" + "ad")(_fd, 64).decode().strip()
except PermissionError:
    r = "DENIED"
r
>>
  print("R=" + string(r))
}
EOF
out=$(run o.naab)
if grep -q 'R=DENIED' <<<"$out" && ! grep -q 'VAULT_CONTENT' <<<"$out"; then ok "PY-04" "an obfuscated os.open of a blocked path is denied"
else bad "PY-04" "obfuscated read got through" "$(grep -m2 -E 'R=|rror' <<<"$out")"; fi

cat > "$W/w.naab" <<'EOF'
main {
  let r = <<python
try:
    open("outside.txt", "w").write("x")
    r = "WROTE"
except PermissionError:
    r = "DENIED"
r
>>
  print("R=" + string(r))
}
EOF
rm -f "$W/outside.txt"
out=$(run w.naab)
if grep -q 'R=DENIED' <<<"$out" && [ ! -e "$W/outside.txt" ]; then ok "PY-05" "a write outside allowed_paths is denied and nothing is created"
else bad "PY-05" "write outside allowed_paths went through" "$(grep -m2 -E 'R=' <<<"$out")"; fi

cat > "$W/a.naab" <<'EOF'
main {
  let r = <<python
import json, ast, hashlib
json.dumps({"n": len(ast.parse("x = 1").body)}) + "|" + open("data/ok.txt").read().strip()
>>
  print("R=" + string(r))
}
EOF
out=$(run a.naab)
if grep -q 'R={"n": 1}|DATA_CONTENT' <<<"$out"; then ok "PY-06" "CONTROL: an allowed path reads and stdlib imports load"
else bad "PY-06" "allowed read or imports broken" "$(grep -m3 -E 'R=|rror' <<<"$out")"; fi

cat > "$W/y.naab" <<'EOF'
async fn peek() {
  let c = <<python
try:
    r = open("vault/s.txt").read().strip()
except PermissionError:
    r = "DENIED"
r
>>
  return c
}
main {
  let f = peek()
  print("R=" + string(await f))
}
EOF
out=$(run y.naab)
if grep -q 'R=DENIED' <<<"$out" && ! grep -q 'VAULT_CONTENT' <<<"$out"; then ok "PY-07" "a block in an async fn (worker thread) is denied on the VM"
else bad "PY-07" "async python read a blocked path" "$(grep -m2 -E 'R=|rror' <<<"$out")"; fi

policy '{"mode":"write"}'
read_block '"govern.json"' > "$W/g.naab"
out=$(run g.naab)
if grep -q 'R=DENIED' <<<"$out"; then ok "PY-08" "the auto-protected govern.json is denied to Python"
else bad "PY-08" "python read govern.json" "$(grep -m1 -E 'R=' <<<"$out" | cut -c1-80)"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
if [ $FAIL -gt 0 ]; then echo -e "Failures:$FAILURES"; exit 1; fi
exit 0
