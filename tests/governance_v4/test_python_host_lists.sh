#!/usr/bin/env bash
# ============================================================
# test_python_host_lists.sh -- capabilities.network host lists reach embedded
# Python's sockets: connect, sendto, sendmsg and the lookups (A35, Bundle 3)
#
# The embedded-Python audit hook judged socket.connect on NET_CONNECT alone, so
# allowed_hosts / blocked_hosts -- which http.* meets through
# Sandbox::canConnect -- never reached a <<python>> block, and sendto / sendmsg
# were not judged at all. Measured before the fix (observed, this branch's
# binary before A35): with allowed_hosts naming another host, a TCP connect to
# 127.0.0.1 landed; with blocked_hosts ["127.0.0.1"] too; a UDP datagram went
# out under the allowlist, and with network.enabled false through an import the
# text check cannot see.
#
# The owner's decision (2026-10-26, "admit allowed lookups"): the NAME is judged
# where the program gives one -- at getaddrinfo / gethostbyname[_ex] and at a
# connect given a name. When an allowed name is looked up, the engine resolves
# it too and admits those addresses; any other address is refused unless an IP
# entry admits it, and a resolver answering differently fails closed.
#
# Every verdict is observed at a listener (TCP, UDP and an AF_UNIX socket) the
# suite starts: each arm sends its own token, and the listener's log is read
# for it.
#
#   HL-00  CONTROL: no host lists -- a TCP connect to 127.0.0.1 lands
#   HL-01  allowed_hosts naming another host: the connect is refused, nothing lands
#   HL-02  CONTROL: an IP entry (allowed_hosts ["127.0.0.1"]) admits the connect
#   HL-03  blocked_hosts ["127.0.0.1"]: the connect is refused
#   HL-03b blocked_hosts ["127.0.0.1"]: a legacy form (2130706433) is refused too
#   HL-03c blocked_hosts ["127.0.0.1"]: the IPv4-mapped IPv6 form is refused
#          (UNMEASURABLE when the same connect does not land with no lists)
#   HL-04  allowed_hosts ["localhost"]: create_connection(("localhost", P)) lands
#          -- the lookup admits the addresses it resolves to
#   HL-05  allowed_hosts ["localhost"]: a connect to 127.0.0.1 with no lookup
#          is refused (no IP entry and no allowed lookup admitted it)
#   HL-05b allowed_hosts ["localhost"]: after getaddrinfo("localhost") the same
#          connect lands
#   HL-06  allowed_hosts naming another host: getaddrinfo("localhost") is refused
#   HL-06b allowed_hosts naming another host: gethostbyname("localhost") is refused
#   HL-07  blocked_hosts ["localhost"]: create_connection(("localhost", P)) is refused
#   HL-08c CONTROL: no host lists -- a UDP sendto lands
#   HL-08  allowed_hosts naming another host: the UDP sendto is refused
#   HL-09  network.enabled false: a UDP sendto through an import the text check
#          cannot see is refused (it was sent before the fix)
#   HL-09b network.enabled false: a name lookup through that import is refused
#   HL-10  allowed_hosts ["localhost"]: sendmsg to 127.0.0.1 with no lookup is refused
#   HL-11  CONTROL: allowed_hosts naming another host: an AF_UNIX connect still
#          lands -- a path is not a host, the lists say nothing about it
#   HL-12  CONTROL: allowed_hosts ["localhost"]: connect(("localhost", P)) lands --
#          a connect given an allowed name is judged by the name
#   HL-13c CONTROL: no host lists -- gethostbyaddr("127.0.0.1") answers
#   HL-13  allowed_hosts naming another host: gethostbyaddr("127.0.0.1") is refused
#   HL-14  CONTROL: a run with no govern.json connects (no policy, no change)
#
# A thread the block spawns (threading.Thread) has no ScopedSandbox of its own;
# the policy takes the run-level one there (auditSandbox(), A85):
#   HL-15c CONTROL: no host lists -- a spawned thread's TCP connect lands
#   HL-15  allowed_hosts naming another host: a spawned thread's connect is refused
#   HL-16  network.enabled false: a spawned thread's UDP sendto is refused
#   HL-17  CONTROL: an IP entry admits a spawned thread's connect (no over-block)
#   HL-18  CONTROL: a run with no govern.json: a spawned thread connects
#
# Kill table (each mutant rebuilt, this suite run):
#   the socket.* branch back to the capability check alone   HL-01/03/03b/03c/05/06/06b/07/08/09/09b/10/13
#   the lookup admits nothing (admitResolvedAddresses gone)  HL-04, HL-05b
#   ipLiteralForms() without the legacy IPv4 parse           HL-03b
#   ipLiteralForms() without the IPv4-mapped form            HL-03c
#   sendto/sendmsg not routed to the policy                  HL-08, HL-09, HL-10
#   the net policy without the run-level fallback (A85)      HL-15, HL-16
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

