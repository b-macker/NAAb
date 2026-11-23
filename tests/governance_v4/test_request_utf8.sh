#!/usr/bin/env bash
# ============================================================
# test_request_utf8.sh -- a request a model receives carries text that is not
# UTF-8 (A209)
#
# The engine serialised every request body with nlohmann's strict handler, so
# one byte that is not UTF-8 anywhere in a request ended the run (exit 1,
# `[json.exception.type_error.316]`) before anything was sent. A207 fixed the
# evidence writers; this is what the model receives. Since the fix:
#   - text is made valid (one U+FFFD per invalid byte, include/naab/utf8_text.h)
#     where it ENTERS a request and BEFORE the scan that judges it, so a scan
#     judges the bytes the request carries: the prompt of agent.send() and
#     agent.propose(), a tool's result (before truncation and the result scan;
#     truncation now cuts at a character boundary), a guidance section (at
#     parse), a registered tool's description and schema;
#   - every request is serialised by naab::text::dumpRequest(), which repairs
#     anything else and cannot throw on a stray byte -- the engine's own calls
#     (agent review, the governance voice, naab-gov explain) included.
#
#   RU-00c  an ASCII prompt: sent, exit 0 (the harness control)
#   RU-01   a prompt holding a Latin-1 byte: sent with U+FFFD, exit 0
#   RU-01h  ... and AGENT_RESPONSE's request_sha256 is the hash of the bytes the
#           stub received, so the body the engine sent was valid UTF-8 (the
#           stub decodes with replacement, so its copy alone cannot show it)
#   RU-02   a tool returning text with a Latin-1 byte: the result is sent, the
#           loop finishes (the A209 observation); RU-02c the ASCII control
#   RU-03   a VALID UTF-8 result cut by tool_result_max_chars inside "é": the
#           cut lands on the character boundary and the result is sent;
#           RU-03c an ASCII result cut at the same limit
#   RU-04   a tool returning a dict holding the byte: sent as the result, not
#           "failed to serialize tool result"
#   RU-05   a guidance section holding the byte, agent channel: delivered with
#           the send, not "the send fails" (A207's agent-channel note)
#   RU-06   agent review under its DEFAULT fail_policy ("open"): a program with
#           one Latin-1 byte in a comment used to make every reviewer call
#           fail, so the review was skipped and the program ran -- a route
#           around review. The reviewer is asked and its finding enforces;
#           RU-06c the ASCII program, the same verdict
#   RU-07   agent.propose() with the byte in its prompt: a candidate returns
#   RU-08   a registered tool whose description holds the byte: the send works
#           and the declaration carries U+FFFD
#   RU-09   the prompt scan judges the bytes sent: with no_secrets on,
#           `password = "abcdef<byte>"` is 7 characters raw but 9 bytes sent,
#           which the 8-or-more Hardcoded Password pattern matches -- refused,
#           nothing sent. RU-09c 6 ASCII characters pass and are sent; RU-09p 8
#           ASCII characters are refused (the pattern is live)
#   RU-09t  the same for a tool result: refused before the result is sent;
#           RU-09tc the 6-character result is sent
#
# Not separately run: the governance voice and naab-gov explain reach the
# model through callAgentSimple(), the function RU-06 exercises (traced).
#
# KILL TABLE (each mutant applied alone, naab-lang rebuilt, the suite re-run;
# the arms that turned red):
#   V1  agent.send()'s prompt not made valid before its scan      -> RU-09
#   V2  agent.propose()'s prompt likewise                         -> RU-09r
#   V3  a tool result not made valid before its cut and scan      -> RU-09t
#   V4  parseSections() reads the raw text                        -> RU-09g
#   V5  appendRowSection() reads the raw text                     -> RU-09d
#   V6  a query row read raw                                      -> RU-09q
#   C1  the result cut by byte (substr), not at a boundary        -> RU-03
#   C2  a structured result dumped strictly                       -> RU-04
#   D1  the send's messages dumped strictly                       -> RU-10 RU-10t RU-10f
#   D2  propose's messages dumped strictly                        -> RU-10p
#   D3  the tool loop's messages dumped strictly                  -> none alone:
#       dumpRequest() repairs the object in place, so after D1's dump only
#       text appended later can be invalid. With V3 as well (a raw result
#       appended after it)                                        -> RU-02 RU-09t RU-10f
#   D4  the final tool-less turn dumped strictly                  -> none, also
#       with V3: the loop's dump repairs the messages before the final turn
#       copies them, and what is appended after it is the model's calls
#       and the engine's "not executed" answers (equivalent by construction)
#   D5  the first call's tool schema dumped strictly              -> RU-08
#   D6  the tool loop's tool schema dumped strictly               -> RU-08
#   P1  callGemini() (agent review, the voice, explain) strict    -> RU-06
#   P2  callAgentWithStatus()'s Gemini body strict                -> none: it is
#       built only from parsed JSON and config text, both valid (equivalent)
#   P3  callAgentWithTools()'s Gemini body strict                 -> RU-08
# Not mutated: the three Anthropic bodies (the stub speaks Gemini only) and
# the step-up challenge's messages (no arm stages a challenge over edited
# history); each is the same one-call change as its Gemini twin.
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
TEST_TMP="$(mktemp -d)"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
# The detail is on the FAIL line, with any CR shown as \r, so run-all-tests.sh's
# summary (which quotes lines that say "fail") carries it.
fail() { local d="${3:-}"; d="${d//$'\r'/\\r}"; FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2${d:+ -- $d}"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_STARTED=0
cleanup() {
    [ "$STUB_STARTED" = 1 ] && stop_stub >/dev/null 2>&1
    teardown_isolated_trust
    [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"
}
trap cleanup EXIT
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }

echo "=== requests carry text that is not UTF-8 (A209) ==="

case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;; *) WINDOWS=0 ;; esac
[ -n "${WINDIR:-}" ] && WINDOWS=1
if [ "$WINDOWS" = 1 ]; then
    skip "RU-STUB" "stub-backed arms are not run on Windows (stub_platform.sh)"
    echo ""; echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
    [ "$FAIL_COUNT" -eq 0 ]; exit $?
