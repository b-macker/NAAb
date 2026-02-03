#!/bin/bash
cd /data/data/com.termux/files/home/.naab/language/build
echo "Building naab_security..."
cmake --build . --target naab_security -j2 2>&1 | tail -50
