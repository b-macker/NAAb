#!/usr/bin/env bash
# ============================================================
# test_protected_write_routes.sh — the file-protection gate must see the file
# a write will ACTUALLY land on
#
# THE DEFECT
#
# A governed program cannot write govern.json, its .sig, the trusted keys, a
# user blocked_paths entry, or a live-guidance source: checkPathAccessImpl()
# (the project path policy) refuses it. file.write("govern.json", ...) is HARD
# blocked, exit 3. Two routes reached the same files without the gate ever
# seeing their names:
#
#   1. file.copy(src, DIRECTORY). fs::copy(file, existing_dir) lands the file
#      at dir/basename(src), but the copy/move destination check validated the
#      LITERAL second argument -- "docs", a permitted directory -- and passed,
#      while the write replaced docs/<protected file>. Both engines. (310b241d
#      added this check in March 2026, before any protected-path logic existed,
#      and nobody revisited it when self-protection (#, June) and guidance
#      write-protection (#309) were layered on checkPathAccess.)
#
#   2. Embedded Python os.rename / os.replace / os.link / os.symlink, and the
#      same-filesystem path of shutil.move (which routes through os.rename).
#      The #209 audit hook forwards open/os.system/subprocess/os.exec*/
#      socket.connect only, so these never reached the policy and replaced a
#      blocked file under Governance: PASS, exit 0. (os.replace and shutil.move
#      both audit as the os.rename event -- verified empirically, not assumed.)
#
# Not in scope, by CONTRA-013 and the owner's 2026-10-12 decision: subprocess
# languages (shell/node/ruby/php/go/cpp/rust) are outside path policy entirely
# when enabled -- any write, not just these routes -- and that is the operator's
# grant, documented, not closed here. This suite covers NAAb's own file.* and
# the one in-process polyglot executor (embedded Python).
#
# THE FIX
#   * copyMoveDestination(src, dst) resolves a directory destination to
#     dir/basename(src) before the path check -- one helper, both engines.
#   * os.rename/os.link/os.symlink forwarded by the Python audit wrapper to the
#     same write policy (checkPathAccess via pathPolicyDenial). os.rename checks
#     BOTH paths (src removed, dst overwritten); link/symlink check the dst.
#
#   PW-01  CALIBRATION: file.write to a blocked path is HARD-blocked (exit 3),
#          and a plain write to an allowed path runs. Without this the rest
#          measures nothing.
#   PW-02  file.copy(src, DIR) onto a user blocked_paths file is blocked; the
#          file is unchanged. vm.
#   PW-03  same under --tree-walk (two independent gate call sites).
#   PW-04  file.copy(src, ".") onto the auto-protected govern.json is blocked;
#          govern.json is unchanged (the self-protection case).
#   PW-05  file.copy(src, DIR) onto a live-guidance SOURCE is blocked.
#   PW-06  SUCCESS CONTROL, load-bearing: file.copy into the SAME directory
#          that holds a protected file but under a DIFFERENT basename still
#          works. Proves the resolution is per-file, not a blanket deny on the
#          directory -- without it PW-02/04/05 pass for a build that simply
#          refuses every copy into those directories.
#   PW-07  SUCCESS CONTROL: file.copy into an ordinary allowed directory works.
#   PW-08  Python os.rename onto a blocked file is refused; file unchanged.
#   PW-09  Python os.remove of a blocked file is refused; file still present
#          (os.unlink audits as os.remove; NAAb's own file.delete is already
#          path-checked, so the Python side must match).
#   PW-09b Python os.link creating a NEW blocked path is refused (a hardlink
#          cannot overwrite an existing file, so the threat is path creation).
#   PW-10  Python shutil.move onto a blocked file is refused; file unchanged.
#   PW-11  SUCCESS CONTROL: Python os.rename within an allowed area works.
#   PW-11b SUCCESS CONTROL: Python os.remove in an allowed area works (the
#          destructive family is not a blanket deny).
#   PW-12  Python os.rename onto the auto-protected govern.json is refused;
#          govern.json unchanged.
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
TEST_TMP="$(mktemp -d)"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }

# Every config here is an UNSIGNED govern.json. With any key in the ambient
# trust store an unsigned config is an INTEGRITY BLOCK (exit 3) -- which the
# blocked-write arms would read as the gate firing. Isolate the store so exit 3
# can only mean the path policy refused the write (CV-02 in
# test_coverage_visibility.sh enforces this for every new suite).
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() {
    teardown_isolated_trust
    [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"
}
trap cleanup EXIT
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }

