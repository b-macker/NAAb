#!/usr/bin/env bash
# test_code_provenance.sh -- provenance for generated code (Bundle 14, PR 1)
#
# An execution event (CODEGEN_EXEC, POLYGLOT_EXEC) records the SHA-256 of the
# text executed and whether that exact text equals something a model wrote in
# this run (include/naab/code_provenance.h). On a match it names the model, the
# hash of the request body as sent and the hash of the model's whole reply.
# AGENT_RESPONSE gains content_sha256 (the whole reply) and request_sha256;
# content_hash, which covers the first 500 characters, is unchanged (A160).
#
# Every expected hash is computed HERE, by python, from inputs the engine did
# not produce: the fixture's reply text, the program's code text, and the
# request body the stub received and wrote to req_N.json. Nothing is compared
# with a value the engine printed elsewhere.
#
#   CP-00  CONTROL: the harness runs -- a reply is received, the code is
#          executed (CODEGEN_EXEC, exit_code 0) and telemetry is written
#   CP-01  a plain reply run through codegen.run(): code_sha256 is the hash of
#          the code, attribution is a match, origin_model is the agent's model,
#          origin_request_sha256 is the hash of the body the stub received,
#          origin_content_sha256 the hash of the reply
#   CP-02  a reply that is one fenced block: agent.send() strips the fence, the
#          stripped text is matched (returned_content) and origin_content_sha256
#          is the hash of the FENCED reply, which differs from code_sha256
#   CP-03  prose around a fenced block, taken out with agent.extract_code():
#          matched, derivation extract_code
#   CP-04  NEGATIVE: the program appends one character to the reply before
#          running it: unattributed, and code_sha256 is the edited text's hash.
#          Without it CP-01..03 pass for a build that matches everything
#   CP-05  NEGATIVE: code the program wrote itself (no agent call at all) is
#          unattributed, through codegen.run() and as a static block
#          (POLYGLOT_EXEC carries code_sha256 and source_file/line)
#   CP-06  a string a model passes to a tool, which the tool runs (under
#          codegen.allow_tainted_code, since a tool's arguments are tainted):
#          matched, derivation tool_argument, the request hash that of the
#          call the tool call answered
#   CP-07  a proposal candidate run before any commit: matched, derivation
#          proposal
#   CP-08  AGENT_RESPONSE for a reply over 500 characters: content_hash is
#          still the first 500 characters' hash, content_sha256 the whole
#          reply's, and the two differ; request_sha256 is the stub's body hash
#   CP-09  the same code text from two different replies (one fenced, one
#          plain): matched, origin_distinct_replies is 2
#   CP-10  hashes only: a token the model wrote appears in the program's
#          output (CONTROL) and nowhere in the telemetry file
#   CP-11  the telemetry chain still verifies with the new fields
#   CP-12  with telemetry off nothing is written and the program still runs
#          (there is no new key: the fields follow telemetry)
#
# Kill table (observed 2026-11-11, each mutant built and run alone):
#   see the table at the end of this header, filled from the revert checks.
#KILLTABLE
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_cp_XXXXXX")

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -12 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));   echo "  UNMEASURABLE [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_code_provenance.sh"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
# Unsigned configs: with a key in the trust store each run would be an
# integrity block and no arm would reach its subject.
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
cleanup() {
    stop_stub; teardown_isolated_trust
    if [ -n "${CP_KEEP:-}" ]; then echo "  kept: $W"; else rm -rf "$W"; fi
}
trap cleanup EXIT

echo "=== provenance for generated code ==="
if [ ! -x "$NAAB" ]; then echo "  naab-lang not built: UNMEASURABLE, not a pass"; exit 1; fi
export CP_FAKE_KEY="x"

# fixture JSON from python, so quoting is right: $1 = file, rest = specs
mkfix() { python3 -c 'import json,sys; json.dump({"responses":[json.loads(a) for a in sys.argv[2:]]}, open(sys.argv[1],"w",encoding="utf-8"))' "$@"; }

