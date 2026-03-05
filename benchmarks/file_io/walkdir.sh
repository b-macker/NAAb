#!/bin/bash
start=$(date +%s%N)
find ~/.naab/language/src -type f | wc -l > /dev/null
end=$(date +%s%N)
echo $(( (end - start) / 1000 ))