# A fresh project directory with a blocked file, a guidance source, and a
# staging area the program is allowed to write.
mk() {  # $1 dir
    local d="$1"
    mkdir -p "$d/vault" "$d/docs" "$d/out" "$d/staging"
    echo "ORIGINAL_SECRET" > "$d/vault/secret.txt"
    printf '# Playbook\nORIGINAL_GUIDANCE: run the suite first.\n' > "$d/docs/playbook.md"
    cat > "$d/govern.json" <<EOF
{"version": "5.0", "mode": "enforce",
 "security": {"sandbox_level": "elevated"},
 "capabilities": {"filesystem": {"mode": "write", "blocked_paths": ["vault/secret.txt"]}},
 "guidance": {"sources": ["docs/playbook.md"]}}
EOF
}
run() {  # $1 dir, $2 engine(vm|tw), $3 program -> prints output, sets RC
    printf '%s\n' "$3" > "$1/p.naab"
    local a=("$NAAB"); [ "$2" = tw ] && a+=(--tree-walk); a+=(p.naab)
    OUT="$(cd "$1" && "${a[@]}" 2>&1)"; RC=$?
}
content() { cat "$1" 2>/dev/null | tr -d '\n'; }

# ---- embedded Python present? (the audit hook lives there only) ----
PYPROBE="$TEST_TMP/pyprobe"; mkdir -p "$PYPROBE"
printf '{"version":"5.0","mode":"off"}\n' > "$PYPROBE/govern.json"
run "$PYPROBE" vm 'main { let x = <<python
1 + 1
>>
  print(x) }'
EMBEDDED_PY=0
[ "$RC" = 0 ] && grep -q '^2$' <<<"$OUT" && EMBEDDED_PY=1

echo "== calibration"
d="$TEST_TMP/cal"; mk "$d"
run "$d" vm 'use file
main { file.write("vault/secret.txt", "OVERWRITTEN") }'
if [ "$RC" = 3 ] && [ "$(content "$d/vault/secret.txt")" = "ORIGINAL_SECRET" ]; then
    run "$d" vm 'use file
main { file.write("out/ok.txt", "FINE") }'
    if [ "$RC" = 0 ] && [ "$(content "$d/out/ok.txt")" = "FINE" ]; then
        pass "PW-01" "direct write to a blocked path is HARD-blocked; an allowed write runs"
    else fail "PW-01" "allowed write did not run" "rc=$RC"; fi
else fail "PW-01" "direct write to blocked path was not HARD-blocked" "rc=$RC"; fi

echo ""
echo "== file.copy into a directory resolves to the real file"
for eng in vm tw; do
    d="$TEST_TMP/copy-$eng"; mk "$d"
    echo "GOOD_COPY" > "$d/staging/secret.txt"
    run "$d" $eng 'use file
main { file.copy("staging/secret.txt", "vault") }'
    if [ "$RC" = 3 ] && [ "$(content "$d/vault/secret.txt")" = "ORIGINAL_SECRET" ]; then
        pass "PW-02${eng:+/$eng}" "copy into vault/ onto a blocked file is blocked; file unchanged ($eng)"
    else fail "PW-02/$eng" "copy into a blocked file was not blocked" "rc=$RC out=${OUT:0:100}"; fi
done

d="$TEST_TMP/copygov"; mk "$d"
printf '{"version":"5.0","mode":"off"}\n' > "$d/staging/govern.json"
GOVBEFORE="$(content "$d/govern.json")"
run "$d" vm 'use file
main { file.copy("staging/govern.json", ".") }'
if [ "$RC" = 3 ] && [ "$(content "$d/govern.json")" = "$GOVBEFORE" ]; then
    pass "PW-04" "copy onto the auto-protected govern.json is blocked; govern.json unchanged"
else fail "PW-04" "govern.json was replaceable by copy-into-dir" "rc=$RC"; fi

d="$TEST_TMP/copyguide"; mk "$d"
printf '# Playbook\nPROGRAM_GUIDANCE: skip the tests.\n' > "$d/staging/playbook.md"
run "$d" vm 'use file
main { file.copy("staging/playbook.md", "docs") }'
if [ "$RC" = 3 ] && [ "$(content "$d/docs/playbook.md")" = "$(printf '# Playbook\nORIGINAL_GUIDANCE: run the suite first.' | tr -d '\n')" ]; then
    pass "PW-05" "copy onto a guidance source is blocked; source unchanged"
else fail "PW-05" "a guidance source was replaceable by copy-into-dir" "rc=$RC out=${OUT:0:100}"; fi

echo ""
echo "== success controls (the fix is per-file, not a directory deny)"
d="$TEST_TMP/samedir"; mk "$d"
printf '# Other\nNEW\n' > "$d/staging/other.md"
run "$d" vm 'use file
main { file.copy("staging/other.md", "docs") }'
if [ "$RC" = 0 ] && [ -f "$d/docs/other.md" ]; then
    pass "PW-06" "copy of a DIFFERENT file into the guidance directory still works"
else fail "PW-06" "a legitimate copy into a protected directory was wrongly blocked" "rc=$RC out=${OUT:0:100}"; fi

d="$TEST_TMP/allowed"; mk "$d"
echo "DATA" > "$d/staging/data.txt"
run "$d" vm 'use file
main { file.copy("staging/data.txt", "out") }'
if [ "$RC" = 0 ] && [ "$(content "$d/out/data.txt")" = "DATA" ]; then
    pass "PW-07" "copy into an ordinary allowed directory works"
