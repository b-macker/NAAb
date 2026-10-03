#!/usr/bin/env bash
# ============================================================
# test_entry_point_parity.sh -- the same policy, through every door
#
# WHY THIS EXISTS. Enforcement is installed per entry point rather than per
# policy. Of the seven places that construct an Interpreter, TWO install a
# ScopedSandbox: src/cli/main.cpp and src/interpreter/context.cpp. The four REPL
# sources and src/api/rest_api.cpp install none. Worse, the one in main.cpp sits
# INSIDE the `if (command == "run")` branch, so `calibrate` and `race` are
# siblings outside its scope.
#
# So "is this allowed?" currently has a different answer depending on which door
# the code came through, and nothing measures that. This suite does: one policy,
# several entry points, and the verdicts must agree.
#
# IT IS DELIBERATELY NOT ALL-OR-NOTHING, because the two enforcement layers
# behave differently and conflating them hides the finding:
#
#   EP-01  a GOVERNANCE-layer rule (capabilities.filesystem.blocked_paths).
#          The governance engine runs in-process at both entry points, so this
#          arm is expected to AGREE. It is the control that keeps EP-02 honest:
#          without it, a red EP-02 could just mean "the REST arm fails at
#          everything" or "the harness cannot drive REST at all".
#
#   EP-02  a SANDBOX-layer rule (sandbox_level "restricted" forbidding polyglot
#          execution). This is the one that diverges.
#
#   EP-03  the process must survive a refusal. The REST /execute handler answers
#          a HARD governance block with _exit(3), so a single request takes the
#          whole daemon down and every concurrent request with it.
#
#   EP-04  a PERMITTED sandbox rule must be honoured at both doors too. This is
#          the assertion the first version of this file was missing, and its
#          absence hid a real defect: after the deny-by-default change the REST
#          arm refused a config that said "elevated", because `naab-lang api`
#          never established the process policy at all. EP-02 still passed --
#          both doors refused the restricted case -- so the suite reported
#          parity while the two doors agreed only on "no". Parity that holds in
#          one direction is not parity, and a test that only checks the
#          refusing direction will certify a build that refuses everything.
#
# EVIDENCE IS A SIDE EFFECT, never a message. Each probe writes a marker file;
# its presence is execution. A polyglot block's stdout is captured rather than
# forwarded, so asserting on printed output measures nothing (that mistake cost
# a full rewrite of the sibling suite, tests/security/test_polyglot_gate_coverage.sh).
#
# A FRESH SERVER PER REST PROBE, because of EP-03: once a probe trips the
# _exit(3) path there is no server left to run the next one, and reusing it
# would report later probes as "refused" when really nothing was listening.
#
#   EP-05  a STOPPED server must be gone. The server used to be started as
#          `( cd "$WDIR" && "$NAAB" api ... ) &` without exec, so $! was the
#          subshell and `kill -9 $!` killed only that: every server this suite
#          started outlived it -- four orphaned `naab-lang api` per Linux run,
#          the "Terminate orphan process: naab-lang" GitHub's job cleanup
#          reported. The arm asks the PORT, not a PID, so it sees an orphan
#          whichever process the script happens to hold; the same /health
#          probe answered moments earlier in start_server, which is what makes
#          its silence mean something. See docs/findings/build-windows-ci.md.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }
report() { echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

echo "=== Entry-point parity: one policy, every door ==="

if [ ! -x "$NAAB" ]; then
    skip "EP-00" "naab-lang not built -- UNMEASURABLE, not a pass"; report; exit 0
fi
if ! command -v curl >/dev/null 2>&1; then
    skip "EP-00" "curl unavailable -- cannot drive the REST entry point"; report; exit 0
fi

# Unsigned govern.json files below would be an INTEGRITY BLOCK (exit 3) on any
# machine holding a trusted key, before either layer is consulted -- and "no
# marker" is exactly what this suite reads as "refused".
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust

WDIR="$(mktemp -d)"
SRV_PID=""
STOPPED=0; SURVIVED=0
cleanup() {
    # wait reaps the server, so it no longer holds $WDIR/naab.db open when the
    # rm below runs (Windows refuses to delete an open file).
    [ -n "${SRV_PID:-}" ] && { kill -9 "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null; }
    teardown_isolated_trust
    rm -rf "$WDIR"
}
trap cleanup EXIT
mkdir -p "$WDIR/workspace"
echo "SECRET" > "$WDIR/workspace/secret.txt"

write_cfg() {  # $1 = full govern.json body
    printf '%s\n' "$1" > "$WDIR/govern.json"
}

POLY_CFG='{ "version": "4.0", "mode": "enforce", "security": { "sandbox_level": "restricted" } }'
PATH_CFG='{ "version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "capabilities": { "filesystem": { "mode": "read_write", "blocked_paths": ["./workspace/secret.txt"] } } }'

# ---- CLI arm ------------------------------------------------------------
cli_probe() {  # $1=cfg $2=naab source $3=marker ; returns 0 if it EXECUTED
    write_cfg "$1"
    printf '%s\n' "$2" > "$WDIR/probe.naab"
    rm -f "$3"
    ( cd "$WDIR" && timeout 120 "$NAAB" run probe.naab >/dev/null 2>&1 )
    [ -f "$3" ]
}

# ---- REST arm -----------------------------------------------------------
start_server() {  # $1 = cfg ; sets SRV_PID and SRV_PORT
    local i
    write_cfg "$1"
    SRV_PORT=$(( (RANDOM % 20000) + 20000 ))
    # exec, so the backgrounded subshell IS the server and $! is its pid (the
    # same shape tests/api/test_platform_fixes.sh documents). Without it the
    # kill in stop_server hits the subshell and the server is orphaned (EP-05).
    ( cd "$WDIR" && exec "$NAAB" api "$SRV_PORT" > "$WDIR/server.log" 2>&1 ) &
    SRV_PID=$!
    for i in $(seq 1 60); do
        curl -sS --max-time 2 "http://127.0.0.1:$SRV_PORT/health" >/dev/null 2>&1 && return 0
        kill -0 "$SRV_PID" 2>/dev/null || return 1
        sleep 0.5
    done
    return 1
}
stop_server() {
    [ -n "${SRV_PID:-}" ] || return 0
    kill -9 "$SRV_PID" 2>/dev/null; wait "$SRV_PID" 2>/dev/null
    SRV_PID=""
    # EP-05's evidence: the port, not the pid we hold.
    STOPPED=$((STOPPED+1))
    curl -sS --max-time 2 "http://127.0.0.1:$SRV_PORT/health" >/dev/null 2>&1 && SURVIVED=$((SURVIVED+1))
    return 0
}

check_stopped_servers() {
    if [ "$STOPPED" -eq 0 ]; then
        skip "EP-05" "no server was started and stopped -- UNMEASURABLE"
    elif [ "$SURVIVED" -eq 0 ]; then
        ok "EP-05" "every stopped server is gone ($STOPPED stopped, none still answering)"
    else
        bad "EP-05" "$SURVIVED of $STOPPED stopped servers still answer /health -- orphaned"
    fi
}

json_escape() { python3 -c 'import json,sys; print(json.dumps(sys.stdin.read()))'; }

rest_probe() {  # $1=cfg $2=naab source $3=marker ; returns 0 if it EXECUTED
    rm -f "$3"
    start_server "$1" || { REST_UP=0; return 1; }
    REST_UP=1
    local payload
    payload="$(printf '%s\n' "$2" | json_escape)"
    curl -sS --max-time 120 -X POST "http://127.0.0.1:$SRV_PORT/api/v1/execute" \
        -H 'Content-Type: application/json' \
        -d "{\"code\": $payload}" >/dev/null 2>&1
    SRV_ALIVE=0; kill -0 "$SRV_PID" 2>/dev/null && SRV_ALIVE=1
    stop_server
    [ -f "$3" ]
}

POLY_SRC='main {
    <<python
open("'"$WDIR"'/marker_poly.txt","w").write("x")
>>
}'
PATH_SRC='main {
    let c = file.read("./workspace/secret.txt")
    file.write("'"$WDIR"'/marker_path.txt", c)
}'

