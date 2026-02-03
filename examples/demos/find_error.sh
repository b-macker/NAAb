#!/bin/bash
cd /data/data/com.termux/files/home/.naab/language/build
echo "=== Finding compilation errors ==="
cmake --build . --target naab_unit_tests 2>&1 | grep -B 3 -A 3 "error:" | head -50