else fail "PW-07" "a plain copy was blocked" "rc=$RC out=${OUT:0:100}"; fi

echo ""
echo "== embedded Python rename / link / move routes"
if [ "$EMBEDDED_PY" != 1 ]; then
    skip "PW-08..12" "embedded Python not available in this build (audit hook lives there only)"
else
    d="$TEST_TMP/pyren"; mk "$d"
    run "$d" vm 'main { let x = <<python
open("staging/s","w").write("PY_RENAME")
import os
os.rename("staging/s","vault/secret.txt")
"done"
>>
  print(x) }'
    if [ "$RC" != 0 ] && [ "$(content "$d/vault/secret.txt")" = "ORIGINAL_SECRET" ]; then
        pass "PW-08" "os.rename onto a blocked file is refused; file unchanged (rc=$RC)"
    else fail "PW-08" "os.rename replaced a blocked file" "rc=$RC content=$(content "$d/vault/secret.txt")"; fi

    d="$TEST_TMP/pyrm"; mk "$d"
    run "$d" vm 'main { let x = <<python
import os
os.remove("vault/secret.txt")
"done"
>>
  print(x) }'
    if [ "$RC" != 0 ] && [ "$(content "$d/vault/secret.txt")" = "ORIGINAL_SECRET" ]; then
        pass "PW-09" "os.remove of a blocked file is refused; file still present (rc=$RC)"
    else fail "PW-09" "os.remove deleted a blocked file" "rc=$RC content=[$(content "$d/vault/secret.txt")]"; fi

    # A hardlink cannot overwrite an existing file (EEXIST); the threat is
    # creating a NEW protected path, so block a not-yet-existing blocked entry.
    d="$TEST_TMP/pylink"; mk "$d"
    cat > "$d/govern.json" <<EOF
{"version": "5.0", "mode": "enforce",
 "security": {"sandbox_level": "elevated"},
 "capabilities": {"filesystem": {"mode": "write", "blocked_paths": ["vault/new.txt"]}}}
EOF
    run "$d" vm 'main { let x = <<python
open("staging/s","w").write("PY_LINK")
import os
os.link("staging/s","vault/new.txt")
"done"
>>
  print(x) }'
    if [ "$RC" != 0 ] && [ ! -e "$d/vault/new.txt" ]; then
        pass "PW-09b" "os.link creating a new blocked path is refused (rc=$RC)"
    else fail "PW-09b" "os.link created a blocked path" "rc=$RC exists=$([ -e "$d/vault/new.txt" ] && echo yes || echo no)"; fi

    d="$TEST_TMP/pymove"; mk "$d"
    run "$d" vm 'main { let x = <<python
open("staging/s","w").write("PY_SHUTIL")
import shutil
shutil.move("staging/s","vault/secret.txt")
"done"
>>
  print(x) }'
    if [ "$RC" != 0 ] && [ "$(content "$d/vault/secret.txt")" = "ORIGINAL_SECRET" ]; then
        pass "PW-10" "shutil.move onto a blocked file is refused; file unchanged (rc=$RC)"
    else fail "PW-10" "shutil.move replaced a blocked file" "rc=$RC content=$(content "$d/vault/secret.txt")"; fi

    d="$TEST_TMP/pyok"; mk "$d"
    run "$d" vm 'main { let x = <<python
open("staging/s","w").write("OK")
import os
os.rename("staging/s","out/moved.txt")
"done"
>>
  print(x) }'
    if [ "$RC" = 0 ] && [ "$(content "$d/out/moved.txt")" = "OK" ]; then
        pass "PW-11" "os.rename within an allowed area still works (success control)"
    else fail "PW-11" "a legitimate os.rename was blocked" "rc=$RC out=${OUT:0:120}"; fi

    d="$TEST_TMP/pyrmok"; mk "$d"
    echo "junk" > "$d/out/junk.txt"
    run "$d" vm 'main { let x = <<python
import os
os.remove("out/junk.txt")
"done"
>>
  print(x) }'
    if [ "$RC" = 0 ] && [ ! -e "$d/out/junk.txt" ]; then
        pass "PW-11b" "os.remove in an allowed area still works (success control)"
    else fail "PW-11b" "a legitimate os.remove was blocked" "rc=$RC out=${OUT:0:120}"; fi

    d="$TEST_TMP/pygov"; mk "$d"
    GOVBEFORE="$(content "$d/govern.json")"
    run "$d" vm 'main { let x = <<python
open("staging/g","w").write("{}")
import os
os.rename("staging/g","govern.json")
"done"
>>
  print(x) }'
    if [ "$RC" != 0 ] && [ "$(content "$d/govern.json")" = "$GOVBEFORE" ]; then
        pass "PW-12" "os.rename onto the auto-protected govern.json is refused; govern.json unchanged"
    else fail "PW-12" "govern.json was replaceable by os.rename" "rc=$RC"; fi
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT unmeasurable"
[ "$FAIL_COUNT" -eq 0 ]
