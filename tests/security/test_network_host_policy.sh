#!/usr/bin/env bash
# ============================================================
# test_network_host_policy.sh — two network keys that read as enforcement
# and enforced nothing
#
# F21 — capabilities.network.https_only was read in exactly ONE place,
# module_resolver.cpp, for URL module imports. Nowhere on the request path. So
# http.get("http://...") ignored it entirely while the key sat in govern.json
# looking like a transport guarantee.
#
# F10 — capabilities.network.allowed_hosts was parsed and never reached the
# thing that could enforce it. The sandbox has ALWAYS implemented host
# allowlisting (Sandbox::canConnect, which http_impl.cpp already calls); the
# governance config simply never populated SandboxConfig::allowed_hosts. Two
# working halves, never connected. blocked_hosts had no sandbox field at all
# and appeared only in contradiction checks.
#
# Measured on 2785654 against a routable local endpoint, with a control proving
# the endpoint was reachable: both probes RAN under a policy that named them.
#
# WHY THE FIXES LAND IN DIFFERENT PLACES. https_only is checked in
# http_impl.cpp because the sandbox sees a host and a port, not a scheme, and
# "port 443" is not the same statement as "TLS". Host policy is checked in the
# sandbox because canConnect() is already the chokepoint every caller reaches.
#
# ONE MATCHER for allowed_hosts and blocked_hosts. An operator must not have to
# know which list a name is on to predict how it is matched. Case-insensitive
# (RFC 1035) with subdomain matching on a dot boundary, so "example.com" covers
# "api.example.com" and never "notexample.com".
#
# DENY WINS. Same asymmetry the path policy settled on: a deny list exists to
# carve exceptions out of an allow, so letting an allow override it would make
# the deny unwritable.
#
#   NH-00  CONTROL: the endpoint is reachable with no policy. Without this
#          every "blocked" below could just mean "nothing was listening"
#   NH-01  F21: a plaintext http:// request is refused under https_only
#   NH-02  F21 NEGATIVE CONTROL: with https_only OFF the same request runs —
#          so the refusal is the policy, not a broken request path
#   NH-03  F10: a host outside allowed_hosts is refused
#   NH-04  F10 NEGATIVE CONTROL: a host INSIDE allowed_hosts still runs
#   NH-05  F10: a host on blocked_hosts is refused
#   NH-06  F10: blocked_hosts WINS over allowed_hosts when both match
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"; setup_isolated_trust

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
bad(){ FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; }
skip(){ echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

T="${TMPDIR:-/tmp}/naab-nhp-$$"; mkdir -p "$T"; SRV=""
cleanup(){ [ -n "$SRV" ] && kill $SRV 2>/dev/null; teardown_isolated_trust; rm -rf "$T"; }
trap cleanup EXIT
cd "$T"

command -v python3 >/dev/null 2>&1 || { skip "NH-00" "python3 unavailable"; exit 0; }

# A ROUTABLE, non-private local address: loopback is refused by the SSRF socket
# gate, and a non-resolving host makes every arm read as blocked by DNS failure.
IP=""
for cand in $(hostname -I 2>/dev/null || true); do
    case "$cand" in
        127.*|10.*|192.168.*|169.254.*|172.1[6-9].*|172.2[0-9].*|172.3[01].*|*:*) : ;;
        *) IP="$cand"; break ;;
    esac
done
[ -z "$IP" ] && { skip "NH-00" "no routable non-private local address — UNMEASURABLE"; exit 0; }
PORT=19955

cat > srv.py <<'PY_EOF'
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
class H(BaseHTTPRequestHandler):
    def do_GET(self):
        b=b"REACHED"; self.send_response(200); self.send_header("Content-Length",str(len(b)))
        self.end_headers(); self.wfile.write(b)
    def log_message(self,*a): pass
HTTPServer((sys.argv[1], int(sys.argv[2])), H).serve_forever()
PY_EOF
python3 srv.py "$IP" "$PORT" >/dev/null 2>&1 & SRV=$!
up=no; for i in $(seq 1 40); do curl -s -m2 -o /dev/null "http://$IP:$PORT/" 2>/dev/null && { up=yes; break; }; sleep 0.25; done
[ "$up" != yes ] && { skip "NH-00" "endpoint did not come up — UNMEASURABLE"; exit 0; }

# $1 = network JSON body -> ran|blocked
probe() {
    cat > govern.json <<JSON_EOF
{ "version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
  "capabilities":{"network":{"enabled":true$1}}}
JSON_EOF
    printf 'use http\nmain { let r = http.get("http://%s:%s/") print("MARKER") }\n' "$IP" "$PORT" > t.naab
    local o; o=$(timeout 30s "$NAAB" t.naab 2>&1)
    case "$o" in *MARKER*) echo ran ;; *) echo blocked ;; esac
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  https_only and host policy read as enforcement, enforced none|${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""
echo "  endpoint http://$IP:$PORT"

R=$(probe "")
[ "$R" = ran ] && ok "NH-00" "CONTROL: the endpoint is reachable with no policy" \
  || { bad "NH-00" "CONTROL: the endpoint is reachable with no policy" "got '$R' — every arm below would be meaningless"; echo ""; exit 1; }

R=$(probe ',"https_only":true')
[ "$R" = blocked ] && ok "NH-01" "F21: plaintext http:// is refused under https_only" \
  || bad "NH-01" "F21: plaintext http:// is refused under https_only" "the key is inert on the request path"

R=$(probe ',"https_only":false')
[ "$R" = ran ] && ok "NH-02" "F21 NEGATIVE CONTROL: https_only off, request runs" \
  || bad "NH-02" "F21 NEGATIVE CONTROL: https_only off, request runs" "got '$R' — plaintext is being blocked unconditionally"

R=$(probe ',"allowed_hosts":["example.com"]')
[ "$R" = blocked ] && ok "NH-03" "F10: a host outside allowed_hosts is refused" \
  || bad "NH-03" "F10: a host outside allowed_hosts is refused" "allowed_hosts never reaches the sandbox"

R=$(probe ",\"allowed_hosts\":[\"$IP\"]")
[ "$R" = ran ] && ok "NH-04" "F10 NEGATIVE CONTROL: a host inside allowed_hosts runs" \
  || bad "NH-04" "F10 NEGATIVE CONTROL: a host inside allowed_hosts runs" "got '$R' — an allowlist that permits nothing is not an allowlist"

R=$(probe ",\"blocked_hosts\":[\"$IP\"]")
[ "$R" = blocked ] && ok "NH-05" "F10: a host on blocked_hosts is refused" \
  || bad "NH-05" "F10: a host on blocked_hosts is refused" "blocked_hosts had no sandbox field at all"

R=$(probe ",\"allowed_hosts\":[\"$IP\"],\"blocked_hosts\":[\"$IP\"]")
[ "$R" = blocked ] && ok "NH-06" "F10: blocked_hosts WINS over allowed_hosts" \
  || bad "NH-06" "F10: blocked_hosts WINS over allowed_hosts" "an allow overrode a deny — the deny becomes unwritable"

echo ""
echo -e "${CYAN}--------------------------------------------------------------${NC}"
echo -e "  Passed: ${GREEN}${PASS}${NC}   Failed: ${RED}${FAIL}${NC}"
[ "$FAIL" -gt 0 ] && { echo ""; exit 1; }
echo -e "  ${GREEN}ALL PASSED${NC}"; echo ""; exit 0
