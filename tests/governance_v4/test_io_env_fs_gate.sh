#!/usr/bin/env bash
# ============================================================
# test_io_env_fs_gate.sh — io, env and path were outside the filesystem gate
#
# THE DEFECT
#
# GovernanceEngine::filesystemAccessMode() classified file, csv and log, and
# returned "" for everything else. Both engines path-check only when it returns
# non-empty, so io.read_file / io.write_file / io.exists / io.list_dir and
# env.load_dotenv reached the disk with no capabilities.filesystem.blocked_paths
# or allowed_paths check at all. Each of those implementations calls
# Sandbox::canRead / canWrite and stops there, and the sandbox is an allowlist
# with no concept of blocked_paths — so the project path policy was simply not
# in the path.
#
# Measured on fb1e4bd, filesystem mode "write" with blocked_paths ["vault/"]:
# file.read("vault/secret.txt") exited 3, io.read_file("vault/secret.txt")
# exited 0 and returned the contents, on BOTH engines. The sharpest case is
# self-protection: addGovernanceProtectedPaths() puts govern.json in
# blocked_paths on every load, and io.write_file("govern.json", ...) OVERWROTE
# the policy file in its own run while the dashboard printed
# "Governance: PASS (0 violations)".
#
# Note "none" mode already blocked these, but by the SANDBOX (exit 1, catchable)
# rather than by governance (exit 3) — which is why the gap survived a config
# that switches the filesystem off entirely and only shows up once a project
# enables the filesystem and restricts WHICH paths.
#
# THE FIX
#
# io and env join file/csv/log in filesystemAccessMode(), the one classifier
# both vm.cpp and call_dispatch.cpp consult. io is an explicit allowlist of its
# four file entry points and deliberately has NO write-by-default fallback: io
# also carries the console surface, and io.write / io.output / io.println name
# no file.
#
# env.load_dotenv() with no argument defaults to ".env". The engine-side gate
# path-checks the first argument, and there is no first argument to check, so
# that one default is resolved and checked inside env_impl.cpp where it lives.
#
#   IE-01  CALIBRATION: file.read on the blocked path is blocked, and runs
#          when nothing is blocked. Without this the rest measures nothing
#   IE-02  io.read_file is blocked by blocked_paths, with its permit control
#   IE-03  io.write_file is blocked by blocked_paths, with its permit control
#   IE-04  io.exists and io.list_dir are classified READ and gated
#   IE-05  both directions hold under --tree-walk (two independent gate
#          implementations; fixing one would leave the other open)
#   IE-06  NEGATIVE CONTROL, load-bearing: the CONSOLE surface is untouched.
#          io.write / io.println / io.output still work under filesystem mode
#          "none". Without this arm a blanket deny on module=="io" passes every
#          other arm while breaking every print statement in the language
#   IE-07  govern.json self-protection now holds against io.write_file, and the
#          file is still on disk unmodified afterwards
#   IE-08  env.load_dotenv(path) is gated, with its permit control
#   IE-09  env.load_dotenv() with NO argument gates the resolved ".env"
#          default, with its permit control
#   IE-10  NEGATIVE CONTROL: env.parse_env_file takes content, not a path, and
#          must not be gated
#   IE-11  path.exists and path.resolve are gated (register row A10, same
#          function, same mechanism — folded in rather than left behind in a
#          commit that rewrites the classifier)
#   IE-12  NEGATIVE CONTROL: the LEXICAL path functions are untouched.
#          path.join / dirname / basename / normalize manipulate a string and
#          open nothing; gating them would refuse path arithmetic on names the
#          program is allowed to compute
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

