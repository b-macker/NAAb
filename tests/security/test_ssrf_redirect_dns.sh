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
#   SS-05  register row F15/A11, closed by the SAME gate: the URL-level filter
#          parses with inet_pton, which accepts ONLY dotted-quad, while
#          getaddrinfo also accepts the short, integer and octal forms. So
#          "127.1", "2130706433" and "0177.0.0.1" were each read as "not an IP
#          literal, therefore not private" and then resolved to 127.0.0.1.
#          Measured on master: all three REACHED the loopback endpoint that the
#          dotted-quad spelling of the SAME address is blocked from. The socket
#          gate closes them because it reads the RESOLVED sockaddr and never
#          sees the spelling at all. Pinned here so a future refactor cannot
#          reintroduce the notation bypass silently -- it was fixed
#          incidentally, and incidental fixes rot.
#
# URL MODULE IMPORTS (SI-*) -- the second copy
#
# `import "http://host/mod.naab"` downloads code with its own curl call
# (module_resolver.cpp). #213 gave it a URL-STRING check; #229's socket gate
# reached http.get only. Measured on 526044de under an enforce config with the
# network enabled: an import from 127.0.0.1 was refused while the SAME module
# imported and ran from 127.1, 2130706433 and 0x7f000001. Both paths now share
# one gate (naab::net::refusePrivateOpenSocket, curl_egress.h).
#
#   SI-01  CONTROL: the dotted-quad loopback import is refused (string check)
#   SI-02  a hostname resolving to loopback is refused
#   SI-03  a redirect from a NON-private host to loopback is refused at the hop
#   SI-04  CONTROL: an import from a non-private address still works -- also
#          proves SI-03's first hop is permitted, so its refusal is the hop's
#   SI-05  alternate IP notations for loopback are refused
#
# A refused import prints only "Failed to download", which a dead server also
# prints. So "refused" here means: no module text, AND the target server's
# request log has no entry for that arm, AND the target answered curl after
# the attempt. Anything else is UNMEASURED, reported as a failure.
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
PUB_PID=""
cleanup() { [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null; [ -n "$PUB_PID" ] && kill "$PUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$TEST_TMP"; }
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
LOG = sys.argv[3] if len(sys.argv) > 3 else None
def log_path(p):
    if LOG:
        with open(LOG, "a") as f: f.write(p + "\n")
class Redirector(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(302)
        self.send_header("Location", "http://127.0.0.1:%d/secret" % TPORT)
        self.end_headers()
    def log_message(self, *a): pass
class Target(BaseHTTPRequestHandler):
    def do_GET(self):
        log_path(self.path)
        if self.path.split("?")[0].endswith(".naab"):
            body = b'export let s = "LOOPBACK_MODULE_REACHED"\n'
        else:
            body = b"LOOPBACK_SECRET_REACHED"
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
threading.Thread(target=lambda: HTTPServer(("127.0.0.1", TPORT), Target).serve_forever(), daemon=True).start()
HTTPServer(("127.0.0.1", RPORT), Redirector).serve_forever()
PY_EOF
TLOG="$TEST_TMP/target.log"; : > "$TLOG"
python3 "$TEST_TMP/srv.py" "$RPORT" "$TPORT" "$TLOG" >/dev/null 2>&1 &
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
    python3 - "$NONPRIV" "$((TPORT+1))" "$TPORT" >/dev/null 2>&1 <<'PY2_EOF' &
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer
class H(BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/gomod":
            # SI-03: a non-private first hop that redirects to loopback.
            self.send_response(302)
            self.send_header("Location", "http://127.0.0.1:%s/mod.naab?arm=SI-03" % sys.argv[3])
            self.end_headers(); return
        if path.endswith(".naab"):
            b = b'export let s = "PUBLIC_MODULE_REACHED"\n'
        else:
            b = b"LOOPBACK_SECRET_REACHED"
        self.send_response(200); self.send_header("Content-Length", str(len(b)))
        self.end_headers(); self.wfile.write(b)
    def log_message(self, *a): pass
HTTPServer((sys.argv[1], int(sys.argv[2])), H).serve_forever()
PY2_EOF
    PUB_PID=$!
    sleep 1
    D=$(fetch "http://$NONPRIV:$((TPORT+1))/secret")
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
echo "SS-05: alternate IP notations (F15/A11) — same gate, different spelling"
# Resolver control first: if the platform's getaddrinfo does NOT accept a form,
# that arm is UNMEASURABLE rather than a pass. Without this a platform that
# simply cannot resolve "127.1" would report the bypass as closed.
for form in "127.1" "2130706433" "0177.0.0.1"; do
    if ! curl -s -m 5 -o /dev/null "http://${form}:${TPORT}/secret" 2>/dev/null; then
        skip "SS-05/${form}" "resolver does not accept this form here — UNMEASURABLE"
        continue
    fi
    r=$(fetch "http://${form}:${TPORT}/secret")
    if [ "$r" = "blocked" ]; then
        pass "SS-05/${form}" "alternate notation for loopback is blocked"
    elif [ "$r" = "UNMEASURED" ]; then
        fail "SS-05/${form}" "alternate notation for loopback is blocked" "UNMEASURED — broken probe, not a pass"
    else
        fail "SS-05/${form}" "alternate notation for loopback is blocked" \
             "reached 127.0.0.1 spelled '${form}' — the dotted-quad form of the same address is blocked"
    fi
done

echo ""
echo "SI: URL module imports -- the same gate on the import path"
# $1=arm id  $2=url -> "reached" | "blocked" | "UNMEASURED"
import_try() {
    local arm="$1" url="$2" out home="$TEST_TMP/home-$1"
    rm -rf "$home"; mkdir -p "$home"   # the module cache lives under HOME
    printf 'import "%s" as r\nmain { print("GOT=" + string(r.s)) }\n' "$url" > "$W/imp.naab"
    out=$(cd "$W" && HOME="$home" timeout 30s "$NAAB" imp.naab 2>&1)
    case "$out" in
        *GOT=LOOPBACK_MODULE_REACHED*|*GOT=PUBLIC_MODULE_REACHED*) echo "reached"; return ;;
    esac
    # Refused only if the target never saw this arm and is still answering.
    if grep -q "arm=$arm" "$TLOG" 2>/dev/null; then echo "reached"; return; fi
    if ! curl -s -m 5 -o /dev/null "http://127.0.0.1:$TPORT/alive" 2>/dev/null; then echo "UNMEASURED"; return; fi
    case "$out" in
        *"private network denied"*|*"Failed to download"*) echo "blocked" ;;
        *) echo "UNMEASURED" ;;
    esac
}
si_check() {  # $1=arm $2=result $3=description
    case "$2" in
        blocked) pass "$1" "$3" ;;
        UNMEASURED) fail "$1" "$3" "UNMEASURED -- broken probe, not a pass" ;;
        *) fail "$1" "$3" "the module was downloaded from loopback and ran" ;;
    esac
}
si_check "SI-01" "$(import_try SI-01 "http://127.0.0.1:$TPORT/mod.naab?arm=SI-01")" \
    "CONTROL: a dotted-quad loopback import is refused"
