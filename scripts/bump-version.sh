#!/bin/bash
# bump-version.sh - Semantic version bumping with validation
# Usage: ./bump-version.sh <major|minor|patch> [--prerelease <label>]

set -e

# Configuration
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMAKE_FILE="$PROJECT_ROOT/CMakeLists.txt"
CHANGELOG="$PROJECT_ROOT/CHANGELOG.md"
CONFIG_H="$PROJECT_ROOT/include/naab/config.h"
NAAB_TOML="$PROJECT_ROOT/naab.toml"
GOV_MAIN="$PROJECT_ROOT/src/cli/gov_main.cpp"
VSCODE_PKG="$PROJECT_ROOT/vscode-naab/package.json"
VSCODE_LOCK="$PROJECT_ROOT/vscode-naab/package-lock.json"
DEPS_LOCK="$PROJECT_ROOT/DEPENDENCIES.lock"
USER_GUIDE="$PROJECT_ROOT/USER_GUIDE.md"
SECURITY_MD="$PROJECT_ROOT/SECURITY.md"
BUG_TEMPLATE="$PROJECT_ROOT/.github/ISSUE_TEMPLATE/bug_report.yml"
TEST_CLI_FLAGS="$PROJECT_ROOT/tests/cli/test_cli_flags.sh"
TEST_NAAB_GOV="$PROJECT_ROOT/tests/cli/test_naab_gov.sh"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Extract current version from CMakeLists.txt
get_current_version() {
    grep "project(naab_lang" "$CMAKE_FILE" -A 5 | grep "VERSION" | sed 's/.*VERSION //' | sed 's/).*//' | tr -d ' '
}

CURRENT_VERSION=$(get_current_version)

if [ -z "$CURRENT_VERSION" ]; then
    echo -e "${RED}Error: Could not extract current version from CMakeLists.txt${NC}"
    exit 1
fi

usage() {
    echo "Usage: $0 <major|minor|patch> [--prerelease <label>]"
    echo ""
    echo "Bump semantic version:"
    echo "  major:      1.0.0 -> 2.0.0 (breaking changes)"
    echo "  minor:      1.0.0 -> 1.1.0 (new features, backward compatible)"
    echo "  patch:      1.0.0 -> 1.0.1 (bug fixes)"
    echo ""
    echo "Options:"
    echo "  --prerelease <label>   Add prerelease label (e.g., alpha, beta, rc.1)"
    echo ""
    echo "Current version: $CURRENT_VERSION"
    echo ""
    echo "Examples:"
    echo "  $0 patch                    # 0.1.0 -> 0.1.1"
    echo "  $0 minor                    # 0.1.0 -> 0.2.0"
    echo "  $0 major                    # 0.1.0 -> 1.0.0"
    echo "  $0 minor --prerelease beta.1 # 0.1.0 -> 0.2.0-beta.1"
    exit 1
}

if [ $# -eq 0 ]; then
    usage
fi

BUMP_TYPE=$1
PRERELEASE=""

# Parse prerelease flag
if [ "$2" == "--prerelease" ]; then
    if [ -z "$3" ]; then
        echo -e "${RED}Error: --prerelease requires a label${NC}"
        usage
    fi
    PRERELEASE="-$3"
fi

# Parse current version
IFS='.' read -ra VERSION_PARTS <<< "$CURRENT_VERSION"
MAJOR=${VERSION_PARTS[0]}
MINOR=${VERSION_PARTS[1]}
PATCH=${VERSION_PARTS[2]}

# Remove any prerelease suffix from patch
PATCH=$(echo "$PATCH" | sed 's/-.*//')

# Bump version based on type
case $BUMP_TYPE in
    major)
        MAJOR=$((MAJOR + 1))
        MINOR=0
        PATCH=0
        echo -e "${YELLOW}Major version bump (breaking changes)${NC}"
        ;;
    minor)
        MINOR=$((MINOR + 1))
        PATCH=0
        echo -e "${GREEN}Minor version bump (new features)${NC}"
        ;;
    patch)
        PATCH=$((PATCH + 1))
        echo -e "${GREEN}Patch version bump (bug fixes)${NC}"
        ;;
    *)
        echo -e "${RED}Error: Invalid bump type '$BUMP_TYPE'${NC}"
        usage
        ;;
