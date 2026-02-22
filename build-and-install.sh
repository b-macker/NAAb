#!/usr/bin/env bash
# NAAb Build and Install Script

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
INSTALL_DIR="${INSTALL_DIR:-$HOME/.local}"

echo "=== NAAb Build and Install Script ==="
echo ""
echo "Source:  $SCRIPT_DIR"
echo "Build:   $BUILD_DIR"
echo "Install: $INSTALL_DIR"
echo ""

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure
echo "Configuring CMake..."
cmake "$SCRIPT_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR"

# Build
echo ""
echo "Building (using $(nproc) cores)..."
make -j$(nproc)

# Install
echo ""
echo "Installing to $INSTALL_DIR/bin/..."
mkdir -p "$INSTALL_DIR/bin"
cp naab-lang "$INSTALL_DIR/bin/" 2>/dev/null || true
cp naab-lsp "$INSTALL_DIR/bin/" 2>/dev/null || true

chmod +x "$INSTALL_DIR/bin"/naab-* 2>/dev/null || true

echo ""
echo "=== Build Complete ==="
echo ""
echo "To run:"
echo "  $INSTALL_DIR/bin/naab-lang --help"
echo ""
echo "Add to PATH (if not already):"
echo "  export PATH=\"$INSTALL_DIR/bin:\$PATH\""
echo ""
