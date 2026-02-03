#!/bin/bash
# Simple rebuild script - run this manually

cd /data/data/com.termux/files/home/.naab/language/build

echo "Building naab-lang..."
make -j4 naab-lang

if [ $? -eq 0 ]; then
    echo ""
    echo "✅ Build successful!"
    echo ""
    echo "Now run tests:"
    echo "  ./naab-lang ../examples/test_function_return_inference.naab"
    echo "  ./naab-lang ../examples/test_generic_argument_inference.naab"
else
    echo ""
    echo "❌ Build failed - check errors above"
fi
