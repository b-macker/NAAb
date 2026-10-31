#!/usr/bin/env bash
# test_version_consistency.sh -- every file that declares NAAb's version agrees
#                                with CMakeLists.txt, and scripts/bump-version.sh
#                                reaches each one
#
# Before 2.0.0 the version was written by hand in a dozen places and they had
# drifted apart: CMake and config.h said 0.10.0, naab.toml 0.8.1,
# SECURITY_GUIDE.md and the VS Code extension 0.9.0, the bug template v0.8.1,
# SECURITY.md listed 1.0.x as current, the newest tag was v1.8.0 -- while
# bump-version.sh, which replaced only text equal to the CMake version,
# reported every one of them updated.
#
#   VC-01  every declaration equals CMakeLists.txt's VERSION
#   VC-02  naab-lang --version and naab-gov --version print it
#   VC-03  CHANGELOG.md has a section for it
#   VC-04  naab-gov and the C API take it from config.h (no literal of their own)
#   VC-05  CONTROL: a planted drift in a copy of the files is reported
#   VC-06  bump-version.sh on a copy sets every declaration to a test version,
#          and the checker then passes there
#   VC-07  bump-version.sh fails, not skips, when a declaration is missing
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
GOV="${NAAB_GOV:-$REPO/build/naab-gov}"
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_vc_XXXXXX"); trap 'rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -12 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));   echo "  UNMEASURABLE [$1] $2"; }

echo "=== one NAAb version everywhere ==="

FILES="CMakeLists.txt include/naab/config.h naab.toml pyproject.toml bindings/rust/Cargo.toml
DEPENDENCIES.lock vscode-naab/package.json vscode-naab/package-lock.json docs/SECURITY_GUIDE.md
.github/ISSUE_TEMPLATE/bug_report.yml SECURITY.md CHANGELOG.md scripts/bump-version.sh"

cmake_version() {
    sed -n '/project(naab_lang/,/)/ s/^[[:space:]]*VERSION \([^[:space:])]*\).*/\1/p' "$1/CMakeLists.txt"
}

