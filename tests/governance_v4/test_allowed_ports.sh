#!/usr/bin/env bash
# A87: capabilities.network.allowed_ports binds the connections the program
# makes (the owner's decision 15, 2026-11-20).
#
# The sandbox's canConnect() and hostPolicyAllows() have always checked a port
# list, and the loader has always read allowed_ports, but syncGovernanceToSandbox()
# (main.cpp) copied the host lists and not the ports, so the list bound nothing
# (observed on c1312132: an embedded-Python connect to an unlisted port landed,
# and http.* reached the network). The list now reaches the sandbox at startup
# with the host lists, so http.*, URL imports and embedded Python's sockets
# answer to it. Like the host lists, a mid-run reload does not change it at
# runtime (recorded in A87, not an arm here).
#
#   AP-00  CONTROL: no port list, a connect to the second listener lands
#   AP-01  CONTROL: allowed_ports [first]: a connect to the first lands
#   AP-02  allowed_ports [first]: a connect to the second is refused and does
#          not land                                          (red on c1312132)
#   AP-03  http.get to port 443 under allowed_ports [1] is refused by the
#          sandbox before any connection                     (red on c1312132)
#   AP-03c CONTROL: under allowed_ports [443] the same call is not refused by
#          the sandbox (it fails later, on the name, which does not resolve)
#
# Kill table (mutants of src/cli/main.cpp, run by hand, revert-checked
# 2026-11-20):
#   P1 allowed_ports not copied into the sandbox       -> AP-02, AP-03
#   P2 an unlisted port admitted by the copy (the list copied empty)
#                                                      -> AP-02, AP-03
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
W="$(mktemp -d)"
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
LP=""
trap '[ -n "$LP" ] && { touch "$W/stop"; kill "$LP" 2>/dev/null; }; teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -10 | sed 's/^/      | /'; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] UNMEASURABLE: $2"; SKIP=$((SKIP+1)); }
[ -x "$NAAB" ] || { echo "not built: $NAAB"; exit 1; }

cat > "$W/listener.py" <<'PY'
import os, socket, sys
d = sys.argv[1]
socks = []
for _ in range(2):
    t = socket.socket(); t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    t.bind(("127.0.0.1", 0)); t.listen(16); t.settimeout(0.1); socks.append(t)
with open(os.path.join(d, "ports.tmp"), "w") as f:
    f.write("%d %d\n" % (socks[0].getsockname()[1], socks[1].getsockname()[1]))
os.rename(os.path.join(d, "ports.tmp"), os.path.join(d, "ports"))
while not os.path.exists(os.path.join(d, "stop")):
    for s in socks:
        try:
            c, _ = s.accept()
        except socket.timeout:
            continue
        c.settimeout(0.5)
        try: data = c.recv(64)
        except Exception: data = b""
        with open(os.path.join(d, "events"), "a") as f:
            f.write(data.decode(errors="replace") + "\n")
        c.close()
PY
python3 "$W/listener.py" "$W" & LP=$!
for _ in $(seq 1 100); do [ -f "$W/ports" ] && break; sleep 0.1; done
[ -f "$W/ports" ] || { echo "  FAIL [SETUP] the listener did not start"; exit 1; }
read -r P1 P2 < "$W/ports"
touch "$W/events"

# py ID NETWORK_JSON PORT -> OUT, LANDED (0/1)
py() {
    local d="$W/$1"; mkdir -p "$d"
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}, "capabilities": {"network": %s}}\n' "$2" > "$d/govern.json"
    printf 'main {\n    let r = <<python\nimport socket\nout = "?"\ntry:\n    s = socket.socket(); s.settimeout(3); s.connect(("127.0.0.1", %s)); s.send(b"%s"); s.close(); out = "OK"\nexcept PermissionError as e:\n    out = "REFUSED"\nexcept Exception as e:\n    out = "ERR " + type(e).__name__\nout\n>>\n    print("R=" + string(r))\n}\n' "$3" "$1" > "$d/p.naab"
    OUT="$(cd "$d" && timeout 60 "$NAAB" run p.naab 2>&1)"
    sleep 0.5
    LANDED=0; grep -qx "$1" "$W/events" && LANDED=1
}

py AP-00 '{"enabled": true}' "$P2"
if [[ "$OUT" == *R=OK* ]] && [ "$LANDED" = 1 ]; then ok AP-00 "no port list: a connect to the second listener lands"; live=1
elif [[ "$OUT" != *R=* ]]; then skip AP-00 "the <<python>> block did not run here"; live=0
else bad AP-00 "the connect did not land with no port list" "$OUT"; live=0; fi

if [ "$live" = 1 ]; then
    py AP-01 "{\"enabled\": true, \"allowed_ports\": [$P1]}" "$P1"
    if [[ "$OUT" == *R=OK* ]] && [ "$LANDED" = 1 ]; then ok AP-01 "allowed_ports [first]: a connect to the first lands"
    else bad AP-01 "a connect to a listed port did not land" "landed=$LANDED $OUT"; fi
    py AP-02 "{\"enabled\": true, \"allowed_ports\": [$P1]}" "$P2"
    if [[ "$OUT" == *R=REFUSED* ]] && [ "$LANDED" = 0 ]; then ok AP-02 "allowed_ports [first]: a connect to the second is refused"
    else bad AP-02 "a connect to an unlisted port was made" "landed=$LANDED $OUT"; fi
else
    skip AP-01 "AP-00 did not run"; skip AP-02 "AP-00 did not run"
fi

# http ID NETWORK_JSON -> OUT
http() {
    local d="$W/$1"; mkdir -p "$d"
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}, "capabilities": {"network": %s}}\n' "$2" > "$d/govern.json"
    printf 'use http\nmain {\n    try {\n        let r = http.get("https://naab-ap.invalid/x")\n        print("GOT")\n    } catch (e) {\n        print("E=" + e["message"])\n    }\n    print("DONE")\n}\n' > "$d/p.naab"
    OUT="$(cd "$d" && timeout 60 "$NAAB" run p.naab 2>&1)"
}
http AP-03c '{"enabled": true, "allowed_ports": [443]}'
if grep -q DONE <<<"$OUT" && ! grep -q 'denied by sandbox' <<<"$OUT"; then ok AP-03c "allowed_ports [443]: the https call is not refused by the sandbox"; hlive=1
else bad AP-03c "a listed port was refused, or the program did not run" "$OUT"; hlive=0; fi
http AP-03 '{"enabled": true, "allowed_ports": [1]}'
if [ "$hlive" != 1 ]; then skip AP-03 "AP-03c did not run"
elif grep -q 'denied by sandbox' <<<"$OUT"; then ok AP-03 "allowed_ports [1]: http.get to port 443 is refused by the sandbox"
else bad AP-03 "http.get to an unlisted port was not refused" "$OUT"; fi

echo
echo "Results: $PASS passed, $FAIL failed, $SKIP unmeasurable"
[ "$FAIL" -eq 0 ]
