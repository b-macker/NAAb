#!/usr/bin/env bash
# ============================================================
# test_guidance_connectors.sh — guidance from an https URL or a command,
# fetched whole and governed (docs/plan-live-guidance.md, "Connectors, slice 3a")
#
# An external source is fetched WHOLE in the background into a cache and
# selected locally. Nothing from the run is ever sent to it. These arms pin
# the decisions recorded in the plan, each refusal paired with a success
# through the same fixture.
#
#   https       C-01 JSON rows delivered (pull), with the refresh telemetry;
#               C-01c no source -> nothing (control)
#               C-02 a markdown document over https
#               C-03 a violation found AT LOAD carries fetched guidance (the
#                    first fetch finishes before static checks)
#   policy      C-04 network.enabled false -> refused, nothing delivered
#               C-05 blocked_hosts / allowed_hosts refuse; an allowed host
#                    passes (control)
#               C-06 https_only refuses plain http
#               C-07 a redirect is not followed
#               C-08 cloud metadata and link-local addresses are refused on the
#                    address actually connected to; a private (RFC 1918)
#                    address is NOT refused (control: it fails to connect,
#                    it is not refused)
#   credential  C-09 auth_env sends a bearer token; the program cannot read
#                    that variable (env.get HARD-blocked); another var reads fine
#   live        C-10 a changed document reaches a pull mid-run, and the
#                    program's env.set (a dead proxy) does not steer the refresh
#               C-21 telemetry never carries a URL's query string
#   command     C-11 JSON rows from a command delivered -- also with shell and
#                    network disabled (it runs like a hook, decision b)
#               C-12 the command receives nothing from the run: empty stdin,
#                    and the load-time environment (the program's env.set is
#                    invisible to it)
#               C-13 a hung command is killed at its timeout, with what it
#                    started (process group)
#               C-14 a failing command -> nothing delivered; C-15 a later
#                    failure keeps the last good copy
#               C-17 the note that host lists cannot bind a command source
#               C-18 naab-gov (a static checker) never runs a command source;
#                    naab-lang run does (control)
#               C-19 output over max_source_kb -> refused
#   engines     C-22 the tree-walker delivers the same fetched guidance
#               C-23 one run fetches once: a command source runs exactly once
#                    per run on BOTH engines (the tree-walker holds two engines
#                    and an async interpreter a third; each used to fetch)
#               C-24 a mid-run reload that disables the network stops the next
#                    refresh and drops the copy, on both engines; C-24c control:
#                    a reload that changes nothing the policy reads keeps it
#   self      C-25 the program cannot rewrite a command source's script -- it
#                    runs with the operator's authority, so a rewrite would be
#                    program code run outside the sandbox (measured before the
#                    fix); C-25c a file beside it stays writable
#             C-26 a command source in a directory the program may write is
#                    refused (A36, decision (b)): the json.py the program writes
#                    beside it never runs; C-26c the same script in a blocked
#                    directory is fetched; C-26b a read-only mode protects it
#
# Windows: command sources are not supported in this build, and the loopback
# test server is not started there -- every arm is UNMEASURABLE.
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
NAAB_GOV="${NAAB_GOV:-$REPO/build/naab-gov}"
TEST_TMP="$(mktemp -d)"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> ${3:0:400}"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }
summary() {
    echo ""
    echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT unmeasurable"
    [ "$FAIL_COUNT" -eq 0 ]
}

# Every config here is an unsigned govern.json: isolate the trust store, or an
# ambient key turns each into an integrity block (CV-02).
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
source "$SCRIPT_DIR/../helpers/config_swap.sh"
setup_isolated_trust
SERVER_PID_FILE="$TEST_TMP/server.pid"
cleanup() {
    if [ -f "$SERVER_PID_FILE" ]; then kill "$(cat "$SERVER_PID_FILE")" 2>/dev/null; fi
    if [ -f "$TEST_TMP/proxy.pid" ]; then kill "$(cat "$TEST_TMP/proxy.pid")" 2>/dev/null; fi
    # Hung-command arms leave nothing behind if the suite works; make sure.
    if [ -f "$TEST_TMP/hang.pids" ]; then
        while read -r p; do kill -9 "$p" 2>/dev/null; done < "$TEST_TMP/hang.pids"
    fi
    command -v stop_swap_operators >/dev/null 2>&1 && stop_swap_operators
    teardown_isolated_trust
    [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"
}
trap cleanup EXIT
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }

# The engine takes its proxy from the environment it starts in. A host that
# sets one (CI does not; a cloud container may) would route the metadata arms
# through it and measure the proxy instead of the address check -- observed:
# C-08 read "Failure when receiving data from the peer" through a container
# proxy. Every arm runs with NO proxy unless it sets one itself (C-08x).
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY all_proxy ALL_PROXY no_proxy NO_PROXY

case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;; *) WINDOWS=0 ;; esac
[ -n "${WINDIR:-}" ] && WINDOWS=1
if [ "$WINDOWS" = 1 ]; then
    skip "C-ALL" "command sources are not supported on Windows and the loopback server is not started there"
    summary; exit $?
fi
python3 -c "import http.server" 2>/dev/null || { skip "C-ALL" "python3 http.server unavailable"; summary; exit $?; }