fi
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
export FAKE_KEY_RU="fake-key-request-utf8"

FFFD=$'\xef\xbf\xbd'

# run_arm DIR FIXTURE_JSON PROGRAM [AGENT_EXTRA] [TOP_EXTRA]
# One run in DIR against a fresh stub. Its exit status in DIR/rc (read with
# rc DIR: arms run in loops, so no shared variable holds it); stdout+stderr in
# DIR/out.txt, the stub's request bodies in DIR/stub/req_N.json.
run_arm() {
    local d="$1" fx="$2" prog="$3" ax="${4:-}" tx="${5:-}"
    mkdir -p "$d/stub"
    printf '%s\n' "$fx" > "$d/fixture.json"
    echo "NOT_RUN" > "$d/rc"
    if [ "$STUB_STARTED" = 1 ]; then stop_stub >/dev/null 2>&1; STUB_STARTED=0; fi
    start_stub "$d/fixture.json" "$d/stub" >/dev/null 2>&1 || { echo "STUB_FAIL" > "$d/rc"; return; }
    STUB_STARTED=1
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},\n "telemetry": {"enabled": true, "output_file": "tele.jsonl", "tamper_evidence": {"enabled": true}},\n "agents": {"worker": {"provider": "gemini", "model": "stub-model", "api_base": "http://127.0.0.1:%s", "api_key_env": "FAKE_KEY_RU", "max_tokens": 100, "max_turns": 20, "retry": {"max_attempts": 1}%s}}%s}\n' \
        "$STUB_PORT" "$ax" "$tx" > "$d/govern.json"
    printf '%s\n' "$prog" > "$d/p.naab"
    (cd "$d" && timeout 60s "$NAAB" run p.naab > out.txt 2>&1); echo "$?" > "$d/rc"
    stop_stub >/dev/null 2>&1; STUB_STARTED=0
}
rc() { cat "$1/rc" 2>/dev/null; }
ran() { grep -q 'RUN_DONE' "$1/out.txt"; }
# req_has DIR N BYTES: the stub's Nth request body holds BYTES (grep -F, byte mode).
req_has() { [ -f "$1/stub/req_$2.json" ] && LC_ALL=C grep -qF -- "$3" "$1/stub/req_$2.json"; }
nreq() { ls "$1/stub" 2>/dev/null | grep -c '^req_[0-9]*\.json$'; }
why() { printf 'rc=%s reqs=%s %s' "$(rc "$1")" "$(nreq "$1")" "$(grep -m1 -o 'json.exception[^]]*\]\|Governance error[^.]*\|failed to serialize[^"]*' "$1/out.txt" "$1"/stub/req_*.json 2>/dev/null | head -1)"; }

