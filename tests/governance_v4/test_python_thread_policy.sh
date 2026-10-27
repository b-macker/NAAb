#!/usr/bin/env bash
# ============================================================
# test_python_thread_policy.sh — governance reaches threads a <<python>> block spawns
#
# THE FAILURE THIS CATCHES
#
# The embedded-Python audit hook is installed PROCESS-WIDE (PySys_AddAuditHook)
# and fires on every thread that runs Python. But the policy it consults
# (naabPythonAuditPolicy) read two THREAD-LOCALS -- ScopedSandbox::getCurrent()
# and GovernanceEngine::getCurrent(). A threading.Thread a <<python>> block
# spawns runs on a fresh OS thread where neither was installed, so the policy
# saw sandbox==nullptr / gov==nullptr and FAILED OPEN. Measured on the pre-fix
# binary, enforce + elevated: a spawned thread read a blocked_paths file
# (returned its contents) and ran os.system under shell.enabled:false (created
# its marker), while the block's OWN thread was correctly refused.
#
# The fix gives the policy a RUN-level fallback -- the sandbox config the run
# established (SandboxManager default, exactly what vm.cpp builds every polyglot
# ScopedSandbox from) and the process-wide engine (GovernanceEngine::
# getProcessCurrent()) -- consulted only when the thread-local is absent. It
# turns PASS->BLOCK on spawned threads and adds no false denial, because the
# fallback is the same policy the owning thread already enforces.
#
#   TP-00  CONTROL: the block's OWN thread is refused the blocked path -- the
#          fixture really puts the path in policy
#   TP-01  SUBJECT: a thread the block spawns is refused the blocked path (VM)
#   TP-02  CONTROL (over-block direction): a spawned thread may still read an
#          ALLOWED path -- the fix is not a blanket deny (load-bearing: a
#          fail-closed mutant passes every refusal arm but this one)
#   TP-03c CONTROL: the block's OWN thread is refused os.system (shell off)
#   TP-03  SUBJECT: a spawned thread is refused os.system and no marker is made
#   TP-04c CONTROL: the block's OWN thread is refused os.remove of a blocked path
#   TP-04  SUBJECT: a spawned thread is refused os.remove (the rename/remove
#          audit branch), and the file survives
#   TP-05  CONTROL (non-regression): an UNGOVERNED run (no govern.json) still
#          lets a spawned thread read -- the fallback does not globally close
#   TP-06  SUBJECT: a grandchild thread (thread spawns a thread) is also refused
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

W="${TMPDIR:-/tmp}/naab-pythread-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W/data" "$W/vault"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }

echo "VAULT_CONTENT" > "$W/vault/s.txt"
echo "DATA_CONTENT"  > "$W/data/ok.txt"

# The embedded executor is what this is about; skip where Python is absent
# (a subprocess Python fallback has no in-process audit hook, so the bypass and
# its fix do not apply there).
printf 'main { let v = <<python\n1+1\n>>\nprint(string(v)) }\n' > "$W/probe.naab"
if [ "$(cd "$W" && "$NAAB" --no-governance probe.naab 2>/dev/null | tail -1)" != "2" ]; then
    echo "  SKIP: embedded Python executor unavailable"; exit 0
fi

run() { ( cd "$W" && timeout 60 "$NAAB" ${2:-} "$1" 2>&1 ); }

# ---- Group A: file-read path policy on a spawned thread --------------------
echo -e "${CYAN}=== Group TP: governance reaches Python-spawned threads ===${NC}"

# A block whose thread reads $1 and reports R=DENIED on refusal, R=<content> otherwise.
thread_read() {  # $1 = path expression (python)
    cat <<EOF
main {
  let r = <<python
import threading
box = {}
def go():
    try:
        box["v"] = open($1).read().strip()
    except PermissionError:
        box["v"] = "DENIED"
    except Exception as e:
        box["v"] = "ERR:" + type(e).__name__
t = threading.Thread(target=go); t.start(); t.join()
box.get("v", "?")
>>
  print("R=" + string(r))
}
EOF
}