si_check "SI-02" "$(import_try SI-02 "http://$ALIAS:$TPORT/mod.naab?arm=SI-02")" \
    "an import from a hostname resolving to loopback is refused"
if [ -z "$NONPRIV" ]; then
    skip "SI-03" "no non-private local address -- redirect arm UNMEASURABLE"
    skip "SI-04" "no non-private local address -- over-block control UNMEASURABLE"
else
    si_check "SI-03" "$(import_try SI-03 "http://$NONPRIV:$((TPORT+1))/gomod")" \
        "an import redirected from a non-private host to loopback is refused at the hop"
    r4=$(import_try SI-04 "http://$NONPRIV:$((TPORT+1))/mod.naab?arm=SI-04")
    if [ "$r4" = "reached" ]; then
        pass "SI-04" "CONTROL: an import from a non-private address still works"
    else
        fail "SI-04" "CONTROL: an import from a non-private address still works" \
             "got '$r4' -- the gate refuses non-private addresses, or the import path is broken; SI-03 proves nothing"
    fi
    kill "$PUB_PID" 2>/dev/null
fi
for form in "127.1" "2130706433" "0177.0.0.1"; do
    if ! curl -s -m 5 -o /dev/null "http://${form}:${TPORT}/secret" 2>/dev/null; then
        skip "SI-05/${form}" "resolver does not accept this form here -- UNMEASURABLE"
        continue
    fi
    si_check "SI-05/${form}" "$(import_try "SI-05-$form" "http://${form}:${TPORT}/mod.naab?arm=SI-05-$form")" \
        "an import from loopback spelled '${form}' is refused"
done

echo ""
echo -e "${CYAN}--------------------------------------------------------------${NC}"
echo -e "  Passed: ${GREEN}${PASS_COUNT}${NC}   Failed: ${RED}${FAIL_COUNT}${NC}   Skipped: ${YELLOW}${SKIP_COUNT}${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}FAILURES:${NC}${FAILURES}"; echo ""; exit 1
fi
echo -e "  ${GREEN}ALL PASSED${NC}"; echo ""; exit 0
