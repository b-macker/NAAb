#!/usr/bin/env bash
# test_reply_secret_default.sh -- what a model writes is scanned for secrets
#                                 unless the operator turns it off
#
# code_quality.no_secrets defaulted to off everywhere, so a config that never
# named it handed the program every key a model wrote (CLAUDE.md, "Response
# secret/PII scanning is OPT-IN"; it was left off then because turning it on
# would block replies that passed). Bundle 6 turns it on for MODEL OUTPUT
# only -- the reply, each propose candidate, and the arguments of a tool call
# the model makes -- when the config does not set no_secrets at all.
# `"no_secrets": false` (code_quality or restrictions) keeps it off; the
# program's own text, its prompts and its tool results stay opt-in. A signed
# reload that writes `false` over the default is a loosening, refused.
#
#   RS-00  CONTROL: a reply with no key reaches the program (the harness runs)
#   RS-01  no no_secrets key: a reply holding a key ends the run (exit 3) and
#          never reaches the program
#   RS-02  "code_quality": {"no_secrets": false}: the same reply is delivered
#   RS-02b "restrictions": {"no_secrets": false} (the alias): delivered
#   RS-03  a tool call whose ARGUMENTS hold a key ends the run before the
#          tool runs; RS-03c CONTROL: the same tool call without the key runs
#   RS-04  a propose candidate holding a key ends the run
#   RS-05  SCOPE: the program's own text is not scanned by default -- a
#          program holding a key-shaped literal runs (exit 0). Without this,
#          RS-01 passes for a build that switched no_secrets on everywhere
#   RS-06  a signed reload from no key to "no_secrets": false is refused, and
#          the next reply is still scanned; R-00 CONTROL: the swap harness
#          reloads and an unrelated tightening is accepted
#
# Kill table (observed 2026-10-30, each mutant built and run alone):
#   reply site back to checkSecrets()             -> RS-01 red
#   default on but explicit false ignored         -> RS-02, RS-02b red
#   tool-argument site back to checkSecrets()     -> RS-03 red
#   propose site back to checkSecrets()           -> RS-04 red
#   rules.no_secrets defaulted true instead       -> RS-05 red
#   ratchet entry removed                         -> RS-06 red
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_rs_XXXXXX")

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -12 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));   echo "  UNMEASURABLE [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_reply_secret_default.sh"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
# Unsigned configs until the reload arms sign their own: with any key in the
# trust store each would be an integrity block, and every refusal arm would
# pass for it.
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
SWAP_LOADED=0
cleanup() {
    stop_stub
    [ "$SWAP_LOADED" = 1 ] && stop_swap_operators
    teardown_isolated_trust
    rm -rf "$W"
}
trap cleanup EXIT

echo "=== agent replies are scanned for secrets by default ==="
if [ ! -x "$NAAB" ]; then echo "  naab-lang not built: UNMEASURABLE, not a pass"; exit 1; fi

# A key-shaped value built at run time (the precedent in test_guidance.sh), so
# no literal secret sits in this file.
KEY="AKIA$(printf 'Q%.0s' $(seq 1 16))"
export RS_FAKE_KEY="x"

# fixture JSON (from python, so quoting is right): $1 = file, rest = specs
mkfix() { python3 -c 'import json,sys; json.dump({"responses":[json.loads(a) for a in sys.argv[2:]]}, open(sys.argv[1],"w"))' "$@"; }

N=0
# $1 = top-level JSON members (may be empty), $2 = agent extras, $3 = program
# body (inside main), rest = fixture specs. Sets RC, OUT.
run_case() {
    local extra="$1" agent_extra="$2" body="$3"; shift 3
    N=$((N+1)); local d="$W/c$N"; mkdir -p "$d"
    mkfix "$d/fixture.json" "$@"
    if ! start_stub "$d/fixture.json" "$d"; then stop_stub; RC=-1; OUT="stub did not start"; return; fi
    cat > "$d/govern.json" <<EOF
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" }${extra},
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "RS_FAKE_KEY",
      "max_tokens": 50${agent_extra} } } }
EOF
    printf 'use agent\n%s\nmain {\n%s\n}\n' "${PRELUDE:-}" "$body" > "$d/p.naab"
    OUT=$(cd "$d" && timeout 120 "$NAAB" p.naab </dev/null 2>&1); RC=$?
    stop_stub
}
got() { grep -q "^$1" <<<"$OUT"; }
SEND='    let h = agent.create("worker")
    let r = agent.send(h, "hello")
    print("REPLY_RECEIVED")'
