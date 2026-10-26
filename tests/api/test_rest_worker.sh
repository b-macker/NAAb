#!/usr/bin/env bash
# ============================================================
# test_rest_worker.sh -- REST /execute runs each request in a fresh
# `naab-lang run` worker on the default engine (Bundle 2 item 2.2)
#
# /execute used to run every request on a tree-walker inside the server
# process. Under enforce the tree-walker is refused (Bundle 2 item 2.1), and in
# the server it shared one process with every other request. Each request now
# runs in its own worker process (src/api/worker.h, shared with
# `naab-lang mcp`), under the govern.json the server's directory resolves to.
#
#   RW-00   CONTROL: the CLI refuses the sequence program (exit 3, no write) --
#           the pattern fires on the default engine
#   RW-01   REST refuses it the same way: the request ran on the VM (the
#           tree-walker records no stdlib sequence events, B11, so on the old
#           in-process path it ran and wrote the file)
#   RW-02   CONTROL: a program without the first step runs and writes
#   RW-03   nothing one request leaves in Python is there for the next
#           (UNMEASURABLE without the embedded Python executor)
#   RW-04   a failing request: "output" keeps what the program printed,
#           "error" carries the error, exit_code 1
#   RW-05   no program file is left in the server's directory
#   RW-06   a server whose directory resolves to no govern.json refuses a
#           request (exit_code 4), as a file run does ...
#   RW-06c  CONTROL ... and runs it when the operator started the server with
#           --no-governance
#
# Kill table (master's binary, which runs /execute in process; then each mutant
# rebuilt and run):
#   in-process tree-walker (master)    RW-01, RW-03, RW-06 (RW-00/02/06c pass)
#   the waiver not passed to workers   RW-06c
#   the program file left behind       RW-05
# ============================================================
set -uo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"

if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2
    exit 1
fi
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        echo "  UNMEASURABLE [RW] /execute is not available on Windows (POSIX workers)"
        echo "  Results: 0 passed, 0 failed, 1 unmeasurable"
        exit 0 ;;
esac
if ! command -v curl >/dev/null 2>&1; then
    echo "  UNMEASURABLE [RW] curl not available"
    echo "  Results: 0 passed, 0 failed, 1 unmeasurable"
    exit 0
fi

PASS=0; FAIL=0; UNM=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       | /'; }
unm() { UNM=$((UNM+1)); echo "  UNMEASURABLE [$1] $2"; }

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-restw-XXXXXX")"
PIDS=()
cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; done
    rm -rf "$W"
}
trap cleanup EXIT

mkdir -p "$W/srv" "$W/nocfg" "$W/nocfg_waived"
cat > "$W/srv/govern.json" <<'EOF'
{ "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "behavioral_sequences": { "enabled": true, "default_pattern_enforcement": "observe",
    "patterns": [ { "name": "env_then_write", "level": "hard", "max_gap": 5,
                    "sequence": ["env.get", "file.write"] } ] } }
EOF

# start DIR [GLOBAL_FLAGS...] -> PORT, appends the pid to PIDS; 1 if not up
start() {
    local d="$1"; shift
    PORT=$((20000 + RANDOM % 9000))
    ( cd "$W/$d" && exec "$NAAB" "$@" api "$PORT" --api-key k-rw >"$W/$d.log" 2>&1 ) &
    PIDS+=("$!")
    for _ in $(seq 1 100); do
        curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && return 0
        sleep 0.1
    done
    return 1
}
# execute PORT SOURCE -> RESP (the JSON body)
execute() {
    RESP="$(python3 -c 'import json,sys; print(json.dumps({"code": sys.stdin.read()}))' <<<"$2" |
        curl -s --max-time 60 -X POST "http://127.0.0.1:$1/api/v1/execute" \
            -H "Authorization: Bearer k-rw" -H "Content-Type: application/json" --data-binary @-)"
}
field() { python3 -c 'import json,sys; d=json.load(sys.stdin); v=d.get(sys.argv[1]); print("" if v is None else v)' "$1" <<<"$RESP" 2>/dev/null; }

SEQ='use env
use file
main {
    let v = env.get("HOME")
    file.write("out.txt", "x")
    print("RAN")
}'
PLAIN='use file
main {
    file.write("out.txt", "x")
    print("RAN")
}'

# --- RW-00: the CLI control -------------------------------------------------
printf '%s\n' "$SEQ" > "$W/srv/seq.naab"
OUT="$(cd "$W/srv" && timeout 60 "$NAAB" run seq.naab 2>&1)"; RC=$?
if [ "$RC" = 3 ] && [ ! -e "$W/srv/out.txt" ]; then
    ok "RW-00" "control: the CLI refuses the sequence (exit 3, nothing written)"
