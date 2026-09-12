#!/usr/bin/env bash
# ============================================================
# test_ssrf_redirect_dns.sh — the SSRF filter checked a STRING, not an ADDRESS
#
# THE DEFECT (register rows F43 and F47 — one cause, two faces)
#
# isPrivateHost() in src/stdlib/http_impl.cpp inspects the host substring of
# the URL the CALLER passed. That string is not what libcurl connects to:
#
#   F47  libcurl RESOLVES the name. The filter tests the literal "localhost"
#        and then inet_pton, so anything that is not an IP literal returns
#        false -- a hostname pointing at 127.0.0.1 walks straight through.
#        Sandbox::canConnect() is also a pure string compare and resolves
#        nothing, so there is no second line of defence.
#
#   F43  libcurl FOLLOWS redirects itself (CURLOPT_FOLLOWLOCATION, MAXREDIRS
#        5). The filter never sees a hop. CURLOPT_REDIR_PROTOCOLS_STR bounds
#        the SCHEME only, so a same-scheme hop to a private address is
#        unguarded.
#
# Measured on bf489f0, all three arms against the SAME target endpoint:
#
#   A  http://127.0.0.1:PORT/secret     exit 1  BLOCKED   <- the filter works
#   B  http://<alias>:PORT/secret       exit 0  SECRET    <- F47, no redirect
#   C  http://<alias>:RPORT/go --302->  exit 0  SECRET    <- F43
#
# Arm C's redirect target is the LITERAL 127.0.0.1 that arm A blocks.
#
# The real-world target is http://169.254.169.254/latest/meta-data/ (cloud
# instance metadata, i.e. credentials). This uses loopback instead because it
# proves the same thing without leaving the machine, and because retrieving a
# real marker from a destination arm A blocks is a stronger claim than a
# connection error to an unroutable address.
#
# THE FIX
#
# Not another string check: filtering Location headers closes F43 and leaves
# F47 open, and re-checking the hostname closes neither. Enforcement moved to
# CURLOPT_OPENSOCKETFUNCTION, which libcurl calls with the RESOLVED
# curl_sockaddr once per socket -- initial request and every redirect hop --
# before the socket exists. isPrivateHost() stays as a cheap first-pass
# rejection with a better error message.
#
#   SS-01  POSITIVE CONTROL: the IP-literal form is blocked. If this fails the
#          filter is off entirely and the other arms prove nothing
#   SS-02  F47: a hostname resolving to loopback is blocked
#   SS-03  F43: a redirect to loopback is blocked
#   SS-04  NEGATIVE CONTROL: a non-private destination is still reachable, so
#          the fix is not "block everything". Uses the same alias resolved to a
#          NON-loopback local address, so it needs no external network
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$REPO/build/naab-lang"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

TEST_TMP="${TMPDIR:-/tmp}/naab-ssrf-$$"
SRV_PID=""
cleanup() { [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP"

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  SSRF: the filter checked a string, not an address            |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

command -v python3 >/dev/null 2>&1 || { skip "SS-00" "python3 unavailable — cannot stage local servers"; exit 0; }

# The F47 arm needs a name that is NOT "localhost" and NOT an IP literal but
# resolves to loopback. Probe for one rather than assume; UNMEASURABLE is a
# third outcome, not a silent pass.
ALIAS=""
for cand in runsc vm localtest.me; do
    if getent hosts "$cand" >/dev/null 2>&1; then ALIAS="$cand"; break; fi
done
if [ -z "$ALIAS" ]; then
    skip "SS-00" "no non-literal hostname resolves to loopback here — F43/F47 arms UNMEASURABLE"
    exit 0
fi
echo "  using loopback alias: $ALIAS"

RPORT=18431; TPORT=18432
cat > "$TEST_TMP/srv.py" <<'PY_EOF'
import sys, threading
from http.server import BaseHTTPRequestHandler, HTTPServer
RPORT = int(sys.argv[1]); TPORT = int(sys.argv[2])
class Redirector(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(302)
        self.send_header("Location", "http://127.0.0.1:%d/secret" % TPORT)
        self.end_headers()
    def log_message(self, *a): pass
class Target(BaseHTTPRequestHandler):
    def do_GET(self):
        body = b"LOOPBACK_SECRET_REACHED"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
threading.Thread(target=lambda: HTTPServer(("127.0.0.1", TPORT), Target).serve_forever(), daemon=True).start()
HTTPServer(("127.0.0.1", RPORT), Redirector).serve_forever()
PY_EOF
python3 "$TEST_TMP/srv.py" "$RPORT" "$TPORT" >/dev/null 2>&1 &
SRV_PID=$!
for _ in $(seq 1 30); do
    if curl -s -o /dev/null "http://127.0.0.1:$TPORT/secret" 2>/dev/null; then break; fi
    sleep 0.2
done
if ! curl -s -o /dev/null "http://127.0.0.1:$TPORT/secret" 2>/dev/null; then
    skip "SS-00" "local servers did not come up — arms UNMEASURABLE"
    exit 0
fi

W="$TEST_TMP/w"; mkdir -p "$W"
cat > "$W/govern.json" <<'JSON_EOF'
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "capabilities": { "network": { "enabled": true } } }
JSON_EOF

# $1=url -> "reached" | "blocked" | "UNMEASURED"
fetch() {
    printf 'use http\nmain { let r = http.get("%s") print("MARKER=" + string(r.get("body"))) }\n' "$1" > "$W/t.naab"
    local out
    out=$(cd "$W" && timeout 30s "$NAAB" t.naab 2>&1)
    case "$out" in
        *LOOPBACK_SECRET_REACHED*) echo "reached" ;;
        *"private network denied"*|*"Security:"*|*"HTTP error"*|*"HTTP request failed"*) echo "blocked" ;;
        *MARKER=*) echo "reached" ;;
        *) echo "UNMEASURED" ;;
    esac
}