ONE='{"responses": [{"content": "noted", "input_tokens": 5, "output_tokens": 3}]}'
TOOLFX='{"responses": [{"tool_calls": [{"name": "lookup", "args": {"path": "x"}}]}, {"content": "done", "input_tokens": 5, "output_tokens": 3}]}'
TOOLS=', "tools_enabled": true, "tools": ["lookup"]'
send_prog() {  # the prompt is read from msg.txt, so the program text stays ASCII
    printf 'use agent\nuse file\nmain {\n    let h = agent.create("worker")\n    let r = agent.send(h, file.read("msg.txt"))\n    print("RUN_DONE " + r.content)\n}'
}
tool_prog() {  # $1 the tool body's return expression
    printf 'use agent\nuse file\nfn lookup(path) { return %s }\nmain {\n    let h = agent.create("worker")\n    agent.register_tool("lookup", lookup, {"description": "d", "parameters": {"path": {"type": "string", "description": "p"}}})\n    let r = agent.send(h, "look it up")\n    print("RUN_DONE " + r.content)\n}' "$1"
}

# ------------------------------------------------------------------
# RU-00c / RU-01 / RU-01h -- the prompt
# ------------------------------------------------------------------
d="$TEST_TMP/ru00c"; mkdir -p "$d"; printf 'cafe order' > "$d/msg.txt"
run_arm "$d" "$ONE" "$(send_prog)"
if [ "$(rc "$d")" = 0 ] && ran "$d" && req_has "$d" 1 "cafe order"; then
    pass "RU-00c" "an ASCII prompt is sent and the run finishes"
else
    fail "RU-00c" "the harness control -- every arm below measures nothing" "$(why "$d")"
fi
d="$TEST_TMP/ru01"; mkdir -p "$d"; printf 'caf\xe9 order' > "$d/msg.txt"
run_arm "$d" "$ONE" "$(send_prog)"
if [ "$(rc "$d")" = 0 ] && ran "$d" && req_has "$d" 1 "caf${FFFD} order"; then
    pass "RU-01" "a prompt holding a Latin-1 byte is sent with U+FFFD in its place"
else
    fail "RU-01" "the prompt ended the run or was not sent" "$(why "$d")"
fi
H01="$(python3 -I -c 'import hashlib,json,sys
body=open(sys.argv[1],"rb").read()
ev=[json.loads(l) for l in open(sys.argv[2],"rb") if l.strip()]
rs=[e.get("request_sha256") for e in ev if e.get("event_type")=="AGENT_RESPONSE"]
sys.stdout.buffer.write(("%s %s" % (len(rs), rs[0]==hashlib.sha256(body).hexdigest() if rs else "-")).encode("ascii"))' \
    "$d/stub/req_1.json" "$d/tele.jsonl" 2>/dev/null)"
