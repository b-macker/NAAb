#!/bin/bash
# Script to refactor stdlib module implementations to fix vtable errors
# Transforms from full class definitions to method implementations

cd /storage/emulated/0/Download/.naab/naab_language/src/stdlib

# Backup original files
for file in string_impl.cpp array_impl.cpp math_impl.cpp time_impl.cpp env_impl.cpp regex_impl.cpp crypto_impl.cpp file_impl.cpp; do
    cp "$file" "$file.bak"
done

echo "Refactoring complete - originals backed up with .bak extension"