A=$(fetch "http://127.0.0.1:$TPORT/secret")
B=$(fetch "http://$ALIAS:$TPORT/secret")
C=$(fetch "http://$ALIAS:$RPORT/go")
echo "  measured: A(ip-literal)=$A  B(hostname)=$B  C(redirect)=$C"
echo ""

if [ "$A" = "blocked" ]; then
    pass "SS-01" "POSITIVE CONTROL: the IP-literal form is blocked"
else
    fail "SS-01" "POSITIVE CONTROL: the IP-literal form is blocked" \
         "got '$A' — the SSRF filter is not running at all; SS-02/SS-03 prove nothing"
fi

if [ "$B" = "blocked" ]; then
    pass "SS-02" "F47: a hostname resolving to loopback is blocked"
elif [ "$B" = "UNMEASURED" ]; then
    fail "SS-02" "F47: a hostname resolving to loopback is blocked" "UNMEASURED — broken probe, not a pass"
else
    fail "SS-02" "F47: a hostname resolving to loopback is blocked" \
         "reached the loopback endpoint via '$ALIAS' — the filter never resolves DNS"
fi

if [ "$C" = "blocked" ]; then
    pass "SS-03" "F43: a redirect to loopback is blocked"
elif [ "$C" = "UNMEASURED" ]; then
    fail "SS-03" "F43: a redirect to loopback is blocked" "UNMEASURED — broken probe, not a pass"
else
    fail "SS-03" "F43: a redirect to loopback is blocked" \
         "followed the 302 to 127.0.0.1 — the literal address SS-01 blocks"
fi

# SS-04 — over-block control. A non-loopback, non-private local address proves
# the gate rejects by RANGE and not by "any address at all". Skipped rather
# than faked when the machine has no such address.
NONPRIV=""
for ip in $(command -v hostname >/dev/null 2>&1 && hostname -I 2>/dev/null || true); do
    case "$ip" in
        127.*|10.*|192.168.*|169.254.*|172.1[6-9].*|172.2[0-9].*|172.3[01].*) : ;;
        *:*) : ;;
        *) NONPRIV="$ip"; break ;;
    esac
done
if [ -z "$NONPRIV" ]; then
    skip "SS-04" "no non-private local address available — over-block control UNMEASURABLE"
else
    python3 - "$NONPRIV" "$((TPORT+1))" >/dev/null 2>&1 <<'PY2_EOF' &
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
class H(BaseHTTPRequestHandler):
    def do_GET(self):
        b = b"LOOPBACK_SECRET_REACHED"
        self.send_response(200); self.send_header("Content-Length", str(len(b)))
        self.end_headers(); self.wfile.write(b)
    def log_message(self, *a): pass
HTTPServer((sys.argv[1], int(sys.argv[2])), H).serve_forever()
PY2_EOF
    PUB_PID=$!
    sleep 1
    D=$(fetch "http://$NONPRIV:$((TPORT+1))/secret")
    kill "$PUB_PID" 2>/dev/null
    if [ "$D" = "reached" ]; then
        pass "SS-04" "NEGATIVE CONTROL: a non-private address is still reachable"
    elif [ "$D" = "UNMEASURED" ]; then
        skip "SS-04" "over-block control UNMEASURABLE (server did not respond)"
    else
        fail "SS-04" "NEGATIVE CONTROL: a non-private address is still reachable" \
             "got '$D' — the gate is blocking addresses outside the private ranges"
    fi
fi

echo ""
echo -e "${CYAN}--------------------------------------------------------------${NC}"
echo -e "  Passed: ${GREEN}${PASS_COUNT}${NC}   Failed: ${RED}${FAIL_COUNT}${NC}   Skipped: ${YELLOW}${SKIP_COUNT}${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}FAILURES:${NC}${FAILURES}"; echo ""; exit 1
fi
echo -e "  ${GREEN}ALL PASSED${NC}"; echo ""; exit 0
