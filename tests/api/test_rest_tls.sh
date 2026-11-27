#!/usr/bin/env bash
# ============================================================
# test_rest_tls.sh -- naab-lang api serves HTTPS from govern.json's
# api.tls_cert / api.tls_key (Bundle 19 PR 6, the owner's decision 28), and
# never falls back to plain HTTP. Plus two refusals found while building it:
# a govern.json that does not load no longer starts the server (A232,
# decision 31), and an api value the loader would drop refuses the config
# (A233, decision 32).
#
# Before this, both keys were parsed and read by nothing: the server was a
# plain httplib::Server, so an operator who set a certificate sent every API
# key in plain text (step 3 warned at load instead). The client is Python's
# ssl module trusting ONLY the suite's own self-signed certificate, so a 200
# over https proves the server presented that certificate; the certificate
# reaches the client as text (TLS_CA_PEM), never as a path, because a native
# Windows python cannot open an MSYS path.
#
#   TL-00  CONTROL: no TLS keys -> plain HTTP answers, and the https client
#          fails against it (the client can tell the two apart)
#   TL-00b the plain server says requests and keys travel unencrypted
#   TL-01  both keys -> HTTPS answers, verified against the suite's cert, and
#          the presented certificate is that cert
#   TL-02  plain HTTP to the HTTPS port gets no HTTP response
#   TL-03  the API key still decides over HTTPS (401 without, 200 with)
#   TL-04  relative paths are relative to govern.json, not the server's cwd
#   TL-05a/b one key without the other refuses to start (exit 4)
#   TL-06  a certificate file that does not exist refuses to start
#   TL-07  a key that does not match the certificate refuses to start
#   TL-08  an encrypted key refuses to start; TL-08p under a terminal, where
#          OpenSSL's default callback would prompt (UNMEASURABLE without
#          `script`)
#   TL-09  a certificate file that is not PEM refuses to start
#   TL-10  A232: a govern.json that does not load refuses to start, with and
#          without --no-governance; TL-10c CONTROL: no govern.json at all
#          still starts
#   TL-11  A233: each api value the loader would drop refuses the config;
#          TL-11c CONTROL: the well-typed forms load and a scoped key is
#          refused outside its scope
#   TL-12  an /execute program cannot read the TLS private key; TL-12c
#          CONTROL: it reads the certificate (POSIX: /execute is 501 on Windows)
#   TL-13  setting the TLS keys draws no "is read by nothing" warning;
#          TL-13c CONTROL: a still-warned setting warns in the same run
#   TL-14  `naab-lang version` says HTTPS is available (every arm above that
#          serves HTTPS is UNMEASURABLE when it is not)
#
# KILL TABLE (each mutant run 2026-11-27, one build each; each turned its arms red)
#   K1  makeServer() always builds a plain server          -> TL-01, TL-02, TL-03, TL-04, TL-12
#   K2  main.cpp skips checkTlsFiles() (makeServer() then
#       throws: exit 1, not the refusal)                   -> TL-05a/b, TL-06..TL-09, TL-08p
#   K3  paths resolved against the cwd, not govern.json    -> TL-04
#   K4  the A232 refusal removed                           -> TL-10, TL-11 (a refused config
#       reaches the server only as one that did not load)
#   K5  the api type checks removed                        -> TL-11
#   K6  the TLS key not added to the protected paths       -> TL-12
#   K7  the two warn-table entries kept                    -> TL-13
#   K8  the no-passphrase callback removed                 -> TL-08p
#   Not mutated: SSL_CTX_set_min_proto_version (OpenSSL 3's default security
#   level already refuses TLS 1.0/1.1, so no client here can tell); and
#   makeServer()'s own refusal, which sits behind main.cpp's check -- removing
#   it alone changes nothing a test can see (one layer of two).
# ============================================================
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB_BIN:-$REPO/build/naab-lang}"   # NAAB_BIN: measure another build (red-first)
source "$REPO/tests/helpers/trust_setup.sh"; isolate_trust_store

PASS=0; FAIL=0; UNMEAS=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> ${3:0:600}"; }
unmeasurable() { UNMEAS=$((UNMEAS+1)); echo "  UNMEASURABLE [$1] $2"; }