# ---- EP-00: both doors must work at all --------------------------------
# Without this every "refused" below is unfalsifiable: a REST arm that cannot
# execute anything reports perfect compliance.
BENIGN_SRC='main {
    file.write("'"$WDIR"'/marker_benign.txt", "ok")
}'
OPEN_CFG='{ "version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" } }'

cli_probe "$OPEN_CFG" "$BENIGN_SRC" "$WDIR/marker_benign.txt" && CLI_OK=1 || CLI_OK=0
rest_probe "$OPEN_CFG" "$BENIGN_SRC" "$WDIR/marker_benign.txt" && REST_OK=1 || REST_OK=0
if [ "$CLI_OK" = 1 ] && [ "$REST_OK" = 1 ]; then
    ok "EP-00" "both entry points execute a permitted program"
else
    skip "EP-00" "cli=$CLI_OK rest=$REST_OK -- cannot drive both doors, UNMEASURABLE"
    # The REST probe above may still have started (and stopped) a server, and
    # this early exit is the path the Windows runner takes -- the one orphan its
    # job cleanup reported came from here. EP-05 does not need both doors.
    check_stopped_servers
    report
    [ $FAIL -eq 0 ] || exit 1
    exit 0
fi

# ---- EP-01: governance layer, expected to AGREE -------------------------
echo "--- EP-01: a governance-layer rule (blocked_paths)"
cli_probe "$PATH_CFG" "$PATH_SRC" "$WDIR/marker_path.txt" && CLI_PATH=EXECUTED || CLI_PATH=refused
rest_probe "$PATH_CFG" "$PATH_SRC" "$WDIR/marker_path.txt" && REST_PATH=EXECUTED || REST_PATH=refused
REST_PATH_ALIVE="$SRV_ALIVE"
if [ "$CLI_PATH" = "$REST_PATH" ]; then
    ok "EP-01" "both doors agree (cli=$CLI_PATH rest=$REST_PATH)"
