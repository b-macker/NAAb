#!/bin/bash
# Test script for REPL - demonstrates features

echo "Testing NAAb REPL..."
echo ""

# Send commands to REPL
/data/data/com.termux/files/home/naab-repl << 'EOF'
:help
let x = 42
print("x =", x)
let y = x + 10
print("y =", y)
print("x + y =", x + y)
:history
:exit
EOF

echo ""
echo "REPL test complete!"
