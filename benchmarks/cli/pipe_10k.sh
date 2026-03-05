#!/bin/bash
seq 10000 | while read -r i; do echo "line $i: the quick brown fox"; done > ${TMPDIR:-/tmp}/bench_pipe.txt
start=$(date +%s%N)
tr '[:lower:]' '[:upper:]' < ${TMPDIR:-/tmp}/bench_pipe.txt > /dev/null
end=$(date +%s%N)
rm -f ${TMPDIR:-/tmp}/bench_pipe.txt
echo $(( (end - start) / 1000 ))
