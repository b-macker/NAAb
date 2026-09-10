#!/usr/bin/env bash
# ============================================================
# test_package_integrity.sh -- F38: the lockfile pin, and when it is checked
#
# Two defects in one mechanism, both in src/packages/package_manager.cpp.
#
# (1) VERIFIED AFTER EXTRACTION. The hash was computed before extraction and
#     compared after install() got its package back. In between, extractTarball()
#     had already run `remove_all(dest_dir)` and moved the new tree into its
#     place -- so a rejected download had ALREADY replaced the installed copy,
#     and the rejection then deleted what was left. A tampered tarball could not
#     be installed, but it could destroy the verified package it was meant to
#     replace. (It could not write outside the tree: GNU tar refuses `..`
#     members and strips a leading `/`, which is what bounds this to
#     destruction rather than arbitrary write.)
#
# (2) THE PIN NAMED THE WRONG ARTEFACT. install() wrote the lockfile entry from
#     the member last_download_hash_ -- read AFTER the transitive-dependency
#     loop, which calls install() recursively and overwrites it. Any package
#     with a github dependency was therefore pinned to its last dependency's
#     tarball. That fails closed, but it fails ALWAYS: the next legitimate
#     upgrade is reported as "this could indicate a supply chain attack", and
#     the message's own remedy was to delete naab.lock -- which turns off
#     integrity checking for every package in the project.
#
# Group A  the pin must name this package's own tarball
#          A-02 is the POSITIVE CONTROL: a leaf package with no dependencies
#          must be pinned correctly too, or A-01 would pass for a build that
#          records no hash at all.
# Group B  the check must run BEFORE extraction
#          B-01 positive control -- a matching pin must still install
#          B-02 the gate must fire on a mismatch (or B-03 proves nothing)
#          B-03 the installed copy must survive that rejection
#          B-04 negative control -- no lock entry is trust-on-first-use and
#               must still install, or the fix would read as "block everything"
# Group C  NAAB_PKG_API_BASE, the seam this test runs on, must stay loopback-only
#
# The stub is served over loopback http because the manager's URLs are
# https://api.github.com: reaching a real TLS endpoint under our control would
# need a CA installed in the system trust store, which is not something a test
# suite should do to the machine it runs on.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }

source "$REPO/tests/helpers/stub_platform.sh"
skip_if_no_stub_support

echo "=== F38: package integrity pin and its ordering ==="

if [ ! -x "$NAAB" ]; then
    skip "PI-00" "naab-lang not built -- UNMEASURABLE, not a pass"
    echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"
    exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    skip "PI-00" "python3 unavailable -- cannot serve the stub"
    echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"
    exit 0
fi

WDIR="$(mktemp -d)"
SERVE="$WDIR/serve"; mkdir -p "$SERVE"
STUB_PID=""
cleanup() {
    if [ -n "${STUB_PID:-}" ]; then kill "$STUB_PID" 2>/dev/null || true; fi
    rm -rf "$WDIR"
}
trap cleanup EXIT

# ---- fixtures -------------------------------------------------------------
# Built here, not committed: the tarball's bytes ARE the thing under test, so a
# committed fixture whose hash drifted from the manifest would start passing for
# the wrong reason.
mk_tarball() {  # $1=name $2=version $3=deps-toml $4=marker
    local name="$1" ver="$2" deps="$3" marker="$4"
    local stage="$WDIR/stage/testorg-$name-abc1234"
    rm -rf "$WDIR/stage"; mkdir -p "$stage/src"
    {
        echo '[package]'
        echo "name = \"$name\""
        echo "version = \"$ver\""
        echo ''
        echo '[exports]'
        echo 'main = "src/lib.naab"'
        [ -n "$deps" ] && printf '%s\n' "$deps"
    } > "$stage/naab.toml"
    echo "fn hello() { return \"$name-$ver-$marker\" }" > "$stage/src/lib.naab"
    tar czf "$SERVE/testorg-$name-$ver.tar.gz" -C "$WDIR/stage" "testorg-$name-abc1234"
}

