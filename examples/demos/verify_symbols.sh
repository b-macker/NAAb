#!/bin/bash
echo "Building naab-lsp..."
cd build
make naab-lsp 2>&1 | tail -10

if [ ! -f "naab-lsp" ]; then
    echo "✗ Build failed!"
    exit 1
fi

echo "✓ Build successful!"
echo ""
echo "Running hover test..."
cd ..
python3 test_hover.py