# ---------------------------------------------------------------------------
# Loopback test server. Serves files from DOCROOT (re-read per request), a
# redirect, and a bearer-protected path; logs every request with its headers.
DOCROOT="$TEST_TMP/www"; mkdir -p "$DOCROOT"
REQLOG="$TEST_TMP/requests.log"; : > "$REQLOG"
cat > "$TEST_TMP/server.py" <<'PY'
import http.server, os, sys, time
docroot, reqlog, portfile, token = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _log(self):
        with open(reqlog, "a") as f:
            f.write("%s %s auth=%s body=%s\n" % (self.command, self.path,
                    self.headers.get("Authorization", "-"), self.headers.get("Content-Length", "0")))
    def do_GET(self):
        self._log()
        path = self.path.split("?", 1)[0]
        if path.startswith("/slow/"):
            time.sleep(0.4); path = path[len("/slow"):]
        if path == "/redirect":
            self.send_response(302); self.send_header("Location", "/g.json"); self.end_headers(); return
        if path.startswith("/auth/"):
            if self.headers.get("Authorization") != "Bearer " + token:
                self.send_response(401); self.end_headers(); return
            path = path[len("/auth"):]
        fp = os.path.join(docroot, path.lstrip("/"))
        if not os.path.isfile(fp):
            self.send_response(404); self.end_headers(); return
        data = open(fp, "rb").read()
        self.send_response(200); self.send_header("Content-Length", str(len(data))); self.end_headers()
        self.wfile.write(data)
    def do_POST(self):
        self._log(); self.send_response(405); self.end_headers()
s = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
open(portfile + ".tmp", "w").write(str(s.server_address[1])); os.replace(portfile + ".tmp", portfile)
s.serve_forever()
PY
# A recording proxy: logs the first line of every request it receives
# (CONNECT host:port for https) and answers 502. It proves which requests a
# proxied fetch actually handed to the proxy.
PROXYLOG="$TEST_TMP/proxy.log"; : > "$PROXYLOG"
cat > "$TEST_TMP/proxy.py" <<'PY'
import socketserver, os, sys
log, portfile = sys.argv[1], sys.argv[2]
class P(socketserver.StreamRequestHandler):
    def handle(self):
        line = self.rfile.readline(4096).decode("latin-1").strip()
        with open(log, "a") as f: f.write(line + "\n")
        self.wfile.write(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
class S(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True
s = S(("127.0.0.1", 0), P)
open(portfile + ".tmp", "w").write(str(s.server_address[1])); os.replace(portfile + ".tmp", portfile)
s.serve_forever()
PY
TOKEN="tok-$RANDOM$RANDOM"
python3 "$TEST_TMP/server.py" "$DOCROOT" "$REQLOG" "$TEST_TMP/port" "$TOKEN" >/dev/null 2>&1 &
echo $! > "$SERVER_PID_FILE"
for _ in $(seq 1 100); do [ -s "$TEST_TMP/port" ] && break; sleep 0.05; done
PORT="$(cat "$TEST_TMP/port" 2>/dev/null)"
if [ -z "$PORT" ]; then skip "C-ALL" "the loopback test server did not start"; summary; exit $?; fi
BASE="http://127.0.0.1:$PORT"
python3 "$TEST_TMP/proxy.py" "$PROXYLOG" "$TEST_TMP/pport" >/dev/null 2>&1 &
echo $! > "$TEST_TMP/proxy.pid"
for _ in $(seq 1 100); do [ -s "$TEST_TMP/pport" ] && break; sleep 0.05; done
PPORT="$(cat "$TEST_TMP/pport" 2>/dev/null)"

rows() {  # $1 file, $2 marker -> a guidance document as JSON rows
    printf '[{"topic": "deployment", "body": "%s: roll back with the rollback script before retrying."},\n {"topic": "No placeholders", "body": "%s_PH: unfinished work goes in stubs/ with a ticket."}]\n' "$2" "$2" > "$1"
}
# cfg DIR SOURCES_JSON [EXTRA_TOP] [EXTRA_GUIDANCE]
# A command source may not live where the program can write (A36, decision
# (b)): every config lists the scripts' directory in blocked_paths, unless the
# arm sets its own filesystem policy (C-25, C-26).
cfg() {
    mkdir -p "$1"
    local top="${3:-}" prot="\"filesystem\": {\"blocked_paths\": [\"$TEST_TMP/bin\"]}"
    if [[ "$top" == *'"filesystem"'* ]]; then :
    elif [[ "$top" == *'"capabilities": {'* ]]; then top="${top/'"capabilities": {'/'"capabilities": {'"$prot, "}"
    else top="\"capabilities\": {$prot}, $top"; fi
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},\n "telemetry": {"enabled": true, "output_file": "tele.jsonl"},\n %s\n "guidance": {"sources": %s %s}}\n' \
        "$top" "$2" "${4:-}" > "$1/govern.json"
}
run() {  # $1 dir, $2 engine, $3 program -> sets OUT, RC
    printf '%s\n' "$3" > "$1/p.naab"
    local a=("$NAAB"); [ "$2" = tw ] && a+=(--tree-walk); a+=(p.naab)
    OUT="$(cd "$1" && timeout 60 "${a[@]}" 2>&1)"; RC=$?
}
PULL='use governance
use array
main {
    let g = governance.guidance("deployment rollback")
    print("N=" + string(array.length(g)))
    print(g)
}'
tele() { cat "$1/tele.jsonl" 2>/dev/null; }

