#!/usr/bin/env bash
# ============================================================
# test_operator_egress_env.sh -- an agent call and the telemetry webhook keep
# the proxy and CA bundle the process STARTED with, whatever the program later
# writes into its environment.
#
# WHY
#
# An agent call and the telemetry webhook are the operator's connections: the
# endpoint, the key and the data are declared in govern.json, not by the
# program. libcurl left alone reads its proxy from the live environment, and
# both sites used to read SSL_CERT_FILE / CURL_CA_BUNDLE with getenv at call
# time -- while env.set writes the real process environment. #313 fixed both
# sites to a snapshot taken at config load; its only test exercised the header
# (tests/unit/net_proxy_snapshot_test.cpp), so a call site that went back to
# reading the live environment would have stayed green. These arms run the
# real binary end to end.
#
# The probes never intercept anything. The proxy the program sets is a closed
# local port (127.0.0.1:9), and the CA bundle it sets does not exist: if a
# connection honoured either, it would FAIL. Each arm asserts the operator's
# connection still succeeds.
#
# CONTROLS
#   EO-00  each stub answers with the process environment left alone (the arms
#          below measure a working harness)
#   EO-01c the SAME closed proxy, set before the process starts, breaks the
#          agent call: the snapshot carries the operator's proxy rather than
#          ignoring proxies, and libcurl does proxy a loopback target. Without
#          it, a build that never used a proxy would pass EO-01.
#   EO-03c a missing CA bundle set before start breaks the https call: the CA
#          the arm relies on is really consulted.
#   Every arm prints what env.get reads back after env.set, so an arm cannot
#   pass because env.set was refused.
#
# ARMS
#   EO-01/EO-01t  agent call over http, program sets *_proxy (VM / tree-walker;
#                 the tree-walker's arms run under audit, EO-TW: refused under
#                 enforce)
#   EO-02         webhook over http, program sets *_proxy, event sent after it
#   EO-03/EO-03t  agent call over https, program points SSL_CERT_FILE and
#                 CURL_CA_BUNDLE at a missing file
#   EO-04         webhook over https, same
#   EO-05/EO-05t  the program's OWN http.get, program sets http_proxy: the
#                 request goes to the URL's host, never through the program's
#                 proxy -- the host lists judge the URL's host, so a proxy the
#                 program chose would carry the connection to a host the
#                 policy never lists. Probe: a proxy NAME that cannot resolve
#                 (.invalid) and a destination that cannot (.test); curl's
#                 error says which one it tried. Nothing connects.
#   EO-05c        control: the same proxy, set before start, IS used by
#                 http.get (curl reports the proxy name) -- the operator's
#                 proxy still applies, and the probe can tell the two apart
##   EO-06/EO-06t/EO-06h  the program sets no_proxy=* under an operator proxy:
#                 the agent call (VM / tree-walker) and http.get stay on the
#                 operator's proxy. libcurl reads no_proxy from the live
#                 environment whenever CURLOPT_NOPROXY is unset, even with
#                 CURLOPT_PROXY set -- #313 set it only when the snapshot had a
#                 value, and on that build all three went direct. The operator
#                 proxy is the closed port, so staying on it means FAILING;
#                 EO-01c is the matched control that it breaks the call.
#
# Windows: the https stub needs openssl and a POSIX signal story; the suite is
# UNMEASURABLE there rather than failed.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
PASS=0; FAIL=0; SKIP=0
pass() { PASS=$((PASS + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL=$((FAIL + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo "       -> ${3:0:600}"; }
skip() { SKIP=$((SKIP + 1)); echo -e "  ${YELLOW}UNMEASURABLE${NC} [$1] $2"; }

echo "=== operator egress keeps its load-time proxy and CA ==="

case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) skip "EO-*" "Windows: https stub not supported here"; exit 0 ;;
esac
if [ ! -x "$NAAB" ]; then skip "EO-*" "naab-lang not built"; exit 0; fi
if ! command -v openssl >/dev/null 2>&1; then skip "EO-*" "openssl not available for the https stub"; exit 0; fi

# Every arm starts with NO proxy and NO CA override; each sets its own.
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY all_proxy ALL_PROXY no_proxy NO_PROXY
unset SSL_CERT_FILE CURL_CA_BUNDLE

_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="$(mktemp -d "${_SYSTMP}/egress-env-XXXXXX")"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() {
    for f in "$TEST_TMP"/*.pid; do [ -f "$f" ] && kill "$(cat "$f")" 2>/dev/null; done
    teardown_isolated_trust
    rm -rf "$TEST_TMP"
}
trap cleanup EXIT
export EGRESS_TEST_KEY="egress-test-not-a-secret"
DEAD_PROXY="http://127.0.0.1:9"
MISSING_CA="$TEST_TMP/no-such-dir/ca.pem"

# ------------------------------------------------------------------
# One stub, http or https: answers every POST with a Gemini-shaped reply and
# logs "<path> <first 300 bytes of body>" per request.
# ------------------------------------------------------------------
cat > "$TEST_TMP/stub.py" <<'PY'
import http.server, json, ssl, sys, threading
mode, portfile, logfile = sys.argv[1], sys.argv[2], sys.argv[3]
cert = sys.argv[4] if len(sys.argv) > 4 else ""
key = sys.argv[5] if len(sys.argv) > 5 else ""
lock = threading.Lock()
REPLY = {"candidates": [{"content": {"parts": [{"text": "EGRESS_OK"}], "role": "model"},
                         "finishReason": "STOP"}],
         "usageMetadata": {"promptTokenCount": 5, "candidatesTokenCount": 3, "totalTokenCount": 8}}
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        with lock, open(logfile, "a", encoding="utf-8", errors="replace") as f:
            f.write("%s %s\n" % (self.path, body[:300].decode("utf-8", "replace").replace("\n", " ")))
        out = json.dumps(REPLY).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
if mode == "https":
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
with open(portfile, "w") as f:
    f.write(str(srv.server_address[1]))
srv.serve_forever()
PY

start_stub() {  # $1=name $2=mode [cert key]; sets PORT_<name>
    local name="$1" mode="$2"; shift 2
    : > "$TEST_TMP/$name.log"
    python3 "$TEST_TMP/stub.py" "$mode" "$TEST_TMP/$name.port" "$TEST_TMP/$name.log" "$@" \
        >/dev/null 2>"$TEST_TMP/$name.err" &
    echo $! > "$TEST_TMP/$name.pid"
    for _ in $(seq 1 100); do [ -s "$TEST_TMP/$name.port" ] && break; sleep 0.1; done
    [ -s "$TEST_TMP/$name.port" ] || return 1
    printf -v "PORT_$name" '%s' "$(cat "$TEST_TMP/$name.port")"
}

# A self-signed certificate for 127.0.0.1; the operator's CA bundle is it.
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=127.0.0.1" \
    -addext "subjectAltName=IP:127.0.0.1" \
    -keyout "$TEST_TMP/key.pem" -out "$TEST_TMP/cert.pem" >/dev/null 2>&1
if [ ! -s "$TEST_TMP/cert.pem" ]; then skip "EO-*" "could not make a test certificate"; exit 0; fi

start_stub plain http || { skip "EO-*" "http stub did not start"; exit 0; }
start_stub tls https "$TEST_TMP/cert.pem" "$TEST_TMP/key.pem" || { skip "EO-*" "https stub did not start"; exit 0; }
HTTP_BASE="http://127.0.0.1:$PORT_plain"
HTTPS_BASE="https://127.0.0.1:$PORT_tls"

cfg() {  # $1=dir $2=agent api_base $3=webhook url ("" = none)
    mkdir -p "$1"
    local hook=""
    [ -n "$3" ] && hook=", \"webhook_url\": \"$3\", \"forward_batch_size\": 1"
    cat > "$1/govern.json" <<EOF
{
    "mode": "${MODE:-enforce}",
    "security": { "sandbox_level": "elevated" },
    "telemetry": { "enabled": true, "output_file": "tele.jsonl"$hook },
    "agents": {
        "worker": {
            "provider": "gemini",
            "model": "stub-model",
            "api_base": "$2",
            "api_key_env": "EGRESS_TEST_KEY",
            "max_tokens": 64,
            "max_turns": 5,
            "system_prompt": "You answer briefly."
        }
    }
}
EOF
}

# $1=dir $2=engine(vm|tw) $3=program; extra env for the process via $RUN_ENV.
run() {
    local d="$1" eng="$2" prog="$3" flag=""
    [ "$eng" = tw ] && flag="--tree-walk"
    printf '%s\n' "$prog" > "$d/p.naab"
    OUT="$(cd "$d" && env ${RUN_ENV:-} timeout 60 "$NAAB" run $flag p.naab 2>&1)"; RC=$?
}

SET_PROXY='    env.set("http_proxy", "'"$DEAD_PROXY"'")
    env.set("https_proxy", "'"$DEAD_PROXY"'")
    env.set("all_proxy", "'"$DEAD_PROXY"'")
    print("SEEN=" + env.get("http_proxy"))'
SET_CA='    env.set("SSL_CERT_FILE", "'"$MISSING_CA"'")
    env.set("CURL_CA_BUNDLE", "'"$MISSING_CA"'")
    print("SEEN=" + env.get("SSL_CERT_FILE"))'

agent_prog() {  # $1 = lines run before the call
    printf 'use agent\nuse env\nmain {\n%s\n    let h = agent.create("worker")\n    let r = agent.send(h, "hello")\n    print("REPLY=" + r.get("content"))\n}\n' "$1"
}
hook_prog() {  # $1 = lines run before the event; the event is sent AFTER them
    printf 'use governance\nuse env\nuse time\nmain {\n%s\n    let g = governance.health()\n    time.sleep(3)\n    print("HOOK_DONE")\n}\n' "$1"
}

# ------------------------------------------------------------------
echo ""
echo "== controls"
d="$TEST_TMP/c0a"; cfg "$d" "$HTTP_BASE" ""
RUN_ENV="" run "$d" vm "$(agent_prog '    print("SEEN=none")')"
c0a=no; [[ "$OUT" == *"REPLY=EGRESS_OK"* ]] && c0a=yes
d="$TEST_TMP/c0b"; cfg "$d" "$HTTPS_BASE" ""
RUN_ENV="SSL_CERT_FILE=$TEST_TMP/cert.pem" run "$d" vm "$(agent_prog '    print("SEEN=none")')"
c0b=no; [[ "$OUT" == *"REPLY=EGRESS_OK"* ]] && c0b=yes
d="$TEST_TMP/c0c"; cfg "$d" "$HTTP_BASE" "$HTTP_BASE/hook"
: > "$TEST_TMP/plain.log"
RUN_ENV="" run "$d" vm "$(hook_prog '    print("SEEN=none")')"
c0c=no; grep -q '^/hook .*GOVERNANCE_HEALTH_QUERY' "$TEST_TMP/plain.log" && c0c=yes
if [ "$c0a$c0b$c0c" = yesyesyes ]; then
    pass "EO-00" "agent call over http and https, and the webhook, all reach their stubs with the environment left alone"
else
    fail "EO-00" "a stub was not reached with nothing changed (http=$c0a https=$c0b hook=$c0c)" "$OUT"
    echo "  harness broken; the arms below would measure nothing"; exit 1
fi

d="$TEST_TMP/c1"; cfg "$d" "$HTTP_BASE" ""
RUN_ENV="http_proxy=$DEAD_PROXY" run "$d" vm "$(agent_prog '    print("SEEN=none")')"
if [[ "$OUT" != *"REPLY=EGRESS_OK"* ]] && [ "$RC" -ne 0 ]; then
    pass "EO-01c" "the closed proxy, present at start, breaks the agent call (the snapshot carries the operator's proxy)"
else fail "EO-01c" "a proxy set before start did not reach the agent call -- EO-01 could not fail" "rc=$RC $OUT"; fi

d="$TEST_TMP/c3"; cfg "$d" "$HTTPS_BASE" ""
RUN_ENV="SSL_CERT_FILE=$MISSING_CA" run "$d" vm "$(agent_prog '    print("SEEN=none")')"
if [[ "$OUT" != *"REPLY=EGRESS_OK"* ]] && [ "$RC" -ne 0 ]; then
    pass "EO-03c" "a missing CA bundle, present at start, breaks the https agent call (the CA is consulted)"
else fail "EO-03c" "a CA set before start did not reach the agent call -- EO-03 could not fail" "rc=$RC $OUT"; fi

# ------------------------------------------------------------------
echo ""
echo "== the program changes its environment; the operator's connections do not move"
# The tree-walker is refused under enforce (EO-TW below), so its arms (the
# ...t ids) run under audit: the environment snapshot is taken at load
# whatever the mode, and these arms are about where a connection goes.
mode_for() { [ "$1" = tw ] && echo audit || echo enforce; }
source "$(dirname "$0")/../helpers/tree_walk_refusal.sh"
d="$TEST_TMP/atw"; cfg "$d" "$HTTP_BASE" ""
RUN_ENV="" run "$d" tw "$(agent_prog "$SET_PROXY")"
if tw_refused "$RC" "$OUT" && [[ "$OUT" != *SEEN=* ]]; then
    pass "EO-TW" "under an enforce config --tree-walk is refused before the program runs"
else fail "EO-TW" "--tree-walk ran under an enforce config" "rc=$RC $OUT"; fi
for eng in vm tw; do
    export MODE=$(mode_for $eng)
    id="EO-01"; [ "$eng" = tw ] && id="EO-01t"
    d="$TEST_TMP/a1-$eng"; cfg "$d" "$HTTP_BASE" ""
    RUN_ENV="" run "$d" "$eng" "$(agent_prog "$SET_PROXY")"
    if [[ "$OUT" == *"SEEN=$DEAD_PROXY"* ]] && [[ "$OUT" == *"REPLY=EGRESS_OK"* ]]; then
        pass "$id" "agent call reached its endpoint after the program set *_proxy to a closed port ($eng)"
    else fail "$id" "the program's proxy setting reached the agent call ($eng)" "rc=$RC $OUT"; fi

    id="EO-03"; [ "$eng" = tw ] && id="EO-03t"
    d="$TEST_TMP/a3-$eng"; cfg "$d" "$HTTPS_BASE" ""
    RUN_ENV="SSL_CERT_FILE=$TEST_TMP/cert.pem" run "$d" "$eng" "$(agent_prog "$SET_CA")"
    if [[ "$OUT" == *"SEEN=$MISSING_CA"* ]] && [[ "$OUT" == *"REPLY=EGRESS_OK"* ]]; then
        pass "$id" "https agent call kept the operator's CA after the program pointed SSL_CERT_FILE elsewhere ($eng)"
    else fail "$id" "the program's CA setting reached the agent call ($eng)" "rc=$RC $OUT"; fi
done
unset MODE

d="$TEST_TMP/a2"; cfg "$d" "$HTTP_BASE" "$HTTP_BASE/hook"
: > "$TEST_TMP/plain.log"
RUN_ENV="" run "$d" vm "$(hook_prog "$SET_PROXY")"
if [[ "$OUT" == *"SEEN=$DEAD_PROXY"* ]] && [[ "$OUT" == *"HOOK_DONE"* ]] &&
   grep -q '^/hook .*GOVERNANCE_HEALTH_QUERY' "$TEST_TMP/plain.log"; then
    pass "EO-02" "an event sent after the program set *_proxy still reached the webhook"
else fail "EO-02" "the program's proxy setting reached the webhook" "rc=$RC $OUT || log: $(head -c 300 "$TEST_TMP/plain.log")"; fi

d="$TEST_TMP/a4"; cfg "$d" "$HTTP_BASE" "$HTTPS_BASE/hook"
: > "$TEST_TMP/tls.log"
RUN_ENV="SSL_CERT_FILE=$TEST_TMP/cert.pem" run "$d" vm "$(hook_prog "$SET_CA")"
if [[ "$OUT" == *"SEEN=$MISSING_CA"* ]] && [[ "$OUT" == *"HOOK_DONE"* ]] &&
   grep -q '^/hook .*GOVERNANCE_HEALTH_QUERY' "$TEST_TMP/tls.log"; then
    pass "EO-04" "an event sent after the program moved SSL_CERT_FILE still reached the https webhook"
else fail "EO-04" "the program's CA setting reached the webhook" "rc=$RC $OUT || log: $(head -c 300 "$TEST_TMP/tls.log")"; fi

# ------------------------------------------------------------------
echo ""
echo "== the program's own http requests use the proxy the process started with"
PROBE_PROXY="http://naab-proxy-probe.invalid:3128"
hcfg() {
    mkdir -p "$1"
    cat > "$1/govern.json" <<EOF
{
    "mode": "${MODE:-enforce}",
    "security": { "sandbox_level": "elevated" },
    "capabilities": { "network": { "enabled": true, "allowed_hosts": ["listed-host.test"] } }
}
EOF
}
http_prog() {  # $1 = lines run before the request
    printf 'use http\nuse env\nmain {\n%s\n    try { let r = http.get("http://listed-host.test/") print("GOT") } catch (e) { print("ERR=" + e) }\n}\n' "$1"
}
d="$TEST_TMP/c5"; hcfg "$d"
RUN_ENV="http_proxy=$PROBE_PROXY" run "$d" vm "$(http_prog '    print("SEEN=none")')"
if [[ "$OUT" == *"resolve proxy"* ]]; then
    pass "EO-05c" "a proxy set before start carries http.get (curl names the proxy it could not resolve)"
else fail "EO-05c" "the operator's proxy did not reach http.get, or the probe cannot tell proxy from host" "rc=$RC $OUT"; fi
for eng in vm tw; do
    id="EO-05"; [ "$eng" = tw ] && id="EO-05t"
    d="$TEST_TMP/a5-$eng"; MODE=$(mode_for $eng) hcfg "$d"
    RUN_ENV="" run "$d" "$eng" "$(http_prog '    env.set("http_proxy", "'"$PROBE_PROXY"'")
    env.set("all_proxy", "'"$PROBE_PROXY"'")
    print("SEEN=" + env.get("http_proxy"))')"
    if [[ "$OUT" == *"SEEN=$PROBE_PROXY"* ]] && [[ "$OUT" == *"resolve host"* ]] && [[ "$OUT" != *"resolve proxy"* ]]; then
        pass "$id" "http.get went to the URL's host, not through the proxy the program set ($eng)"
    else fail "$id" "the program's proxy setting routed its http request ($eng)" "rc=$RC $OUT"; fi
done

# ------------------------------------------------------------------
echo ""
echo "== the program cannot switch the operator's proxy off (no_proxy)"
# The operator's proxy here is the closed port, so a call that honours it
# FAILS. Setting no_proxy=* from the program must not let the call go direct.
# EO-01c (same proxy at start, nothing set) is the matched control that this
# proxy breaks the call; EO-00 that the call works with no proxy at all.
for eng in vm tw; do
    id="EO-06"; [ "$eng" = tw ] && id="EO-06t"
    d="$TEST_TMP/a6-$eng"; MODE=$(mode_for $eng) cfg "$d" "$HTTP_BASE" ""
    RUN_ENV="http_proxy=$DEAD_PROXY" run "$d" "$eng" "$(agent_prog '    env.set("no_proxy", "*")
    env.set("NO_PROXY", "*")
    print("SEEN=" + env.get("no_proxy"))')"
    if [[ "$OUT" == *"SEEN=*"* ]] && [[ "$OUT" != *"REPLY=EGRESS_OK"* ]] && [ "$RC" -ne 0 ]; then
        pass "$id" "no_proxy set by the program did not take the agent call off the operator's proxy ($eng)"
    else fail "$id" "the program's no_proxy took the agent call off the operator's proxy ($eng)" "rc=$RC $OUT"; fi
done
d="$TEST_TMP/a6h"; hcfg "$d"
RUN_ENV="http_proxy=$PROBE_PROXY" run "$d" vm "$(http_prog '    env.set("no_proxy", "*")
    env.set("NO_PROXY", "*")
    print("SEEN=" + env.get("no_proxy"))')"
if [[ "$OUT" == *"SEEN=*"* ]] && [[ "$OUT" == *"resolve proxy"* ]]; then
    pass "EO-06h" "no_proxy set by the program did not take http.get off the operator's proxy"
else fail "EO-06h" "the program's no_proxy took http.get off the operator's proxy" "rc=$RC $OUT"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP unmeasurable"
[ "$FAIL" -eq 0 ]
