#!/data/data/com.termux/files/usr/bin/bash
# NAAb Build and Install Script
# Works around Android storage execution limitations

set -e  # Exit on error

echo "=== NAAb Build and Install Script ==="
echo ""

# Configuration
SOURCE_DIR="/storage/emulated/0/Download/.naab/naab_language"
BUILD_DIR="/data/data/com.termux/files/home/naab-build"
INSTALL_DIR="/data/data/com.termux/files/home/naab-install"

echo "Source: $SOURCE_DIR"
echo "Build:  $BUILD_DIR"
echo "Install: $INSTALL_DIR"
echo ""

# Clean previous build
if [ -d "$BUILD_DIR" ]; then
    echo "Cleaning previous build..."
    rm -rf "$BUILD_DIR"
fi

# Create build directory
echo "Creating build directory..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure
echo ""
echo "Configuring CMake..."
cmake "$SOURCE_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR"

# Build
echo ""
echo "Building (using $(nproc) cores)..."
make -j$(nproc)

# Install
echo ""
echo "Installing to $INSTALL_DIR..."
mkdir -p "$INSTALL_DIR/bin"
cp naab-lang "$INSTALL_DIR/bin/" || true
cp naab-repl "$INSTALL_DIR/bin/" || true
cp test_block_registry "$INSTALL_DIR/bin/" || true
cp test_examples "$INSTALL_DIR/bin/" || true

chmod +x "$INSTALL_DIR/bin"/* || true

echo ""
echo "=== Build Complete ==="
echo ""
echo "Executables installed to: $INSTALL_DIR/bin/"
echo ""
echo "To run:"
echo "  $INSTALL_DIR/bin/naab-lang version"
echo "  $INSTALL_DIR/bin/naab-repl"
echo ""
echo "Add to PATH:"
echo "  export PATH=\"$INSTALL_DIR/bin:\$PATH\""
echo "  # Add above line to ~/.bashrc to make permanent"
echo ""
