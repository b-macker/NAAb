#!/usr/bin/env bash
# ============================================================
# test_telemetry_count.sh — the exit summary counts every line it wrote
#
# THE FAILURE THIS CATCHES
#
# "[governance] Telemetry: N events written to F" counted only the check
# results dumped at exit. Agent events (AGENT_SEND, CDD_TURN,
# VALIDATION_RECORDED, ...), the chain anchors, attestations and end-of-run
# health warnings are written by other paths, so an agent run printed
# "30 events written" over a file of 112 lines -- which reads as telemetry
# loss. An outside dogfood run (Gemini, release-notes pipeline, F-04) spent its
# time investigating file locking and flushing because of it.
#
#   TC-01  agent run: the printed total equals the lines in the file, and the
#          breakdown names both halves
#   TC-02  CONTROL: a run with no agent still matches (no double counting)
#   TC-03  the total is printed once per run, not once per writeReports() call
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

W="${TMPDIR:-/tmp}/naab-telcount-$$"
STUB_PID=""
cleanup(){ [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }

echo "=== Telemetry exit summary count ==="

reported() { grep -oE 'Telemetry: [0-9]+ events written' "$1" | grep -oE '[0-9]+' | tail -1; }

# TC-02 first: no agent.
printf '{"version":"5.0","mode":"enforce","security":{"sandbox_level":"standard"},"telemetry":{"enabled":true,"output_file":"t.jsonl","tamper_evidence":{"enabled":true}}}\n' > "$W/govern.json"
printf 'use string\nmain { print(string.upper("x")) }\n' > "$W/p.naab"
(cd "$W" && "$NAAB" p.naab > out.txt 2> err.txt)
n=$(reported "$W/err.txt"); lines=$(wc -l < "$W/t.jsonl" | tr -d ' ')
if [ -n "$n" ] && [ "$n" = "$lines" ]; then ok "TC-02" "CONTROL: no-agent run reports $n and the file has $lines lines"
else bad "TC-02" "no-agent run reported '$n' for $lines lines"; fi

IS_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;; esac
if ! command -v python3 >/dev/null 2>&1 || [ "$IS_WINDOWS" = 1 ]; then
    skip "TC-01" "agent stub unavailable (UNMEASURABLE)"; skip "TC-03" "agent stub unavailable (UNMEASURABLE)"
else
    rm -f "$W/t.jsonl"
    printf '{"routes": {"TCOUNT": {"responses": [{"content": "hello there", "output_tokens": 20}]}}}\n' > "$W/stub.json"
    if start_stub "$W/stub.json" "$W"; then
        cat > "$W/govern.json" <<EOF
{"version":"5.0","mode":"enforce","security":{"sandbox_level":"standard"},
 "telemetry":{"enabled":true,"output_file":"t.jsonl","tamper_evidence":{"enabled":true}},
 "behavioral_sequences":{"enabled":true},
 "context_drift":{"enabled":true,"check_interval_turns":1},
 "agents":{"a":{"provider":"gemini","model":"gemini-2.5-flash","api_key_env":"GEMINI_API_KEY",
   "api_base":"http://127.0.0.1:$STUB_PORT","system_prompt":"TCOUNT probe","max_tokens":100}}}
EOF
        printf 'use agent\nmain {\n  let h = agent.create("a")\n  agent.send(h, "one")\n  agent.send(h, "two")\n  agent.record_validation(h, true, "ok")\n  print("done")\n}\n' > "$W/a.naab"
        (cd "$W" && GEMINI_API_KEY=stub "$NAAB" a.naab > out.txt 2> err.txt)
        n=$(reported "$W/err.txt"); lines=$(wc -l < "$W/t.jsonl" | tr -d ' ')
        if [ -n "$n" ] && [ "$n" = "$lines" ] && grep -q 'agent, anchor and health events' "$W/err.txt"; then
            ok "TC-01" "agent run reports $n and the file has $lines lines, with the breakdown"
        else
            bad "TC-01" "agent run reported '$n' for $lines lines" "$(grep 'Telemetry:' "$W/err.txt")"
        fi
        cnt=$(grep -c 'events written to' "$W/err.txt")
        if [ "$cnt" -eq 1 ]; then ok "TC-03" "the summary is printed once"
        else bad "TC-03" "the summary is printed $cnt times"; fi
    else
        skip "TC-01" "stub failed to start (UNMEASURABLE)"; skip "TC-03" "stub failed to start (UNMEASURABLE)"
    fi
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