# ---------------------------------------------------------------------------
echo "== https: fetched whole, selected locally"
rows "$DOCROOT/g.json" CONN_HTTPS_m1
d="$TEST_TMP/c01"; cfg "$d" "[\"$BASE/g.json\"]"
run "$d" vm "$PULL"
if [[ "$OUT" == *"N=1"* ]] && [[ "$OUT" == *"CONN_HTTPS_m1: roll back"* ]] &&
   grep -q '"GUIDANCE_SOURCE_REFRESH"' <<<"$(tele "$d")" && grep -q '"outcome":"ok"' <<<"$(tele "$d")"; then
    pass "C-01" "an https JSON document is fetched and a pull delivers its row; GUIDANCE_SOURCE_REFRESH ok"
else fail "C-01" "https rows not delivered" "rc=$RC $OUT"; fi
d="$TEST_TMP/c01c"; cfg "$d" '[]'
run "$d" vm "$PULL"
if [[ "$OUT" == *"N=0"* ]]; then pass "C-01c" "control: with no source the same pull returns nothing"
else fail "C-01c" "control returned guidance" "$OUT"; fi

printf '# Ops\n\n## Deployment rollback\nCONN_MD_m2: roll back first, then retry the deployment.\n' > "$DOCROOT/ops.md"
d="$TEST_TMP/c02"; cfg "$d" "[\"$BASE/ops.md\"]"
run "$d" vm "$PULL"
if [[ "$OUT" == *"CONN_MD_m2"* ]]; then pass "C-02" "a markdown document over https is split at its headings"
else fail "C-02" "markdown over https not delivered" "rc=$RC $OUT"; fi

PLACEHOLDER='fn work() {
    // TODO: finish this later
    return 1
}
main {
    print(work())
}'
QC='"code_quality": {"no_placeholders": {"enabled": true, "level": "hard"}},'
# The source answers after 400 ms: inside the load budget (5 s here, so a loaded
# runner cannot push it out), slower than the static checks. A background-only fetch loses that race; on an instant source
# it wins it, and the arm passed with the initial fetch removed.
d="$TEST_TMP/c03"; cfg "$d" "[\"$BASE/slow/g.json\"]" "$QC" ', "initial_fetch_ms": 5000'
run "$d" vm "$PLACEHOLDER"
# The label is asserted whole: "Project guidance (" alone passed while an
# https source printed an empty name -- "( > ...)".
if [ "$RC" = 3 ] && [[ "$OUT" == *"Project guidance (g > "* ]] && [[ "$OUT" == *"CONN_HTTPS_m1_PH"* ]]; then
    pass "C-03" "a violation found at load shows the fetched row named for its check, under the source's name (first fetch precedes static checks)"
else fail "C-03" "no fetched guidance on the load-time block" "rc=$RC $OUT"; fi

# ---------------------------------------------------------------------------
echo ""
echo "== policy: held to capabilities.network"
d="$TEST_TMP/c04"; cfg "$d" "[\"$BASE/g.json\"]" '"capabilities": {"network": {"enabled": false}},'
run "$d" vm "$PULL"
if [[ "$OUT" == *"N=0"* ]] && [[ "$OUT" == *"was not fetched"* ]] && grep -q '"outcome":"refused"' <<<"$(tele "$d")"; then
    pass "C-04" "network disabled -> the source is refused, nothing delivered, refusal reported"
else fail "C-04" "a disabled network did not refuse the fetch" "rc=$RC $OUT"; fi

d="$TEST_TMP/c05a"; cfg "$d" "[\"$BASE/g.json\"]" '"capabilities": {"network": {"blocked_hosts": ["127.0.0.1"]}},'
run "$d" vm "$PULL"; o1="$OUT"
d="$TEST_TMP/c05b"; cfg "$d" "[\"$BASE/g.json\"]" '"capabilities": {"network": {"allowed_hosts": ["example.com"]}},'
run "$d" vm "$PULL"; o2="$OUT"
d="$TEST_TMP/c05c"; cfg "$d" "[\"$BASE/g.json\"]" '"capabilities": {"network": {"allowed_hosts": ["127.0.0.1"]}},'
run "$d" vm "$PULL"; o3="$OUT"
if [[ "$o1" == *"N=0"* ]] && [[ "$o2" == *"N=0"* ]] && [[ "$o3" == *"N=1"* ]]; then
    pass "C-05" "blocked_hosts refuses, an allowlist without the host refuses, an allowlist with it passes"
else fail "C-05" "host lists not applied" "blocked: ${o1:0:120} | other-allowed: ${o2:0:120} | allowed: ${o3:0:120}"; fi

d="$TEST_TMP/c06"; cfg "$d" "[\"$BASE/g.json\"]" '"capabilities": {"network": {"https_only": true}},'
run "$d" vm "$PULL"
if [[ "$OUT" == *"N=0"* ]] && [[ "$OUT" == *"requires https"* ]]; then
    pass "C-06" "https_only refuses a plain-http source"
else fail "C-06" "https_only not applied" "rc=$RC $OUT"; fi

: > "$REQLOG"
d="$TEST_TMP/c07"; cfg "$d" "[\"$BASE/redirect\"]"
run "$d" vm "$PULL"
if [[ "$OUT" == *"N=0"* ]] && [[ "$OUT" == *"redirect"* ]] && ! grep -q 'GET /g.json' "$REQLOG"; then
    pass "C-07" "a redirect is refused and never followed (the target was not requested)"
else fail "C-07" "redirect followed or not refused" "rc=$RC $OUT / log: $(cat "$REQLOG")"; fi

