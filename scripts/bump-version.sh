#!/bin/bash
# bump-version.sh - set the NAAb version in every file that declares it
#
# Usage: scripts/bump-version.sh <major|minor|patch|X.Y.Z> [--prerelease <label>] [--yes]
#
# CMakeLists.txt's project VERSION is the source the others follow. Every
# declaration is rewritten whatever value it holds now: the earlier version of
# this script replaced only text equal to the CMake version, so a file that had
# drifted was skipped while the script printed "updated" -- naab.toml sat at
# 0.8.1, SECURITY_GUIDE.md at 0.9.0 and the bug template at v0.8.1 while CMake
# said 0.10.0. A declaration that is not found is an error, not a skip.
# tests/self-audit/test_version_consistency.sh checks that every declaration
# agrees, and that this script reaches each one.
#
# It does not commit or tag. NAAb merges by squash, so a tag made on the
# branch would never point at master: commit, open the release PR, and tag the
# merged commit (`git tag -a vX.Y.Z <merged sha>`), which release.yml publishes.

set -e

PROJECT_ROOT="${NAAB_VERSION_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CMAKE_FILE="$PROJECT_ROOT/CMakeLists.txt"
CHANGELOG="$PROJECT_ROOT/CHANGELOG.md"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

get_current_version() {
    grep "project(naab_lang" "$CMAKE_FILE" -A 5 | grep "VERSION" | sed 's/.*VERSION //' | sed 's/).*//' | tr -d ' '
}

usage() {
    echo "Usage: $0 <major|minor|patch|X.Y.Z> [--prerelease <label>] [--yes]"
    echo ""
    echo "  major|minor|patch   bump the CMakeLists.txt version"
    echo "  X.Y.Z               set this version"
    echo "  --prerelease LABEL  append -LABEL (alpha, beta.1, rc.1)"
    echo "  --yes               do not ask for confirmation"
    echo ""
    echo "Current version: ${CURRENT_VERSION:-unknown}"
    exit 1
}

CURRENT_VERSION=$(get_current_version)
if [ -z "$CURRENT_VERSION" ]; then
    echo -e "${RED}Error: Could not extract current version from CMakeLists.txt${NC}"
    exit 1
fi

[ $# -eq 0 ] && usage
BUMP_TYPE=$1; shift
PRERELEASE=""
ASSUME_YES=0
while [ $# -gt 0 ]; do
    case "$1" in
        --prerelease) [ -z "${2:-}" ] && usage; PRERELEASE="-$2"; shift 2 ;;
        --yes) ASSUME_YES=1; shift ;;
        *) usage ;;
    esac
done