# check_tree ROOT VERSION -> one line per declaration that is missing or differs
check_tree() {
    local r="$1" v="$2" minor="${2%.*}" got
    want() {  # want LABEL FOUND
        if [ -z "$2" ]; then echo "$1: no declaration found"
        elif [ "$2" != "$v" ]; then echo "$1: $2 (expected $v)"; fi
    }
    want "include/naab/config.h" "$(sed -n 's/^#define NAAB_VERSION_STRING "\(.*\)"$/\1/p' "$r/include/naab/config.h")"
    want "naab.toml" "$(sed -n '0,/^version = /s/^version = "\(.*\)"$/\1/p' "$r/naab.toml")"
    want "pyproject.toml" "$(sed -n '0,/^version = /s/^version = "\(.*\)"$/\1/p' "$r/pyproject.toml")"
    want "bindings/rust/Cargo.toml" "$(sed -n '0,/^version = /s/^version = "\(.*\)"$/\1/p' "$r/bindings/rust/Cargo.toml")"
    want "DEPENDENCIES.lock" "$(sed -n 's/^naab_version: //p' "$r/DEPENDENCIES.lock")"
    want "vscode-naab/package.json" "$(sed -n '0,/"version": /s/.*"version": "\([^"]*\)".*/\1/p' "$r/vscode-naab/package.json")"
    got=$(sed -n '1,12 s/^\(  \|      \)"version": "\([^"]*\)".*/\2/p' "$r/vscode-naab/package-lock.json" | sort -u | tr '\n' ' ' | sed 's/ $//')
    want "vscode-naab/package-lock.json" "$got"
    want "docs/SECURITY_GUIDE.md" "$(sed -n 's/^\*\*Version:\*\* \([^ ]*\).*/\1/p' "$r/docs/SECURITY_GUIDE.md")"
    want ".github/ISSUE_TEMPLATE/bug_report.yml" "$(sed -n 's/.*placeholder: "v\([0-9][^"]*\)".*/\1/p' "$r/.github/ISSUE_TEMPLATE/bug_report.yml")"
    got=$(sed -n 's/^| \([0-9][0-9.]*\)\.x *| :white_check_mark: | Yes (current).*/\1/p' "$r/SECURITY.md")
    if [ -z "$got" ]; then echo "SECURITY.md: no current row found"
    elif [ "$got" != "$minor" ]; then echo "SECURITY.md: current row $got.x (expected $minor.x)"; fi
}

copy_tree() {  # copy_tree DEST: the files that declare the version, at their paths
    local f
    for f in $FILES; do mkdir -p "$1/$(dirname "$f")"; cp "$REPO/$f" "$1/$f"; done
}

V=$(cmake_version "$REPO")
if [ -z "$V" ]; then echo "  CMakeLists.txt declares no version: UNMEASURABLE, not a pass"; exit 1; fi

# ---- VC-01 ----------------------------------------------------------------------
D=$(check_tree "$REPO" "$V")
if [ -z "$D" ]; then pass VC-01 "every declaration says $V"
else fail VC-01 "declarations disagree with CMakeLists.txt ($V)" "$D"; fi

# ---- VC-02 ----------------------------------------------------------------------
if [ -x "$NAAB" ] && [ -x "$GOV" ]; then
    NV=$("$NAAB" --version 2>&1); GV=$("$GOV" --version 2>&1)
    if grep -qF "naab-lang $V" <<<"$NV" && grep -qF "naab-gov $V" <<<"$GV"; then
        pass VC-02 "naab-lang and naab-gov print $V"
    else fail VC-02 "a binary prints another version" "naab-lang: $NV"$'\n'"naab-gov: $GV"; fi
else unm VC-02 "naab-lang or naab-gov not built"; fi

# ---- VC-03 ----------------------------------------------------------------------
if grep -q "^## \[${V//./\\.}\] - " "$REPO/CHANGELOG.md"; then pass VC-03 "CHANGELOG.md has a [$V] section"
else fail VC-03 "CHANGELOG.md has no [$V] section"; fi

# ---- VC-04 ----------------------------------------------------------------------
LIT=$(grep -nE 'NAAB_GOV_VERSION[^;]*"[0-9]+\.[0-9]+\.[0-9]+' "$REPO/src/cli/gov_main.cpp" "$REPO/src/api/governance_c_api.cpp" || true)
if [ -z "$LIT" ] && grep -q 'NAAB_GOV_VERSION NAAB_VERSION_STRING' "$REPO/src/cli/gov_main.cpp" \
   && grep -q 'NAAB_GOV_VERSION = NAAB_VERSION_STRING' "$REPO/src/api/governance_c_api.cpp"; then
    pass VC-04 "naab-gov and the C API take the version from config.h"
else fail VC-04 "a second version literal is back" "$LIT"; fi

# ---- VC-05 control: the checker sees a drift ---------------------------------------
copy_tree "$W/drift"
sed -i 's/^naab_version: .*/naab_version: 0.0.1/' "$W/drift/DEPENDENCIES.lock"
D=$(check_tree "$W/drift" "$V")
if grep -q '^DEPENDENCIES.lock: 0.0.1' <<<"$D" && [ "$(wc -l <<<"$D")" -eq 1 ]; then
    pass VC-05 "CONTROL: a planted drift is reported, and only it"
else fail VC-05 "the checker did not report exactly the planted drift" "$D"; fi

# ---- VC-06 the bump script reaches every declaration -------------------------------
copy_tree "$W/bump"
OUT=$(NAAB_VERSION_ROOT="$W/bump" bash "$W/bump/scripts/bump-version.sh" 9.8.7 --yes 2>&1); RC=$?
D=$(check_tree "$W/bump" "9.8.7")
if [ "$RC" -eq 0 ] && [ "$(cmake_version "$W/bump")" = "9.8.7" ] && [ -z "$D" ] \
   && grep -q '^## \[9\.8\.7\] - ' "$W/bump/CHANGELOG.md"; then
    pass VC-06 "bump-version.sh 9.8.7 sets every declaration and adds the CHANGELOG section"
else fail VC-06 "the bump script missed a declaration (rc=$RC)" "$D"$'\n'"$OUT"; fi

# ---- VC-07 a missing declaration fails the script -----------------------------------
copy_tree "$W/missing"
sed -i '/^naab_version: /d' "$W/missing/DEPENDENCIES.lock"
OUT=$(NAAB_VERSION_ROOT="$W/missing" bash "$W/missing/scripts/bump-version.sh" 9.8.7 --yes 2>&1); RC=$?
if [ "$RC" -ne 0 ] && grep -q 'DEPENDENCIES.lock: no version declaration found' <<<"$OUT"; then
    pass VC-07 "a missing declaration fails the script and is named"
else fail VC-07 "the script did not fail on a missing declaration (rc=$RC)" "$OUT"; fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ]
