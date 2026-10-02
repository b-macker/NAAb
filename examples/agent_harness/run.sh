#!/usr/bin/env bash
# run.sh -- run the agent harness under its signed governance, in a fresh run directory.
#
#   ./run.sh --stub            local scripted model (no API key, deterministic)
#   ./run.sh --live            real provider; needs GEMINI_API_KEY
#
# 1. LOCK CHECK. src/govern.json must verify against keys/harness-signing.pub
#    (tools/lock_check.sh). A broken lock stops here in both modes; a stale one
#    stops --live (the engine refuses it anyway) and is reported in --stub.
# 2. Every run gets out/run-<timestamp>-<pid>/ holding a copy of the harness,
#    its govern.json + signature, the workspace and fixtures, and everything
#    the run produced. Nothing is shared between runs: telemetry appends by
#    design (the hash chain is file-anchored), so two runs on one file would
#    make per-run numbers cumulative.
# 3. The run itself uses a throwaway trust store, so it is pinned to ONE key:
#      --live  the committed public key, and the committed signature.
#      --stub  the run's copy differs in exactly one setting -- api_base on
#              each agent, pointed at the local stub -- so it is re-signed with
#              a key generated for this run alone. Signature verification,
#              flag locks and staleness stay in force; only the key differs.
#    NAAB_SIGNING_KEY is never exported to the run.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="${NAAB_REPO:-$(cd "$HERE/../.." && pwd)}"
NAAB="${NAAB:-$REPO/build/naab-lang}"
MODE="${1:---stub}"

case "$MODE" in --stub|--live) ;; *) echo "usage: $0 [--stub|--live]" >&2; exit 2 ;; esac
[ -x "$NAAB" ] || { echo "naab-lang not found at $NAAB (build it, or set NAAB=)" >&2; exit 2; }

lock="$(NAAB="$NAAB" NAAB_REPO="$REPO" "$HERE/tools/lock_check.sh")"; lock_rc=$?
echo "$lock" | head -1
case "$lock_rc" in
    0) ;;
    2) [ "$MODE" = "--live" ] && { echo "$lock"; echo "re-sign with tools/sign.sh" >&2; exit 3; } ;;
    *) echo "$lock" >&2; echo "governance lock is not intact -- refusing to run" >&2; exit 3 ;;
esac

RUN="$HERE/out/run-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN/out"
cp "$HERE/src/harness.naab" "$HERE/src/govern.json" "$HERE/src/govern.json.sig" "$RUN/"
cp -r "$HERE/workspace" "$HERE/fixtures" "$RUN/"

TS="$(mktemp -d "${TMPDIR:-/tmp}/ah-trust-XXXXXX")"
EPH=""
STUB_PID=""
cleanup() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    rm -rf "$TS" ${EPH:+"$EPH"}
}
trap cleanup EXIT

if [ "$MODE" = "--live" ]; then
    NAAB_TRUST_STORE_DIR="$TS" "$NAAB" --trust-key "$HERE/keys/harness-signing.pub" >/dev/null 2>&1 \
        || { echo "could not install the harness public key" >&2; exit 2; }
else
    source "$REPO/tests/helpers/stub_launch.sh"
    start_stub "$RUN/fixtures/stub_responses.json" "$RUN/out" || { echo "stub failed to start" >&2; exit 2; }
    python3 - "$RUN/govern.json" "$STUB_PORT" <<'PY'
import json, sys
p, port = sys.argv[1], sys.argv[2]
cfg = json.load(open(p))
for a in cfg["agents"].values():
    a["api_base"] = "http://127.0.0.1:" + port
json.dump(cfg, open(p, "w"), indent=2)
PY
    EPH="$(mktemp -d "${TMPDIR:-/tmp}/ah-runkey-XXXXXX")"
    "$NAAB" --keygen "$EPH/run.pem" >/dev/null 2>&1 \
        && NAAB_TRUST_STORE_DIR="$TS" "$NAAB" --trust-key "$EPH/run.pem.pub" >/dev/null 2>&1 \
        && NAAB_SIGNING_KEY="$EPH/run.pem" "$NAAB" --sign-governance "$RUN/govern.json" >/dev/null 2>&1 \
        || { echo "could not sign the run's copy" >&2; exit 2; }
    rm -rf "$EPH"; EPH=""
    export GEMINI_API_KEY="stub-key"
fi

echo "run directory: $RUN"
(cd "$RUN" && env -u NAAB_SIGNING_KEY NAAB_TRUST_STORE_DIR="$TS" \
    "$NAAB" harness.naab > out/stdout.txt 2> out/stderr.txt)
RC=$?
echo "exit code: $RC"
grep -E '^(STATIC|PLAN|STEP|REVIEW|VERDICT|SUMMARY|REPORT)\|' "$RUN/out/stdout.txt" || true
if [ "$RC" -ne 0 ]; then
    echo "--- stdout (last 15 lines) ---"; tail -15 "$RUN/out/stdout.txt"
    echo "--- stderr (last 15 lines) ---"; tail -15 "$RUN/out/stderr.txt"
fi
exit "$RC"