if [ "$H01" = "1 True" ]; then
    pass "RU-01h" "request_sha256 is the hash of the bytes the stub received: the body sent was valid UTF-8"
else
    fail "RU-01h" "the recorded request hash does not match the body received" "got=[$H01]"
fi

# ------------------------------------------------------------------
# RU-02 / RU-02c -- a tool's text result
# ------------------------------------------------------------------
for arm in ru02c ru02; do
    d="$TEST_TMP/$arm"; mkdir -p "$d"
    if [ "$arm" = ru02 ]; then printf 'caf\xe9 menu' > "$d/data.txt"; else printf 'cafe menu' > "$d/data.txt"; fi
    run_arm "$d" "$TOOLFX" "$(tool_prog 'file.read("data.txt")')" "$TOOLS"
done
d="$TEST_TMP/ru02c"
if ran "$d" && req_has "$d" 2 "cafe menu"; then
    pass "RU-02c" "an ASCII tool result is sent and the loop finishes"
else
    fail "RU-02c" "the tool control -- RU-02/03/04/08/09t measure nothing" "$(why "$d")"
fi
d="$TEST_TMP/ru02"
if ran "$d" && req_has "$d" 2 "caf${FFFD} menu"; then
    pass "RU-02" "a tool result holding a Latin-1 byte is sent with U+FFFD and the loop finishes"
else
    fail "RU-02" "the tool result ended the run or was not sent" "$(why "$d")"
fi

# ------------------------------------------------------------------
# RU-03 / RU-03c -- truncation lands on a character boundary
# ------------------------------------------------------------------
for arm in ru03c ru03; do
    d="$TEST_TMP/$arm"; mkdir -p "$d"
    if [ "$arm" = ru03 ]; then printf 'caf\xc3\xa9 menu' > "$d/data.txt"; else printf 'cafes menu' > "$d/data.txt"; fi
    run_arm "$d" "$TOOLFX" "$(tool_prog 'file.read("data.txt")')" "$TOOLS, \"tool_result_max_chars\": 4"
done
d="$TEST_TMP/ru03c"
if ran "$d" && req_has "$d" 2 'cafe\n[truncated]'; then
    pass "RU-03c" "an ASCII result is cut at tool_result_max_chars and sent"
else
    fail "RU-03c" "the truncation control" "$(why "$d")"
fi
d="$TEST_TMP/ru03"
if ran "$d" && req_has "$d" 2 'caf\n[truncated]' && ! req_has "$d" 2 "$FFFD"; then
    pass "RU-03" "a valid UTF-8 result cut inside a character is cut at the boundary before it, and sent"
else
    fail "RU-03" "a cut inside a character ended the run or was sent damaged" "$(why "$d")"
fi

# ------------------------------------------------------------------
# RU-04 -- a structured tool result
# ------------------------------------------------------------------
d="$TEST_TMP/ru04"; mkdir -p "$d"; printf 'caf\xe9 menu' > "$d/data.txt"
run_arm "$d" "$TOOLFX" "$(tool_prog '{"note": file.read("data.txt")}')" "$TOOLS"
if ran "$d" && req_has "$d" 2 "caf${FFFD} menu" && ! req_has "$d" 2 "failed to serialize"; then
    pass "RU-04" "a dict result holding the byte is sent as the result"
else
    fail "RU-04" "a structured result holding the byte was not sent" "$(why "$d")"
fi

# ------------------------------------------------------------------
# RU-05 -- a guidance section, agent channel
# ------------------------------------------------------------------
d="$TEST_TMP/ru05"; mkdir -p "$d/docs"
printf '# Playbook\n\n## worker\nCaf\xe9 rule: write the real logic.\n' > "$d/docs/playbook.md"
printf 'cafe order' > "$d/msg.txt"
run_arm "$d" "$ONE" "$(send_prog)" "" ', "guidance": {"sources": ["docs/playbook.md"]}'
if [ "$(rc "$d")" = 0 ] && ran "$d" && req_has "$d" 1 "Caf${FFFD} rule"; then
    pass "RU-05" "a guidance section holding the byte is delivered with the send"