KEY_REPLY="{\"content\": \"here is the key $KEY\", \"output_tokens\": 9}"
OK_REPLY='{"content": "nothing to see", "output_tokens": 4}'

run_case "" "" "$SEND" "$OK_REPLY"
if [ "$RC" -eq -1 ]; then unm RS-00 "the agent stub did not start"
elif [ "$RC" -eq 0 ] && got REPLY_RECEIVED; then pass RS-00 "CONTROL: a reply with no key reaches the program"
else fail RS-00 "the control reply did not arrive (rc=$RC)" "$OUT"; fi

run_case "" "" "$SEND" "$KEY_REPLY"
if [ "$RC" -eq -1 ]; then unm RS-01 "the agent stub did not start"
elif [ "$RC" -eq 3 ] && ! got REPLY_RECEIVED && grep -qF 'code_quality.no_secrets' <<<"$OUT"; then
    pass RS-01 "no no_secrets key: a reply holding a key ends the run (exit 3)"
else fail RS-01 "expected the reply to be refused (rc=$RC)" "$OUT"; fi

run_case ', "code_quality": { "no_secrets": false }' "" "$SEND" "$KEY_REPLY"
if [ "$RC" -eq -1 ]; then unm RS-02 "the agent stub did not start"
elif [ "$RC" -eq 0 ] && got REPLY_RECEIVED; then pass RS-02 "\"no_secrets\": false keeps reply scanning off"
else fail RS-02 "an explicit false was not honoured (rc=$RC)" "$OUT"; fi

run_case ', "restrictions": { "no_secrets": false }' "" "$SEND" "$KEY_REPLY"
if [ "$RC" -eq -1 ]; then unm RS-02b "the agent stub did not start"
elif [ "$RC" -eq 0 ] && got REPLY_RECEIVED; then pass RS-02b "the restrictions alias turns it off too"
else fail RS-02b "the restrictions alias was not honoured (rc=$RC)" "$OUT"; fi

PRELUDE='fn peek(k) {
    print("TOOL_RAN")
    return "ok"
}'
TOOL_BODY='    agent.register_tool("peek", peek,
        {"description": "probe", "parameters": {"k": {"type": "string", "description": "v"}}})
    let h = agent.create("worker")
    let r = agent.send(h, "go")
    print("REPLY_RECEIVED")'
TOOLS=', "tools_enabled": true, "tools": ["peek"], "max_turns": 10'
run_case "" "$TOOLS" "$TOOL_BODY" "{\"tool_calls\": [{\"name\": \"peek\", \"args\": {\"k\": \"$KEY\"}}]}" "$OK_REPLY"
T_RC=$RC; T_OUT=$OUT
run_case "" "$TOOLS" "$TOOL_BODY" '{"tool_calls": [{"name": "peek", "args": {"k": "plain"}}]}' "$OK_REPLY"
if [ "$T_RC" -eq -1 ] || [ "$RC" -eq -1 ]; then unm RS-03 "the agent stub did not start"
else
    if [ "$RC" -eq 0 ] && got TOOL_RAN && got REPLY_RECEIVED; then
        pass RS-03c "CONTROL: the same tool call without a key runs"
    else fail RS-03c "the control tool call did not run (rc=$RC)" "$OUT"; fi
    if [ "$T_RC" -eq 3 ] && ! grep -q '^TOOL_RAN' <<<"$T_OUT" && grep -qF 'code_quality.no_secrets' <<<"$T_OUT"; then
        pass RS-03 "tool-call arguments holding a key end the run before the tool runs"
    else fail RS-03 "expected the tool call to be refused (rc=$T_RC)" "$T_OUT"; fi
fi
PRELUDE=""

run_case "" ', "propose_candidates_max": 2' '    let h = agent.create("worker")
    let p = agent.propose(h, "hello", 1)
    print("PROPOSED")' "$KEY_REPLY"
if [ "$RC" -eq -1 ]; then unm RS-04 "the agent stub did not start"
elif [ "$RC" -eq 3 ] && ! got PROPOSED && grep -qF 'code_quality.no_secrets' <<<"$OUT"; then
    pass RS-04 "a propose candidate holding a key ends the run"
else fail RS-04 "expected the candidate to be refused (rc=$RC)" "$OUT"; fi

d="$W/scope"; mkdir -p "$d"
echo '{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" } }' > "$d/govern.json"
printf 'main {\n    let k = "%s"\n    print("RAN " + k.length())\n}\n' "$KEY" > "$d/p.naab"
OUT=$(cd "$d" && timeout 60 "$NAAB" p.naab </dev/null 2>&1); RC=$?
if [ "$RC" -eq 0 ] && got RAN; then pass RS-05 "SCOPE: the program's own text is not scanned by default"
else fail RS-05 "a program holding a key literal was refused with no no_secrets key (rc=$RC)" "$OUT"; fi