else
    bad "RW-00" "the CLI should refuse the sequence -- the probe measures nothing" "rc=$RC $OUT"
fi
rm -f "$W/srv/out.txt" "$W/srv/seq.naab"

if ! start srv; then
    unm "RW" "the API server did not start"; head -5 "$W/srv.log"
    echo ""; echo "  Results: $PASS passed, $FAIL failed, $UNM unmeasurable"; [ "$FAIL" -eq 0 ]; exit
fi
SRV_PORT=$PORT

# --- RW-01: REST refuses it the same way ------------------------------------
execute "$SRV_PORT" "$SEQ"
if [ "$(field exit_code)" = 3 ] && [ ! -e "$W/srv/out.txt" ]; then
    ok "RW-01" "REST refuses the sequence too (exit_code 3, nothing written): the request ran on the VM"
else
    bad "RW-01" "REST should refuse the sequence the CLI refuses" "$RESP out.txt=$([ -e "$W/srv/out.txt" ] && echo written || echo absent)"
fi
rm -f "$W/srv/out.txt"

# --- RW-02: control, the program without the first step ---------------------
execute "$SRV_PORT" "$PLAIN"
if [ "$(field exit_code)" = 0 ] && [ -e "$W/srv/out.txt" ] && grep -q RAN <<<"$(field output)"; then
    ok "RW-02" "control: without env.get the request runs and writes"
else
    bad "RW-02" "the plain program should run" "$RESP"
fi
rm -f "$W/srv/out.txt"

# --- RW-03: no Python state crosses requests --------------------------------
execute "$SRV_PORT" 'main {
    let r = <<python
import builtins
builtins.naab_rw_left = "LEFT-BEHIND"
"set"
>>
    print("SET=" + r)
}'
if ! grep -q 'SET=set' <<<"$(field output)"; then
    unm "RW-03" "the first request did not run its Python block (no embedded Python here?)"
else
    execute "$SRV_PORT" 'main {
    let r = <<python
import builtins
getattr(builtins, "naab_rw_left", "none")
>>
    print("GOT=" + r)
}'
    if grep -q 'GOT=none' <<<"$(field output)"; then
        ok "RW-03" "what one request left in Python is gone in the next"
    elif grep -q 'GOT=LEFT-BEHIND' <<<"$(field output)"; then
        bad "RW-03" "the second request read the first request's Python state" "$RESP"
    else
        bad "RW-03" "the second request did not report" "$RESP"
    fi
fi

# --- RW-04: output and error kept apart -------------------------------------
execute "$SRV_PORT" 'main {
    print("BEFORE")
    let x = [1, 2][5]
}'
o="$(field output)"; e="$(field error)"
if [ "$(field exit_code)" = 1 ] && grep -q BEFORE <<<"$o" && ! grep -q 'Error' <<<"$o" && grep -qi 'out of bounds' <<<"$e"; then
    ok "RW-04" "a failing request: output keeps the print, error carries the error, exit_code 1"
else
    bad "RW-04" "output and error should be kept apart" "$RESP"
fi

# --- RW-05: no program file left behind -------------------------------------
left="$(find "$W/srv" -maxdepth 1 -name '.naab-rest-*' | wc -l | tr -d ' ')"
if [ "$left" = 0 ]; then ok "RW-05" "no program file left in the server's directory"
else bad "RW-05" "$left program file(s) left in the server's directory" "$(ls -a "$W/srv")"; fi

# --- RW-06 / RW-06c: no govern.json, with and without the operator's waiver --
if [ -f "$W/govern.json" ] || [ -f "$(dirname "$W")/govern.json" ]; then
    unm "RW-06" "a govern.json sits above $W -- no config-free directory"
else
    if start nocfg; then
        execute "$PORT" "$PLAIN"
        if [ "$(field exit_code)" = 4 ] && [ ! -e "$W/nocfg/out.txt" ]; then
            ok "RW-06" "with no govern.json a request is refused (exit_code 4), as a file run is"
        else
            bad "RW-06" "with no govern.json a request should be refused" "$RESP"
        fi
    else
        unm "RW-06" "the second API server did not start"
    fi
    if start nocfg_waived --no-governance; then
        execute "$PORT" "$PLAIN"
        if [ "$(field exit_code)" = 0 ] && [ -e "$W/nocfg_waived/out.txt" ]; then
            ok "RW-06c" "control: a server started with --no-governance runs it"
        else
            bad "RW-06c" "the operator's waiver should let the request run" "$RESP"
        fi
    else
        unm "RW-06c" "the third API server did not start"
    fi
fi

echo ""
echo "  Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
