#!/bin/bash
python3 -c "
for i in range(100000):
    print(f'The quick brown fox jumps over the lazy dog #{i}')
" | { start=$(date +%s%N); grep -oE '\b[a-zA-Z0-9_]{5,}\b' | wc -l > /dev/null; end=$(date +%s%N); echo $(( (end - start) / 1000 )); }