[ -x "$NAAB" ] || { echo "FAIL: naab-lang not built at $NAAB (UNMEASURABLE, not a pass)"; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not available (UNMEASURABLE)"; exit 0; }

W="$(mktemp -d "${TMPDIR:-/tmp}/naab_tls_XXXXXX")"
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: could not create work dir"; exit 1; }
PIDS="$W/pids"; : > "$PIDS"
cleanup() {
    while read -r p; do [ -n "$p" ] && kill "$p" 2>/dev/null; done < "$PIDS"
    [ -n "${TLS_KEEP:-}" ] || rm -rf "${W:?}"
}
trap cleanup EXIT

# The HTTP(S) client. Modes: https|http PORT PATH [API_KEY] [BODY]; certsha.
cat > "$W/client.py" <<'PYEOF'
import hashlib, http.client, os, ssl, sys
mode = sys.argv[1]
if mode == "certsha":
    print(hashlib.sha256(ssl.PEM_cert_to_DER_cert(os.environ["TLS_CA_PEM"])).hexdigest())
    sys.exit(0)
port, path = int(sys.argv[2]), sys.argv[3]
key = sys.argv[4] if len(sys.argv) > 4 else ""
body = sys.argv[5] if len(sys.argv) > 5 else ""
headers = {"X-API-Key": key} if key else {}
wait = float(os.environ.get("TLS_PROBE_TIMEOUT", "10"))
if body: headers["Content-Type"] = "application/json"
try:
    if mode == "https":
        ctx = ssl.create_default_context(cadata=os.environ["TLS_CA_PEM"])
        conn = http.client.HTTPSConnection("127.0.0.1", port, timeout=wait, context=ctx)
    else:
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=wait)
    conn.request("POST" if body else "GET", path, body=body or None, headers=headers)
    peer = conn.sock.getpeercert(binary_form=True) if mode == "https" else None
    r = conn.getresponse()
    data = r.read()
    line = "STATUS %d" % r.status
    if peer is not None:
        line += " PEER " + hashlib.sha256(peer).hexdigest()
    sys.stdout.write(line + "\n"); sys.stdout.flush()
    sys.stdout.buffer.write(data); sys.stdout.buffer.write(b"\n")
except Exception as e:
    sys.stdout.write("ERROR %s\n" % type(e).__name__)
PYEOF
client() { python3 - "$@" < "$W/client.py"; }

# Certificates: a config file rather than -subj/-addext, so no argument starts
# with "/" (MSYS would rewrite it as a path) and LibreSSL reads it too.
HAVE_OPENSSL=0
command -v openssl >/dev/null 2>&1 && HAVE_OPENSSL=1
mkcert() {  # DIR NAME -> DIR/NAME.crt DIR/NAME.key
    printf '[req]\ndistinguished_name=dn\nx509_extensions=v3\nprompt=no\n[dn]\nCN=127.0.0.1\n[v3]\nsubjectAltName=IP:127.0.0.1\nbasicConstraints=critical,CA:FALSE\n' > "$1/$2.cnf"
    (cd "$1" && openssl req -x509 -newkey rsa:2048 -nodes -keyout "$2.key" -out "$2.crt" \
        -days 2 -config "$2.cnf" >/dev/null 2>&1)
}

# start_api DIR SCHEME PORT [api args...]: start in DIR, wait until it answers
# on SCHEME (or exits); prints "up" or "down rc=N". The wait is bounded by
# time, not by probes: an https probe of a plain server can hang until its own
# timeout (the server waits for a request line), so each probe gets 2 s and the
# whole wait 15 s.
start_api() {
    local d=$1 scheme=$2 port=$3; shift 3
    ( cd "$d" && exec "$NAAB" api "$port" "$@" > "$d/server.log" 2>&1 ) &
    local pid=$!; echo "$pid" >> "$PIDS"
    local r deadline=$(( $(date +%s) + 15 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if ! kill -0 "$pid" 2>/dev/null; then wait "$pid"; echo "down rc=$?"; return; fi
        r=$(TLS_PROBE_TIMEOUT=2 client "$scheme" "$port" /health)
        case "$r" in STATUS\ 200*) echo up; return;; esac
        sleep 0.1
    done
    echo "down rc=timeout"
}
stop_api() { while read -r p; do [ -n "$p" ] && kill "$p" 2>/dev/null; done < "$PIDS"; : > "$PIDS"; sleep 0.2; }
port() { echo $((20000 + RANDOM % 9000)); }
# refuse DIR [api args...] -> sets RC and LOG: the server must exit by itself.
refuse() {
    local d=$1; shift
    (cd "$d" && timeout 20 "$NAAB" api "$(port)" "$@" > "$d/server.log" 2>&1)
    RC=$?; LOG="$(cat "$d/server.log")"
}
cfg() {  # DIR API_JSON [EXTRA]
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},\n "capabilities": {"filesystem": {"mode": "read"}}%s,\n "api": %s}\n' "${3:-}" "$2" > "$1/govern.json"
}

echo "=== naab-lang api over HTTPS ==="