meta_ok=1; meta_detail=""
for u in "https://169.254.169.254/g.json" "https://100.100.100.200/g.json" "https://168.63.129.16/g.json" "https://[fd00:ec2::254]/g.json" "https://[fe80::1]/g.json"; do
    d="$TEST_TMP/c08-$RANDOM"; cfg "$d" "[{\"url\": \"$u\", \"timeout_ms\": 1500}]"
    run "$d" vm "$PULL"
    if ! { [[ "$OUT" == *"N=0"* ]] && [[ "$OUT" == *"cloud metadata or link-local"* ]]; }; then
        meta_ok=0; meta_detail="$meta_detail | $u: ${OUT:0:160}"
    fi
done
d="$TEST_TMP/c08p"; cfg "$d" '[{"url": "https://10.255.255.1/g.json", "timeout_ms": 400}]'
run "$d" vm "$PULL"; op="$OUT"
if [ "$meta_ok" = 1 ] && [[ "$op" == *"was not fetched"* ]] && [[ "$op" != *"cloud metadata or link-local"* ]]; then
    pass "C-08" "metadata (169.254.169.254, 100.100.100.200, 168.63.129.16, fd00:ec2::254) and link-local refused by address; a private address is not refused"
else fail "C-08" "address refusal wrong" "metadata:$meta_detail || private: ${op:0:200}"; fi

# Through a proxy the socket opened is the proxy's, so the connect-time check
# cannot see the source; the source's address is checked before any request.
if [ -z "$PPORT" ]; then
    skip "C-08x" "the recording proxy did not start"
else
    : > "$PROXYLOG"
    export HTTPS_PROXY="http://127.0.0.1:$PPORT"
    px_ok=1; px_detail=""
    for u in "https://169.254.169.254/g.json" "https://168.63.129.16/g.json" "https://[fd00:ec2::254]/g.json"; do
        d="$TEST_TMP/c08x-$RANDOM"; cfg "$d" "[{\"url\": \"$u\", \"timeout_ms\": 1500}]"
        run "$d" vm "$PULL"
        if ! { [[ "$OUT" == *"N=0"* ]] && [[ "$OUT" == *"cloud metadata or link-local"* ]]; }; then
            px_ok=0; px_detail="$px_detail | $u: ${OUT:0:160}"
        fi
    done
    meta_reached="$(grep -E '169\.254\.169\.254|168\.63\.129\.16|fd00:ec2::254' "$PROXYLOG")"
    # Control: a private address goes TO the proxy (proving this arm measured
    # the proxied path) and is not refused as metadata.
    d="$TEST_TMP/c08xp"; cfg "$d" '[{"url": "https://10.255.255.1/g.json", "timeout_ms": 1500}]'
    run "$d" vm "$PULL"; opx="$OUT"
    unset HTTPS_PROXY
    if [ "$px_ok" = 1 ] && [ -z "$meta_reached" ] && grep -q 'CONNECT 10.255.255.1:443' "$PROXYLOG" &&
       [[ "$opx" == *"was not fetched"* ]] && [[ "$opx" != *"cloud metadata or link-local"* ]]; then
        pass "C-08x" "through a proxy, metadata addresses are refused before the proxy is asked; a private address reaches the proxy (control)"
    else fail "C-08x" "proxied address check wrong" "metadata:$px_detail || proxy log: $(cat "$PROXYLOG") || private: ${opx:0:200}"; fi
fi

# ---------------------------------------------------------------------------
echo ""
echo "== credential: sent to the source, withheld from the program"
mkdir -p "$DOCROOT"; rows "$DOCROOT/a.json" CONN_AUTH_m3
: > "$REQLOG"
d="$TEST_TMP/c09"; cfg "$d" "[{\"url\": \"$BASE/auth/a.json\", \"auth_env\": \"KB_TOKEN_C09\"}]"
export KB_TOKEN_C09="$TOKEN" OTHER_C09="visible_c09"
run "$d" vm "$PULL"; o_pull="$OUT"
run "$d" vm 'use env
main { print("OTHER=" + env.get("OTHER_C09")) }'; o_other="$OUT"; rc_other=$RC
run "$d" vm 'use env
main { print("TOK=" + env.get("KB_TOKEN_C09")) }'; o_tok="$OUT"; rc_tok=$RC
unset KB_TOKEN_C09 OTHER_C09
if [[ "$o_pull" == *"CONN_AUTH_m3"* ]] && grep -q "auth=Bearer $TOKEN" "$REQLOG" &&
   [ "$rc_tok" = 3 ] && [[ "$o_tok" != *"$TOKEN"* ]] && [ "$rc_other" = 0 ] && [[ "$o_other" == *"OTHER=visible_c09"* ]]; then
    pass "C-09" "the bearer token reaches the source; env.get of its variable is HARD-blocked; another variable reads fine"
else fail "C-09" "credential handling wrong" "pull: ${o_pull:0:120} | tok rc=$rc_tok ${o_tok:0:120} | other rc=$rc_other ${o_other:0:80} | log: $(cat "$REQLOG")"; fi

# ---------------------------------------------------------------------------
echo ""
echo "== live refresh, and the program cannot steer it"
rows "$DOCROOT/live.json" CONN_LIVE_v1
d="$TEST_TMP/c10"; cfg "$d" "[{\"url\": \"$BASE/live.json?token=SHOULD_NOT_APPEAR_c21\", \"refresh_seconds\": 5}]"
# v2 is written only once the load-time fetch has been served (not after a
# fixed delay, which a slow startup on a loaded runner can outlast).
( for _ in $(seq 1 300); do grep -q 'GET /live.json' "$REQLOG" && break; sleep 0.1; done
  sleep 0.3; rows "$DOCROOT/live.json" CONN_LIVE_v2 ) &
