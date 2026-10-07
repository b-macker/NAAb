#!/usr/bin/env bash
# run_harness.sh -- run examples/agent_harness with every agent's api_base
# pointed at the LLM bridge (tools/llm_bridge/bridge.py).
#
#   run_harness.sh RUN_DIR                      a director answers the queue
#   run_harness.sh RUN_DIR --autoreply FIXTURE  answered from an agent_stub
#                                               fixture (the CONTROL run)
#   run_harness.sh RUN_DIR ... --deviation NAME one named deviation from the
#                                               shipped governance (see
#                                               patch_config.py); must come last
#   run_harness.sh RUN_DIR --replay FIXTURE     NO bridge: tests/helpers/
#                                               agent_stub.py serves FIXTURE
#                                               (e.g. one written by
#                                               `bridge.py export-fixture`),
#                                               same config patch -- the
#                                               deterministic replay of a run
#
# Mirrors examples/agent_harness/run.sh --stub, with the bridge in place of
# the stub. The run's govern.json is a COPY of the shipped one, changed in
# exactly these ways, each written to RUN_DIR/config_changes.json:
#
#   agents.*.api_base                         -> http://127.0.0.1:<bridge port>
#   agent_dispatch.default_timeout_seconds    -> 7200   (per-call HTTP timeout)
#   agent_dispatch.hard_stop.max_agent_time_ms-> 21600000 (cumulative call time)
#   runtime.timeout, limits.timeout.global    -> 21600  (whole-run wall clock)
#   agents.*.standing_lease_seconds (if > 0)  -> 86400
#
# Everything else is the SHIPPED governance. The timeouts are raised because a
# director-answered call takes tens of seconds to minutes, against a few
# seconds for a real provider: left as shipped, they would end the run on the
# instrument's latency, not on anything a model did. The standing lease's
# WALL-CLOCK half is raised for the same reason -- an expired lease forces a
# step-up challenge, which would then be a reaction to the bridge. Its
# TURN-based half (standing_lease_turns) is unchanged, so lease expiry by
# turns is still measured; lease expiry by elapsed time is NOT measured by any
# run made through this script.
#
# The copy is re-signed with a key generated for this run and deleted after,
# exactly as run.sh --stub does; the committed signature must still verify
# first (tools/lock_check.sh), or nothing runs.
#
# On exit, RUN_DIR/queue/naab_exit holds the interpreter's exit code -- that is
# how `bridge.py next` knows the run is over.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${NAAB_REPO:-$(cd "$HERE/../.." && pwd)}"
NAAB="${NAAB:-$REPO/build/naab-lang}"
SRC="$REPO/examples/agent_harness"

RUN="${1:-}"
[ -n "$RUN" ] || { echo "usage: $0 RUN_DIR [--autoreply FIXTURE | --replay FIXTURE]" >&2; exit 2; }
shift
AUTOREPLY=""
REPLAY=""
DEVIATION=""
case "${1:-}" in
    --autoreply) AUTOREPLY="$(cd "$(dirname "${2:?fixture path}")" && pwd)/$(basename "$2")" ;;
    --replay)    REPLAY="$(cd "$(dirname "${2:?fixture path}")" && pwd)/$(basename "$2")" ;;
    --deviation) DEVIATION="${2:?deviation name}" ;;
    "") ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
esac
case "${3:-}" in --deviation) DEVIATION="${4:?deviation name}" ;; esac

[ -x "$NAAB" ] || { echo "naab-lang not found at $NAAB (build it, or set NAAB=)" >&2; exit 2; }
if [ -e "$RUN" ] && [ -n "$(ls -A "$RUN" 2>/dev/null)" ]; then
    echo "run directory $RUN exists and is not empty -- refusing to reuse it" >&2
    exit 2
fi
mkdir -p "$RUN"
RUN="$(cd "$RUN" && pwd)"

lock="$(NAAB="$NAAB" NAAB_REPO="$REPO" "$SRC/tools/lock_check.sh")"; lock_rc=$?
echo "$lock" | head -1
case "$lock_rc" in
    0|2) ;;   # ok, or stale (reported; the run's copy is re-signed fresh)
    *) echo "$lock" >&2; echo "governance lock is not intact -- refusing to run" >&2; exit 3 ;;
esac

H="$RUN/harness"
Q="$RUN/queue"
mkdir -p "$H/out" "$Q"
cp "$SRC/src/harness.naab" "$SRC/src/govern.json" "$SRC/src/govern.json.sig" "$H/"
cp -r "$SRC/workspace" "$SRC/fixtures" "$H/"