# TL-14 first: it decides whether the HTTPS arms can be measured at all.
VER="$("$NAAB" version 2>&1)"
TLS_BUILT=0
if grep -q "HTTPS for naab-lang api: available" <<<"$VER"; then
    TLS_BUILT=1; pass TL-14 "naab-lang version: HTTPS available"
elif [ -n "${TLS_ASSUME_BUILT:-}" ]; then
    # Measurement aid: run the HTTPS arms against a build that cannot say
    # (an older binary through NAAB_BIN), to see how it behaves on them.
    TLS_BUILT=1; fail TL-14 "this build does not say HTTPS is available (TLS_ASSUME_BUILT: arms run anyway)"
else
    fail TL-14 "this build has no HTTPS (every HTTPS arm below is UNMEASURABLE)" "$(grep -i https <<<"$VER")"
fi

# --- TL-00: plain HTTP when no TLS key is set (the client's control) --------
D="$W/plain"; mkdir -p "$D"; cfg "$D" '{"key": "k-plain"}'
P=$(port); S=$(start_api "$D" http "$P")
if [ "$S" = up ]; then
    [ "$HAVE_OPENSSL" = 1 ] && mkcert "$W" other
    if [ -f "$W/other.crt" ]; then
        R=$(TLS_CA_PEM="$(cat "$W/other.crt")" client https "$P" /health)
    else R="ERROR (no cert to try)"; fi
    if grep -q '^ERROR' <<<"$R"; then
        pass TL-00 "CONTROL: plain HTTP answers, and the https client fails against it"
    else fail TL-00 "CONTROL: the https client did not fail against a plain server" "https probe: $R"; fi
    if grep -q "Serving plain HTTP" "$D/server.log" && grep -q "travel unencrypted" "$D/server.log"; then
        pass TL-00b "the plain server says requests and keys travel unencrypted"
    else fail TL-00b "the plain server does not say it is unencrypted" "$(head -c 300 "$D/server.log")"; fi
else fail TL-00 "CONTROL: plain server did not start ($S)" "$(head -c 400 "$D/server.log")"; fi
stop_api

if [ "$HAVE_OPENSSL" != 1 ] || [ "$TLS_BUILT" != 1 ]; then
    for a in TL-01 TL-02 TL-03 TL-04 TL-05a TL-05b TL-06 TL-07 TL-08 TL-08p TL-09 TL-12; do
        unmeasurable "$a" "needs the openssl command and an HTTPS build (openssl=$HAVE_OPENSSL https=$TLS_BUILT)"
    done
