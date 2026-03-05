#!/bin/bash
TMP=$(mktemp)
DATA=$(python3 -c "print('x' * (10*1024*1024), end='')")
start=$(date +%s%N)
echo "$DATA" > "$TMP"
cat "$TMP" > /dev/null
end=$(date +%s%N)
rm -f "$TMP"
echo $(( (end - start) / 1000 ))
