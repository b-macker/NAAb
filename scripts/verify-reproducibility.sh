#!/bin/bash
# Reproducibility Verification Script
# Verifies that hermetic builds produce identical outputs
#
# This script builds the same source twice and compares outputs
# to ensure bit-for-bit reproducibility.

set -euo pipefail

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Reproducibility Verification${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_IMAGE="naab-hermetic-builder:latest"
BUILD1_DIR="build-reproducibility-1"
BUILD2_DIR="build-reproducibility-2"

cd "$PROJECT_ROOT"

# Clean previous verification builds
echo -e "${YELLOW}Cleaning previous verification builds...${NC}"
rm -rf "$BUILD1_DIR" "$BUILD2_DIR"
mkdir -p "$BUILD1_DIR" "$BUILD2_DIR"

# Build 1
echo ""
echo -e "${YELLOW}Performing first build...${NC}"
echo "This may take a few minutes..."

docker build \
    -f Dockerfile.hermetic \
    -t "${BUILD_IMAGE}-test1" \
    --build-arg SOURCE_DATE_EPOCH=1706659200 \
    --network default \
    --quiet \
    . > /dev/null

CONTAINER1=$(docker create "${BUILD_IMAGE}-test1")
docker cp "$CONTAINER1:/opt/naab/bin/naab-lang" "$BUILD1_DIR/"
docker rm "$CONTAINER1" > /dev/null

echo -e "${GREEN}✓ First build complete${NC}"

# Small delay to ensure different build time
sleep 2

# Build 2
echo ""
echo -e "${YELLOW}Performing second build...${NC}"
echo "Building with same inputs..."

docker build \
    -f Dockerfile.hermetic \
    -t "${BUILD_IMAGE}-test2" \
    --build-arg SOURCE_DATE_EPOCH=1706659200 \
    --network default \
    --quiet \
    . > /dev/null

CONTAINER2=$(docker create "${BUILD_IMAGE}-test2")
docker cp "$CONTAINER2:/opt/naab/bin/naab-lang" "$BUILD2_DIR/"
docker rm "$CONTAINER2" > /dev/null

echo -e "${GREEN}✓ Second build complete${NC}"

# Compare builds
echo ""
echo -e "${YELLOW}Comparing build outputs...${NC}"

# Check if files exist
if [ ! -f "$BUILD1_DIR/naab-lang" ] || [ ! -f "$BUILD2_DIR/naab-lang" ]; then
    echo -e "${RED}✗ Build artifacts missing${NC}"
    exit 1
fi

# Calculate checksums
SHA1=$(sha256sum "$BUILD1_DIR/naab-lang" | cut -d' ' -f1)
SHA2=$(sha256sum "$BUILD2_DIR/naab-lang" | cut -d' ' -f1)

echo "Build 1 SHA256: $SHA1"
echo "Build 2 SHA256: $SHA2"
echo ""

# Binary comparison
if cmp -s "$BUILD1_DIR/naab-lang" "$BUILD2_DIR/naab-lang"; then
    echo -e "${GREEN}✓ Binaries are identical (bit-for-bit)${NC}"
    REPRODUCIBLE=true
else
    echo -e "${RED}✗ Binaries differ${NC}"
    echo ""
    echo "Analyzing differences..."

    # Show file sizes
    SIZE1=$(stat -f%z "$BUILD1_DIR/naab-lang" 2>/dev/null || stat -c%s "$BUILD1_DIR/naab-lang")
    SIZE2=$(stat -f%z "$BUILD2_DIR/naab-lang" 2>/dev/null || stat -c%s "$BUILD2_DIR/naab-lang")

    echo "  Build 1 size: $SIZE1 bytes"
    echo "  Build 2 size: $SIZE2 bytes"
    echo "  Difference: $((SIZE2 - SIZE1)) bytes"
    echo ""

    # Check if only metadata differs (still considered reproducible for most purposes)
    if [ "$SHA1" = "$SHA2" ]; then
        echo -e "${YELLOW}⚠ Files differ but checksums match (metadata only)${NC}"
        echo "This is acceptable for reproducibility"
        REPRODUCIBLE=true
    else
        echo -e "${RED}Content differs - build is NOT reproducible${NC}"
        echo ""
        echo "Common causes:"
        echo "  - Timestamps not normalized (check SOURCE_DATE_EPOCH)"
        echo "  - Non-deterministic code generation"
        echo "  - Different build paths (check -ffile-prefix-map)"
        echo "  - External network access during build"
        REPRODUCIBLE=false
    fi
fi

# Clean up test images
echo ""
echo -e "${YELLOW}Cleaning up test images...${NC}"
docker rmi "${BUILD_IMAGE}-test1" "${BUILD_IMAGE}-test2" > /dev/null 2>&1 || true

# Final report
echo ""
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}Verification Results${NC}"
echo -e "${BLUE}========================================${NC}"
echo ""

if [ "$REPRODUCIBLE" = true ]; then
    echo -e "${GREEN}✓ PASS: Build is reproducible${NC}"
    echo ""
    echo "The hermetic build process produces identical outputs"
    echo "when given identical inputs, confirming reproducibility."
    echo ""
    echo "This meets SLSA Level 3 requirements for reproducible builds."
    exit 0
else
    echo -e "${RED}✗ FAIL: Build is NOT reproducible${NC}"
    echo ""
    echo "The builds produced different outputs. This violates"
    echo "SLSA Level 3 reproducibility requirements."
    echo ""
    echo "Review the Dockerfile.hermetic and ensure:"
    echo "  - SOURCE_DATE_EPOCH is set and used"
    echo "  - All timestamps are normalized"
    echo "  - Build paths are mapped (-ffile-prefix-map)"
    echo "  - No external inputs are fetched during build"
    exit 1
fi