# sha256sum is coreutils; macOS ships `shasum -a 256`. A missing tool must not
# silently produce an empty expected value that then "matches" nothing.
if command -v sha256sum >/dev/null 2>&1; then
    sha_of() { echo "sha256:$(sha256sum "$1" | cut -d' ' -f1)"; }
elif command -v shasum >/dev/null 2>&1; then
    sha_of() { echo "sha256:$(shasum -a 256 "$1" | cut -d' ' -f1)"; }
else
    skip "PI-00" "no sha256 tool -- cannot compute expected hashes"
    echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"
    exit 0
fi

DEPS_ON_CHILD='
[dependencies]
child = { github = "testorg/child", version = "1.0.0" }'

mk_tarball child  1.0.0 ""                "leaf"
mk_tarball parent 1.0.0 "$DEPS_ON_CHILD"  "orig"
mk_tarball solo   1.0.0 ""                "solo"

CHILD_SHA="$(sha_of "$SERVE/testorg-child-1.0.0.tar.gz")"
PARENT_SHA="$(sha_of "$SERVE/testorg-parent-1.0.0.tar.gz")"
SOLO_SHA="$(sha_of "$SERVE/testorg-solo-1.0.0.tar.gz")"

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
    skip "PI-00" "package stub did not start -- UNMEASURABLE"
    tail -3 "$WDIR/stub.log" 2>/dev/null
    echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"
    exit 0
fi

# GITHUB_TOKEN/GH_TOKEN would add an Authorization header aimed at a stub;
# http_proxy would send a loopback request through a proxy that cannot reach it.
run_install() {  # $@ = install args; runs in $PROJ
    ( cd "$PROJ" && env -u GITHUB_TOKEN -u GH_TOKEN \
        -u http_proxy -u HTTP_PROXY -u https_proxy -u HTTPS_PROXY \
        no_proxy="127.0.0.1,localhost" NO_PROXY="127.0.0.1,localhost" \
        HOME="$WDIR/home" \
        NAAB_PKG_API_BASE="http://127.0.0.1:$STUB_PORT" \
        "$NAAB" install "$@" 2>&1 )
}

new_project() {
    PROJ="$WDIR/proj-$1"
    rm -rf "$PROJ" "$WDIR/home"; mkdir -p "$PROJ" "$WDIR/home"
    printf '[package]\nname = "probe"\nversion = "0.1.0"\n' > "$PROJ/naab.toml"
}