run "$d" vm 'use governance
use env
use time
use array
main {
    let first = governance.guidance("deployment rollback")
    print("FIRST=" + first[0].get("text"))
    env.set("http_proxy", "http://127.0.0.1:9")
    env.set("HTTP_PROXY", "http://127.0.0.1:9")
    env.set("all_proxy", "http://127.0.0.1:9")
    let i = 0
    let seen = ""
    while i < 30 {
        time.sleep(0.5)
        let g = governance.guidance("deployment rollback")
        if array.length(g) > 0 { seen = g[0].get("text") }
        if seen.contains("CONN_LIVE_v2") { i = 99 } else { i = i + 1 }
    }
    print("LAST=" + seen)
}'
if [[ "$OUT" == *"FIRST=CONN_LIVE_v1"* ]] && [[ "$OUT" == *"LAST=CONN_LIVE_v2"* ]]; then
    pass "C-10" "a changed document reached a pull mid-run, through a refresh the program's env.set (dead proxy) did not steer"
else fail "C-10" "live refresh missing or steered by the program's environment" "rc=$RC $OUT"; fi
if grep -q '"GUIDANCE_SOURCE_REFRESH"' <<<"$(tele "$d")" && ! grep -q 'SHOULD_NOT_APPEAR_c21' <<<"$(tele "$d")" &&
   grep -q '"target":"http://127.0.0.1:'"$PORT"'/live.json"' <<<"$(tele "$d")"; then
    pass "C-21" "telemetry names the source without its query string"
else fail "C-21" "URL query string leaked into telemetry, or no refresh event" "$(grep GUIDANCE_SOURCE_REFRESH "$d/tele.jsonl" | head -2)"; fi

# ---------------------------------------------------------------------------
echo ""
echo "== command sources: run like a hook, given nothing"
mkdir -p "$TEST_TMP/bin"
cat > "$TEST_TMP/bin/rows.sh" <<'SH'
#!/bin/sh
# What the command sees: its stdin and a variable the PROGRAM sets.
cat > "${PROBE_DIR:-/tmp}/stdin.seen" 2>/dev/null
echo "${PROBE_VAR:-unset}" > "${PROBE_DIR:-/tmp}/env.seen"
printf '[{"topic": "deployment", "body": "CONN_CMD_m4: roll back with the rollback script."}]\n'
SH
chmod +x "$TEST_TMP/bin/rows.sh"
d="$TEST_TMP/c11"; mkdir -p "$d"
export PROBE_DIR="$d" PROBE_VAR="load_time_c12"
cfg "$d" "[{\"command\": [\"$TEST_TMP/bin/rows.sh\"], \"refresh_seconds\": 5}]" '"capabilities": {"shell": {"enabled": false}, "network": {"enabled": false}},'
run "$d" vm 'use governance
use env
use time
use array
main {
    env.set("PROBE_VAR", "program_set_c12")
    time.sleep(6.5)
    let g = governance.guidance("deployment rollback")
    print("N=" + string(array.length(g)))
    print(g)
}'
unset PROBE_DIR PROBE_VAR
if [[ "$OUT" == *"CONN_CMD_m4"* ]]; then
    pass "C-11" "a command source's rows are delivered, with shell and network disabled for the program"
else fail "C-11" "command rows not delivered" "rc=$RC $OUT"; fi
if [ -f "$d/env.seen" ] && [ "$(cat "$d/env.seen")" = "load_time_c12" ] && [ ! -s "$d/stdin.seen" ]; then
    pass "C-12" "the command saw empty stdin and the load-time environment, not the program's env.set (after a refresh)"
else fail "C-12" "the command saw run data" "env.seen=$(cat "$d/env.seen" 2>/dev/null) stdin=$(head -c 80 "$d/stdin.seen" 2>/dev/null)"; fi

cat > "$TEST_TMP/bin/hang.sh" <<'SH'
#!/bin/sh
sleep 60 &
echo $! >> "$HANG_PIDS"
echo $$ >> "$HANG_PIDS"
sleep 60
SH
chmod +x "$TEST_TMP/bin/hang.sh"
export HANG_PIDS="$TEST_TMP/hang.pids"; : > "$HANG_PIDS"
d="$TEST_TMP/c13"; cfg "$d" "[{\"command\": [\"$TEST_TMP/bin/hang.sh\"], \"timeout_ms\": 600}]"
t0=$(date +%s)
run "$d" vm "$PULL"
t1=$(date +%s)
unset HANG_PIDS
sleep 0.3
alive=0
while read -r p; do
    [ -n "$p" ] || continue
    st="$(ps -o stat= -p "$p" 2>/dev/null | tr -d ' ')"
    if [ -n "$st" ] && [[ "$st" != Z* ]]; then alive=1; fi
done < "$TEST_TMP/hang.pids"
if [[ "$OUT" == *"N=0"* ]] && [[ "$OUT" == *"ran longer than 600 ms"* ]] && [ "$alive" = 0 ] && [ $((t1 - t0)) -lt 20 ] &&
   [ "$(grep -c . "$TEST_TMP/hang.pids")" -ge 2 ]; then
    pass "C-13" "a hung command is killed at its timeout together with the process it started; the run was not held"
else fail "C-13" "hung command not contained" "rc=$RC alive=$alive secs=$((t1 - t0)) pids=$(tr '\n' ' ' < "$TEST_TMP/hang.pids") $OUT"; fi