# main-thread read of $1 (control that the gate is live for the path).
main_read() {  # $1 = path expression
    cat <<EOF
main {
  let r = <<python
try:
    r = open($1).read().strip()
except PermissionError:
    r = "DENIED"
r
>>
  print("R=" + string(r))
}
EOF
}

FS_POLICY='{"version":"5.0","mode":"enforce","security":{"sandbox_level":"standard"},"languages":{"allowed":["python"]},"capabilities":{"filesystem":{"mode":"write","allowed_paths":["./data","./vault"],"blocked_paths":["./vault"]}}}'
printf '%s\n' "$FS_POLICY" > "$W/govern.json"

main_read '"vault/s.txt"' > "$W/m.naab"
out=$(run m.naab)
if grep -q 'R=DENIED' <<<"$out" && ! grep -q 'VAULT_CONTENT' <<<"$out"; then ok "TP-00" "CONTROL: the block's own thread is refused the blocked path"
else bad "TP-00" "main-thread read not denied -- fixture does not put the path in policy" "$(grep -m2 -E 'R=|rror' <<<"$out")"; fi

thread_read '"vault/s.txt"' > "$W/t.naab"
out=$(run t.naab)
if grep -q 'R=DENIED' <<<"$out" && ! grep -q 'VAULT_CONTENT' <<<"$out"; then ok "TP-01" "SUBJECT: a spawned thread is refused the blocked path (VM)"
else bad "TP-01" "a spawned thread read the blocked path -- audit policy fails open off-thread" "$(grep -m2 -E 'R=|rror' <<<"$out")"; fi

thread_read '"data/ok.txt"' > "$W/ta.naab"
out=$(run ta.naab)
if grep -q 'R=DATA_CONTENT' <<<"$out"; then ok "TP-02" "CONTROL: a spawned thread may still read an allowed path (no over-block)"
else bad "TP-02" "the fix over-blocks: an ALLOWED read on a spawned thread was denied" "$(grep -m2 -E 'R=|rror' <<<"$out")"; fi

# ---- Group B: exec capability on a spawned thread (shell disabled) ---------
SHELL_OFF='{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},"languages":{"allowed":["python"]},"capabilities":{"shell":{"enabled":false}}}'
printf '%s\n' "$SHELL_OFF" > "$W/govern.json"
TOUCH="$(command -v touch)"; : "${TOUCH:=/usr/bin/touch}"

cat > "$W/em.naab" <<EOF
main {
  let r = <<python
import os
try:
    os.system("$TOUCH main_marker"); r = "RAN"
except PermissionError:
    r = "DENIED"
r
>>
  print("R=" + string(r))
}
EOF
rm -f "$W/main_marker"
out=$(run em.naab)
if ! [ -e "$W/main_marker" ]; then ok "TP-03c" "CONTROL: the block's own thread is refused os.system (shell off)"
else bad "TP-03c" "main-thread os.system ran under shell.enabled:false -- exec gate not live" "$(grep -m2 -E 'R=|rror' <<<"$out")"; fi

cat > "$W/et.naab" <<EOF
main {
  let r = <<python
import os, threading
box = {}
def go():
    try:
        os.system("$TOUCH thread_marker"); box["v"] = "RAN"
    except PermissionError:
        box["v"] = "DENIED"
t = threading.Thread(target=go); t.start(); t.join()
box.get("v", "?")
>>
  print("R=" + string(r))
}
EOF
rm -f "$W/thread_marker"
out=$(run et.naab)
if ! [ -e "$W/thread_marker" ]; then ok "TP-03" "SUBJECT: a spawned thread is refused os.system; no marker made"
else bad "TP-03" "a spawned thread ran os.system under shell.enabled:false -- exec gate fails open off-thread" "$(grep -m2 -E 'R=|rror' <<<"$out")"; fi