else
    bad "EP-01" "doors disagree on a governance rule: cli=$CLI_PATH rest=$REST_PATH"
fi

# ---- EP-02: sandbox layer, the divergence -------------------------------
echo "--- EP-02: a sandbox-layer rule (restricted forbids polyglot)"
cli_probe "$POLY_CFG" "$POLY_SRC" "$WDIR/marker_poly.txt" && CLI_POLY=EXECUTED || CLI_POLY=refused
rest_probe "$POLY_CFG" "$POLY_SRC" "$WDIR/marker_poly.txt" && REST_POLY=EXECUTED || REST_POLY=refused
if [ "$CLI_POLY" = "$REST_POLY" ]; then
    ok "EP-02" "both doors agree (cli=$CLI_POLY rest=$REST_POLY)"
else
    bad "EP-02" "doors disagree on a sandbox rule: cli=$CLI_POLY rest=$REST_POLY"
fi

# ---- EP-04: a PERMITTED sandbox rule must be honoured at both doors -----
# The mirror of EP-02. Without it, "both refused" counts as agreement even when
# one door is refusing because it never read the configuration.
echo "--- EP-04: a permissive sandbox rule must be honoured at both doors"
PERMIT_CFG='{ "version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" } }'
cli_probe "$PERMIT_CFG" "$POLY_SRC" "$WDIR/marker_poly.txt" && CLI_OK2=EXECUTED || CLI_OK2=refused
rest_probe "$PERMIT_CFG" "$POLY_SRC" "$WDIR/marker_poly.txt" && REST_OK2=EXECUTED || REST_OK2=refused
if [ "$CLI_OK2" = "EXECUTED" ] && [ "$REST_OK2" = "EXECUTED" ]; then
    ok "EP-04" "both doors honour a permissive sandbox rule"
elif [ "$CLI_OK2" != "EXECUTED" ]; then
    skip "EP-04" "the CLI arm did not execute under a permissive config -- UNMEASURABLE (cli=$CLI_OK2)"
else
    bad "EP-04" "the config permits it and one door still refuses: cli=$CLI_OK2 rest=$REST_OK2"
fi

# ---- EP-03: a refusal must not kill the process -------------------------
echo "--- EP-03: refusing a request must not terminate the server"
if [ "$REST_PATH_ALIVE" = 1 ]; then
    ok "EP-03" "the server survived a refused request"
else
    bad "EP-03" "a single refused request terminated the daemon"
fi

# ---- EP-05: a stopped server must be gone -------------------------------
echo "--- EP-05: stopping the server must stop the server"
check_stopped_servers

report
[ $FAIL -eq 0 ] || exit 1
exit 0