printf '#!/bin/sh\necho "partial"\nexit 3\n' > "$TEST_TMP/bin/fail.sh"; chmod +x "$TEST_TMP/bin/fail.sh"
d="$TEST_TMP/c14"; cfg "$d" "[{\"command\": [\"$TEST_TMP/bin/fail.sh\"]}]"
run "$d" vm "$PULL"
if [[ "$OUT" == *"N=0"* ]] && [[ "$OUT" == *"exited 3"* ]]; then
    pass "C-14" "a command that exits non-zero delivers nothing and says why"
else fail "C-14" "failing command handled wrongly" "rc=$RC $OUT"; fi

cat > "$TEST_TMP/bin/flaky.sh" <<'SH'
#!/bin/sh
if [ -f "$FLAKY_FLAG" ]; then exit 4; fi
printf '[{"topic": "deployment", "body": "CONN_KEEP_m5: roll back first."}]\n'
SH
chmod +x "$TEST_TMP/bin/flaky.sh"
export FLAKY_FLAG="$TEST_TMP/flaky.flag"; rm -f "$FLAKY_FLAG"
d="$TEST_TMP/c15"; cfg "$d" "[{\"command\": [\"$TEST_TMP/bin/flaky.sh\"], \"refresh_seconds\": 5}]"
( sleep 1; : > "$FLAKY_FLAG" ) &
run "$d" vm 'use governance
use time
use array
main {
    time.sleep(7)
    let g = governance.guidance("deployment rollback")
    print("N=" + string(array.length(g)))
    print(g)
}'
unset FLAKY_FLAG
if [[ "$OUT" == *"CONN_KEEP_m5"* ]] && [[ "$OUT" == *"last copy is still used"* ]] &&
   grep -q '"kept_previous_copy":"true"' <<<"$(tele "$d")"; then
    pass "C-15" "a refresh that fails keeps the last good copy and says so"
else fail "C-15" "last good copy not kept" "rc=$RC $OUT"; fi

d="$TEST_TMP/c17"; cfg "$d" "[{\"command\": [\"$TEST_TMP/bin/rows.sh\"]}]" '"capabilities": {"network": {"allowed_hosts": ["example.com"]}},'
run "$d" vm "$PULL"; o_note="$OUT"
d="$TEST_TMP/c17c"; cfg "$d" "[{\"command\": [\"$TEST_TMP/bin/rows.sh\"]}]"
run "$d" vm "$PULL"
if [[ "$o_note" == *"cannot bind the connections it makes"* ]] && [[ "$OUT" != *"cannot bind the connections"* ]]; then
    pass "C-17" "the host-list note appears only when host lists are set beside a command source"
else fail "C-17" "host-list note wrong" "with lists: ${o_note:0:200} | without: ${OUT:0:200}"; fi

# The marker path is an ARGUMENT, not an environment variable: naab-gov takes
# no environment snapshot, so an env-carried marker could not show it running.
# The program holds a placeholder under a hard no_placeholders, so naab-gov's
# exit 3 proves it loaded THIS config and ran its checks -- without that, a
# naab-gov that never got as far as the config passes (the first version of
# this arm called "naab-gov check p.naab", a usage error, and passed so).
printf '#!/bin/sh\ntouch "$1"\nprintf %s\n' "'[{\"topic\": \"deployment\", \"body\": \"x\"}]'" > "$TEST_TMP/bin/mark.sh"; chmod +x "$TEST_TMP/bin/mark.sh"
d="$TEST_TMP/c18"; cfg "$d" "[{\"command\": [\"$TEST_TMP/bin/mark.sh\", \"$d/ran\"]}]" "$QC"
printf '%s\n' "$PLACEHOLDER" > "$d/p.naab"
if [ -x "$NAAB_GOV" ]; then
    gov_out="$(cd "$d" && "$NAAB_GOV" lint p.naab 2>&1)"; gov_rc=$?
    sleep 0.5
    gov_ran=no; [ -e "$d/ran" ] && gov_ran=yes
    run "$d" vm "$PLACEHOLDER"
    run_ran=no; [ -e "$d/ran" ] && run_ran=yes
    if [ "$gov_rc" = 3 ] && [ "$gov_ran" = no ] && [ "$run_ran" = yes ]; then
        pass "C-18" "naab-gov lint (config loaded: exit 3) never runs a command source; naab-lang run does (control)"
    else fail "C-18" "command source ran in the wrong process, or naab-gov never loaded the config" "gov rc=$gov_rc ran=$gov_ran, run ran=$run_ran | ${gov_out:0:200}"; fi
else skip "C-18" "naab-gov not built"; fi

# A command runs with the OPERATOR's authority, outside the sandbox. Measured
# before the fix, with shell disabled: file.write over this script succeeded
# and the next refresh ran the program's code. The script is now a source the
# program may not write; a file beside it stays writable (C-25c), so the
# refusal is per file, not a directory lock.
d="$TEST_TMP/c25"; mkdir -p "$d"
printf '#!/bin/sh\nprintf %s\n' "'[{\"topic\": \"deployment\", \"body\": \"CONN_SELF_m25\"}]'" > "$d/gen.sh"; chmod +x "$d/gen.sh"
cfg "$d" '[{"command": ["./gen.sh"]}]' '"capabilities": {"shell": {"enabled": false}, "filesystem": {"mode": "write"}},'
before="$(cat "$d/gen.sh")"
run "$d" vm 'use file
main {
    file.write("gen.sh", "#!/bin/sh\ntouch WRITTEN_BY_PROGRAM\n")
    print("WROTE")
}'
if [ "$RC" = 3 ] && [[ "$OUT" == *"Writing a project guidance file is blocked: gen.sh"* ]] &&
   [[ "$OUT" != *"WROTE"* ]] && [ "$(cat "$d/gen.sh")" = "$before" ]; then
    pass "C-25" "the program cannot rewrite a command source's script (it would run as the operator)"
