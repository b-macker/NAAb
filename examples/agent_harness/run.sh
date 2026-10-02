#!/usr/bin/env bash
# run.sh -- run the agent harness in a fresh, self-contained run directory.
#
#   ./run.sh --stub            local scripted model (no API key, deterministic)
#   ./run.sh --live            real provider; needs GEMINI_API_KEY, and a signed
#                              govern.json if you have trusted keys installed
#
# Every run gets out/run-<timestamp>/ holding a copy of the harness, its
# govern.json, the workspace and fixtures, and everything the run produced
# (report.json, telemetry.jsonl, transcript.jsonl, stdout/stderr). Nothing is
# overwritten and nothing is shared between runs: telemetry appends across
# runs by design (the hash chain is file-anchored), so pointing two runs at one
# file is how per-run numbers end up cumulative.
#
# --stub rewrites ONLY api_base (to the local stub) in the run's copy of
# govern.json, and isolates the trust store so an unsigned copy is not an
# integrity block. Every other setting is exactly what --live runs.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
MODE="${1:---stub}"

case "$MODE" in --stub|--live) ;; *) echo "usage: $0 [--stub|--live]" >&2; exit 2 ;; esac
[ -x "$NAAB" ] || { echo "naab-lang not found at $NAAB (build it, or set NAAB=)" >&2; exit 2; }

RUN="$HERE/out/run-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN/out"
cp "$HERE/src/harness.naab" "$HERE/src/govern.json" "$RUN/"
cp -r "$HERE/workspace" "$HERE/fixtures" "$RUN/"
# The signature travels with the config (--live; --stub edits the copy, so it
# would not verify there and is not needed with an isolated trust store).
[ "$MODE" = "--live" ] && [ -f "$HERE/src/govern.json.sig" ] && cp "$HERE/src/govern.json.sig" "$RUN/"

STUB_PID=""
cleanup() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    [ -n "${NAAB_TRUST_STORE_DIR:-}" ] && [ "$MODE" = "--stub" ] && rm -rf "$NAAB_TRUST_STORE_DIR"
}
trap cleanup EXIT

if [ "$MODE" = "--stub" ]; then
    source "$REPO/tests/helpers/stub_launch.sh"
    export NAAB_TRUST_STORE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ah-trust-XXXXXX")"
    start_stub "$RUN/fixtures/stub_responses.json" "$RUN/out" || { echo "stub failed to start" >&2; exit 2; }
    python3 - "$RUN/govern.json" "$STUB_PORT" <<'PY'
import json, sys
p, port = sys.argv[1], sys.argv[2]
cfg = json.load(open(p))
for a in cfg["agents"].values():
    a["api_base"] = "http://127.0.0.1:" + port
json.dump(cfg, open(p, "w"), indent=2)
PY
    export GEMINI_API_KEY="stub-key"
fi

echo "run directory: $RUN"
(cd "$RUN" && "$NAAB" harness.naab > out/stdout.txt 2> out/stderr.txt)
RC=$?
echo "exit code: $RC"
grep -E '^(PLAN|STEP|REVIEW|VERDICT|REPORT)\|' "$RUN/out/stdout.txt" || true
if [ "$RC" -ne 0 ]; then
    echo "--- stderr (last 25 lines) ---"
    tail -25 "$RUN/out/stderr.txt"
fi
exit "$RC"
