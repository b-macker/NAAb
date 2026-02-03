#!/bin/bash
echo "Building naab-lsp..."
cd build
make naab-lsp 2>&1 | tail -30
echo ""
if [ -f "naab-lsp" ]; then
    ls -lh naab-lsp
    echo "✓ Build successful!"
    exit 0
else
    echo "✗ Build failed!"
    exit 1
fi