esac

NEW_VERSION="$MAJOR.$MINOR.$PATCH$PRERELEASE"

echo ""
echo -e "Bumping version: ${YELLOW}$CURRENT_VERSION${NC} -> ${GREEN}$NEW_VERSION${NC}"
echo ""

# Confirmation prompt
read -p "Continue? [y/N] " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 1
fi

_sed() {
    if [[ "$OSTYPE" == "darwin"* ]]; then
        sed -i '' "$@"
    else
        sed -i "$@"
    fi
}

# Update CMakeLists.txt
echo "Updating CMakeLists.txt..."
_sed "s/VERSION $CURRENT_VERSION/VERSION $NEW_VERSION/" "$CMAKE_FILE"
echo -e "${GREEN}✓${NC} CMakeLists.txt updated"

# Update include/naab/config.h
if [ -f "$CONFIG_H" ]; then
    echo "Updating config.h..."
    _sed "s/NAAB_VERSION_STRING \"$CURRENT_VERSION\"/NAAB_VERSION_STRING \"$NEW_VERSION\"/" "$CONFIG_H"
    _sed "s/NAAB_BUILD_TIMESTAMP \"[0-9-]*\"/NAAB_BUILD_TIMESTAMP \"$(date +%Y-%m-%d)\"/" "$CONFIG_H"
    echo -e "${GREEN}✓${NC} config.h updated"
fi

# Update naab.toml
if [ -f "$NAAB_TOML" ]; then
    echo "Updating naab.toml..."
    _sed "s/^version = \"$CURRENT_VERSION\"/version = \"$NEW_VERSION\"/" "$NAAB_TOML"
    echo -e "${GREEN}✓${NC} naab.toml updated"
fi

# Update src/cli/gov_main.cpp
if [ -f "$GOV_MAIN" ]; then
    echo "Updating gov_main.cpp..."
    _sed "s/NAAB_GOV_VERSION \"$CURRENT_VERSION\"/NAAB_GOV_VERSION \"$NEW_VERSION\"/" "$GOV_MAIN"
    echo -e "${GREEN}✓${NC} gov_main.cpp updated"
fi

# Update vscode-naab/package.json
if [ -f "$VSCODE_PKG" ]; then
    echo "Updating vscode-naab/package.json..."
    _sed "s/\"version\": \"$CURRENT_VERSION\"/\"version\": \"$NEW_VERSION\"/" "$VSCODE_PKG"
    echo -e "${GREEN}✓${NC} vscode-naab/package.json updated"
fi

# Update vscode-naab/package-lock.json
if [ -f "$VSCODE_LOCK" ]; then
    echo "Updating vscode-naab/package-lock.json..."
    _sed "s/\"version\": \"$CURRENT_VERSION\"/\"version\": \"$NEW_VERSION\"/g" "$VSCODE_LOCK"
    echo -e "${GREEN}✓${NC} vscode-naab/package-lock.json updated"
fi

# Update DEPENDENCIES.lock
if [ -f "$DEPS_LOCK" ]; then
    echo "Updating DEPENDENCIES.lock..."
    _sed "s/naab_version: $CURRENT_VERSION/naab_version: $NEW_VERSION/" "$DEPS_LOCK"
    echo -e "${GREEN}✓${NC} DEPENDENCIES.lock updated"
fi

# Update USER_GUIDE.md
if [ -f "$USER_GUIDE" ]; then
    echo "Updating USER_GUIDE.md..."
    _sed "s/\*\*Version\*\*: $CURRENT_VERSION/\*\*Version\*\*: $NEW_VERSION/" "$USER_GUIDE"
    echo -e "${GREEN}✓${NC} USER_GUIDE.md updated"
fi

# Update docs/API_REFERENCE.md
API_REF="$PROJECT_ROOT/docs/API_REFERENCE.md"
if [ -f "$API_REF" ]; then
    echo "Updating docs/API_REFERENCE.md..."
    _sed "s/Version: $CURRENT_VERSION/Version: $NEW_VERSION/" "$API_REF"
    echo -e "${GREEN}✓${NC} docs/API_REFERENCE.md updated"
fi