PASS=0; FAIL=0; UNM=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -4 | sed 's/^/       | /'; }
unm() { UNM=$((UNM+1)); echo "  UNMEASURABLE [$1] $2"; }

echo "=== embedded Python sockets under capabilities.network host lists ==="
if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2; exit 1
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-pyhosts-XXXXXX")"
LP=""
cleanup() { touch "$W/stop"; [ -n "$LP" ] && wait "$LP" 2>/dev/null; teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT

# The audit hook belongs to the EMBEDDED executor; the subprocess fallback (a
# build without pybind11) has none, so nothing here applies to it.
mkdir -p "$W/probe"
printf 'main { let v = <<python\n1+1\n>>\nprint("V=" + string(v)) }\n' > "$W/probe/p.naab"
if [[ "$(cd "$W/probe" && "$NAAB" run --no-governance p.naab 2>/dev/null)" != *V=2* ]]; then
    echo "  UNMEASURABLE [HL] the embedded Python executor is not built here"; exit 0
fi

cat > "$W/listener.py" <<'PY'
import os, socket, sys
d = sys.argv[1]
t = socket.socket(); t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
t.bind(("127.0.0.1", 0)); t.listen(16); t.settimeout(0.1)
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.bind(("127.0.0.1", 0)); u.settimeout(0.1)
x = socket.socket(socket.AF_UNIX); x.bind(os.path.join(d, "l.sock")); x.listen(16); x.settimeout(0.1)
with open(os.path.join(d, "ports.tmp"), "w") as f:
    f.write("%d %d\n" % (t.getsockname()[1], u.getsockname()[1]))
os.rename(os.path.join(d, "ports.tmp"), os.path.join(d, "ports"))
def log(s):
    with open(os.path.join(d, "events"), "a") as f: f.write(s + "\n")
def serve(srv, kind):
    try:
        c, _ = srv.accept()
    except socket.timeout:
        return
    c.settimeout(0.5)
    try: data = c.recv(64)
    except Exception: data = b""
    log(kind + " " + data.decode(errors="replace")); c.close()
while not os.path.exists(os.path.join(d, "stop")):
    serve(t, "TCP"); serve(x, "UNIX")
    try:
        data, _ = u.recvfrom(64); log("UDP " + data.decode(errors="replace"))
    except socket.timeout:
        pass
PY
python3 "$W/listener.py" "$W" & LP=$!
for _ in $(seq 1 100); do [ -f "$W/ports" ] && break; sleep 0.1; done
if [ ! -f "$W/ports" ]; then echo "  FAIL [SETUP] the listener did not start" >&2; exit 1; fi
read -r TP UP < "$W/ports"
touch "$W/events"

IMPORT='import socket'
# arm ID NETWORK_JSON BODY -> OUT (what the block returned) and LANDED (0/1:
# the listener logged this arm's token). BODY is Python at 4-space indent; it
# sends the token "$ID" and sets out on success. NETWORK_JSON "" = no govern.json.
arm() {
    local id="$1" net="$2" body="$3" d="$W/$1"
    mkdir -p "$d"
    local flag=""
    if [ -n "$net" ]; then
        printf '{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" }, "capabilities": { "network": %s } }\n' "$net" > "$d/govern.json"
    else
        flag="--no-governance"
    fi
    printf 'main {\n    let r = <<python\n%s\nout = "?"\ntry:\n%s\nexcept PermissionError as e:\n    out = "REFUSED " + str(e)[:90]\nexcept Exception as e:\n    out = "ERR " + type(e).__name__ + " " + str(e)[:60]\nout\n>>\n    print("R=" + string(r))\n}\n' \
        "$IMPORT" "$body" > "$d/p.naab"
    OUT="$(cd "$d" && timeout 60 "$NAAB" run $flag p.naab 2>&1)"
    sleep 0.5
    LANDED=0; grep -q " $id\$" "$W/events" && LANDED=1
}
refused() { [[ "$OUT" == *"R=REFUSED"* ]] && [ "$LANDED" = 0 ]; }
landed()  { [[ "$OUT" == *"R=OK"* ]] && [ "$LANDED" = 1 ]; }
tcp()  { printf '    s = socket.socket(%s); s.settimeout(3); s.connect((%s, %s)); s.send(b"%s"); s.close(); out = "OK"' "${4:-}" "$1" "$TP" "$2"; }
udp()  { printf '    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.sendto(b"%s", (%s, %s)); out = "OK"' "$2" "$1" "$UP"; }
# thr BODY -> BODY run on a thread the block spawns, its outcome handed back in out
thr() {
    local inner; inner="$(sed 's/^    /            /; s/out = "OK"/box["out"] = "OK"/' <<<"$1")"
    printf '    import threading\n    box = {"out": "?"}\n    def go():\n        try:\n%s\n        except PermissionError as e:\n            box["out"] = "REFUSED " + str(e)[:90]\n        except Exception as e:\n            box["out"] = "ERR " + type(e).__name__ + " " + str(e)[:60]\n    t = threading.Thread(target=go); t.start(); t.join()\n    out = box["out"]' "$inner"
}

OTHER='{ "enabled": true, "allowed_hosts": ["listed-host.test"] }'
LOCALHOST='{ "enabled": true, "allowed_hosts": ["localhost"] }'
BLOCK_IP='{ "enabled": true, "blocked_hosts": ["127.0.0.1"] }'

arm HL-00 '{ "enabled": true }' "$(tcp '"127.0.0.1"' HL-00)"
if landed; then ok "HL-00" "control: no host lists, a TCP connect to 127.0.0.1 lands"
else bad "HL-00" "the connect did not land with no host lists -- every refusal below is unmeasured" "$OUT"; fi

arm HL-01 "$OTHER" "$(tcp '"127.0.0.1"' HL-01)"
if refused; then ok "HL-01" "allowed_hosts naming another host: the connect is refused"
else bad "HL-01" "a connect no allowed_hosts entry admits was made" "landed=$LANDED $OUT"; fi

arm HL-02 '{ "enabled": true, "allowed_hosts": ["127.0.0.1"] }' "$(tcp '"127.0.0.1"' HL-02)"
if landed; then ok "HL-02" "control: an IP entry admits the connect"
else bad "HL-02" "an allowed_hosts IP entry did not admit its own address" "landed=$LANDED $OUT"; fi

arm HL-03 "$BLOCK_IP" "$(tcp '"127.0.0.1"' HL-03)"
if refused; then ok "HL-03" "blocked_hosts [127.0.0.1]: the connect is refused"
else bad "HL-03" "a connect to a blocked address was made" "landed=$LANDED $OUT"; fi

arm HL-03b "$BLOCK_IP" "$(tcp '"2130706433"' HL-03b)"
if refused; then ok "HL-03b" "blocked_hosts [127.0.0.1]: the legacy form 2130706433 is refused"
else bad "HL-03b" "a legacy spelling of a blocked address reached it" "landed=$LANDED $OUT"; fi

arm HL-03cc '{ "enabled": true }' "$(tcp '"::ffff:127.0.0.1"' HL-03cc socket.AF_INET6)"
if landed; then
    arm HL-03c "$BLOCK_IP" "$(tcp '"::ffff:127.0.0.1"' HL-03c socket.AF_INET6)"
    if refused; then ok "HL-03c" "blocked_hosts [127.0.0.1]: the IPv4-mapped IPv6 form is refused"
    else bad "HL-03c" "the IPv4-mapped form of a blocked address reached it" "landed=$LANDED $OUT"; fi
else
    unm "HL-03c" "an IPv6 socket cannot reach the IPv4 listener here (no lists: $OUT)"
fi

arm HL-04 "$LOCALHOST" "    s = socket.create_connection((\"localhost\", $TP), 3); s.send(b\"HL-04\"); s.close(); out = \"OK\""
if landed; then ok "HL-04" "allowed_hosts [localhost]: create_connection to localhost lands"
else bad "HL-04" "an allowed name's own lookup did not admit its addresses" "landed=$LANDED $OUT"; fi

arm HL-05 "$LOCALHOST" "$(tcp '"127.0.0.1"' HL-05)"
if refused; then ok "HL-05" "allowed_hosts [localhost]: a connect to 127.0.0.1 with no lookup is refused"
else bad "HL-05" "an address no IP entry and no allowed lookup admitted was reached" "landed=$LANDED $OUT"; fi

arm HL-05b "$LOCALHOST" "    socket.getaddrinfo(\"localhost\", $TP)
$(tcp '"127.0.0.1"' HL-05b)"
if landed; then ok "HL-05b" "allowed_hosts [localhost]: after getaddrinfo(localhost) the connect lands"
else bad "HL-05b" "an address an allowed lookup produced was not admitted" "landed=$LANDED $OUT"; fi

arm HL-06 "$OTHER" "    socket.getaddrinfo(\"localhost\", $TP); out = \"OK\""
if [[ "$OUT" == *"R=REFUSED"* ]]; then ok "HL-06" "allowed_hosts naming another host: getaddrinfo(localhost) is refused"
else bad "HL-06" "a lookup of a name the list does not allow was made" "$OUT"; fi

arm HL-06b "$OTHER" "    socket.gethostbyname(\"localhost\"); out = \"OK\""
if [[ "$OUT" == *"R=REFUSED"* ]]; then ok "HL-06b" "allowed_hosts naming another host: gethostbyname(localhost) is refused"
else bad "HL-06b" "gethostbyname of a name the list does not allow was made" "$OUT"; fi

arm HL-07 '{ "enabled": true, "blocked_hosts": ["localhost"] }' "    s = socket.create_connection((\"localhost\", $TP), 3); s.send(b\"HL-07\"); s.close(); out = \"OK\""
if refused; then ok "HL-07" "blocked_hosts [localhost]: create_connection to localhost is refused"
else bad "HL-07" "a connect to a blocked name was made" "landed=$LANDED $OUT"; fi

arm HL-08c '{ "enabled": true }' "$(udp '"127.0.0.1"' HL-08c)"
if landed; then ok "HL-08c" "control: no host lists, a UDP sendto lands"
else bad "HL-08c" "the UDP send did not land with no host lists -- HL-08/09 are unmeasured" "landed=$LANDED $OUT"; fi

arm HL-08 "$OTHER" "$(udp '"127.0.0.1"' HL-08)"
if refused; then ok "HL-08" "allowed_hosts naming another host: the UDP sendto is refused"
else bad "HL-08" "a datagram went to an address no allowed_hosts entry admits" "landed=$LANDED $OUT"; fi

IMPORT='socket = __import__("so" + "cket")'
arm HL-09 '{ "enabled": false }' "$(udp '"127.0.0.1"' HL-09)"
if refused; then ok "HL-09" "network.enabled false: a UDP sendto is refused"
else bad "HL-09" "a datagram went out with network disabled" "landed=$LANDED $OUT"; fi

arm HL-09b '{ "enabled": false }' "    socket.gethostbyname(\"localhost\"); out = \"OK\""
if [[ "$OUT" == *"R=REFUSED"* ]]; then ok "HL-09b" "network.enabled false: a name lookup is refused"
else bad "HL-09b" "a name lookup ran with network disabled" "$OUT"; fi
IMPORT='import socket'

arm HL-10 "$LOCALHOST" "    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.sendmsg([b\"HL-10\"], [], 0, (\"127.0.0.1\", $UP)); out = \"OK\""
if refused; then ok "HL-10" "allowed_hosts [localhost]: sendmsg to 127.0.0.1 with no lookup is refused"
else bad "HL-10" "sendmsg reached an address nothing admitted" "landed=$LANDED $OUT"; fi

arm HL-11 "$OTHER" "    s = socket.socket(socket.AF_UNIX); s.settimeout(3); s.connect(\"$W/l.sock\"); s.send(b\"HL-11\"); s.close(); out = \"OK\""
if landed; then ok "HL-11" "control: an AF_UNIX connect still lands under allowed_hosts"
else bad "HL-11" "the host lists refused a path, which is not a host" "landed=$LANDED $OUT"; fi

arm HL-12 "$LOCALHOST" "$(tcp '"localhost"' HL-12)"
if landed; then ok "HL-12" "control: a connect given an allowed name lands"
else bad "HL-12" "a connect given an allowed name was refused" "landed=$LANDED $OUT"; fi

arm HL-13c '{ "enabled": true }' "    socket.gethostbyaddr(\"127.0.0.1\"); out = \"OK\""
if [[ "$OUT" == *"R=OK"* ]]; then
    ok "HL-13c" "control: no host lists, gethostbyaddr(127.0.0.1) answers"
    arm HL-13 "$OTHER" "    socket.gethostbyaddr(\"127.0.0.1\"); out = \"OK\""
    if [[ "$OUT" == *"R=REFUSED"* ]]; then ok "HL-13" "allowed_hosts naming another host: gethostbyaddr(127.0.0.1) is refused"
    else bad "HL-13" "a reverse lookup the list does not allow was made" "$OUT"; fi
else
    unm "HL-13c" "gethostbyaddr(127.0.0.1) does not answer here: $OUT"; unm "HL-13" "no reverse lookup to judge"
fi

arm HL-14 "" "$(tcp '"127.0.0.1"' HL-14)"
if landed; then ok "HL-14" "control: a run with no govern.json connects"
else bad "HL-14" "an ungoverned run's connect changed" "landed=$LANDED $OUT"; fi

arm HL-15c '{ "enabled": true }' "$(thr "$(tcp '"127.0.0.1"' HL-15c)")"
if landed; then ok "HL-15c" "control: no host lists, a spawned thread's TCP connect lands"
else bad "HL-15c" "a spawned thread's connect did not land with no host lists -- HL-15/16 are unmeasured" "landed=$LANDED $OUT"; fi

arm HL-15 "$OTHER" "$(thr "$(tcp '"127.0.0.1"' HL-15)")"
if refused; then ok "HL-15" "allowed_hosts naming another host: a spawned thread's connect is refused"
else bad "HL-15" "a spawned thread connected where no allowed_hosts entry admits the address" "landed=$LANDED $OUT"; fi

IMPORT='socket = __import__("so" + "cket")'
arm HL-16 '{ "enabled": false }' "$(thr "$(udp '"127.0.0.1"' HL-16)")"
if refused; then ok "HL-16" "network.enabled false: a spawned thread's UDP sendto is refused"
else bad "HL-16" "a spawned thread sent a datagram with network disabled" "landed=$LANDED $OUT"; fi
IMPORT='import socket'

arm HL-17 '{ "enabled": true, "allowed_hosts": ["127.0.0.1"] }' "$(thr "$(tcp '"127.0.0.1"' HL-17)")"
if landed; then ok "HL-17" "control: an IP entry admits a spawned thread's connect"
else bad "HL-17" "the run-level policy over-blocked a spawned thread" "landed=$LANDED $OUT"; fi

arm HL-18 "" "$(thr "$(tcp '"127.0.0.1"' HL-18)")"
if landed; then ok "HL-18" "control: a run with no govern.json, a spawned thread connects"
else bad "HL-18" "an ungoverned run's spawned thread could not connect" "landed=$LANDED $OUT"; fi

echo ""
echo "  Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