N=0; D=""
# $1 = telemetry on|off, $2 = agent extras, $3 = prelude, $4 = main body,
# rest = fixture specs. Sets RC, OUT, D (the case directory).
run_case() {
    local tel="$1" agent_extra="$2" prelude="$3" body="$4"; shift 4
    N=$((N+1)); D="$W/c$N"; mkdir -p "$D"
    mkfix "$D/fixture.json" "$@"
    if ! start_stub "$D/fixture.json" "$D"; then stop_stub; RC=-1; OUT="stub did not start"; return; fi
    local telcfg='"telemetry": {"enabled": true, "output_file": "tel.jsonl", "tamper_evidence": true},'
    [ "$tel" = off ] && telcfg=''
    cat > "$D/govern.json" <<EOF
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "codegen": {"enabled": true${CG_EXTRA:-}},
  $telcfg
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "CP_FAKE_KEY",
      "max_tokens": 50${agent_extra} } } }
EOF
    if ! python3 -c 'import json,sys; json.load(sys.stdin)' < "$D/govern.json" 2>/dev/null; then
        stop_stub; RC=-2; OUT="the generated govern.json is not JSON"; return
    fi
    printf 'use agent\nuse codegen\n%s\nmain {\n%s\n}\n' "$prelude" "$body" > "$D/p.naab"
    OUT=$(cd "$D" && timeout 120 "$NAAB" p.naab </dev/null 2>&1); RC=$?
    stop_stub
}

# The judge. Reads the case's telemetry and compares fields with hashes it
# computes itself. argv: case dir, then CHECK lines of the form
#   EVENT[#k] field op value
# EVENT#k is the k-th event of that type (0-based, default the first); op is
# = (equal), != (differs), absent, present. A value is literal text, or
#   sha:TEXT          sha256 of TEXT (utf-8)
#   sha500:TEXT       sha256 of TEXT's first 500 bytes
#   shafile:NAME      sha256 of the bytes of NAME in the case dir
#   field:EVENT#k.f   another event's field
# Prints OK, or one line per failed check and exits 1; exit 2 when the
# telemetry file is missing or holds no event of a named type (UNMEASURABLE
# for the caller to report: the subject never ran).
judge() {
    python3 - "$@" <<'PY'
import hashlib, json, os, sys
d = sys.argv[1]
path = os.path.join(d, "tel.jsonl")
if not os.path.exists(path):
    print("no telemetry file"); sys.exit(2)
events = {}
with open(path, encoding="utf-8", errors="replace") as f:
    for line in f:
        line = line.strip()
        if not line:
            continue
        try:
            ev = json.loads(line)
        except ValueError:
            continue
        events.setdefault(ev.get("event_type", ""), []).append(ev)

def sha(b):
    return hashlib.sha256(b).hexdigest()

def pick(spec):
    name, _, k = spec.partition("#")
    lst = events.get(name, [])
    k = int(k) if k else 0
    if k >= len(lst):
        print("no event %s (have %d of type %s)" % (spec, len(lst), name)); sys.exit(2)
    return lst[k]

def value(v):
    if v.startswith("sha:"):
        return sha(v[4:].encode("utf-8"))
    if v.startswith("sha500:"):
        return sha(v[7:].encode("utf-8")[:500])
    if v.startswith("shafile:"):
        with open(os.path.join(d, v[8:]), "rb") as f:
            return sha(f.read())
    if v.startswith("field:"):
        spec, _, fld = v[6:].rpartition(".")
        return pick(spec).get(fld)
    return v

bad = []
for chk in sys.argv[2:]:
    parts = chk.split(" ", 3)
    ev = pick(parts[0]); fld = parts[1]; op = parts[2]
    got = ev.get(fld)
    if op == "absent":
        if fld in ev: bad.append("%s: %s present (%r)" % (parts[0], fld, got))
        continue
    if op == "present":
        if not got: bad.append("%s: %s missing or empty" % (parts[0], fld))
        continue
    want = value(parts[3])
    if op == "=" and got != want:
        bad.append("%s: %s is %r, expected %r" % (parts[0], fld, got, want))
    if op == "!=" and got == want:
        bad.append("%s: %s is %r, expected it to differ" % (parts[0], fld, got))
if bad:
    print("\n".join(bad)); sys.exit(1)
print("OK")
PY
}