TS="$(mktemp -d "${TMPDIR:-/tmp}/llmb-trust-XXXXXX")"
EPH=""
BRIDGE_PID=""
AUTO_PID=""
cleanup() {
    [ -n "$AUTO_PID" ] && kill "$AUTO_PID" 2>/dev/null
    [ -n "$BRIDGE_PID" ] && kill "$BRIDGE_PID" 2>/dev/null
    rm -rf "$TS" ${EPH:+"$EPH"}
}
trap cleanup EXIT

if [ -n "$REPLAY" ]; then
    # shellcheck source=/dev/null
    source "$REPO/tests/helpers/stub_launch.sh"
    start_stub "$REPLAY" "$Q" || { echo "stub failed to start" >&2; exit 2; }
    BRIDGE_PID="$STUB_PID"
    PORT="$STUB_PORT"
    echo "replay stub: 127.0.0.1:$PORT  fixture: $REPLAY  state: $Q"
else
    python3 "$HERE/bridge.py" serve --queue "$Q" --response-timeout 7000 \
        > "$RUN/bridge.stdout" 2> "$RUN/bridge.stderr" &
    BRIDGE_PID=$!
    PORT=""
    for _ in $(seq 1 120); do
        PORT="$(sed -n 's/^READY \([0-9][0-9]*\)$/\1/p' "$RUN/bridge.stdout" 2>/dev/null)"
        [ -n "$PORT" ] && break
        kill -0 "$BRIDGE_PID" 2>/dev/null || break
        python3 -c 'import time; time.sleep(0.25)'
    done
    [ -n "$PORT" ] || { echo "bridge failed to start:" >&2; cat "$RUN/bridge.stderr" >&2; exit 2; }
    echo "bridge: 127.0.0.1:$PORT  queue: $Q"
fi

# Feed the config by bytes on stdin, not by path (CLAUDE.md: a native helper
# need not share the shell's path vocabulary).
python3 "$HERE/patch_config.py" "$PORT" $DEVIATION < "$H/govern.json" \
    > "$RUN/govern.patched.json" 2> "$RUN/config_changes.json" \
    || { echo "config patch failed" >&2; cat "$RUN/config_changes.json" >&2; exit 2; }
cp "$RUN/govern.patched.json" "$H/govern.json"

EPH="$(mktemp -d "${TMPDIR:-/tmp}/llmb-runkey-XXXXXX")"
"$NAAB" --keygen "$EPH/run.pem" >/dev/null 2>&1 \
    && NAAB_TRUST_STORE_DIR="$TS" "$NAAB" --trust-key "$EPH/run.pem.pub" >/dev/null 2>&1 \
    && NAAB_SIGNING_KEY="$EPH/run.pem" "$NAAB" --sign-governance "$H/govern.json" >/dev/null 2>&1 \
    || { echo "could not sign the run's copy" >&2; exit 2; }
rm -rf "$EPH"; EPH=""
export GEMINI_API_KEY="bridge-key"

if [ -n "$AUTOREPLY" ]; then
    python3 "$HERE/bridge.py" autoreply --queue "$Q" --fixture "$AUTOREPLY" \
        > "$RUN/autoreply.log" 2>&1 &
    AUTO_PID=$!
fi

date -u +%Y-%m-%dT%H:%M:%SZ > "$RUN/started_at"
echo "running: $H"
(cd "$H" && env -u NAAB_SIGNING_KEY NAAB_TRUST_STORE_DIR="$TS" \
    "$NAAB" harness.naab > out/stdout.txt 2> out/stderr.txt)
RC=$?
echo "$RC" > "$Q/naab_exit"
date -u +%Y-%m-%dT%H:%M:%SZ > "$RUN/finished_at"
[ -n "$AUTO_PID" ] && wait "$AUTO_PID" 2>/dev/null
echo "exit code: $RC"
grep -E '^(STATIC|PLAN|STEP|REVIEW|VERDICT|SUMMARY|REPORT)\|' "$H/out/stdout.txt" || true
if [ "$RC" -ne 0 ]; then
    echo "--- stdout (last 15 lines) ---"; tail -15 "$H/out/stdout.txt"
    echo "--- stderr (last 25 lines) ---"; tail -25 "$H/out/stderr.txt"
fi
exit "$RC"