else
    fail "RU-05" "the guidance section failed the send or was not delivered" "$(why "$d")"
fi

# ------------------------------------------------------------------
# RU-06 / RU-06c -- agent review under the default fail_policy
# ------------------------------------------------------------------
REVIEW_FX='{"responses": [{"content": "FINDING|cat_alpha|alpha issue"}]}'
review_prog() {  # $1 the comment's word
    printf '// %s note\nmain {\n    print("RUN_DONE program")\n}' "$1"
}
for arm in ru06c ru06; do
    d="$TEST_TMP/$arm"; mkdir -p "$d"
    if [ "$arm" = ru06 ]; then w=$'caf\xe9'; else w='cafe'; fi
    run_arm "$d" "$REVIEW_FX" "$(review_prog "$w")" ', "system_prompt": "TOKEN_det"' \
        ', "agent_review": {"enabled": true, "detection": ["worker"], "enforcement": {"green": "hard"}}'
done
d="$TEST_TMP/ru06c"
if [ "$(rc "$d")" = 3 ] && ! ran "$d" && [ "$(nreq "$d")" -ge 1 ]; then
    pass "RU-06c" "the ASCII program: the reviewer is asked and its finding blocks (exit 3)"
else
    fail "RU-06c" "the review control -- RU-06 measures nothing" "$(why "$d")"
fi
d="$TEST_TMP/ru06"
if [ "$(rc "$d")" = 3 ] && ! ran "$d" && req_has "$d" 1 "caf${FFFD} note"; then
    pass "RU-06" "a Latin-1 byte in the program no longer skips agent review: the reviewer is asked and blocks"
else
    fail "RU-06" "the review was skipped or failed, and the program ran" "$(why "$d")"
fi

# ------------------------------------------------------------------
# RU-07 -- agent.propose()
# ------------------------------------------------------------------
d="$TEST_TMP/ru07"; mkdir -p "$d"; printf 'caf\xe9 order' > "$d/msg.txt"
run_arm "$d" "$ONE" "$(printf 'use agent\nuse file\nmain {\n    let h = agent.create("worker")\n    let p = agent.propose(h, file.read("msg.txt"))\n    print("RUN_DONE " + string(len(p["candidates"])))\n}')" ', "propose_candidates_max": 1'
if [ "$(rc "$d")" = 0 ] && grep -q 'RUN_DONE 1' "$d/out.txt" && req_has "$d" 1 "caf${FFFD} order"; then
    pass "RU-07" "agent.propose() with the byte in its prompt returns a candidate"
else
    fail "RU-07" "propose ended the run or sent nothing" "$(why "$d")"
fi

# ------------------------------------------------------------------
# RU-08 -- a registered tool's description
# ------------------------------------------------------------------
d="$TEST_TMP/ru08"; mkdir -p "$d"; printf 'caf\xe9 lookup' > "$d/desc.txt"; printf 'caf\xe9 path' > "$d/param.txt"
run_arm "$d" "$TOOLFX" "$(printf 'use agent\nuse file\nfn lookup(path) { return "x" }\nmain {\n    let h = agent.create("worker")\n    agent.register_tool("lookup", lookup, {"description": file.read("desc.txt"), "parameters": {"path": {"type": "string", "description": file.read("param.txt")}}})\n    let r = agent.send(h, "hello")\n    print("RUN_DONE " + r.content)\n}')" "$TOOLS"
if [ "$(rc "$d")" = 0 ] && ran "$d" && req_has "$d" 1 "caf${FFFD} lookup" && req_has "$d" 1 "caf${FFFD} path" \
   && req_has "$d" 2 "caf${FFFD} lookup" && req_has "$d" 2 "caf${FFFD} path"; then
    pass "RU-08" "a tool's description and schema holding the byte are declared with U+FFFD on the first call and in the loop"
