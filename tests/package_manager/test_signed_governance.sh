#!/usr/bin/env bash
# ============================================================
# test_signed_governance.sh -- F39: the package manager broke its own signature
#
# applyPackageGovernance() and removePackageGovernance() rewrite the project's
# govern.json with nlohmann's dump(2). Nothing there knows about
# govern.json.sig, so the signature is left behind and every later run of the
# project is an INTEGRITY BLOCK (exit 3). Measured end to end: a script that
# ran at exit 0 before `naab-lang install` was blocked after it.
#
# THE WIDER HALF, which the report did not mention. removePackageGovernance()
# wrote UNCONDITIONALLY whenever govern.json had a governance_plugins key -- so
# `naab-lang remove <anything>` bricked a signed project even when the removed
# package had never contributed an entry and the config was semantically
# identical. dump(2) reorders keys and reindents, so the bytes differ and the
# signature fails on a change that changed nothing.
#
# AND THE REASON IT WAS SILENT. main.cpp ends package subcommands with _exit(0),
# which skips the C runtime's flush, so buffered stdout is discarded. On a
# terminal stdout is line-buffered and the output appears; through a pipe or
# into a file it is fully buffered and everything is lost -- including
# "Applied N governance rules", the only notice that a package had edited the
# config at all. Group D covers that, because a warning nobody can see in CI is
# not a warning.
#
# Group A  a signed config must survive an install that wants to change it
#          A-03 is the POSITIVE CONTROL: an UNSIGNED project must still get the
#          package's rules applied, or the fix reads as "never apply anything"
# Group B  a signed config must survive `remove`, and the no-op case must not
#          write at all (B-03 asserts the bytes are untouched)
# Group C  the refusal must not swallow the install: the package stays on disk
#          and pinned in naab.lock, and the exit code is non-zero
# Group D  package-manager stdout must survive a pipe
#
# Trust is isolated through tests/helpers/trust_setup.sh. Never point
# NAAB_TRUST_STORE_DIR at a real store: with any key installed an unsigned
# govern.json is an integrity block that no flag can escape.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }
done_report() { echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

source "$REPO/tests/helpers/stub_platform.sh"
skip_if_no_stub_support

echo "=== F39: package operations must not invalidate a signed govern.json ==="

if [ ! -x "$NAAB" ]; then
    skip "SG-00" "naab-lang not built -- UNMEASURABLE, not a pass"; done_report; exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    skip "SG-00" "python3 unavailable -- cannot serve the stub"; done_report; exit 0
fi

source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust

WDIR="$(mktemp -d)"
SERVE="$WDIR/serve"; mkdir -p "$SERVE"
STUB_PID=""
cleanup() {
    [ -n "${STUB_PID:-}" ] && kill "$STUB_PID" 2>/dev/null
    teardown_isolated_trust
    rm -rf "$WDIR"
}
trap cleanup EXIT

"$NAAB" --keygen "$WDIR/k.pem"      >/dev/null 2>&1
"$NAAB" --trust-key "$WDIR/k.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$WDIR/k.pem"

# ---- fixtures -------------------------------------------------------------
mk_pkg() {  # $1=name  $2=with-governance(yes/no)
    local name="$1" gov="$2"
    local top="$WDIR/stage/testorg-$name-abc1234"
    rm -rf "$WDIR/stage"; mkdir -p "$top/src"
    {
        echo '[package]'
        echo "name = \"$name\""
        echo 'version = "1.0.0"'
        if [ "$gov" = "yes" ]; then
            echo ''
            echo '[package.governance]'
            echo 'plugin_file = "governance/checks.naab"'
            echo 'rules_file = "governance/rules.json"'
        fi
        echo ''
        echo '[exports]'
        echo 'main = "src/lib.naab"'
    } > "$top/naab.toml"
    echo "fn hello() { return \"$name\" }" > "$top/src/lib.naab"
    # The governance/ directory ONLY for the governance fixture. readPackageInfo
    # auto-detects that directory even when naab.toml declares nothing, so a
    # package shipping one is a governance package whatever its manifest says --
    # creating it unconditionally made the "no governance" fixture a governance
    # package, and B-03 measured the wrong thing until a mutant exposed it.
    if [ "$gov" = "yes" ]; then
        mkdir -p "$top/governance"
        echo 'fn check() { return true }' > "$top/governance/checks.naab"
        echo '{ "rules": [ { "id": "pkg_rule", "description": "from the package" } ] }' \
            > "$top/governance/rules.json"
    fi
    tar czf "$SERVE/testorg-$name-1.0.0.tar.gz" -C "$WDIR/stage" "testorg-$name-abc1234"
}
mk_pkg govpkg yes
mk_pkg plain  no

# ---- stub -----------------------------------------------------------------
start_stub() {
    local i try
    for try in 1 2 3; do
        STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
        : > "$WDIR/stub.log"
        python3 "$REPO/tests/helpers/package_stub.py" "$STUB_PORT" "$SERVE" \
            > "$WDIR/stub.log" 2>&1 &
        STUB_PID=$!
        for i in $(seq 1 60); do
            grep -q READY "$WDIR/stub.log" 2>/dev/null && return 0
            kill -0 "$STUB_PID" 2>/dev/null || break
            sleep 0.5
        done
        kill -9 "$STUB_PID" 2>/dev/null || true; STUB_PID=""
    done
    return 1
}
if ! start_stub; then
    skip "SG-00" "package stub did not start -- UNMEASURABLE"
    tail -3 "$WDIR/stub.log" 2>/dev/null; done_report; exit 0
fi

run_pm() {  # runs a package subcommand inside $PROJ
    ( cd "$PROJ" && env -u GITHUB_TOKEN -u GH_TOKEN \
        -u http_proxy -u HTTP_PROXY -u https_proxy -u HTTPS_PROXY \
        no_proxy="127.0.0.1,localhost" NO_PROXY="127.0.0.1,localhost" \
        HOME="$WDIR/home" NAAB_PKG_API_BASE="http://127.0.0.1:$STUB_PORT" \
        "$NAAB" "$@" 2>&1 )
}
run_script() { ( cd "$PROJ" && "$NAAB" hello.naab >/dev/null 2>&1 ); }

new_project() {  # $1=tag  $2=sign(yes/no)  $3=extra govern.json body line
    PROJ="$WDIR/proj-$1"
    rm -rf "$PROJ" "$WDIR/home"; mkdir -p "$PROJ" "$WDIR/home"
    printf '[package]\nname = "probe"\nversion = "0.1.0"\n' > "$PROJ/naab.toml"
    {
        echo '{'
        echo '  "version": "4.0",'
        echo '  "mode": "enforce",'
        echo '  "security": { "sandbox_level": "elevated" }'"${3:+,}"
        [ -n "${3:-}" ] && echo "  $3"
        echo '}'
    } > "$PROJ/govern.json"
    echo 'main { print("script ran") }' > "$PROJ/hello.naab"
    if [ "$2" = "yes" ]; then
        ( cd "$PROJ" && "$NAAB" --sign-governance ) >/dev/null 2>&1
    fi
}

# ---- SG-00: instrument usability -----------------------------------------
# Signing must actually work and the signed project must run. Without this,
# every "the project still runs" assertion below is unfalsifiable -- and every
# "install refused" one would pass for a stub that serves nothing.
new_project usability yes
if [ -f "$PROJ/govern.json.sig" ] && run_script; then
    ok "SG-00" "a signed project runs, and the stub is reachable"
else
    skip "SG-00" "could not sign or run a baseline project -- UNMEASURABLE"
    done_report; exit 0
fi

# ---- Group A: install against a signed config ----------------------------
echo "--- Group A: install"
new_project signed-install yes
SIG_A="$(sha256sum "$PROJ/govern.json.sig" | cut -d' ' -f1)"
CFG_A="$(sha256sum "$PROJ/govern.json" | cut -d' ' -f1)"
OUT="$(run_pm install testorg/govpkg@1.0.0)"; RC=$?

if [ "$(sha256sum "$PROJ/govern.json" | cut -d' ' -f1)" = "$CFG_A" ] && \
   [ "$(sha256sum "$PROJ/govern.json.sig" | cut -d' ' -f1)" = "$SIG_A" ]; then
    ok "A-01" "the signed config and its signature are untouched"
else
    bad "A-01" "install rewrote a signed govern.json"
fi
if run_script; then
    ok "A-02" "the project still runs after the install"
else
    bad "A-02" "the project is blocked after the install (signature invalidated)"
fi

# POSITIVE CONTROL: without a signature the rules must still be applied, or
# A-01/A-02 would pass for a build that simply never touches govern.json.
new_project unsigned-install no
run_pm install testorg/govpkg@1.0.0 >/dev/null 2>&1
if grep -q '"_package"' "$PROJ/govern.json" 2>/dev/null && \
   grep -q 'govpkg.pkg_rule' "$PROJ/govern.json" 2>/dev/null; then
    ok "A-03" "POSITIVE CONTROL: an unsigned config still receives the rules"
else
    bad "A-03" "the package's rules were not applied to an unsigned config"
fi

# ---- Group B: remove against a signed config -----------------------------
echo "--- Group B: remove"
# B-01/B-02: the package that DID contribute an entry. Install unsigned, sign
# afterwards, then try to remove: the entry is there and removing it is a real
# change to a signed file.
new_project signed-remove no
run_pm install testorg/govpkg@1.0.0 >/dev/null 2>&1
( cd "$PROJ" && "$NAAB" --sign-governance ) >/dev/null 2>&1
CFG_B="$(sha256sum "$PROJ/govern.json" | cut -d' ' -f1)"
OUT="$(run_pm remove govpkg)"; RC=$?
if [ $RC -ne 0 ] && [ -d "$PROJ/naab_modules/govpkg" ]; then
    ok "B-01" "the removal is refused and nothing was deleted"
else
    bad "B-01" "removal proceeded against a signed config (rc=$RC)"
fi
if [ "$(sha256sum "$PROJ/govern.json" | cut -d' ' -f1)" = "$CFG_B" ] && run_script; then
    ok "B-02" "the signed config is untouched and the project still runs"
else
    bad "B-02" "remove rewrote a signed govern.json"
fi

# B-03: the no-op case. This package never contributed a governance entry, so
# there is nothing to remove -- and the file must not be rewritten AT ALL.
# Byte equality is the assertion: dump(2) reorders keys, so a semantically
# identical rewrite still breaks the signature.
new_project noop-remove yes '"governance_plugins": []'
run_pm install testorg/plain@1.0.0 >/dev/null 2>&1; INSTALL_RC=$?
CFG_C="$(sha256sum "$PROJ/govern.json" | cut -d' ' -f1)"
if [ $INSTALL_RC -ne 0 ]; then
    bad "B-03" "the no-op fixture never installed (rc=$INSTALL_RC) -- nothing below is measurable"
fi
OUT="$(run_pm remove plain)"; RC=$?
CFG_C_AFTER="$(sha256sum "$PROJ/govern.json" | cut -d' ' -f1)"
run_script; RUN_RC=$?
if [ $RC -eq 0 ] && [ "$CFG_C_AFTER" = "$CFG_C" ] && [ $RUN_RC -eq 0 ]; then
    ok "B-03" "removing a package with no governance entry rewrites nothing"
else
    # Name the half that failed. A three-way AND that reports only "it failed"
    # gets believed over the code, and sends the next person guessing.
    bad "B-03" "no-op removal: remove rc=$RC, config $([ "$CFG_C_AFTER" = "$CFG_C" ] && echo unchanged || echo REWRITTEN), later run rc=$RUN_RC"
    echo "$OUT" | head -3 | sed 's/^/        /'
fi

# ---- Group C: the refusal must not swallow the install -------------------
echo "--- Group C: the install is still complete"
new_project refusal-state yes
OUT="$(run_pm install testorg/govpkg@1.0.0)"; RC=$?
if [ $RC -ne 0 ]; then
    ok "C-01" "the refusal reaches the exit code"
else
    bad "C-01" "install exited 0 despite refusing to apply the rules"
fi
if [ -f "$PROJ/naab_modules/govpkg/naab.toml" ] && \
   grep -q 'name = "govpkg"' "$PROJ/naab.lock" 2>/dev/null; then
    ok "C-02" "the package is still installed and pinned in naab.lock"
else
    bad "C-02" "the refusal left the install half-done"
fi
if echo "$OUT" | grep -q "sign-governance"; then
    ok "C-03" "the message says how to proceed"
else
    bad "C-03" "no actionable message: $(echo "$OUT" | head -2)"
fi

# ---- Group D: the output has to survive a pipe ---------------------------
# _exit(0) skips the stdio flush, so a fully-buffered stdout was discarded.
# This is the delivery path for every message above, including the refusal.
echo "--- Group D: buffered output"
new_project pipe no
PIPED="$(run_pm install testorg/plain@1.0.0 | cat)"
if echo "$PIPED" | grep -q "Installed plain"; then
    ok "D-01" "package-manager stdout survives a pipe"
else
    bad "D-01" "stdout was discarded when not a terminal"
fi

done_report
[ $FAIL -eq 0 ] || exit 1
exit 0
