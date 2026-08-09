#!/usr/bin/env bash
# ============================================================
# stub_launch.sh — one hardened launcher for agent_stub.py
#
# WHY THIS IS SHARED
#
# The naive launcher picks a random port with no bind check and waits a flat
# 5s for READY. Both fail on a loaded CI runner: a collision (or a lingering
# TIME_WAIT socket) leaves the stub dead, and python3 startup + bind can exceed
# 5s. Either way every assertion in the suite then fails, or the suite aborts,
# for a reason that has nothing to do with what it measures.
#
# That diagnosis was made once and the fix reached 2 of 29 suites. The rest
# kept the one-shot pick — invisibly, because CI was not running most of them.
# When the governance_v4 sweep started running all of them, the very first CI
# run went red on test_per_agent_signals.sh with "stub failed to start". The
# defect was years old; only its visibility was new.
#
# So the launcher lives here, once. A fix to it now reaches every caller,
# which is the whole point.
#
# WINDOWS
#
# The retry path is actively harmful under MSYS2 and this is the FAILURE path
# specifically — a stub that comes up promptly never enters it, which is why
# Windows passed until the retry itself was added:
#   * `wait` after a plain TERM can block forever. run-all-tests.sh already
#     warns that native Windows binaries under MSYS2 ignore TERM; a process
#     tree holding an unkillable child is also why the runner could not
#     enforce its own step timeout.
#   * fork/exec costs ~50-100ms there, so a loop spawning grep + kill + sleep
#     per iteration is dominated by spawning rather than by intended waiting.
# Windows therefore keeps the single 5s attempt that ran green for many jobs.
# Most callers also source stub_platform.sh and skip Windows outright, but the
# guard is kept here so this is safe for the ones that do not.
#
# USAGE
#   source "<repo>/tests/helpers/stub_launch.sh"
#   start_stub "$WDIR/fixture.json" "$WDIR" || { skip "X-00" "stub failed"; exit 0; }
#   ...                                     # $STUB_PORT is set
#   stop_stub
#
# Source it by any path you like — it locates agent_stub.py relative to itself,
# so it does not care where the caller lives.
# ============================================================

STUB_PID="${STUB_PID:-}"
STUB_PORT="${STUB_PORT:-}"

# Resolve agent_stub.py from THIS file's location, not the caller's $SCRIPT_DIR.
# The original form was "$SCRIPT_DIR/../helpers/agent_stub.py", which silently
# assumes every caller lives in tests/<something>/ — true of the 30 suites this
# was extracted from, and false the moment examples/living-script_v3/run.sh
# sourced it, where it resolved to examples/helpers/ and the stub "failed to
# start" for a reason having nothing to do with ports.
#
# A helper that only works from one directory depth is a helper that will be
# copied rather than shared, which is the failure this file exists to end.
STUB_HELPER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

start_stub() {  # $1=fixture json  $2=workdir
    local _fx="$1" _dir="$2" _try _i _tries=3
    case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) _tries=1 ;; esac
    [ -n "${WINDIR:-}" ] && _tries=1

    for _try in $(seq 1 $_tries); do
        STUB_PORT=$(( (RANDOM % 20000) + 20000 ))
        : > "$_dir/stub.log"
        python3 "$STUB_HELPER_DIR/agent_stub.py" "$STUB_PORT" "$_fx" "$_dir" \
            > "$_dir/stub.log" 2>&1 &
        STUB_PID=$!

        if [ "$_tries" -eq 1 ]; then
            for _i in $(seq 1 50); do
                grep -q READY "$_dir/stub.log" 2>/dev/null && return 0
                sleep 0.1
            done
            return 1
        fi

        # 30s, not 5s — a slow start is not a failed start. sleep 0.5 keeps the
        # spawn count near the original despite the longer ceiling.
        for _i in $(seq 1 60); do
            grep -q READY "$_dir/stub.log" 2>/dev/null && return 0
            kill -0 "$STUB_PID" 2>/dev/null || break
            sleep 0.5
        done
        # SIGKILL, and no unbounded wait: reaping is not worth a hang.
        kill -9 "$STUB_PID" 2>/dev/null; STUB_PID=""
    done

    echo "  start_stub: no READY after $_tries port attempt(s) — stub log tail:" >&2
    tail -3 "$_dir/stub.log" >&2 2>/dev/null
    return 1
}

stop_stub() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    wait "$STUB_PID" 2>/dev/null
    STUB_PID=""
}
