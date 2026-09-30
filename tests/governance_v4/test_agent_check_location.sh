#!/usr/bin/env bash
# ============================================================
# test_agent_check_location.sh — agent-path checks are stamped with the agent,
# and deduplication never drops a violation into an earlier pass
#
# Found in the repo-sentinel dogfood (round 6): a fixer response echoed a
# fixture secret, code_quality.no_secrets (HARD) ended the run with exit 3, and
# the violation was missing from telemetry. Two defects compounded:
#   - the check location was a plain member, and agent-path checks never set
#     one, so the response scan was stamped with whatever the script checked
#     last (here, a process.run python block);
#   - telemetry.deduplicate_checks keyed on (rule, file, line), so once the
#     response scans looked like a located source check, the run-ending
#     violation shared a key with an earlier PASS and was dropped.
#
#   AL-01  dedup ON: the run-ending no_secrets violation reaches telemetry
#   AL-02  dedup OFF: the violation is attributed to <agent:fixer>, not to the
#          script's last polyglot site
#   AL-03  control: the script's own process.run check keeps its location --
#          without it, AL-02 passes for an engine that stamps everything
#          "<agent:...>"
#   AL-04  control: dedup still collapses repeated located passes -- without
#          it, AL-01 passes for an engine that turned dedup off entirely
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="${_SYSTMP}/agent-check-location-$$"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_agent_check_location.sh"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_PID=""
cleanup() {
    [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null
    teardown_isolated_trust
    rm -rf "${TEST_TMP:?}"
}
trap cleanup EXIT
mkdir -p "$TEST_TMP"
export FAKE_KEY_ACL="fake-key-agent-check-location"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

# $1=arm dir  $2=deduplicate_checks (true|false). Sets RC.
run_arm() {
    local d="$1"; mkdir -p "$d"
    python3 - "$d/fixture.json" <<'EOF'
import json, sys
json.dump({"responses": [
    {"content": "Clean patch: add input validation to the request handler.", "output_tokens": 30},
    {"content": "Patch: api_key = \"sk-ant-api03-AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"", "output_tokens": 30},
]}, open(sys.argv[1], "w"))
EOF
    STUB_PID=""; start_stub "$d/fixture.json" "$d" || return 1
    cat > "$d/govern.json" <<EOF
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl", "deduplicate_checks": $2 },
  "code_quality": { "no_secrets": { "enabled": true, "level": "hard" } },
  "agents": { "fixer": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_ACL",
      "max_tokens": 100, "max_turns": 5, "retry": { "max_attempts": 1, "backoff_ms": 0 } } } }
EOF
    # Two process.run python checks give the script a located check site to
    # leak from (and give AL-04 a repeated located pass to collapse).
    cat > "$d/t.naab" <<'EOF'
use agent
use process
main {
    let a = process.run("python3", ["-c", "print(1)"])
    let b = process.run("python3", ["-c", "print(2)"])
    let h = agent.create("fixer")
    let r1 = agent.send(h, "write a patch")
    let r2 = agent.send(h, "write another patch")
    print("NOT_BLOCKED")
}
EOF
    (cd "$d" && timeout 60 "$NAAB" t.naab > out.txt 2>&1); RC=$?
    kill "$STUB_PID" 2>/dev/null; STUB_PID=""
    return 0
}

# Prints "<event_type> <file>" for each no_secrets row, then the dedup summary.
rows() {
python3 - "$1" <<'EOF'
import json, sys
for l in open(sys.argv[1], encoding="utf-8", errors="replace"):
    try: e = json.loads(l)
    except ValueError: continue
    if e.get("rule_name") == "code_quality.no_secrets":
        print(e["event_type"], e.get("file") or "<none>")
    if e.get("event_type") == "GovernanceCheckSummary":
        print("SUMMARY deduplicated=%s" % e.get("deduplicated"))
EOF
}

echo "=== agent-path check location + dedup ==="
if ! command -v python3 >/dev/null; then skip "AL-00" "python3 unavailable (UNMEASURABLE)"; exit 0; fi

run_arm "$TEST_TMP/dedup" true || { skip "AL-00" "stub failed to start (UNMEASURABLE)"; exit 0; }
if [ "$RC" -ne 3 ] || [ ! -f "$TEST_TMP/dedup/tele.jsonl" ]; then
    fail "AL-00" "fixture did not end in the HARD secret block (rc=$RC) -- every verdict below is void" \
         "$(tail -3 "$TEST_TMP/dedup/out.txt")"
else
    R="$(rows "$TEST_TMP/dedup/tele.jsonl")"
    case "$R" in
        *"RuleViolation"*) pass "AL-01" "dedup on: the run-ending violation is in telemetry" ;;
        *) fail "AL-01" "dedup on: the run-ending violation is missing from telemetry" "$(tr '\n' ' ' <<<"$R")" ;;
    esac
    D=$(sed -n 's/^SUMMARY deduplicated=//p' <<<"$R")
    if [ "${D:-0}" -gt 0 ]; then
        pass "AL-04" "control: dedup still collapses repeated located passes ($D collapsed)"
    else
        fail "AL-04" "dedup collapsed nothing -- AL-01 would pass with dedup switched off" "$(tr '\n' ' ' <<<"$R")"
    fi
fi

run_arm "$TEST_TMP/nodedup" false || { skip "AL-02" "stub failed to start (UNMEASURABLE)"; }
if [ -f "$TEST_TMP/nodedup/tele.jsonl" ]; then
    R="$(rows "$TEST_TMP/nodedup/tele.jsonl")"
    V="$(grep '^RuleViolation' <<<"$R" | head -1)"
    if [ "$V" = "RuleViolation <agent:fixer>" ]; then
        pass "AL-02" "the violation is attributed to <agent:fixer>"
    else
        fail "AL-02" "the violation is attributed elsewhere" "${V:-no violation row}"
    fi
    if grep -q '^GovernanceCheck <process.run:python>' <<<"$R"; then
        pass "AL-03" "control: the script's process.run check keeps its own location"
    else
        fail "AL-03" "the script's own check lost its location" "$(tr '\n' ' ' <<<"$R")"
    fi
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