# Every govern.json below is UNSIGNED. With any key in the ambient trust store
# an unsigned config is an INTEGRITY BLOCK at exit 3 -- the same exit code this
# suite reads as "the filesystem gate fired". Without isolation every gated arm
# would pass for the wrong reason on a developer machine that has ever run
# --trust-key, and only the permit controls would notice. Repoint the store.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/naab-io-env-gate-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
cleanup() { rm -rf "$TEST_TMP"; teardown_isolated_trust; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"
W="$TEST_TMP/w"

# Rebuilt per run so a write from a previous arm cannot leak into the next.
reset_workspace() {
    rm -rf "$W"; mkdir -p "$W/vault"
    printf 'TOP SECRET\n' > "$W/vault/secret.txt"
    printf 'SECRET_TOKEN=abc123\n' > "$W/vault/secret.env"
}

# $1=config json  $2=program  $3=extra CLI args -> "<rc>|<ran|blocked>"
run() {
    reset_workspace
    printf '%s\n' "$1" > "$W/govern.json"
    printf '%s\n' "$2" > "$W/t.naab"
    local o rc
    o=$(cd "$W" && timeout 60s "$NAAB" ${3:-} t.naab 2>&1); rc=$?
    if echo "$o" | grep -q "MARKER"; then echo "$rc|ran"; else echo "$rc|blocked"; fi
}

# filesystem ENABLED, one directory explicitly blocked. This is the shape the
# defect lived in: "none" was already stopped by the sandbox.
BLOCK='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "write", "blocked_paths": ["vault/"] } } }'
# Byte-identical minus the blocked_paths key -> the permit control.
PERMIT='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "write" } } }'
NONE='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "none" } } }'

# $1=id $2=label $3=program $4=engine args
# Asserts: blocked under BLOCK at exit 3, and permitted under PERMIT.
assert_gated() {
    local id="$1" label="$2" prog="$3" eng="${4:-}"
    local b p
    b=$(run "$BLOCK" "$prog" "$eng")
    p=$(run "$PERMIT" "$prog" "$eng")
    if [ "${b#*|}" != "blocked" ]; then
        fail "$id" "$label" "expected a governance block, got $b"
    elif [ "${b%%|*}" != "3" ]; then
        fail "$id" "$label" "blocked but not by HARD governance (exit ${b%%|*}, expected 3)"
    elif [ "${p#*|}" != "ran" ]; then
        fail "$id" "$label" "PERMIT CONTROL failed: blocked with nothing in blocked_paths ($p)"
    else
        pass "$id" "$label"
    fi
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  io, env and path were outside the filesystem gate            |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

P_FILE_READ='use file
main { let x = file.read("vault/secret.txt") print("MARKER") }'
P_IO_READ='use io
main { let x = io.read_file("vault/secret.txt") print("MARKER") }'
P_IO_WRITE='use io
main { io.write_file("vault/new.txt", "x") print("MARKER") }'
P_IO_EXISTS='use io
main { let x = io.exists("vault/secret.txt") print("MARKER") }'
P_IO_LIST='use io
main { let x = io.list_dir("vault") print("MARKER") }'
P_ENV_DOTENV='use env
main { let x = env.load_dotenv("vault/secret.env") print("MARKER") }'

echo "IE-01: calibration — the gate can fire at all"
assert_gated "IE-01" "file.read on the blocked path (VM)" "$P_FILE_READ" ""

echo ""
echo "IE-02..04: the io file surface"
assert_gated "IE-02" "io.read_file is gated (VM)"  "$P_IO_READ"   ""
assert_gated "IE-03" "io.write_file is gated (VM)" "$P_IO_WRITE"  ""
assert_gated "IE-04a" "io.exists is gated (VM)"    "$P_IO_EXISTS" ""
assert_gated "IE-04b" "io.list_dir is gated (VM)"  "$P_IO_LIST"   ""

echo ""
echo "IE-05: engine parity — the same gate exists twice"
assert_gated "IE-05a" "io.read_file is gated (--tree-walk)"  "$P_IO_READ"  "--tree-walk"
assert_gated "IE-05b" "io.write_file is gated (--tree-walk)" "$P_IO_WRITE" "--tree-walk"
assert_gated "IE-05c" "io.exists is gated (--tree-walk)"     "$P_IO_EXISTS" "--tree-walk"
assert_gated "IE-05d" "io.list_dir is gated (--tree-walk)"   "$P_IO_LIST"  "--tree-walk"

echo ""
echo "IE-06: NEGATIVE CONTROL — the console surface is not a filesystem surface"
P_CONSOLE='use io
main { io.write("a ") io.output("b ") io.println("MARKER") }'
for eng in "" "--tree-walk"; do
    r=$(run "$NONE" "$P_CONSOLE" "$eng")
    label="io.write/output/println still work under filesystem \"none\" (${eng:-vm})"
    if [ "${r#*|}" = "ran" ]; then
        pass "IE-06${eng:+t}" "$label"
    else
        fail "IE-06${eng:+t}" "$label" "console I/O was caught by the filesystem gate ($r)"
    fi
done

echo ""
echo "IE-07: govern.json self-protection"
reset_workspace
printf '%s\n' "$PERMIT" > "$W/govern.json"
cat > "$W/t.naab" <<'NAAB_EOF'
use io
main { io.write_file("govern.json", "{\"pwned\":true}") print("MARKER") }
NAAB_EOF
out=$(cd "$W" && timeout 60s "$NAAB" t.naab 2>&1); rc=$?
if echo "$out" | grep -q "MARKER"; then
    fail "IE-07" "io.write_file cannot overwrite govern.json" "the write ran (exit $rc)"
elif [ "$rc" != "3" ]; then
    fail "IE-07" "io.write_file cannot overwrite govern.json" "blocked, but exit $rc not 3"
elif grep -q "pwned" "$W/govern.json"; then
    fail "IE-07" "io.write_file cannot overwrite govern.json" "blocked, but the file was modified anyway"
else
    pass "IE-07" "io.write_file cannot overwrite govern.json"
fi

echo ""
echo "IE-08: env.load_dotenv with an explicit path"
assert_gated "IE-08a" "env.load_dotenv(path) is gated (VM)"          "$P_ENV_DOTENV" ""
assert_gated "IE-08b" "env.load_dotenv(path) is gated (--tree-walk)" "$P_ENV_DOTENV" "--tree-walk"

echo ""
echo "IE-09: env.load_dotenv() with NO argument — the resolved \".env\" default"
# The engine gate checks argument 0. There is no argument 0 here, so this arm
# fails for a fix applied only to filesystemAccessMode().
DOTBLOCK='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "write", "blocked_paths": [".env"] } } }'
P_DOTENV_DEFAULT='use env
main { let x = env.load_dotenv() print("MARKER") }'
for eng in "" "--tree-walk"; do
    reset_workspace
    printf 'K=v\n' > "$W/.env"
    printf '%s\n' "$DOTBLOCK" > "$W/govern.json"
    printf '%s\n' "$P_DOTENV_DEFAULT" > "$W/t.naab"
    o=$(cd "$W" && timeout 60s "$NAAB" ${eng:-} t.naab 2>&1); rc=$?
    blocked=$(echo "$o" | grep -q MARKER && echo no || echo yes)

    reset_workspace
    printf 'K=v\n' > "$W/.env"
    printf '%s\n' "$PERMIT" > "$W/govern.json"
    printf '%s\n' "$P_DOTENV_DEFAULT" > "$W/t.naab"
    po=$(cd "$W" && timeout 60s "$NAAB" ${eng:-} t.naab 2>&1); prc=$?
    permitted=$(echo "$po" | grep -q MARKER && echo yes || echo no)

    label="env.load_dotenv() gates the \".env\" default (${eng:-vm})"
    if [ "$blocked" != "yes" ]; then
        fail "IE-09${eng:+t}" "$label" "the default path was read unchecked (exit $rc)"
    elif [ "$rc" != "3" ]; then
        fail "IE-09${eng:+t}" "$label" "blocked, but exit $rc not 3"
    elif [ "$permitted" != "yes" ]; then
        fail "IE-09${eng:+t}" "$label" "PERMIT CONTROL failed: blocked with nothing in blocked_paths (exit $prc)"
    else
        pass "IE-09${eng:+t}" "$label"
    fi
done

echo ""
echo "IE-10: NEGATIVE CONTROL — parse_env_file takes content, not a path"
P_PARSE='use env
main { let x = env.parse_env_file("A=1") print("MARKER") }'
r=$(run "$NONE" "$P_PARSE" "")
if [ "${r#*|}" = "ran" ]; then
    pass "IE-10" "env.parse_env_file is not gated as a file access"
else
    fail "IE-10" "env.parse_env_file is not gated as a file access" "a pure string function was caught by the filesystem gate ($r)"
fi

echo ""
echo "IE-11: path.exists and path.resolve (A10)"
# path.exists stats the filesystem; path.resolve calls fs::canonical and so
# discloses the symlink target. Both are disclosure, not read-or-write, which
# is why A10 sat below F34 in severity — but both ignored the policy entirely.
P_PATH_EXISTS='use path
main { let x = path.exists("vault/secret.txt") print("MARKER") }'
P_PATH_RESOLVE='use path
main { let x = path.resolve("vault/secret.txt") print("MARKER") }'
assert_gated "IE-11a" "path.exists is gated (VM)"           "$P_PATH_EXISTS"  ""
assert_gated "IE-11b" "path.exists is gated (--tree-walk)"  "$P_PATH_EXISTS"  "--tree-walk"
assert_gated "IE-11c" "path.resolve is gated (VM)"          "$P_PATH_RESOLVE" ""
assert_gated "IE-11d" "path.resolve is gated (--tree-walk)" "$P_PATH_RESOLVE" "--tree-walk"

echo ""
echo "IE-12: NEGATIVE CONTROL — lexical path functions open nothing"
# Under filesystem "none", which is the hardest case: these must still work.
# Without this arm a blanket deny on module=="path" passes IE-11 entirely.
P_PATH_LEXICAL='use path
main {
  let a = path.join("vault", "secret.txt")
  let b = path.dirname(a)
  let c = path.basename(a)
  let d = path.normalize(a)
  print("MARKER")
}'
for eng in "" "--tree-walk"; do
    r=$(run "$NONE" "$P_PATH_LEXICAL" "$eng")
    label="path.join/dirname/basename/normalize still work under filesystem \"none\" (${eng:-vm})"
    if [ "${r#*|}" = "ran" ]; then
        pass "IE-12${eng:+t}" "$label"
    else
        fail "IE-12${eng:+t}" "$label" "a lexical string function was caught by the filesystem gate ($r)"
    fi
done

echo ""
echo -e "${CYAN}--------------------------------------------------------------${NC}"
echo -e "  Passed: ${GREEN}${PASS_COUNT}${NC}   Failed: ${RED}${FAIL_COUNT}${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}FAILURES:${NC}${FAILURES}"
    echo ""
    exit 1
fi
echo -e "  ${GREEN}ALL PASSED${NC}"
echo ""
exit 0