else fail "C-25" "command source script writable by the program" "rc=$RC ${OUT:0:300}"; fi
run "$d" vm 'use file
main {
    file.write("notes.txt", "beside the script")
    print("WROTE")
}'
if [ "$RC" = 0 ] && [[ "$OUT" == *"WROTE"* ]] && [ -f "$d/notes.txt" ]; then
    pass "C-25c" "control: a file beside the script is still writable (per file, not a directory lock)"
else fail "C-25c" "the refusal is wider than the script" "rc=$RC ${OUT:0:300}"; fi

# Decision (b), A36: a command source may not LIVE where the program can write.
# Measured before it, with shell disabled: a json.py the program wrote beside
# gen.py was imported by the next refresh and ran as the operator. C-26 is
# that route end to end: the source is refused, so the program's file never
# runs. C-26c: the same script in a directory the policy blocks is fetched;
# C-26b: a read-only filesystem mode protects it too (the other branch).
c26_src() {  # $1 dir
    mkdir -p "$1/tools"
    printf 'import json\nprint(json.dumps([{"topic": "deployment", "body": "CONN_DIR_m26 roll back first"}]))\n' > "$1/tools/gen.py"
}
d="$TEST_TMP/c26"; c26_src "$d"
cfg "$d" '[{"command": ["python3", "tools/gen.py"], "refresh_seconds": 5}]' '"capabilities": {"shell": {"enabled": false}, "filesystem": {"mode": "write"}},'
run "$d" vm 'use file
use time
use governance
use array
main {
    file.write("tools/json.py", "open(\"SIBLING_RAN\", \"w\").write(\"x\")\ndef dumps(o):\n    return \"[]\"\n")
    time.sleep(7)
    let g = governance.guidance("deployment rollback")
    print("N=" + string(array.length(g)))
}'
if [[ "$OUT" == *"N=0"* ]] && [[ "$OUT" == *"the program may write in $d/tools"* ]] && [ ! -e "$d/SIBLING_RAN" ] &&
   [ -f "$d/tools/json.py" ]; then
    pass "C-26" "a command source in a directory the program may write is refused: the file the program put beside it never runs"
else fail "C-26" "a source where the program can write was run" "rc=$RC sibling_ran=$([ -e "$d/SIBLING_RAN" ] && echo yes || echo no) ${OUT:0:300}"; fi
d="$TEST_TMP/c26c"; c26_src "$d"
cfg "$d" '[{"command": ["python3", "tools/gen.py"]}]' "\"capabilities\": {\"filesystem\": {\"mode\": \"write\", \"blocked_paths\": [\"$d/tools\"]}},"
run "$d" vm "$PULL"
if [[ "$OUT" == *"CONN_DIR_m26"* ]] && [[ "$OUT" != *"the program may write in"* ]]; then
    pass "C-26c" "control: the same script in a directory the policy blocks is fetched"
else fail "C-26c" "a protected source was refused" "rc=$RC ${OUT:0:300}"; fi
d="$TEST_TMP/c26b"; c26_src "$d"
cfg "$d" '[{"command": ["python3", "tools/gen.py"]}]' '"capabilities": {"filesystem": {"mode": "read"}},'
run "$d" vm "$PULL"
if [[ "$OUT" == *"CONN_DIR_m26"* ]] && [[ "$OUT" != *"the program may write in"* ]]; then
    pass "C-26b" "a read-only filesystem mode protects the source's directory too"
else fail "C-26b" "read-only mode not treated as protected" "rc=$RC ${OUT:0:300}"; fi

printf '#!/bin/sh\nhead -c 300000 /dev/zero | tr "\\\\0" a\n' > "$TEST_TMP/bin/big.sh"; chmod +x "$TEST_TMP/bin/big.sh"
d="$TEST_TMP/c19"; cfg "$d" "[{\"command\": [\"$TEST_TMP/bin/big.sh\"]}]" "" ', "max_source_kb": 64'
run "$d" vm "$PULL"
if [[ "$OUT" == *"N=0"* ]] && [[ "$OUT" == *"larger than 64 KB"* ]]; then
    pass "C-19" "output over max_source_kb is refused"
else fail "C-19" "size cap not applied" "rc=$RC ${OUT:0:300}"; fi

# ---------------------------------------------------------------------------
echo ""
echo "== engines"
d="$TEST_TMP/c22"; cfg "$d" "[\"$BASE/g.json\"]"
run "$d" tw "$PULL"
if [[ "$OUT" == *"CONN_HTTPS_m1: roll back"* ]]; then
    pass "C-22" "the tree-walker delivers the same fetched guidance"
else fail "C-22" "tree-walker did not deliver" "rc=$RC $OUT"; fi