else
    T="$W/tls"; mkdir -p "$T"; mkcert "$T" srv; mkcert "$T" alt
    export TLS_CA_PEM="$(cat "$T/srv.crt")"
    CERT_SHA="$(client certsha)"

    # --- TL-01..03: HTTPS, the right certificate, no plain HTTP, auth ---
    cfg "$T" '{"key": "k-tls", "tls_cert": "srv.crt", "tls_key": "srv.key"}'
    P=$(port); S=$(start_api "$T" https "$P")
    if [ "$S" = up ]; then
        R=$(client https "$P" /health)
        if grep -q "^STATUS 200 PEER $CERT_SHA" <<<"$R" && grep -q "Serving HTTPS" "$T/server.log"; then
            pass TL-01 "HTTPS answers with the configured certificate (verified, sha256 matches)"
        else fail TL-01 "HTTPS did not present the configured certificate" "$R"; fi
        R=$(client http "$P" /health)
        if grep -q '^ERROR' <<<"$R"; then pass TL-02 "plain HTTP to the HTTPS port gets no HTTP response ($R)"
        else fail TL-02 "the HTTPS port answered plain HTTP" "$R"; fi
        R1=$(client https "$P" /api/v1/stats); R2=$(client https "$P" /api/v1/stats k-tls)
        if grep -q '^STATUS 401' <<<"$R1" && grep -q '^STATUS 200' <<<"$R2"; then
            pass TL-03 "the API key still decides over HTTPS (401 without, 200 with)"
        else fail TL-03 "auth over HTTPS wrong" "without: $R1 | with: $R2"; fi
    else
        for a in TL-01 TL-02 TL-03; do fail "$a" "the HTTPS server did not start ($S)" "$(head -c 500 "$T/server.log")"; done
    fi
    stop_api

    # --- TL-04: relative to govern.json, from a subdirectory ---
    mkdir -p "$T/sub"
    P=$(port); S=$(start_api "$T/sub" https "$P")
    R=$(client https "$P" /health)
    if [ "$S" = up ] && grep -q "^STATUS 200 PEER $CERT_SHA" <<<"$R"; then
        pass TL-04 "started in a subdirectory, the paths still resolve against govern.json's directory"
    else fail TL-04 "relative TLS paths did not resolve against the project ($S)" "$R; $(head -c 400 "$T/sub/server.log")"; fi
    stop_api

    # --- TL-05..09: refusals; the server must exit by itself with 4 ---
    expect_refusal() {  # ID DESC PATTERN
        if [ "$RC" = 4 ] && grep -q "$3" <<<"$LOG" && grep -q "was not started" <<<"$LOG" && \
           ! grep -q "Serving" <<<"$LOG"; then
            pass "$1" "$2: exit 4, nothing served"
        else fail "$1" "$2 was not refused (rc=$RC; 124 = it served until the timeout)" "$LOG"; fi
    }
    R5="$W/r5"; mkdir -p "$R5"; cp "$T"/srv.* "$T"/alt.* "$R5/"
    cfg "$R5" '{"tls_cert": "srv.crt"}'; refuse "$R5"
    expect_refusal TL-05a "a certificate without its key" "api.tls_cert is set but api.tls_key is not"
    cfg "$R5" '{"tls_key": "srv.key"}'; refuse "$R5"
    expect_refusal TL-05b "a key without its certificate" "api.tls_key is set but api.tls_cert is not"
    cfg "$R5" '{"tls_cert": "missing.crt", "tls_key": "srv.key"}'; refuse "$R5"
    expect_refusal TL-06 "a certificate file that does not exist" "does not exist"
    cfg "$R5" '{"tls_cert": "srv.crt", "tls_key": "alt.key"}'; refuse "$R5"
    expect_refusal TL-07 "a key that does not match the certificate" "does not match"
    (cd "$R5" && openssl pkey -in srv.key -aes256 -passout pass:pw -out enc.key >/dev/null 2>&1)
    if [ -s "$R5/enc.key" ]; then
        cfg "$R5" '{"tls_cert": "srv.crt", "tls_key": "enc.key"}'; refuse "$R5" </dev/null
        expect_refusal TL-08 "an encrypted key" "unencrypted PEM key"
        if command -v script >/dev/null 2>&1 && script -qec true /dev/null >/dev/null 2>&1; then
            P=$(port)
            (cd "$R5" && timeout 20 script -qec "'$NAAB' api $P" /dev/null > "$R5/pty.log" 2>&1 </dev/null)
            RC=$?; LOG="$(cat "$R5/pty.log")"
            expect_refusal TL-08p "an encrypted key under a terminal (no passphrase prompt)" "unencrypted PEM key"
        else unmeasurable TL-08p "no util-linux script to give the server a terminal"; fi
    else
        fail TL-08 "could not make an encrypted key with openssl"; unmeasurable TL-08p "no encrypted key"
    fi
    printf 'not a certificate\n' > "$R5/junk.crt"
    cfg "$R5" '{"tls_cert": "junk.crt", "tls_key": "srv.key"}'; refuse "$R5"
    expect_refusal TL-09 "a certificate file that is not PEM" "could not be read as a PEM certificate"

    # --- TL-12: an /execute program cannot read the TLS key (POSIX) ---
    cfg "$T" '{"key": "k-tls", "tls_cert": "srv.crt", "tls_key": "srv.key"}'
    P=$(port); S=$(start_api "$T" https "$P")
    KEY_MARK="$(sed -n 2p "$T/srv.key" | cut -c1-24)"
    CRT_MARK="$(sed -n 2p "$T/srv.crt" | cut -c1-24)"
    if [ "$S" = up ]; then
        RK=$(client https "$P" /api/v1/execute k-tls '{"code": "use file\nmain { print(file.read(\"srv.key\")) }"}')
        RC_=$(client https "$P" /api/v1/execute k-tls '{"code": "use file\nmain { print(file.read(\"srv.crt\")) }"}')
        if grep -q '^STATUS 501' <<<"$RK"; then
            unmeasurable TL-12 "/execute is not available on this platform (501)"
            unmeasurable TL-12c "/execute is not available on this platform (501)"
        else
            if grep -q '"exit_code": 0' <<<"$RC_" && grep -qF "$CRT_MARK" <<<"$RC_"; then
                pass TL-12c "CONTROL: an /execute program reads the certificate"
            else fail TL-12c "CONTROL: the certificate read failed, so TL-12 proves nothing" "$RC_"; fi
            if grep -q '"exit_code": 3' <<<"$RK" && ! grep -qF "$KEY_MARK" <<<"$RK"; then
                pass TL-12 "an /execute program cannot read the TLS private key (exit 3, nothing leaked)"
            else fail TL-12 "an /execute program read the TLS private key" "$(head -c 300 <<<"$RK")"; fi
        fi
    else fail TL-12 "the HTTPS server did not start ($S)" "$(head -c 400 "$T/server.log")"; fi
    stop_api