else
    fail "RU-08" "the tool declaration ended the run or was not sent" "$(why "$d")"
fi

# ------------------------------------------------------------------
# RU-09 -- the scan judges the bytes the request carries
# ------------------------------------------------------------------
NS=', "code_quality": {"no_secrets": {"enabled": true, "level": "hard"}}'
for arm in ru09c ru09p ru09; do
    d="$TEST_TMP/$arm"; mkdir -p "$d"
    case "$arm" in
        ru09)  printf 'password = "abcdef\xe9"' > "$d/msg.txt" ;;
        ru09c) printf 'password = "abcdef"' > "$d/msg.txt" ;;
        ru09p) printf 'password = "abcdefgh"' > "$d/msg.txt" ;;
    esac
    run_arm "$d" "$ONE" "$(send_prog)" "" "$NS"
done
d="$TEST_TMP/ru09c"
if [ "$(rc "$d")" = 0 ] && ran "$d" && [ "$(nreq "$d")" = 1 ]; then
    pass "RU-09c" "a 6-character value passes the scan and is sent"
else
    fail "RU-09c" "the short-value control" "$(why "$d")"
fi
d="$TEST_TMP/ru09p"
if [ "$(rc "$d")" = 3 ] && [ "$(nreq "$d")" = 0 ]; then
    pass "RU-09p" "an 8-character value is refused before anything is sent (the pattern is live)"
else
    fail "RU-09p" "the pattern control -- RU-09 measures nothing" "$(why "$d")"
fi
d="$TEST_TMP/ru09"
if [ "$(rc "$d")" = 3 ] && [ "$(nreq "$d")" = 0 ]; then
    pass "RU-09" "7 characters raw, 9 bytes sent: the scan judged what would be sent and refused it"
else
    fail "RU-09" "the scan judged the raw text, not the bytes the request carries" "$(why "$d")"
fi
for arm in ru09tc ru09t; do
    d="$TEST_TMP/$arm"; mkdir -p "$d"
    if [ "$arm" = ru09t ]; then printf 'password = "abcdef\xe9"' > "$d/data.txt"; else printf 'password = "abcdef"' > "$d/data.txt"; fi
    run_arm "$d" "$TOOLFX" "$(tool_prog 'file.read("data.txt")')" "$TOOLS" "$NS"
done
d="$TEST_TMP/ru09tc"
if ran "$d" && [ "$(nreq "$d")" = 2 ]; then
    pass "RU-09tc" "a 6-character tool result passes the result scan and is sent"
else
    fail "RU-09tc" "the tool-result scan control" "$(why "$d")"
fi
d="$TEST_TMP/ru09t"
if [ "$(rc "$d")" = 3 ] && [ "$(nreq "$d")" = 1 ]; then
    pass "RU-09t" "the result scan judged the bytes the result would carry and refused it before it was sent"
else
    fail "RU-09t" "the result scan judged the raw text" "$(why "$d")"
fi

# ------------------------------------------------------------------
# RU-09r -- propose's prompt scan judges the bytes sent
# ------------------------------------------------------------------
prop_prog() {
    printf 'use agent\nuse file\nmain {\n    let h = agent.create("worker")\n    let p = agent.propose(h, file.read("msg.txt"))\n    print("RUN_DONE " + string(len(p["candidates"])))\n}'
}
for arm in ru09rc ru09r; do
    d="$TEST_TMP/$arm"; mkdir -p "$d"
    if [ "$arm" = ru09r ]; then printf 'password = "abcdef\xe9"' > "$d/msg.txt"; else printf 'password = "abcdef"' > "$d/msg.txt"; fi
    run_arm "$d" "$ONE" "$(prop_prog)" ', "propose_candidates_max": 1' "$NS"
