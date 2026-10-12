#!/usr/bin/env bash
# A32: a program must not read a withheld environment variable out of the
# process environment FILE (/proc/<id>/environ) when capabilities.env_vars
# withholds it. env_vars.blocked_read / read:false gate env.get(); the environ
# file hands the whole block back as file bytes, past that gate -- measured
# through file.read, io.read_file, env.load_dotenv and embedded Python open().
#
# Each route has a positive control under a config with NO env policy: the
# route must return the marker there, or the arm is UNMEASURABLE (never a free
# pass). The gate arm then runs the same route under blocked_read and requires
# the value withheld. Both engines. A dead interpreter prints nothing, so every
# control fails and the suite fails -- no dead-interpreter allowlist entry.
#
# The subprocess <<sh>> route is the documented CONTRA-013 limit: path policy
# does not reach a separate process, so that arm asserts the route is STILL
# open and names the remedy, flipping if someone closes it without updating A32.
set -u
NAAB="${NAAB:-$(cd "$(dirname "$0")/../.." && pwd)/build/naab-lang}"
WORK="$(mktemp -d)"
# Every fixture here is an UNSIGNED govern.json, which is an INTEGRITY BLOCK
# whenever the ambient trust store holds a key -- isolate it so the result
# does not depend on what else has run (open-investigations D8; CV-02).
source "$(dirname "$0")/../helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$WORK"' EXIT
MARK="A32MARKER_7q3z"
export NAAB_A32_MARK="$MARK"

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] UNMEASURABLE: $2"; SKIP=$((SKIP+1)); }

# cfg DIR POLICY  — POLICY is "control" (no env policy) or "blocked" (blocked_read)
cfg() {
  local d="$1" pol="$2"
  mkdir -p "$d"
  if [ "$pol" = "control" ]; then
    cat > "$d/govern.json" <<'J'
{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"}}
J
  else
    cat > "$d/govern.json" <<'J'
{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},"capabilities":{"env_vars":{"blocked_read":["NAAB_A32_MARK"]}}}
J
  fi
}

# run ENGINE DIR PROGTEXT  -> prints "rc=<n>|<stdout>"
run() {
  local eng="$1" d="$2" prog="$3"
  printf '%s' "$prog" > "$d/p.naab"
  local args=("$NAAB"); [ "$eng" = tw ] && args+=(--tree-walk); args+=("$d/p.naab")
  local out; out="$(cd "$d" && "${args[@]}" 2>/dev/null)"; local rc=$?
  printf 'rc=%s|%s' "$rc" "$out"
}

seen()    { case "$1" in *"V=SEEN"*) return 0;; *) return 1;; esac; }
refused() { case "$1" in rc=3*|rc=1*) return 0;; *) return 1;; esac; }

# A file-reading route: control sees the marker, gate must withhold it.
# $1 id  $2 engine  $3 program (prints V=SEEN when the marker is found)
file_route() {
  local id="$1" eng="$2" prog="$3"
  local cd="$WORK/${id}_${eng}_c" gd="$WORK/${id}_${eng}_g"
  cfg "$cd" control; cfg "$gd" blocked
  local cr gr; cr="$(run "$eng" "$cd" "$prog")"; gr="$(run "$eng" "$gd" "$prog")"
  if ! seen "$cr"; then skip "$id/$eng" "control did not see the marker: ${cr#*|}"; return; fi
  if seen "$gr"; then bad "$id/$eng" "marker leaked under blocked_read: ${gr#*|}"; return; fi
  if refused "$gr"; then ok "$id/$eng" "control reads the environ file; blocked_read refuses it (${gr%%|*})"
  else ok "$id/$eng" "control reads the environ file; blocked_read withholds the marker (${gr%%|*})"; fi
}

P_FILE='use file
use string
main {
    let t = file.read("/proc/self/environ")
    if string.contains(t, "A32MARKER_7q3z") { print("V=SEEN") } else { print("V=NONE") }
}'
P_IO='use io
use string
main {
    let t = io.read_file("/proc/self/environ")
    if string.contains(t, "A32MARKER_7q3z") { print("V=SEEN") } else { print("V=NONE") }
}'
P_DOTENV='use env
use json
use string
main {
    let m = env.load_dotenv("/proc/self/environ")
    let t = json.stringify(m)
    if string.contains(t, "A32MARKER_7q3z") { print("V=SEEN") } else { print("V=NONE") }
}'
# NUL -> newline INSIDE the block: NAAb string marshalling can stop at the
# first NUL, which would truncate the environ before the marker and read as
# "contained" when it is not. Decode latin-1 so every byte survives.
P_PYOPEN='use string
main {
    let t = <<python
open("/proc/self/environ","rb").read().replace(b"\0", b"\n").decode("latin-1")
>>
    if string.contains(t, "A32MARKER_7q3z") { print("V=SEEN") } else { print("V=NONE") }
}'

echo "== file-reading routes: control reads environ file, blocked_read must withhold"
for eng in vm tw; do
  file_route file     "$eng" "$P_FILE"
  file_route io       "$eng" "$P_IO"
  file_route dotenv   "$eng" "$P_DOTENV"
  file_route py_open  "$eng" "$P_PYOPEN"
done

# Over-block control: with NO env policy, the environ file is readable (the
# gate must be scoped to env-restricted configs, not an unconditional block).
echo "== scope control: no env policy leaves the environ file readable"
for eng in vm tw; do
  d="$WORK/scope_$eng"; cfg "$d" control
  r="$(run "$eng" "$d" "$P_FILE")"
  if seen "$r"; then ok "scope/$eng" "no env policy -> environ file still readable (not over-blocked)"
  else skip "scope/$eng" "control did not see the marker: ${r#*|}"; fi