lock_integrity() {  # $1=package name -> its integrity value, or empty
    awk -v want="$1" '
        /^\[\[package\]\]/ { inpkg=0 }
        $1=="name" && $3=="\""want"\"" { inpkg=1 }
        inpkg && $1=="integrity" { gsub(/"/,"",$3); print $3; exit }
    ' "$PROJ/naab.lock" 2>/dev/null
}

# ---- PI-00: instrument usability -----------------------------------------
# Without this every later "install failed" assertion is unfalsifiable: a stub
# that serves nothing, or an ignored NAAB_PKG_API_BASE, fails every install and
# each failure-shaped assertion would read as a pass.
new_project usability
OUT="$(run_install testorg/solo@1.0.0)"; RC=$?
if [ $RC -eq 0 ] && [ -f "$PROJ/naab_modules/solo/naab.toml" ]; then
    ok "PI-00" "stub reachable and a plain install completes"
else
    skip "PI-00" "install through the stub did not work -- UNMEASURABLE"
    echo "$OUT" | head -5
    echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"
    exit 0
fi

# ---- Group A: the pin must name this package's own tarball ---------------
echo "--- Group A: lockfile hash provenance"

if [ "$(lock_integrity solo)" = "$SOLO_SHA" ]; then
    ok "A-02" "POSITIVE CONTROL: a leaf package is pinned to its own tarball"
else
    bad "A-02" "leaf pin is $(lock_integrity solo), tarball is $SOLO_SHA"
fi

new_project pin
OUT="$(run_install testorg/parent@1.0.0)"; RC=$?
GOT="$(lock_integrity parent)"
if [ "$GOT" = "$PARENT_SHA" ]; then
    ok "A-01" "parent pinned to its own tarball, not its dependency's"
elif [ "$GOT" = "$CHILD_SHA" ]; then
    bad "A-01" "parent pinned to the CHILD tarball ($CHILD_SHA) -- hash clobbered by the dependency loop"
else
    bad "A-01" "parent pin is '$GOT', expected $PARENT_SHA (rc=$RC)"
fi
if [ "$(lock_integrity child)" = "$CHILD_SHA" ]; then
    ok "A-03" "the dependency is pinned to its own tarball"
else
    bad "A-03" "child pin is $(lock_integrity child), expected $CHILD_SHA"
fi

# ---- Group B: the check must run before extraction -----------------------
echo "--- Group B: verification order"

# B-01 positive control: a MATCHING pin must install.
new_project match
run_install testorg/solo@1.0.0 >/dev/null 2>&1
OUT="$(run_install testorg/solo@1.0.0)"; RC=$?
if [ $RC -eq 0 ]; then
    ok "B-01" "POSITIVE CONTROL: a matching pin still installs"
else
    bad "B-01" "a matching pin was rejected (rc=$RC): $(echo "$OUT" | head -2)"
fi

# B-02/B-03: the threat the pin exists for -- the SAME version, re-tagged to
# different bytes (a moved tag, a poisoned cache, a compromised mirror).
#
# Reaching the download path at all needs the installed copy to look stale:
# install() returns early with "already installed" when the on-disk naab.toml
# carries the requested version. Editing that one field is the whole contrivance;
# the request, the ref and the pin all stay at 1.0.0, so this asserts nothing
# about whether an explicit version BUMP should re-pin (it currently does not --
# the lock entry is keyed by name, which is pre-existing and out of scope here).
new_project order
run_install testorg/solo@1.0.0 >/dev/null 2>&1
echo "sentinel" > "$PROJ/naab_modules/solo/SENTINEL"
sed -i.bak 's/^version = "1.0.0"/version = "0.9.0"/' "$PROJ/naab_modules/solo/naab.toml"
rm -f "$PROJ/naab_modules/solo/naab.toml.bak"
mk_tarball solo 1.0.0 "" "TAMPERED"     # same name and ref, different bytes
OUT="$(run_install testorg/solo@1.0.0)"; RC=$?
if [ $RC -ne 0 ] && echo "$OUT" | grep -q "Integrity check failed"; then
    ok "B-02" "the gate fires on a mismatch (rc=$RC)"
else
    bad "B-02" "mismatch was not rejected (rc=$RC): $(echo "$OUT" | head -3)"
fi
if [ -f "$PROJ/naab_modules/solo/SENTINEL" ] && \
   grep -q 'TAMPERED' "$PROJ/naab_modules/solo/src/lib.naab" 2>/dev/null; then
    bad "B-03" "the tampered content was extracted over the installed copy"
elif [ -f "$PROJ/naab_modules/solo/SENTINEL" ]; then
    ok "B-03" "the installed copy survived the rejected download"
else
    bad "B-03" "the rejected download destroyed the installed copy"
fi

# B-04 negative control: trust-on-first-use must still work.
new_project tofu
OUT="$(run_install testorg/child@1.0.0)"; RC=$?
if [ $RC -eq 0 ] && [ "$(lock_integrity child)" = "$CHILD_SHA" ]; then
    ok "B-04" "NEGATIVE CONTROL: a first install with no pin records one and proceeds"
else
    bad "B-04" "first install did not proceed/record (rc=$RC, pin=$(lock_integrity child))"
fi

# ---- Group C: the seam must stay loopback-only ---------------------------
echo "--- Group C: NAAB_PKG_API_BASE scope"
CBASE_OUT="$( cd "$PROJ" && env -u GITHUB_TOKEN -u GH_TOKEN \
    -u http_proxy -u HTTP_PROXY -u https_proxy -u HTTPS_PROXY \
    HOME="$WDIR/home" NAAB_PKG_API_BASE="http://naab-pkg-probe.invalid" \
    timeout 20 "$NAAB" install testorg/child@9.9.9 2>&1 )"
if echo "$CBASE_OUT" | grep -q "NAAB_PKG_API_BASE ignored (loopback only)"; then
    ok "C-01" "a non-loopback override is refused and says so"
else
    bad "C-01" "no loopback warning: $(echo "$CBASE_OUT" | head -2)"
fi

echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ] || exit 1
exit 0