done
d="$TEST_TMP/ru09rc"
if [ "$(rc "$d")" = 0 ] && grep -q 'RUN_DONE 1' "$d/out.txt" && [ "$(nreq "$d")" = 1 ]; then
    pass "RU-09rc" "a 6-character propose prompt passes the scan and is sent"
else
    fail "RU-09rc" "the propose scan control" "$(why "$d")"
fi
d="$TEST_TMP/ru09r"
if [ "$(rc "$d")" = 3 ] && [ "$(nreq "$d")" = 0 ]; then
    pass "RU-09r" "propose's scan judged the bytes the request would carry and refused it"
else
    fail "RU-09r" "propose's scan judged the raw text" "$(why "$d")"
fi

# ------------------------------------------------------------------
# RU-09g / RU-09d / RU-09q -- a guidance section's scan judges the bytes
# delivered: a document (parseSections), a guidance table read as a
# document (appendRowSection) and an operator query (its own row reader).
# A section the scan matches is WITHHELD; the send itself goes on.
# ------------------------------------------------------------------
make_gdb() {  # $1 dir, $2 the body as Python bytes literal
    mkdir -p "$1/data"
    python3 -I - "$1/data/g.db" "$2" <<'PY'
import ast, sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("CREATE TABLE guidance(topic TEXT, body TEXT)")
db.execute("INSERT INTO guidance VALUES (?, CAST(? AS TEXT))", ("worker", ast.literal_eval(sys.argv[2])))
db.commit(); db.close()
PY
}
withheld() { grep -q '"GUIDANCE_WITHHELD"' "$1/tele.jsonl" 2>/dev/null; }
for arm in ru09gc ru09g ru09dc ru09d ru09qc ru09q; do
    d="$TEST_TMP/$arm"; mkdir -p "$d/docs"; printf 'cafe order' > "$d/msg.txt"
    case "$arm" in
        ru09g)  printf '# P\n\n## worker\npassword = "abcdef\xe9"\n' > "$d/docs/playbook.md" ;;
        ru09gc) printf '# P\n\n## worker\npassword = "abcdef"\n' > "$d/docs/playbook.md" ;;
        ru09d|ru09q)   make_gdb "$d" "b'password = \"abcdef\\xe9\"'" ;;
        ru09dc|ru09qc) make_gdb "$d" "b'password = \"abcdef\"'" ;;
    esac
    case "$arm" in
        ru09g*) G=', "guidance": {"sources": ["docs/playbook.md"]}' ;;
        ru09d*) G=', "guidance": {"sources": ["data/g.db"]}' ;;
        ru09q*) G=', "guidance": {"enabled": true, "sources": {"kb": {"type": "sqlite", "path": "data/g.db", "query": "SELECT topic, body FROM guidance"}}, "rules": [{"on": "start", "from": "kb", "to": ["agent"]}]}' ;;
    esac
    run_arm "$d" "$ONE" "$(send_prog)" "" "$NS$G"
done
for kind in g d q; do
    case "$kind" in g) what="a document section" ;; d) what="a guidance-table row" ;; q) what="a query row" ;; esac
    d="$TEST_TMP/ru09${kind}c"
    if [ "$(rc "$d")" = 0 ] && ran "$d" && req_has "$d" 1 'abcdef' && ! withheld "$d"; then
        pass "RU-09${kind}c" "$what holding 6 characters passes the guidance scan and is delivered"
    else
        fail "RU-09${kind}c" "the guidance control for $what" "$(why "$d")"
    fi
    d="$TEST_TMP/ru09${kind}"
    if [ "$(rc "$d")" = 0 ] && ran "$d" && ! req_has "$d" 1 'abcdef' && withheld "$d"; then
        pass "RU-09${kind}" "$what is judged as delivered (7 characters raw, 9 bytes sent): withheld, and the send goes on"
    else
        fail "RU-09${kind}" "the guidance scan judged the raw text of $what" "$(why "$d")"
    fi
