#!/usr/bin/env bash
# test_rest_hard_block_survives.sh -- a HARD governance block over REST must
# fail the REQUEST, not the server (F28).
#
# main.cpp answers a HARD block with _exit(3): right for the CLI, one script
# and one verdict. That handler was copied into the REST /execute worker, so
# any client could terminate the daemon -- and every concurrent request --
# by submitting code that trips governance. Fixed in #223 (c5a645f), but
# nothing pinned it: this suite is that pin.
#
# R-01 is the positive control: the request really was HARD-blocked
#      (exit_code 3, and the protected file's contents did not leak). Without
#      it, R-02 passes for a server that never blocked anything.
# R-02 the server still answers /health afterwards.
# R-03 and still executes the next request normally.

set -uo pipefail
PASS=0
FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="$REPO/build/naab-lang"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/naab_resthb.XXXXXX")"
[ -n "$WORK" ] && [ -d "$WORK" ] || { echo "FATAL: could not create work dir" >&2; exit 1; }
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
API_PID=""
cleanup() {
    [ -n "$API_PID" ] && { kill "$API_PID" 2>/dev/null; wait "$API_PID" 2>/dev/null; }
    rm -rf "$WORK"
    teardown_isolated_trust
}
trap cleanup EXIT

if [ ! -x "$NAAB" ]; then
    echo "FAIL: naab-lang not built at $NAAB (UNMEASURABLE, not a pass)"
    exit 1
fi
if ! command -v curl >/dev/null 2>&1; then
    echo "SKIP: curl not available (UNMEASURABLE)"
    exit 0
fi

pass() { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
fail() { echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "         $3"; FAIL=$((FAIL+1)); }

# Reading the server's own govern.json is a HARD block: it is added to
# blocked_paths at load. The marker lets R-01 check nothing leaked.
cat > "$WORK/govern.json" <<'EOF'
{ "version": "4.0", "mode": "enforce", "meta": { "note": "RESTHB_MARKER" },
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "read" } } }
EOF

PORT=$((19000 + RANDOM % 900))
( cd "$WORK" && exec "$NAAB" api "$PORT" --api-key "k-test" >"$WORK/server.log" 2>&1 ) &
API_PID=$!
up=0
for _ in $(seq 1 100); do
    if curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then up=1; break; fi
    sleep 0.1
done
if [ "$up" -ne 1 ]; then
    echo "SKIP: API server did not start (UNMEASURABLE)"
    head -5 "$WORK/server.log"
    exit 0
fi

execute() {  # execute NAAB_SOURCE -> response body
    python3 -c 'import json,sys; print(json.dumps({"code": sys.stdin.read()}))' <<<"$1" |
        curl -s -X POST "http://127.0.0.1:$PORT/api/v1/execute" \
            -H "Authorization: Bearer k-test" -H "Content-Type: application/json" \
            --data-binary @- 2>&1
}

echo "=== R: a HARD block fails the request, not the server ==="
resp="$(execute 'use file
main {
    let s = file.read("govern.json")
    print(s)
}')"
case "$resp" in
    *RESTHB_MARKER*) fail R-01 "the protected config was returned -- nothing was blocked" ;;
    *'"exit_code": 3'*) pass R-01 "control: the request was HARD-blocked (exit_code 3)" ;;
    *) fail R-01 "no HARD block in the response" "$(head -c 300 <<<"$resp")" ;;
esac

if curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && kill -0 "$API_PID" 2>/dev/null; then
    pass R-02 "the server is still up after the HARD block"
else
    fail R-02 "the server died on a HARD-blocked request" "$(tail -3 "$WORK/server.log")"
fi

resp="$(execute 'main {
    print("NEXT_OK")
}')"
case "$resp" in
    *NEXT_OK*) pass R-03 "the next request still executes" ;;
    *) fail R-03 "the next request did not execute" "$(head -c 300 <<<"$resp")" ;;
esac

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
