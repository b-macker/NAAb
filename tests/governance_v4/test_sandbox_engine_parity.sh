#!/usr/bin/env bash
# ============================================================
# test_sandbox_engine_parity.sh — govern.json sandbox settings must reach BOTH engines
#
# syncGovernanceToSandbox() and the sandbox_level rebuild lived inline inside
# `if (use_vm)` in main.cpp, so under --tree-walk every govern.json sandbox
# setting was silently inert:
#
#   security.sandbox_level              never applied
#   capabilities.shell.enabled: false   SYS_EXEC left in place
#   capabilities.network.enabled: false NET_CONNECT left in place
#   enforce-mode fail-closed upgrade    never happened
#
# A govern.json that reads as locked down enforced nothing on a supported engine.
# Passing --sandbox-level on the CLI worked on both, which is why the gap was
# invisible to anyone testing that way.
#
# The VM still has an equivalent block inline rather than calling the shared
# helper — deliberately, because swapping the default engine's working code
# carries more risk than the duplication does. This file is what makes that safe:
# the #115 taint divergence was not caused by duplication, it was caused by
# nothing testing that the two paths agreed.
#
# Every case asserts BOTH engines agree AND that the expected verdict is the one
# they agree on — two engines that both fail to enforce agree perfectly.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/sandbox-parity-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

# POSIX-only, same as test_subprocess_containment.sh. Every probe here asserts a
# filesystem decision about an ABSOLUTE path, and that is not portable to the
# Windows job: naab-lang is a native binary running under an MSYS2 harness, so an
# MSYS path like /etc/hostname or /tmp/... is not a path it can open. The read
# then fails for file-not-found rather than sandbox denial, which is
# indistinguishable from a block — and the "elevated must ALLOW" control fails,
# correctly reporting that the probe cannot tell the two apart. That is the same
# trap that broke test_nonformation_proof.sh on Windows in #104.
#
# The engine-parity guard this file provides is not weakened much: build-linux
# and Build & Test both run it, so a VM/tree-walker divergence still fails CI.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo ""
        echo "  test_sandbox_engine_parity.sh: SKIPPED (POSIX-only —"
        echo "    absolute-path probes are not meaningful for a native binary under MSYS2)"
        exit 0 ;;
esac
if [ -n "${WINDIR:-}" ]; then
    echo "  test_sandbox_engine_parity.sh: SKIPPED (POSIX-only)"
    exit 0
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"

# Probes. READ_* uses an absolute path, denied below "elevated"; shell= exercises
# the capability sync specifically (elevated permits exec, so only the sync can
# remove SYS_EXEC).
cat > "$TEST_TMP/read_sync.naab" << 'EOF'
use file
main {
    try { let c = file.read("/etc/hostname") print("READ_ALLOWED") }
    catch (e) { print("READ_BLOCKED") }
}
EOF
cat > "$TEST_TMP/read_async.naab" << 'EOF'
use file
async fn probe() {
    try { let c = file.read("/etc/hostname") print("READ_ALLOWED") }
    catch (e) { print("READ_BLOCKED") }
    return 1
}
main { let f = probe() let v = await f }
EOF
cat > "$TEST_TMP/shell.naab" << 'EOF'
use process
main {
    try { let r = process.run("echo", ["hi"]) print("shell=ALLOWED") }
    catch (e) { print("shell=BLOCKED") }
}
EOF

write_govern() { printf '%s\n' "$2" > "$TEST_TMP/govern.json"
    (cd "$TEST_TMP" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

# The read probe prints READ_BLOCKED (underscore) and the shell probe
# shell=BLOCKED (equals). A pattern covering only one silently reports <none>
# for the other and makes a working engine look untested.
verdict() {  # $1=script $2=engine-flag
    (cd "$TEST_TMP" && timeout 30s "$NAAB" ${2:-} "$1.naab" 2>&1) \
        | grep -oE "READ_(ALLOWED|BLOCKED)|shell=(ALLOWED|BLOCKED)" | head -1
}

check() {  # $1=id $2=script $3=expected $4=description
    local vm tw
    vm=$(verdict "$2" ""); tw=$(verdict "$2" "--tree-walk")
    if [ -z "$vm" ] || [ -z "$tw" ]; then
        fail "$1" "probe produced no verdict — nothing was measured" "vm='${vm:-}' tree-walk='${tw:-}'"
    elif [ "$vm" != "$tw" ]; then
        fail "$1" "engines disagree: $4" "VM=$vm tree-walk=$tw"
    elif [ "$vm" != "$3" ]; then
        fail "$1" "both engines agree on the WRONG verdict: $4" "expected $3, both gave $vm"
    else
        pass "$1" "$4 (both $vm)"
    fi
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  govern.json sandbox settings must reach BOTH engines        |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

write_govern x '{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "standard" } }'
check "SBX-01" read_sync  READ_BLOCKED "govern.json sandbox_level=standard denies an absolute read"
check "SBX-02" read_async READ_BLOCKED "async fn inherits the sandbox (thread_local, fails open when unset)"

write_govern x '{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" } }'
check "SBX-03" read_sync  READ_ALLOWED "control: elevated permits it, so SBX-01 is not blocking unconditionally"

write_govern x '{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": { "enabled": false } } }'
check "SBX-04" shell shell=BLOCKED "capabilities.shell.enabled=false removes SYS_EXEC even at elevated"

write_govern x '{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" } }'
check "SBX-05" shell shell=ALLOWED "control: without the capability restriction exec is permitted"

write_govern x '{ "version": "5.0", "mode": "enforce" }'
check "SBX-06" read_sync READ_BLOCKED "enforce mode with no level set upgrades to standard (fail-closed)"

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}Failures:${NC}$FAILURES"
    exit 1
fi
exit 0