# Update docs/SECURITY_GUIDE.md
SEC_GUIDE="$PROJECT_ROOT/docs/SECURITY_GUIDE.md"
if [ -f "$SEC_GUIDE" ]; then
    echo "Updating docs/SECURITY_GUIDE.md..."
    _sed "s/\*\*Version:\*\* $CURRENT_VERSION/\*\*Version:\*\* $NEW_VERSION/" "$SEC_GUIDE"
    echo -e "${GREEN}✓${NC} docs/SECURITY_GUIDE.md updated"
fi

# Update SECURITY.md supported versions table
if [ -f "$SECURITY_MD" ]; then
    echo "Updating SECURITY.md..."
    CURRENT_MINOR="${CURRENT_VERSION%.*}"
    NEW_MINOR="${NEW_VERSION%.*}"
    _sed "s/| ${CURRENT_MINOR}.x/| ${NEW_MINOR}.x/g" "$SECURITY_MD"
    echo -e "${GREEN}✓${NC} SECURITY.md updated"
fi

# Update bug report template placeholder
if [ -f "$BUG_TEMPLATE" ]; then
    echo "Updating bug_report.yml..."
    _sed "s/placeholder: \"v$CURRENT_VERSION\"/placeholder: \"v$NEW_VERSION\"/" "$BUG_TEMPLATE"
    echo -e "${GREEN}✓${NC} bug_report.yml updated"
fi

# Update version checks in CLI tests
if [ -f "$TEST_CLI_FLAGS" ]; then
    echo "Updating test_cli_flags.sh..."
    _sed "s/$CURRENT_VERSION/$NEW_VERSION/g" "$TEST_CLI_FLAGS"
    echo -e "${GREEN}✓${NC} test_cli_flags.sh updated"
fi

if [ -f "$TEST_NAAB_GOV" ]; then
    echo "Updating test_naab_gov.sh..."
    _sed "s/$CURRENT_VERSION/$NEW_VERSION/g" "$TEST_NAAB_GOV"
    echo -e "${GREEN}✓${NC} test_naab_gov.sh updated"
fi

# Update CHANGELOG.md
if [ -f "$CHANGELOG" ]; then
    echo "Updating CHANGELOG.md..."
    DATE=$(date +%Y-%m-%d)
    _sed "s/## \[Unreleased\]/## [Unreleased]\n\n## [$NEW_VERSION] - $DATE/" "$CHANGELOG"
    echo -e "${GREEN}✓${NC} CHANGELOG.md updated"
else
    echo -e "${YELLOW}Warning: CHANGELOG.md not found${NC}"
fi

# Git operations
if git rev-parse --git-dir > /dev/null 2>&1; then
    echo "Creating git commit and tag..."

    git add "$CMAKE_FILE" "$CHANGELOG" "$CONFIG_H" "$NAAB_TOML" "$GOV_MAIN" \
        "$VSCODE_PKG" "$VSCODE_LOCK" "$DEPS_LOCK" "$USER_GUIDE" "$SECURITY_MD" \
        "$BUG_TEMPLATE" "$TEST_CLI_FLAGS" "$TEST_NAAB_GOV" \
        "$PROJECT_ROOT/docs/API_REFERENCE.md" "$PROJECT_ROOT/docs/SECURITY_GUIDE.md" 2>/dev/null || true
    git commit -m "chore: bump version to $NEW_VERSION" || {
        echo -e "${YELLOW}Warning: No changes to commit${NC}"
    }

    # Create annotated tag
    git tag -a "v$NEW_VERSION" -m "Release v$NEW_VERSION" || {
        echo -e "${YELLOW}Warning: Tag v$NEW_VERSION already exists${NC}"
    }

    echo -e "${GREEN}✓${NC} Git commit and tag created"
    echo ""
    echo -e "${GREEN}Success!${NC} Version bumped to ${GREEN}$NEW_VERSION${NC}"
    echo ""
    echo "Next steps:"
    echo "  1. Review the changes: git show HEAD"
    echo "  2. Push to remote:     git push && git push --tags"
    echo "  3. Create release:     gh release create v$NEW_VERSION"
else
    echo -e "${YELLOW}Warning: Not a git repository${NC}"
    echo -e "${GREEN}Success!${NC} Version bumped to ${GREEN}$NEW_VERSION${NC} in CMakeLists.txt"
fi

echo ""
echo -e "${GREEN}Version bump complete!${NC}"
