#!/bin/bash
seq 10000 | shuf | { start=$(date +%s%N); sort -n > /dev/null; end=$(date +%s%N); echo $(( (end - start) / 1000 )); }