fi

# --- TL-10: A232, a govern.json that does not load ----------------------------
B="$W/broken"; mkdir -p "$B"
printf '{"version": "5.0", "mode": "enforce", "api": {"key": "k-broken"}, BROKEN\n' > "$B/govern.json"
refuse "$B"; RC1=$RC; L1="$LOG"
refuse "$B" --no-governance; RC2=$RC; L2="$LOG"
if [ "$RC1" = 4 ] && [ "$RC2" = 4 ] && grep -q "could not be loaded" <<<"$L1" && \
   grep -q "The server was not started" <<<"$L2"; then
    pass TL-10 "A232: a govern.json that does not load refuses to start, with and without --no-governance"
else fail TL-10 "A232: the server started beside a govern.json that does not load" "rc=$RC1/$RC2 $L1"; fi
N="$W/none"; mkdir -p "$N"
P=$(port); S=$(start_api "$N" http "$P")
if [ "$S" = up ]; then pass TL-10c "CONTROL: with no govern.json at all the server still starts"
else fail TL-10c "CONTROL: the server did not start without a govern.json ($S)" "$(head -c 300 "$N/server.log")"; fi
stop_api

# --- TL-11: A233, api values the loader would drop ---------------------------
Y="$W/types"; mkdir -p "$Y"
ty_refused=0; ty_total=0; ty_bad=""
for shape in '"x"' '{"key": 5}' '{"tls_cert": true}' '{"tls_key": 7}' '{"keys": {}}' \
             '{"keys": ["k-a"]}' '{"keys": [{"key": 5}]}' '{"keys": [{"name": "n"}]}' \
             '{"keys": [{"key": "k-a", "scopes": "check"}]}' '{"keys": [{"key": "k-a", "scopes": [1]}]}'; do
    ty_total=$((ty_total+1))
    cfg "$Y" "$shape"; refuse "$Y"
    if [ "$RC" = 4 ] && grep -q '"api' <<<"$LOG" && grep -q "config is refused" <<<"$LOG"; then
        ty_refused=$((ty_refused+1))
    else ty_bad="$ty_bad | $shape rc=$RC"; fi
done
if [ "$ty_refused" = "$ty_total" ]; then pass TL-11 "A233: all $ty_total ill-typed api values refuse the config"
else fail TL-11 "A233: $((ty_total-ty_refused)) of $ty_total ill-typed api values loaded" "$ty_bad"; fi
cfg "$Y" '{"keys": [{"key": "k-chk", "name": "checker", "scopes": ["check"]}, {"key": "k-all", "scopes": []}]}'
P=$(port); S=$(start_api "$Y" http "$P")
R1=$(client http "$P" /api/v1/stats k-chk); R2=$(client http "$P" /api/v1/stats k-all)
if [ "$S" = up ] && grep -q '^STATUS 403' <<<"$R1" && grep -q '^STATUS 200' <<<"$R2"; then
    pass TL-11c "CONTROL: well-typed keys load; a check-only key is refused /stats, an unscoped key is not"
else fail TL-11c "CONTROL: well-typed api keys did not behave ($S)" "chk: $R1 | all: $R2 | $(head -c 300 "$Y/server.log")"; fi
stop_api

# --- TL-13: no no-effect warning for the TLS keys ------------------------------
V="$W/warn"; mkdir -p "$V"
printf 'main { print("ok") }\n' > "$V/p.naab"
cfg "$V" '{"tls_cert": "c.pem", "tls_key": "k.pem"}' ', "limits": {"memory": {"per_block_mb": 64}}'
OUT="$(cd "$V" && "$NAAB" run p.naab 2>&1)"
if grep -q "per_block_mb" <<<"$OUT"; then pass TL-13c "CONTROL: a still-warned setting warns in the same run"
else fail TL-13c "CONTROL: the no-effect warning did not fire, so TL-13 proves nothing" "$OUT"; fi
if ! grep -q "api.tls_" <<<"$OUT"; then pass TL-13 "setting api.tls_cert/api.tls_key draws no no-effect warning"
else fail TL-13 "the TLS keys are still warned as doing nothing" "$(grep 'api.tls_' <<<"$OUT")"; fi

echo
echo "=== Results: $PASS passed, $FAIL failed, $UNMEAS unmeasurable ==="
[ "$FAIL" -eq 0 ]