# Report one arm from the judge's verdict. $1 id, $2 message, $3.. checks.
arm() {
    local id="$1" msg="$2"; shift 2
    if [ "$RC" -eq -1 ]; then unm "$id" "the agent stub did not start"; return; fi
    if [ "$RC" -eq -2 ]; then fail "$id" "$OUT"; return; fi
    local v jr; v=$(judge "$D" "$@"); jr=$?
    if [ "$jr" -eq 0 ]; then pass "$id" "$msg"
    elif [ "$jr" -eq 2 ]; then fail "$id" "the subject never ran: $v (rc=$RC)" "$OUT"
    else fail "$id" "$msg" "$v"; fi
}

MATCH="code_attribution = model_output_match"
SEND='    let h = agent.create("worker")
    let r = agent.send(h, "write code")
    print("REPLY=" + r["content"])'
RUN='    let x = codegen.run("javascript", r["content"])
    print("END")'

# ---- CP-00 / CP-01: a plain reply -----------------------------------------
CODE1='6 * 7'
run_case on "" "" "$SEND
$RUN" "{\"content\": \"$CODE1\"}"
if [ "$RC" -eq -1 ]; then unm CP-00 "the agent stub did not start"
elif [ "$RC" -eq 0 ] && grep -qxF "REPLY=$CODE1" <<<"$OUT" && grep -q '^END$' <<<"$OUT" \
     && [ "$(judge "$D" "CODEGEN_EXEC exit_code = 0")" = OK ]; then
    pass CP-00 "a reply is received, its code runs, telemetry is written"
else fail CP-00 "the harness did not run its subject (rc=$RC)" "$OUT"; fi
arm CP-01 "a plain reply run as code is matched to its model, request and reply" \
    "CODEGEN_EXEC code_sha256 = sha:$CODE1" "CODEGEN_EXEC $MATCH" \
    "CODEGEN_EXEC origin_model = stub-model" \
    "CODEGEN_EXEC origin_request_sha256 = shafile:req_1.json" \
    "CODEGEN_EXEC origin_content_sha256 = sha:$CODE1" \
    "CODEGEN_EXEC origin_config_name = worker" \
    "CODEGEN_EXEC origin_derivation = response" \
    "CODEGEN_EXEC origin_distinct_replies = 1" \
    "POLYGLOT_EXEC code_sha256 = sha:$CODE1" "POLYGLOT_EXEC $MATCH"

# ---- CP-02: a wholly fenced reply ------------------------------------------
FENCED=$'```javascript\n6 * 7\n```'
run_case on "" "" "$SEND
$RUN" "$(python3 -c 'import json,sys; print(json.dumps({"content": sys.argv[1]}))' "$FENCED")"
arm CP-02 "a fence-stripped reply is matched; the reply hash is the fenced text's" \
    "CODEGEN_EXEC code_sha256 = sha:$CODE1" "CODEGEN_EXEC $MATCH" \
    "CODEGEN_EXEC origin_derivation = returned_content" \
    "CODEGEN_EXEC origin_content_sha256 = sha:$FENCED" \
    "CODEGEN_EXEC origin_content_sha256 != sha:$CODE1" \
    "CODEGEN_EXEC origin_request_sha256 = shafile:req_1.json"

# ---- CP-03: agent.extract_code() -------------------------------------------
PROSE=$'Here is the code you asked for.\n\n```javascript\n6 * 7\n```\n\nIt multiplies.'
run_case on "" "" "$SEND"'
    let code = agent.extract_code(r["content"], "javascript")
    let x = codegen.run("javascript", code)
    print("END")' "$(python3 -c 'import json,sys; print(json.dumps({"content": sys.argv[1]}))' "$PROSE")"
# What extract_code returns is the engine's to decide; the judge takes the
# executed text's hash from the event and checks only that it is NOT the whole
# reply's and that the origin is the whole reply.
arm CP-03 "code taken out of a reply with agent.extract_code() is matched" \
    "CODEGEN_EXEC $MATCH" "CODEGEN_EXEC origin_derivation = extract_code" \
    "CODEGEN_EXEC origin_content_sha256 = sha:$PROSE" \
    "CODEGEN_EXEC code_sha256 != sha:$PROSE" \
    "CODEGEN_EXEC origin_request_sha256 = shafile:req_1.json"

# ---- CP-04: NEGATIVE, the program edits the reply ---------------------------
run_case on "" "" "$SEND"'
    let x = codegen.run("javascript", r["content"] + " ")
    print("END")' "{\"content\": \"$CODE1\"}"
arm CP-04 "an edited reply is unattributed, with the edited text's hash" \
    "CODEGEN_EXEC code_sha256 = sha:$CODE1 " "CODEGEN_EXEC code_attribution = unattributed" \
    "CODEGEN_EXEC origin_model absent" "CODEGEN_EXEC origin_request_sha256 absent" \
    "CODEGEN_EXEC origin_content_sha256 absent"

# ---- CP-05: NEGATIVE, the program's own code --------------------------------
run_case on "" "" '    let x = codegen.run("javascript", "2 + 3")
    let y = <<javascript
1 + 1
>>
    print("END")' '{"content": "unused"}'
arm CP-05 "the program's own code is unattributed, by codegen.run() and as a block" \
    "CODEGEN_EXEC code_sha256 = sha:2 + 3" "CODEGEN_EXEC code_attribution = unattributed" \
    "CODEGEN_EXEC origin_model absent" \
    "POLYGLOT_EXEC#1 code_attribution = unattributed" "POLYGLOT_EXEC#1 code_sha256 present" \
    "POLYGLOT_EXEC#1 line = 6" "POLYGLOT_EXEC#1 origin_model absent"

# ---- CP-06: a tool argument -------------------------------------------------
TOOL_PRELUDE='fn runit(code) {
    let x = codegen.run("javascript", code)
    print("TOOL_RAN")
    return "done"
}'
TOOL_BODY='    agent.register_tool("runit", runit,
        {"description": "run code", "parameters": {"code": {"type": "string", "description": "code"}}})
    let h = agent.create("worker")
    let r = agent.send(h, "go")
    print("END")'