IFS='.' read -ra VERSION_PARTS <<< "${CURRENT_VERSION%%-*}"
MAJOR=${VERSION_PARTS[0]}; MINOR=${VERSION_PARTS[1]}; PATCH=${VERSION_PARTS[2]}
case $BUMP_TYPE in
    major) MAJOR=$((MAJOR + 1)); MINOR=0; PATCH=0 ;;
    minor) MINOR=$((MINOR + 1)); PATCH=0 ;;
    patch) PATCH=$((PATCH + 1)) ;;
    [0-9]*.[0-9]*.[0-9]*)
        [[ "$BUMP_TYPE" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || usage
        IFS='.' read -r MAJOR MINOR PATCH <<< "$BUMP_TYPE" ;;
    *) echo -e "${RED}Error: Invalid bump type '$BUMP_TYPE'${NC}"; usage ;;
esac
NEW_VERSION="$MAJOR.$MINOR.$PATCH$PRERELEASE"
NEW_MINOR="$MAJOR.$MINOR"

echo -e "Setting version: ${YELLOW}$CURRENT_VERSION${NC} -> ${GREEN}$NEW_VERSION${NC}"
if [ "$ASSUME_YES" -ne 1 ]; then
    read -p "Continue? [y/N] " -n 1 -r
    echo
    [[ $REPLY =~ ^[Yy]$ ]] || { echo "Aborted."; exit 1; }
fi

MISSING=0
# set_line FILE SED-EXPRESSION CHECK-REGEX: rewrite, then require CHECK-REGEX
# to match afterwards (it names the new value, so an expression that matched
# nothing cannot pass for an update).
set_line() {
    local file="$1" expr="$2" check="$3" rel="${1#"$PROJECT_ROOT"/}"
    if [ ! -f "$file" ]; then
        echo -e "${RED}✗${NC} $rel: file not found"; MISSING=1; return
    fi
    sed -E -i.bak "$expr" "$file" && rm -f "$file.bak"
    if grep -Eq "$check" "$file"; then
        echo -e "${GREEN}✓${NC} $rel"
    else
        echo -e "${RED}✗${NC} $rel: no version declaration found"; MISSING=1
    fi
}

V="${NEW_VERSION//./\\.}"
set_line "$CMAKE_FILE" \
    "/project\\(naab_lang/,/\\)/ s/^([[:space:]]*VERSION )[^[:space:])]+/\\1$NEW_VERSION/" \
    "^[[:space:]]*VERSION $V\$"
set_line "$PROJECT_ROOT/include/naab/config.h" \
    "s/^(#define NAAB_VERSION_STRING )\"[^\"]*\"/\\1\"$NEW_VERSION\"/; s/^(#define NAAB_BUILD_TIMESTAMP )\"[^\"]*\"/\\1\"$(date +%Y-%m-%d)\"/" \
    "^#define NAAB_VERSION_STRING \"$V\"\$"
set_line "$PROJECT_ROOT/naab.toml" \
    "0,/^version = / s/^version = \"[^\"]*\"/version = \"$NEW_VERSION\"/" \
    "^version = \"$V\"\$"
set_line "$PROJECT_ROOT/pyproject.toml" \
    "0,/^version = / s/^version = \"[^\"]*\"/version = \"$NEW_VERSION\"/" \
    "^version = \"$V\"\$"
set_line "$PROJECT_ROOT/bindings/rust/Cargo.toml" \
    "0,/^version = / s/^version = \"[^\"]*\"/version = \"$NEW_VERSION\"/" \
    "^version = \"$V\"\$"
set_line "$PROJECT_ROOT/DEPENDENCIES.lock" \
    "s/^naab_version: .*/naab_version: $NEW_VERSION/" \
    "^naab_version: $V\$"
set_line "$PROJECT_ROOT/vscode-naab/package.json" \
    "0,/\"version\": / s/(\"version\": )\"[^\"]*\"/\\1\"$NEW_VERSION\"/" \
    "\"version\": \"$V\""
# The lockfile's own version appears twice (the root and packages[""]), on
# lines 3 and 9; every other "version" there is a dependency's.
set_line "$PROJECT_ROOT/vscode-naab/package-lock.json" \
    "1,12 s/^(  \"version\": |      \"version\": )\"[^\"]*\"/\\1\"$NEW_VERSION\"/" \
    "^  \"version\": \"$V\""
set_line "$PROJECT_ROOT/docs/SECURITY_GUIDE.md" \
    "s/^\\*\\*Version:\\*\\* [^ ]+/**Version:** $NEW_VERSION/" \
    "^\\*\\*Version:\\*\\* $V"
set_line "$PROJECT_ROOT/.github/ISSUE_TEMPLATE/bug_report.yml" \
    "s/placeholder: \"v[0-9][^\"]*\"/placeholder: \"v$NEW_VERSION\"/" \
    "placeholder: \"v$V\""
set_line "$PROJECT_ROOT/SECURITY.md" \
    "s/^\\| [0-9]+\\.[0-9]+\\.x( *)\\| :white_check_mark: \\| Yes \\(current\\)/| $NEW_MINOR.x\\1| :white_check_mark: | Yes (current)/" \
    "^\\| ${NEW_MINOR//./\\.}\\.x +\\| :white_check_mark: \\| Yes \\(current\\)"

if [ -f "$CHANGELOG" ] && ! grep -q "^## \\[$V\\]" "$CHANGELOG"; then
    sed -i.bak "s/^## \\[Unreleased\\]/## [Unreleased]\\n\\n## [$NEW_VERSION] - $(date +%Y-%m-%d)/" "$CHANGELOG" && rm -f "$CHANGELOG.bak"
    echo -e "${GREEN}✓${NC} CHANGELOG.md: section added under [Unreleased]"
else
    echo -e "${GREEN}✓${NC} CHANGELOG.md: section for $NEW_VERSION already present"
fi

if [ "$MISSING" -ne 0 ]; then
    echo -e "${RED}Some declarations were not updated (above).${NC}"
    exit 1
fi
echo ""
echo -e "${GREEN}Version set to $NEW_VERSION.${NC} Next: review the diff, commit, open the release PR,"
echo "and tag the merged commit: git tag -a v$NEW_VERSION <merged sha> -m \"Release v$NEW_VERSION\""
