#!/bin/bash
start=$(date +%s%N)
for i in $(seq 100); do
    true
done
end=$(date +%s%N)
echo $(( (end - start) / 1000 ))