TOOLS=', "tools_enabled": true, "tools": ["runit"], "max_turns": 10'
CODE6='9 - 4'
# A tool's arguments are tainted, and codegen refuses tainted code unless the
# operator allows it (observed: without the grant the tool's call is refused
# and no CODEGEN_EXEC is written). The arm is about what is recorded when the
# operator has allowed it.
CG_EXTRA=', "allow_tainted_code": true'
run_case on "$TOOLS" "$TOOL_PRELUDE" "$TOOL_BODY" \
    "{\"tool_calls\": [{\"name\": \"runit\", \"args\": {\"code\": \"$CODE6\"}}]}" '{"content": "all done"}'
if [ "$RC" -ge 0 ] && ! grep -q '^TOOL_RAN$' <<<"$OUT"; then
    fail CP-06 "the tool never ran, so the arm has no subject (rc=$RC)" "$OUT"
else
    arm CP-06 "a string a model passes to a tool is matched when the tool runs it" \
        "CODEGEN_EXEC code_sha256 = sha:$CODE6" "CODEGEN_EXEC $MATCH" \
        "CODEGEN_EXEC origin_derivation = tool_argument" \
        "CODEGEN_EXEC origin_model = stub-model" \
        "CODEGEN_EXEC origin_request_sha256 = shafile:req_1.json"
fi
CG_EXTRA=""

# ---- CP-07: a proposal candidate --------------------------------------------
CODE7='8 + 8'
run_case on ', "propose_candidates_max": 2' "" '    let h = agent.create("worker")
    let p = agent.propose(h, "write code", 1)
    let c = p["candidates"][0]
    let x = codegen.run("javascript", c["content"])
    print("END")' "{\"content\": \"$CODE7\"}"
arm CP-07 "a proposal candidate run before any commit is matched" \
    "CODEGEN_EXEC code_sha256 = sha:$CODE7" "CODEGEN_EXEC $MATCH" \
    "CODEGEN_EXEC origin_derivation = proposal" \
    "CODEGEN_EXEC origin_request_sha256 = shafile:req_1.json"

