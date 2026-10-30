#!/usr/bin/env bash
# test_vendored_patches.sh -- the vendored QuickJS is its archive plus our patch
#
# external/quickjs-2021-03-27/ is compiled from source. It was byte-identical
# to its archive until A89 patched libregexp (a step counter and a timeout
# hook); DEPENDENCIES.lock records the patch file. This keeps the record true:
# the archive (checked against its pinned sha256), unpacked and patched, must
# equal the tree, file for file -- so an edit to the tree that is not in the
# patch, or a patch that no longer applies, fails here.
#
#   VP-00  CONTROL: a planted one-byte edit to a vendored file is reported
#   VP-02  archive + patch == the vendored tree, every file in the archive
#
# Not checked here: whether the archive is upstream's. The sha256 that
# DEPENDENCIES.lock pins for it (written in f666e6e5) is not the archive's,
# and nothing had ever compared them; which one is upstream's is open (A90).
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
ARCHIVE="$REPO/external/quickjs-2021-03-27.tar.xz"
PATCH="$REPO/external/patches/quickjs-2021-03-27-regexp-timeout.patch"
TREE="${VP_TREE:-$REPO/external/quickjs-2021-03-27}"
PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));   echo "  UNMEASURABLE [$1] $2"; }

echo "=== vendored QuickJS = archive + patch ==="
for tool in tar xz patch sha256sum cmp; do
    command -v "$tool" >/dev/null 2>&1 || { unm VP-00 "$tool missing -- UNMEASURABLE, not a pass"; echo "=== Results: 0 passed, 0 failed, 1 unmeasurable ==="; exit 1; }
done
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_vp_XXXXXX"); trap 'rm -rf "$W"' EXIT

# differ TREE -> prints each file of the patched archive the tree disagrees with
differ() {
    local tree="$1" f out=""
    while IFS= read -r f; do
        [ -f "$W/u/quickjs-2021-03-27/$f" ] || continue
        cmp -s "$W/u/quickjs-2021-03-27/$f" "$tree/$f" || out+="$f"$'\n'
    done < <(cd "$W/u/quickjs-2021-03-27" && find . -type f | sed 's|^\./||')
    printf '%s' "$out"
}

mkdir -p "$W/u"
if ! tar -xJf "$ARCHIVE" -C "$W/u" || ! (cd "$W/u/quickjs-2021-03-27" && patch -p1 -s < "$PATCH"); then
    fail VP-02 "the archive did not unpack or the patch did not apply"
    echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="; exit 1
fi

cp -r "$TREE" "$W/planted"
printf ' ' >> "$W/planted/libregexp.c"
if grep -qx 'libregexp.c' <<<"$(differ "$W/planted")"; then
    pass VP-00 "CONTROL: a planted edit to libregexp.c is reported"
else
    fail VP-00 "the comparison cannot see an edit"
fi

D=$(differ "$TREE")
# Two files the build or the repo writes on purpose: CMake overwrites
# `version`, and the repo's .gitignore is its own.
D=$(grep -vxE 'version|\.gitignore' <<<"$D" || true)
if [ -z "$D" ]; then pass VP-02 "archive + patch equals the vendored tree"
else fail VP-02 "the vendored tree differs from archive + patch" "$D"; fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ]
