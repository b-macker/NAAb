#!/usr/bin/env bash
# ============================================================
# test_api_base_loopback.sh -- plain-http api_base is accepted only for a REAL loopback host
#
# Per-agent api_base is https-only, except plain http to loopback (test stubs).
# The check was a string PREFIX: "http://127.0.0.1@192.0.2.2:PORT" starts with
# "http://127.0.0.1" -- the part before '@' is userinfo -- and the agent's
# request, API-key header included, went over plaintext HTTP to 192.0.2.2.
# Both the config loader and the HTTP client now share naab::net::
# isLoopbackHttpUrl, which compares the host itself.
#
# The observable is the loader's own refusal ("api_base ignored"), so no
# network is needed.
#   AB-01..04  non-loopback disguises are refused: userinfo, a host that merely
#              starts with "localhost", a host that starts with "127.0.0.1", and
#              a non-numeric port
#   AB-05c     CONTROL: real loopback forms are accepted -- otherwise AB-01..04
#              pass for a loader that refuses every plain-http api_base
#   AB-06c     CONTROL: https is accepted for any host
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== api_base: plain http only for a real loopback host ==="

if [ ! -x "$NAAB" ]; then
    for id in AB-01 AB-02 AB-03 AB-04 AB-05c AB-06c; do skip "$id" "naab-lang not built (UNMEASURABLE)"; done
    echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-apibase.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust   # the unsigned govern.json below must not meet a populated trust store
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT
printf 'main {\n    print("RAN")\n}\n' > "$W/p.naab"

# verdict <api_base> -> "refused" / "accepted" / "broken:<detail>"
verdict() {
    cat > "$W/govern.json" <<EOF
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "agents": { "probe": { "provider": "gemini", "model": "m", "api_key_env": "AB_KEY",
                         "api_base": "$1" } } }
EOF
    local out
    out=$(cd "$W" && timeout 30 "$NAAB" p.naab 2>&1)
    case "$out" in
        *RAN*) ;;
        *) echo "broken:$(printf '%s' "$out" | tail -1)"; return ;;
    esac
    case "$out" in
        *"api_base ignored"*) echo refused ;;
        *) echo accepted ;;
    esac
}

expect() {  # id url expected description
    local v; v=$(verdict "$2")
    if [ "$v" = "$3" ]; then ok "$1" "$4"
    else bad "$1" "$4" "api_base=$2 -> $v (expected $3)"; fi
}

expect AB-01 "http://127.0.0.1@192.0.2.2:8080/" refused "userinfo before a non-loopback host is refused"
expect AB-02 "http://localhost.example.com/"    refused "a host that only STARTS with localhost is refused"
expect AB-03 "http://127.0.0.1.example.com/"    refused "a host that only STARTS with 127.0.0.1 is refused"
expect AB-04 "http://127.0.0.1:80x/"            refused "a non-numeric port is refused"

c5=""
for u in "http://127.0.0.1:9/" "http://localhost:9/" "http://[::1]:9/" "http://127.0.0.1"; do
    v=$(verdict "$u"); [ "$v" = accepted ] || c5="$c5 $u->$v"
done
if [ -z "$c5" ]; then ok "AB-05c" "CONTROL: real loopback forms are accepted (127.0.0.1, localhost, [::1], no port)"
else bad "AB-05c" "a real loopback api_base was refused -- AB-01..04 prove nothing" "$c5"; fi

v=$(verdict "https://api.example.com/")
if [ "$v" = accepted ]; then ok "AB-06c" "CONTROL: https is accepted for any host"
else bad "AB-06c" "https api_base was not accepted" "$v"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