done

# ------------------------------------------------------------------
# RU-10 -- history the program wrote into the handle (A211's route)
# ------------------------------------------------------------------
# No scan reads it (A211, open), so the repair that keeps these requests
# from ending the run is dumpRequest() at each place the messages are
# serialised: the send's first call, each tool-loop call, the final
# tool-less turn, propose.
hist_prog() {  # $1 the call: send / tool / propose; $2 the tool's result (default "x")
    local reg="" call='let r = agent.send(h, "next")' out='r.content' res="${2:-\"x\"}"
    [ "$1" = tool ] && reg='    agent.register_tool("lookup", lookup, {"description": "d", "parameters": {"path": {"type": "string", "description": "p"}}})\n'
    [ "$1" = propose ] && { call='let p = agent.propose(h, "next")'; out='string(len(p["candidates"]))'; }
    printf 'use agent\nuse file\nfn lookup(path) { return %s }\nmain {\n    let h = agent.create("worker")\n%b    h["messages"] = [{"role": "user", "content": file.read("hist.txt")}, {"role": "assistant", "content": "ok"}]\n    %s\n    print("RUN_DONE " + %s)\n}' "$res" "$reg" "$call" "$out"
}
FINALFX='{"responses": [{"tool_calls": [{"name": "lookup", "args": {"path": "x"}}]}, {"tool_calls": [{"name": "lookup", "args": {"path": "y"}}]}, {"content": "done", "input_tokens": 5, "output_tokens": 3}]}'
for arm in ru10 ru10t ru10f ru10p; do
    d="$TEST_TMP/$arm"; mkdir -p "$d"; printf 'caf\xe9 hist' > "$d/hist.txt"
    case "$arm" in
        ru10)  run_arm "$d" "$ONE" "$(hist_prog send)" ;;
        ru10t) run_arm "$d" "$TOOLFX" "$(hist_prog tool)" "$TOOLS" ;;
        ru10f) printf 'caf\xe9 tool' > "$d/res.txt"
               run_arm "$d" "$FINALFX" "$(hist_prog tool 'file.read("res.txt")')" "$TOOLS, \"max_tool_loop_turns\": 1" ;;
        ru10p) run_arm "$d" "$ONE" "$(hist_prog propose)" ', "propose_candidates_max": 1' ;;
    esac
done
d="$TEST_TMP/ru10"
if [ "$(rc "$d")" = 0 ] && ran "$d" && req_has "$d" 1 "caf${FFFD} hist"; then
    pass "RU-10" "history holding the byte is sent with U+FFFD on the send's call"
else
    fail "RU-10" "the send's call ended the run" "$(why "$d")"
fi
d="$TEST_TMP/ru10t"
if [ "$(rc "$d")" = 0 ] && ran "$d" && req_has "$d" 2 "caf${FFFD} hist"; then
    pass "RU-10t" "... and on the tool loop's call"
else
    fail "RU-10t" "the tool loop's call ended the run" "$(why "$d")"
fi
d="$TEST_TMP/ru10f"
if [ "$(rc "$d")" = 0 ] && ran "$d" && [ "$(nreq "$d")" = 3 ] && req_has "$d" 3 "caf${FFFD} hist" && req_has "$d" 3 "caf${FFFD} tool" \
   && grep -q '"mode":"NONE"' "$d/stub/req_3.json"; then
    pass "RU-10f" "... and on the final tool-less turn"
else
    fail "RU-10f" "the final tool-less turn ended the run or was not made" "$(why "$d")"
fi
d="$TEST_TMP/ru10p"
if [ "$(rc "$d")" = 0 ] && grep -q 'RUN_DONE 1' "$d/out.txt" && req_has "$d" 1 "caf${FFFD} hist"; then
    pass "RU-10p" "... and on propose's call"
else
    fail "RU-10p" "propose's call ended the run" "$(why "$d")"
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