# ---- RS-06: the reload ratchet -------------------------------------------------
if ! command -v python3 >/dev/null 2>&1; then
    unm R-00 "python3 missing -- the reload arms are UNMEASURABLE"
else
    source "$REPO/tests/helpers/config_swap.sh"; SWAP_LOADED=1
    "$NAAB" --keygen "$W/k.pem" >/dev/null 2>&1
    "$NAAB" --trust-key "$W/k.pem.pub" >/dev/null 2>&1
    export NAAB_SIGNING_KEY="$W/k.pem"
    # reload_run NAME NEXT_EXTRA -> OUT, RC. Starts with no no_secrets key;
    # the program swaps in NEXT, then sends (a reload is checked before each
    # send) and gets a reply holding a key.
    reload_run() {
        local d="$W/$1" n="$W/$1.next"; mkdir -p "$d" "$n"
        mkfix "$d/fixture.json" "$KEY_REPLY"
        if ! start_stub "$d/fixture.json" "$d"; then stop_stub; RC=-1; OUT="stub did not start"; return; fi
        local hdr="\"version\": \"5.0\", \"mode\": \"enforce\", \"security\": { \"sandbox_level\": \"elevated\" },
  \"agents\": { \"worker\": { \"provider\": \"gemini\", \"model\": \"stub-model\",
      \"api_base\": \"http://127.0.0.1:$STUB_PORT\", \"api_key_env\": \"RS_FAKE_KEY\", \"max_tokens\": 50 } }"
        printf '{ %s }\n' "$hdr" > "$d/govern.json"
        printf '{ %s%s }\n' "$hdr" "$2" > "$n/govern.json"
        (cd "$d" && "$NAAB" --sign-governance >/dev/null 2>&1); (cd "$n" && "$NAAB" --sign-governance >/dev/null 2>&1)
        if [ ! -f "$d/govern.json.sig" ] || [ ! -f "$n/govern.json.sig" ]; then stop_stub; RC=-2; OUT="signing failed"; return; fi
        cat > "$d/r.naab" <<EOF
use agent
main {
    let h = agent.create("worker")
    let s = <<python
import os, time
def _swap(src, dst):
    if os.path.exists(".swap_done"):
        os.remove(".swap_done")
    with open(".swap_req", "w") as f:
        f.write(src + "\t" + dst + "\n")
    open(".swap_req.ready", "w").close()
    for _ in range(1200):
        if os.path.exists(".swap_done"):
            return
        time.sleep(0.05)
    raise RuntimeError("config swap operator did not respond")
time.sleep(1)
_swap("$n/govern.json.sig", "$d/govern.json.sig")
_swap("$n/govern.json", "$d/govern.json")
"swapped"
>>
    print(s)
    let r = agent.send(h, "hello")
    print("REPLY_RECEIVED")
}
EOF
        start_swap_operator "$d"
        OUT=$(cd "$d" && timeout 120 "$NAAB" r.naab </dev/null 2>&1); RC=$?
        stop_stub
    }
    swapped() { grep -q '^swapped' <<<"$OUT"; }

    reload_run R00 ', "limits": { "timeout": { "global": 600 } }'
    if [ "$RC" -lt 0 ]; then unm R-00 "$OUT -- the reload arms are UNMEASURABLE"
    elif ! swapped; then unm R-00 "the config swap did not happen -- the reload arms are UNMEASURABLE"
    elif grep -q 'Reload rejected' <<<"$OUT"; then fail R-00 "CONTROL: an unrelated tightening reload was refused" "$OUT"
    elif [ "$RC" -eq 3 ] && ! got REPLY_RECEIVED; then
        pass R-00 "CONTROL: the swap harness reloads, the reload is accepted, the reply is still scanned"
    else fail R-00 "CONTROL: unexpected outcome (rc=$RC)" "$OUT"; fi

    reload_run RS06 ', "code_quality": { "no_secrets": false }'
    if [ "$RC" -lt 0 ] || ! swapped; then unm RS-06 "the config swap did not happen"
    elif grep -q 'Reload rejected: ratchet violation.*no_secrets' <<<"$OUT" && [ "$RC" -eq 3 ] && ! got REPLY_RECEIVED; then
        pass RS-06 "a reload writing no_secrets false over the default is refused; the reply is still scanned"
    else fail RS-06 "expected the reload refused and the reply blocked (rc=$RC)" "$OUT"; fi
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ]
