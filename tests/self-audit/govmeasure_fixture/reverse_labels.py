#!/usr/bin/env python3
"""Copy a cases file with every label reversed (test_govmeasure.sh GM-04c)."""
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f, \
        open(sys.argv[2], "w", encoding="utf-8", newline="\n") as g:
    for line in f:
        c = json.loads(line)
        c["bad"] = not c["bad"]
        g.write(json.dumps(c) + "\n")