# One fetch per run, whichever engine runs it. The command appends a line per
# execution; the async futures make the tree-walker build one interpreter (and
# engine) per call, and each future pulls guidance too.
cat > "$TEST_TMP/bin/count.sh" <<'SH'
#!/bin/sh
echo x >> "$1"
printf '[{"topic": "deployment", "body": "CONN_ONCE_m6: roll back first."}]\n'
SH
chmod +x "$TEST_TMP/bin/count.sh"
ONCE_PROG='use governance
use array
async fn work(n) {
    let g = governance.guidance("deployment rollback")
    return array.length(g) + n
}
main {
    let g = governance.guidance("deployment rollback")
    print("N=" + string(array.length(g)))
    let f1 = work(10)
    let f2 = work(20)
    print("ASYNC=" + string(await f1) + "," + string(await f2))
}'
once_detail=""; once_ok=1
for eng in vm tw; do
    d="$TEST_TMP/c23-$eng"; mkdir -p "$d"
    cfg "$d" "[{\"command\": [\"$TEST_TMP/bin/count.sh\", \"$d/calls.log\"]}]"
    run "$d" "$eng" "$ONCE_PROG"
    calls="$(grep -c x "$d/calls.log" 2>/dev/null || echo 0)"
    if ! { [[ "$OUT" == *"N=1"* ]] && [[ "$OUT" == *"ASYNC=11,21"* ]] && [ "$calls" = 1 ]; }; then
        once_ok=0
    fi
    once_detail="$once_detail | $eng: calls=$calls ${OUT:0:120}"
done
if [ "$once_ok" = 1 ]; then
    pass "C-23" "a command source runs exactly once per run on both engines, and main and async code both receive its rows"
else fail "C-23" "a run fetched more (or less) than once" "$once_detail"; fi

# A mid-run reload that disables the network must stop the next refresh and
# drop the cached copy. Under --tree-walk the reload lands on the interpreter's
# engine while the CLI's engine keeps the old rules: the refusal must follow
# the engine that tightened. The <<sql>> block is only there to trigger the
# reload check (it runs in-process, so it needs neither shell nor network).
rows "$DOCROOT/reload.json" CONN_RELOAD_m7
reload_prog() {  # $1 replacement config -> a program that swaps it in mid-run
    cat <<EOF2
use governance
use file
use time
use array
main {
    let a = governance.guidance("deployment rollback")
    print("BEFORE=" + string(array.length(a)))
    file.write(".swap_req", "$1\t\$DIR/govern.json\n")
    file.write(".swap_req.ready", "")
    let waited = 0
    while !file.exists(".swap_done") && waited < 200 { time.sleep(0.05); waited = waited + 1 }
    let r = <<sql
SELECT 1 AS x
>>
    let n = 1
    let i = 0
    while i < 40 {
        let g = governance.guidance("deployment rollback")
        n = array.length(g)
        if n == 0 { i = 99 } else { time.sleep(0.25); i = i + 1 }
    }
    print("AFTER=" + string(n))
}
EOF2
}
reload_ok=1; reload_detail=""; ctl_ok=1; ctl_detail=""
for eng in vm tw; do
    for kind in tight neutral; do
        d="$TEST_TMP/c24-$eng-$kind"; mkdir -p "$d"
        cfg "$d" "[{\"url\": \"$BASE/reload.json\", \"refresh_seconds\": 5}]"
        if [ "$kind" = tight ]; then
            cfg "$TEST_TMP/c24-new-$eng-$kind" "[{\"url\": \"$BASE/reload.json\", \"refresh_seconds\": 5}]" '"capabilities": {"network": {"enabled": false}},'
        else
            cfg "$TEST_TMP/c24-new-$eng-$kind" "[{\"url\": \"$BASE/reload.json\", \"refresh_seconds\": 5}]" "" ', "cooldown_turns": 4'
        fi
        touch -d '2 minutes ago' "$d/govern.json" 2>/dev/null || true
        start_swap_operator "$d"
        prog="$(reload_prog "$TEST_TMP/c24-new-$eng-$kind/govern.json")"
        run "$d" "$eng" "${prog//\$DIR/$d}"
        reloaded=no; [[ "$OUT" == *"Config reloaded mid-run"* ]] && reloaded=yes
        if [ "$kind" = tight ]; then
            if ! { [[ "$OUT" == *"BEFORE=1"* ]] && [[ "$OUT" == *"AFTER=0"* ]] && [ "$reloaded" = yes ] &&
                   [[ "$OUT" == *"disables network access"* ]]; }; then
                reload_ok=0
            fi
            reload_detail="$reload_detail | $eng: reloaded=$reloaded $(grep -oE 'BEFORE=[0-9]+|AFTER=[0-9]+' <<<"$OUT" | tr '\n' ' ')"
        else
            if ! { [[ "$OUT" == *"BEFORE=1"* ]] && [[ "$OUT" == *"AFTER=1"* ]] && [ "$reloaded" = yes ]; }; then
                ctl_ok=0
            fi
            ctl_detail="$ctl_detail | $eng: reloaded=$reloaded $(grep -oE 'BEFORE=[0-9]+|AFTER=[0-9]+' <<<"$OUT" | tr '\n' ' ')"
        fi
    done
done
if [ "$ctl_ok" = 1 ]; then
    pass "C-24c" "control: a reload that changes nothing the policy reads keeps the copy (both engines)"
else fail "C-24c" "control failed: the reload itself lost the copy, or did not happen" "$ctl_detail"; fi
if [ "$reload_ok" = 1 ] && [ "$ctl_ok" = 1 ]; then
    pass "C-24" "a mid-run reload that disables the network drops the copy at the next refresh, on both engines"
else fail "C-24" "a tightening reload did not stop the fetch" "$reload_detail"; fi

summary