# ---- CP-08: AGENT_RESPONSE, a reply over 500 characters ---------------------
LONG="$(printf 'a%.0s' $(seq 1 600))Z"
run_case on "" "" "$SEND"'
    print("END")' "{\"content\": \"$LONG\"}"
arm CP-08 "AGENT_RESPONSE keeps content_hash and adds the whole reply's hash" \
    "AGENT_RESPONSE content_hash = sha500:$LONG" \
    "AGENT_RESPONSE content_sha256 = sha:$LONG" \
    "AGENT_RESPONSE content_sha256 != sha500:$LONG" \
    "AGENT_RESPONSE request_sha256 = shafile:req_1.json"

# ---- CP-09: one code text from two replies ----------------------------------
run_case on "" "" '    let h = agent.create("worker")
    let a = agent.send(h, "first")
    let b = agent.send(h, "second")
    let x = codegen.run("javascript", b["content"])
    print("END")' "$(python3 -c 'import json,sys; print(json.dumps({"content": sys.argv[1]}))' "$FENCED")" \
    "{\"content\": \"$CODE1\"}"
arm CP-09 "the same text from two different replies is counted as two" \
    "CODEGEN_EXEC code_sha256 = sha:$CODE1" "CODEGEN_EXEC $MATCH" \
    "CODEGEN_EXEC origin_distinct_replies = 2" \
    "CODEGEN_EXEC origin_request_sha256 = shafile:req_1.json"

# ---- CP-10 / CP-11: hashes only, and the chain ------------------------------
TOKEN="zqprovenancetoken$(date +%s)"
run_case on "" "" "$SEND"'
    let x = codegen.run("javascript", r["content"])
    print("END")' "{\"content\": \"'$TOKEN'\"}"
if [ "$RC" -eq -1 ]; then unm CP-10 "the agent stub did not start"
elif ! grep -q "$TOKEN" <<<"$OUT"; then
    fail CP-10 "CONTROL: the token never reached the program, so its absence proves nothing (rc=$RC)" "$OUT"
elif [ ! -s "$D/tel.jsonl" ]; then fail CP-10 "no telemetry was written" "$OUT"
elif grep -q "$TOKEN" "$D/tel.jsonl"; then
    fail CP-10 "model-written text is in the telemetry file" "$(grep -o ".\{60\}$TOKEN.\{20\}" "$D/tel.jsonl" | head -3)"
else pass CP-10 "the token reached the program and is nowhere in telemetry (hashes only)"; fi

if [ "$RC" -eq -1 ]; then unm CP-11 "the agent stub did not start"
else
    V=$(cd "$D" && "$NAAB" --verify-telemetry-chain tel.jsonl 2>&1); VR=$?
    # CONTROL for the verifier: one changed character must be reported.
    sed 's/"code_attribution":"model_output_match"/"code_attribution":"unattributed"/' "$D/tel.jsonl" > "$D/tampered.jsonl"
    T=$(cd "$D" && "$NAAB" --verify-telemetry-chain tampered.jsonl 2>&1); TR=$?
    if cmp -s "$D/tel.jsonl" "$D/tampered.jsonl"; then
        fail CP-11 "CONTROL: the tamper edit changed nothing, so the verifier was not exercised"
    elif [ "$VR" -eq 0 ] && [ "$TR" -ne 0 ]; then
        pass CP-11 "the chain verifies with the new fields, and an edited attribution is reported"
    else fail CP-11 "chain verification: untouched rc=$VR, edited rc=$TR" "$V
$T"; fi
fi

# ---- CP-12: telemetry off ---------------------------------------------------
run_case off "" "" "$SEND
$RUN" "{\"content\": \"$CODE1\"}"
if [ "$RC" -eq -1 ]; then unm CP-12 "the agent stub did not start"
elif [ "$RC" -eq 0 ] && grep -q '^END$' <<<"$OUT" && [ ! -e "$D/tel.jsonl" ]; then
    pass CP-12 "with telemetry off the program runs and nothing is written"
else fail CP-12 "telemetry off: rc=$RC, tel.jsonl $( [ -e "$D/tel.jsonl" ] && echo present || echo absent )" "$OUT"; fi

echo
echo "  Passed: $PASS  Failed: $FAIL  Unmeasurable: $UNM"
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ]
