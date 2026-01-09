#!/bin/bash
# bump_version.sh - Semantic version bumping with validation
# Usage: ./bump_version.sh <major|minor|patch> [--prerelease <label>]

set -e

# Configuration
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CMAKE_FILE="$PROJECT_ROOT/CMakeLists.txt"
CHANGELOG="$PROJECT_ROOT/CHANGELOG.md"

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

# Update CMakeLists.txt
echo "Updating CMakeLists.txt..."
if [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS
    sed -i '' "s/VERSION $CURRENT_VERSION/VERSION $NEW_VERSION/" "$CMAKE_FILE"
else
    # Linux/Android
    sed -i "s/VERSION $CURRENT_VERSION/VERSION $NEW_VERSION/" "$CMAKE_FILE"
fi

# Update CHANGELOG.md
if [ -f "$CHANGELOG" ]; then
    echo "Updating CHANGELOG.md..."
    DATE=$(date +%Y-%m-%d)

    # Add new version section to CHANGELOG
    if [[ "$OSTYPE" == "darwin"* ]]; then
        sed -i '' "s/## \[Unreleased\]/## [Unreleased]\n\n## [$NEW_VERSION] - $DATE/" "$CHANGELOG"
    else
        sed -i "s/## \[Unreleased\]/## [Unreleased]\n\n## [$NEW_VERSION] - $DATE/" "$CHANGELOG"
    fi

    echo -e "${GREEN}✓${NC} CHANGELOG.md updated"
else
    echo -e "${YELLOW}Warning: CHANGELOG.md not found${NC}"
fi

# Git operations
if git rev-parse --git-dir > /dev/null 2>&1; then
    echo "Creating git commit and tag..."

    git add "$CMAKE_FILE" "$CHANGELOG" 2>/dev/null || true
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