done

# env.get of a DIFFERENT, non-blocked var still works under blocked_read -- the
# gate withholds the environ FILE, not env reads in general.
echo "== non-regression: a non-blocked env var is still readable under blocked_read"
export NAAB_A32_OTHER="OTHER_ok"
for eng in vm tw; do
  d="$WORK/other_$eng"; cfg "$d" blocked
  r="$(run "$eng" "$d" 'use env
main {
    let v = env.get("NAAB_A32_OTHER") ?? "none"
    if v == "OTHER_ok" { print("V=SEEN") } else { print("V=NONE") }
}')"
  if seen "$r"; then ok "other/$eng" "a non-blocked var still reads under blocked_read"
  else bad "other/$eng" "blocked_read over-blocked an unrelated env var: ${r#*|}"; fi
done

# _naab_saved_env: the scrub's own save dict must not hand the value back.
# Corroboration (positive control that the scrub is active and the engine ran):
# os.environ of the blocked var returns NONE in the same program.
echo "== embedded Python: scrub save dict is not readable from the block"
for eng in vm tw; do
  d="$WORK/saved_$eng"; cfg "$d" blocked
  r="$(run "$eng" "$d" 'use string
main {
    let oe = <<python
import os
os.environ.get("NAAB_A32_MARK", "NONE")
>>
    let sv = <<python
str(globals().get("_naab_saved_env", "NO_NAME"))
>>
    if string.contains(oe, "A32MARKER_7q3z") { print("SCRUB=OFF") } else { print("SCRUB=ON") }
    if string.contains(sv, "A32MARKER_7q3z") { print("V=SEEN") } else { print("V=HIDDEN") }
}')"
  body="${r#*|}"
  if ! case "$body" in *"SCRUB=ON"*) true;; *) false;; esac; then
    skip "saved/$eng" "scrub not active (control): $body"; continue
  fi
  if seen "$r"; then bad "saved/$eng" "_naab_saved_env handed the value back: $body"
  else ok "saved/$eng" "scrub active and _naab_saved_env withholds the value"; fi
done

# NON-REGRESSION arm for the save dict's ERROR path -- not a revert-check arm.
# A block that raises leaves through the executor's error exits, which release
# the GIL before returning; the hidden dict must be put back there while the GIL
# is still held. An earlier draft did it in a destructor, after the release, and
# a worker thread segfaulted under Python 3.12. That crash is NOT reproducible
# here: it needs Python >= 3.12 on a worker thread, and it is covered by the
# unit test PolyglotAsyncTest.PythonException, which CI builds against 3.12.
# What this arm checks, on both engines: the raising block still returns control
# to the program, and no later block reads the value back. It passes on a
# pre-fix build too -- there, the error path never restores the environment, so
# a later block has nothing to scrub -- so it guards the new code's error path,
# not the fix itself.
echo "== embedded Python: a raising block, then the save dict is still not readable"
for eng in vm tw; do
  d="$WORK/saved_err_$eng"; cfg "$d" blocked
  r="$(run "$eng" "$d" 'use string
main {
    try {
        let x = <<python
raise ValueError("a32 error path")
>>
    } catch (e) {
        print("CAUGHT")
    }
    let sv = <<python
str(globals().get("_naab_saved_env", "NO_NAME"))
>>
    if string.contains(sv, "A32MARKER_7q3z") { print("V=SEEN") } else { print("V=HIDDEN") }
}')"
  body="${r#*|}"
  if ! case "$body" in *CAUGHT*) true;; *) false;; esac; then
    bad "saved_err/$eng" "the raising block did not return control to the program: ${r%%|*} $body"
  elif seen "$r"; then bad "saved_err/$eng" "_naab_saved_env handed the value back after an error path: $body"
  else ok "saved_err/$eng" "raising block caught; a later block still cannot read the save dict"; fi
done

# Documented CONTRA-013 limit: a <<sh>> child reads its parent's environ file;
# path policy does not reach a separate process. This arm asserts the route is
# STILL open so it flips (and forces an A32 update) if someone closes it. The
# remedy is capabilities.shell.enabled:false or a lease without SHELL_EXEC.
echo "== documented limit (CONTRA-013): <<sh>> child reads /proc/PPID/environ"
if command -v sh >/dev/null 2>&1; then
  d="$WORK/sh_limit"; cfg "$d" blocked
  r="$(run vm "$d" 'use string
main {
    let t = <<sh
tr "\0" "\n" < /proc/$PPID/environ | grep NAAB_A32_MARK || true
>>
    if string.contains(t, "A32MARKER_7q3z") { print("V=SEEN") } else { print("V=NONE") }
}')"
  cd2="$WORK/sh_ctl"; cfg "$cd2" blocked
  c="$(run vm "$cd2" 'main {
    let t = <<sh
echo A32SH_OK
>>
    print(t)
}')"
  if ! case "${c#*|}" in *A32SH_OK*) true;; *) false;; esac; then
    skip "sh_limit" "shell executor not usable here: ${c#*|}"
  elif seen "$r"; then
    ok "sh_limit" "documented OPEN (CONTRA-013): <<sh>> reads the environ file; close with shell.enabled:false / no SHELL_EXEC"
  else
    bad "sh_limit" "route is no longer open -- update docs/open-investigations.md A32 and this arm: ${r#*|}"
  fi
else
  skip "sh_limit" "no sh on PATH"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP unmeasurable"
[ "$FAIL" -eq 0 ]