# ---- Group C: the rename/remove audit branch on a spawned thread -----------
printf '%s\n' "$FS_POLICY" > "$W/govern.json"

rm_block() {  # $1 = "main" or "thread"
    if [ "$1" = "main" ]; then
        cat <<EOF
main {
  let r = <<python
import os
try:
    os.remove("vault/s.txt"); r = "REMOVED"
except PermissionError:
    r = "DENIED"
r
>>
  print("R=" + string(r))
}
EOF
    else
        cat <<EOF
main {
  let r = <<python
import os, threading
box = {}
def go():
    try:
        os.remove("vault/s.txt"); box["v"] = "REMOVED"
    except PermissionError:
        box["v"] = "DENIED"
t = threading.Thread(target=go); t.start(); t.join()
box.get("v", "?")
>>
  print("R=" + string(r))
}
EOF
    fi
}

echo "VAULT_CONTENT" > "$W/vault/s.txt"
rm_block main > "$W/rm.naab"
out=$(run rm.naab)
if grep -q 'R=DENIED' <<<"$out" && [ -e "$W/vault/s.txt" ]; then ok "TP-04c" "CONTROL: the block's own thread is refused os.remove of a blocked path"
else bad "TP-04c" "main-thread os.remove not denied / file gone -- remove gate not live" "$(grep -m2 -E 'R=|rror' <<<"$out")"; fi

echo "VAULT_CONTENT" > "$W/vault/s.txt"
rm_block thread > "$W/rmt.naab"
out=$(run rmt.naab)
if grep -q 'R=DENIED' <<<"$out" && [ -e "$W/vault/s.txt" ]; then ok "TP-04" "SUBJECT: a spawned thread is refused os.remove; the file survives"
else bad "TP-04" "a spawned thread removed a blocked path -- remove audit branch fails open off-thread" "$(grep -m2 -E 'R=|rror' <<<"$out")"; fi

# ---- Group D: non-regression -- an ungoverned run still reads on a thread ---
rm -f "$W/govern.json"
thread_read '"data/ok.txt"' > "$W/u.naab"
out=$(run u.naab --no-governance)
if grep -q 'R=DATA_CONTENT' <<<"$out"; then ok "TP-05" "CONTROL: an ungoverned run still reads on a spawned thread (no global fail-close)"
else bad "TP-05" "the fix closed an UNGOVERNED run's spawned-thread read -- behaviour changed" "$(grep -m2 -E 'R=|rror' <<<"$out")"; fi

# ---- Group E: a grandchild thread (thread spawns a thread) -----------------
# Re-create the file so this arm does not depend on TP-04's outcome: on the
# pre-fix binary TP-04 actually removes it, which would mask this arm's own
# bypass as a FileNotFoundError instead of a clean read.
printf '%s\n' "$FS_POLICY" > "$W/govern.json"
echo "VAULT_CONTENT" > "$W/vault/s.txt"
cat > "$W/gc.naab" <<'EOF'
main {
  let r = <<python
import threading
box = {}
def inner():
    try:
        box["v"] = open("vault/s.txt").read().strip()
    except PermissionError:
        box["v"] = "DENIED"
    except Exception as e:
        box["v"] = "ERR:" + type(e).__name__
def outer():
    g = threading.Thread(target=inner); g.start(); g.join()
t = threading.Thread(target=outer); t.start(); t.join()
box.get("v", "?")
>>
  print("R=" + string(r))
}
EOF
out=$(run gc.naab)
if grep -q 'R=DENIED' <<<"$out" && ! grep -q 'VAULT_CONTENT' <<<"$out"; then ok "TP-06" "SUBJECT: a grandchild thread is also refused the blocked path"
else bad "TP-06" "a grandchild thread read the blocked path" "$(grep -m2 -E 'R=|rror' <<<"$out")"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
if [ $FAIL -gt 0 ]; then echo -e "${RED}FAILURES:${NC}$FAILURES"; exit 1; fi
exit 0
