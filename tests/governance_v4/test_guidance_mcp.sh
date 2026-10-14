#!/usr/bin/env bash
# ============================================================
# test_guidance_mcp.sh -- an MCP server (stdio) as a live-guidance source
# (docs/plan-live-guidance.md, "MCP source"). Held to the connector rules of
# test_guidance_connectors.sh: fetched WHOLE (initialize, list, read every
# resource, stop the server), nothing from the run sent, the load-time
# environment, operator authority like a hook.
#
# The stub server (tests/helpers/mcp_guidance_stub.py) logs every message it
# receives, so what the client SENT is observed, not inferred.
#
#   deliver   M-01/M-01t resources become sections; a pull delivers one (VM,
#                        tree-walker); GUIDANCE_SOURCE_REFRESH type mcp ok
#             M-08       a resource on the second page of a paginated list
#             M-09       "resources" in govern.json: only those URIs are read,
#                        and the server is not asked for its list
#             M-10       a blob and an unreadable resource are skipped, the
#                        text resource beside them is delivered
#   nothing   M-02       across a load-time fetch AND a refresh after the
#   sent                 program ran: only initialize, initialized, list and
#                        read reach the server, and none carries the program's
#                        pull topic or env.set value (M-02c: the log recorded
#                        both sessions -- the probe can see a message)
#             M-03       the server sees the LOAD-TIME environment: not the
#                        program's env.set, not NAAb's signing-key variable
#                        (M-03c: a load-time variable is visible -- the probe
#                        reads the environment at all)
#             M-04       a server's ping is answered {}, its sampling request
#                        "method not found"; delivery still works
#   refuse    M-05       a server that never answers is killed at timeout_ms
#                        with what it started (process group)
#             M-06       an unknown protocol version is not spoken
#             M-07       a server with no resources capability
#             M-11       text over max_source_kb
#             M-16       a server that exits mid-session
#   process   M-12       one server start per run, both engines
#             M-13       runs with the program's network disabled (like a
#                        hook), and the host-list note is printed
#             M-14       naab-gov lint never starts it (exit 3 proves the
#                        config was loaded); naab-lang run does (control)
#             M-15       a refresh that fails keeps the last good copy
#   self      M-17       a source whose file sits where the program may write
#                        is refused, its server never started (A36, decision
#                        (b)); M-17c the same file in a blocked directory is
#                        fetched
#             M-18       the program cannot rewrite a file the server's
#                        arguments name (defence in depth); M-18c a file
#                        beside it stays writable
#   label     M-19       a violation's terminal guidance names the source
#             M-20       so do the agent's request and the end-of-run block
#                        (a pathless source used to print an empty name)
#
# Windows: MCP sources are refused at load in this build -- every arm is
# UNMEASURABLE there.
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
NAAB_GOV="${NAAB_GOV:-$REPO/build/naab-gov}"
STUB="$REPO/tests/helpers/mcp_guidance_stub.py"
TEST_TMP="$(mktemp -d)"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> ${3:0:500}"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }
summary() {
    echo ""
    echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT unmeasurable"
    [ "$FAIL_COUNT" -eq 0 ]
}

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() {
    # A hung-server arm leaves nothing behind if the suite works; make sure.
    for f in "$TEST_TMP"/*/child.pid; do [ -f "$f" ] && kill -9 "$(cat "$f")" 2>/dev/null; done
    teardown_isolated_trust
    [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"
}
trap cleanup EXIT
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }

case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;; *) WINDOWS=0 ;; esac
[ -n "${WINDIR:-}" ] && WINDOWS=1
if [ "$WINDOWS" = 1 ]; then
    skip "M-ALL" "MCP sources are not supported on Windows in this build"
    summary; exit $?
fi
command -v python3 >/dev/null 2>&1 || { skip "M-ALL" "python3 unavailable for the stub server"; summary; exit $?; }

# An MCP source may not live where the program can write (A36, decision (b)):
# each arm's behaviour file is kept in SRV, and every config lists SRV and the
# stub's own directory in blocked_paths -- unless the arm sets its own
# filesystem policy (M-17). BEH_AT overrides where the behaviour file goes.
SRV="$TEST_TMP/srv"; mkdir -p "$SRV"
HELPERS="$(dirname "$STUB")"
beh_path() { echo "${BEH_AT:-$SRV/$(basename "$1").json}"; }
# behaviour DIR MARKER [EXTRA_JSON_FIELDS] -> the behaviour file, and the
# default two resources: "deployment" and "No placeholders". The stub's log
# and session counter go in DIR (the stub writes them, not the program).
behaviour() {
    mkdir -p "$1"
    printf '{"log": "%s/stub.log", "counter": "%s/sessions",\n "pages": [[{"uri": "kb://deploy", "name": "deployment"}, {"uri": "kb://ph", "name": "No placeholders"}]],\n "texts": {"kb://deploy": "%s: roll back with the rollback script before retrying.", "kb://ph": "%s_PH: unfinished work goes in stubs/ with a ticket."}%s}\n' \
        "$1" "$1" "$2" "$2" "${3:-}" > "$(beh_path "$1")"
}
# src DIR [EXTRA_SOURCE_FIELDS] -> the source object
src() { printf '{"mcp": ["python3", "%s", "%s"]%s}' "$STUB" "$(beh_path "$1")" "${2:-}"; }
# cfg DIR SOURCES_JSON [EXTRA_TOP] [EXTRA_GUIDANCE]
cfg() {
    mkdir -p "$1"
    local top="${3:-}" prot="\"filesystem\": {\"blocked_paths\": [\"$SRV\", \"$HELPERS\"]}"
    if [[ "$top" == *'"filesystem"'* ]]; then :
    elif [[ "$top" == *'"capabilities": {'* ]]; then top="${top/'"capabilities": {'/'"capabilities": {'"$prot, "}"
    else top="\"capabilities\": {$prot}, $top"; fi
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},\n "telemetry": {"enabled": true, "output_file": "tele.jsonl"},\n %s\n "guidance": {"sources": %s %s}}\n' \
        "$top" "$2" "${4:-}" > "$1/govern.json"
}
run() {  # $1 dir, $2 engine, $3 program -> sets OUT, RC
    printf '%s\n' "$3" > "$1/p.naab"
    local a=("$NAAB"); [ "$2" = tw ] && a+=(--tree-walk); a+=(p.naab)
    OUT="$(cd "$1" && timeout 90 "${a[@]}" 2>&1)"; RC=$?
}
PULL='use governance
use array
main {
    let g = governance.guidance("deployment rollback")
    print("N=" + string(array.length(g)))
    print(g)
}'
tele() { cat "$1/tele.jsonl" 2>/dev/null; }
sessions() { grep -c . "$1/sessions" 2>/dev/null || echo 0; }

# ---------------------------------------------------------------------------
echo "== resources are fetched whole and delivered like a document"
for eng in vm tw; do
    id="M-01"; [ "$eng" = tw ] && id="M-01t"
    d="$TEST_TMP/m01-$eng"; behaviour "$d" "MCP_M1"; cfg "$d" "[$(src "$d")]"
    run "$d" "$eng" "$PULL"
    if [[ "$OUT" == *"N=1"* ]] && [[ "$OUT" == *"MCP_M1: roll back"* ]] &&
       grep -q '"type":"mcp"' <<<"$(tele "$d")" && grep -q '"outcome":"ok"' <<<"$(tele "$d")"; then
        pass "$id" "a resource is delivered by a pull; GUIDANCE_SOURCE_REFRESH type mcp ok ($eng)"
    else fail "$id" "MCP resource not delivered ($eng)" "rc=$RC $OUT"; fi
done

d="$TEST_TMP/m08"; behaviour "$d" "MCP_M8" ', "pages": [[{"uri": "kb://a", "name": "alpha notes"}], [{"uri": "kb://deploy", "name": "deployment"}]]'
cfg "$d" "[$(src "$d")]"
run "$d" vm "$PULL"
lists=$(grep -c '"resources/list"' "$d/stub.log" 2>/dev/null)
if [[ "$OUT" == *"MCP_M8: roll back"* ]] && [ "$lists" = 2 ] && grep -q '"cursor": "1"' "$d/stub.log"; then
    pass "M-08" "a resource on the second page of the list is read (the cursor is followed)"
else fail "M-08" "pagination not followed" "lists=$lists rc=$RC ${OUT:0:300}"; fi

d="$TEST_TMP/m09"; behaviour "$d" "MCP_M9"; cfg "$d" "[$(src "$d" ', "resources": ["kb://deploy"]')]"
run "$d" vm "$PULL"
if [[ "$OUT" == *"MCP_M9: roll back"* ]] && grep -q '"uri": "kb://deploy"' "$d/stub.log" &&
   ! grep -q 'kb://ph' "$d/stub.log" && ! grep -q '"resources/list"' "$d/stub.log"; then
    pass "M-09" "\"resources\" reads exactly the URIs govern.json names, without listing"
else fail "M-09" "resource filter not applied" "rc=$RC ${OUT:0:200} || $(cut -c1-120 "$d/stub.log" 2>/dev/null | tr '\n' ' ')"; fi

d="$TEST_TMP/m10"; behaviour "$d" "MCP_M10" ', "pages": [[{"uri": "kb://bin", "name": "binary thing"}, {"uri": "kb://gone", "name": "missing thing"}, {"uri": "kb://deploy", "name": "deployment"}]], "blobs": ["kb://bin"], "fail_read": ["kb://gone"]'
cfg "$d" "[$(src "$d")]"
run "$d" vm "$PULL"
if [[ "$OUT" == *"MCP_M10: roll back"* ]] && grep -q '"outcome":"ok"' <<<"$(tele "$d")" &&
   grep -q '"sections":"1"' <<<"$(tele "$d")"; then
    pass "M-10" "a blob and an unreadable resource are skipped; the text resource beside them is delivered"
else fail "M-10" "skip handling wrong" "rc=$RC ${OUT:0:300} $(grep GUIDANCE_SOURCE_REFRESH "$d/tele.jsonl" | cut -c1-300)"; fi

# ---------------------------------------------------------------------------
echo ""
echo "== nothing from the run reaches the server; it sees the load-time environment"
# One run covers both: a load-time session, then (refresh_seconds 5) a second
# session AFTER the program has pulled a topic and changed its environment.
d="$TEST_TMP/m02"; behaviour "$d" "MCP_M2" ', "env_probe": ["PROBE_LOAD", "PROBE_RUN", "NAAB_SIGNING_KEY"]'
cfg "$d" "[$(src "$d" ', "refresh_seconds": 5')]"
PROBE_LOAD="load-time-value" NAAB_SIGNING_KEY="/nonexistent/naab-signing-key-marker" run "$d" vm 'use governance
use env
use time
use array
main {
    let g = governance.guidance("deployment rollback RUNTOKEN_m2")
    env.set("PROBE_RUN", "RUNTOKEN_m2_env")
    env.set("PROBE_LOAD", "RUNTOKEN_m2_changed")
    time.sleep(7)
    let h = governance.guidance("RUNTOKEN_m2 second pull")
    print("N=" + string(array.length(g)))
}'
n_sess=$(grep -c '"initialize"' "$d/stub.log" 2>/dev/null)
methods="$(python3 -c '
import json, sys
seen = set()
for line in open(sys.argv[1]):
    o = json.loads(line)
    if "recv" in o and "method" in o["recv"]: seen.add(o["recv"]["method"])
print(" ".join(sorted(seen)))' "$d/stub.log" 2>/dev/null)"
if [ "${n_sess:-0}" -ge 2 ] && grep -q '"resources/read"' "$d/stub.log"; then
    pass "M-02c" "the stub logged both sessions, the second after the program ran ($n_sess initialize)"
    # Messages the client SENT only; the environment is M-03's question.
    if [ "$methods" = "initialize notifications/initialized resources/list resources/read" ] &&
       ! grep '"recv"' "$d/stub.log" | grep -q 'RUNTOKEN_m2'; then
        pass "M-02" "only initialize/initialized/list/read reached the server, none carrying the program's topic or env.set"
    else fail "M-02" "the client sent something else, or run data" "methods=[$methods] $(grep '"recv"' "$d/stub.log" | grep -o 'RUNTOKEN_m2[a-z_]*' | head -3 | tr '\n' ' ')"; fi
    envs="$(grep '"env"' "$d/stub.log")"
    if grep -q '"PROBE_LOAD": "load-time-value"' <<<"$envs"; then
        pass "M-03c" "the server sees a variable from the environment NAAb started with (the probe reads it)"
        if ! grep -q 'RUNTOKEN_m2' <<<"$envs" && ! grep -q 'naab-signing-key-marker' <<<"$envs" &&
           [ "$(grep -c '"PROBE_LOAD": "load-time-value"' <<<"$envs")" -ge 2 ]; then
            pass "M-03" "a refresh after env.set still sees the load-time environment, and never NAAb's signing-key variable"
        else fail "M-03" "the server saw the program's environment or NAAb's key" "$envs"; fi
    else fail "M-03c" "the environment probe saw nothing -- M-03 would measure nothing" "$envs"; fi
else
    fail "M-02c" "the stub did not log two sessions -- M-02/M-03 would measure nothing" "sessions=$n_sess rc=$RC ${OUT:0:300}"
fi

d="$TEST_TMP/m04"; behaviour "$d" "MCP_M4" ', "ask_first": true'
cfg "$d" "[$(src "$d")]"
run "$d" vm "$PULL"
replies="$(grep '"srv-' "$d/stub.log")"
if [[ "$OUT" == *"MCP_M4: roll back"* ]] &&
   grep -q '"id": "srv-ping", "jsonrpc": "2.0", "result": {}' <<<"$replies" &&
   grep -q '"error": {"code": -32601' <<<"$(grep '"srv-sample"' <<<"$replies")"; then
    pass "M-04" "a server ping is answered {}, a sampling request refused (method not found); delivery unaffected"
else fail "M-04" "server-to-client requests handled wrongly" "rc=$RC ${OUT:0:200} || $replies"; fi

# ---------------------------------------------------------------------------
echo ""
echo "== refused or failed sources deliver nothing and say why"
d="$TEST_TMP/m05"; behaviour "$d" "MCP_M5" ", \"hang_on\": \"resources/list\", \"spawn_child\": \"$TEST_TMP/m05/child.pid\""
cfg "$d" "[$(src "$d" ', "timeout_ms": 1200')]"
t0=$(date +%s); run "$d" vm "$PULL"; t1=$(date +%s)
sleep 0.3
cpid="$(cat "$d/child.pid" 2>/dev/null)"
alive=0
if [ -n "$cpid" ]; then
    st="$(ps -o stat= -p "$cpid" 2>/dev/null | tr -d ' ')"
    if [ -n "$st" ] && [[ "$st" != Z* ]]; then alive=1; fi   # a zombie is dead (pid 1 may not reap)
fi
if [ -n "$cpid" ] && [ "$alive" = 0 ] && [[ "$OUT" == *"N=0"* ]] && [[ "$OUT" == *"took longer than 1200 ms"* ]] &&
   [ $((t1 - t0)) -lt 20 ]; then
    pass "M-05" "a server that never answers is killed at timeout_ms with the process it started"
else fail "M-05" "hung server not contained" "child=$cpid alive=$alive secs=$((t1 - t0)) rc=$RC ${OUT:0:300}"; fi

d="$TEST_TMP/m06"; behaviour "$d" "MCP_M6" ', "version": "1999-01-01"'
cfg "$d" "[$(src "$d")]"
run "$d" vm "$PULL"
if [[ "$OUT" == *"N=0"* ]] && [[ "$OUT" == *'protocol version "1999-01-01"'* ]] && ! grep -q '"resources/list"' "$d/stub.log"; then
    pass "M-06" "an unknown protocol version is refused before anything is listed"
else fail "M-06" "unknown version accepted" "rc=$RC ${OUT:0:300}"; fi

d="$TEST_TMP/m07"; behaviour "$d" "MCP_M7" ', "no_resources": true'
cfg "$d" "[$(src "$d")]"
run "$d" vm "$PULL"
if [[ "$OUT" == *"N=0"* ]] && [[ "$OUT" == *"offers no resources"* ]]; then
    pass "M-07" "a server without the resources capability delivers nothing and says why"
else fail "M-07" "no-resources server mishandled" "rc=$RC ${OUT:0:300}"; fi

big="$(head -c 3000 /dev/zero | tr '\0' 'a')"
d="$TEST_TMP/m11"; behaviour "$d" "MCP_M11" ", \"texts\": {\"kb://deploy\": \"deployment $big\", \"kb://ph\": \"x\"}"
cfg "$d" "[$(src "$d")]" "" ', "max_source_kb": 1'
run "$d" vm "$PULL"
if [[ "$OUT" == *"N=0"* ]] && [[ "$OUT" == *"larger than 1 KB"* ]]; then
    pass "M-11" "resource text over max_source_kb is refused"
else fail "M-11" "size cap not applied" "rc=$RC ${OUT:0:300}"; fi

d="$TEST_TMP/m16"; behaviour "$d" "MCP_M16" ', "exit_on": "resources/list"'
cfg "$d" "[$(src "$d")]"
run "$d" vm "$PULL"
if [[ "$OUT" == *"N=0"* ]] && [[ "$OUT" == *"closed its output before answering resources/list"* ]]; then
    pass "M-16" "a server that exits mid-session delivers nothing and says where it stopped"
else fail "M-16" "early exit mishandled" "rc=$RC ${OUT:0:300}"; fi

# ---------------------------------------------------------------------------
echo ""
echo "== one server start per run; operator authority; never from naab-gov"
for eng in vm tw; do
    id="M-12"; [ "$eng" = tw ] && id="M-12t"
    d="$TEST_TMP/m12-$eng"; behaviour "$d" "MCP_M12"; cfg "$d" "[$(src "$d")]"
    run "$d" "$eng" 'use governance
use array
async fn work(n) {
    let g = governance.guidance("deployment rollback")
    return array.length(g) + n
}
main {
    let g = governance.guidance("deployment rollback")
    print("N=" + string(array.length(g)))
    let f1 = work(10)
    let f2 = work(20)
    print("ASYNC=" + string(await f1) + "," + string(await f2))
}'
    if [[ "$OUT" == *"N=1"* ]] && [[ "$OUT" == *"ASYNC=11,21"* ]] && [ "$(sessions "$d")" = 1 ]; then
        pass "$id" "one server start for the whole run, async workers included ($eng)"
    else fail "$id" "the server started $(sessions "$d") times ($eng)" "rc=$RC ${OUT:0:300}"; fi
done

d="$TEST_TMP/m13"; behaviour "$d" "MCP_M13"
cfg "$d" "[$(src "$d")]" '"capabilities": {"shell": {"enabled": false}, "network": {"enabled": false, "allowed_hosts": ["example.com"]}},'
run "$d" vm "$PULL"
if [[ "$OUT" == *"MCP_M13: roll back"* ]] && [[ "$OUT" == *"cannot bind the connections it makes"* ]]; then
    pass "M-13" "with the program's shell and network disabled the server still runs (like a hook), and the host-list note is printed"
else fail "M-13" "MCP source gated on the program's capabilities, or no note" "rc=$RC ${OUT:0:300}"; fi

PLACEHOLDER='fn work() {
    // TODO: finish this later
    return 1
}
main {
    print(work())
}'
QC='"code_quality": {"no_placeholders": {"enabled": true, "level": "hard"}},'
d="$TEST_TMP/m14"; behaviour "$d" "MCP_M14"; cfg "$d" "[$(src "$d")]" "$QC"
printf '%s\n' "$PLACEHOLDER" > "$d/p.naab"
if [ -x "$NAAB_GOV" ]; then
    gov_out="$(cd "$d" && "$NAAB_GOV" lint p.naab 2>&1)"; gov_rc=$?
    sleep 0.5
    gov_sessions="$(sessions "$d")"
    run "$d" vm "$PLACEHOLDER"
    if [ "$gov_rc" = 3 ] && [ "$gov_sessions" = 0 ] && [ "$(sessions "$d")" = 1 ]; then
        pass "M-14" "naab-gov lint (config loaded: exit 3) never starts an MCP source; naab-lang run does (control)"
    else fail "M-14" "MCP source started in the wrong process" "gov rc=$gov_rc sessions=$gov_sessions, after run $(sessions "$d") | ${gov_out:0:200}"; fi
else skip "M-14" "naab-gov not built"; fi

d="$TEST_TMP/m15"; behaviour "$d" "MCP_M15" ", \"mode_file\": \"$TEST_TMP/m15/mode\""
cfg "$d" "[$(src "$d" ', "refresh_seconds": 5')]"
( for _ in $(seq 1 300); do [ "$(sessions "$d")" -ge 1 ] && break; sleep 0.1; done
  sleep 0.3; [ -d "$d" ] && echo crash > "$d/mode" ) &
writer=$!
run "$d" vm 'use governance
use time
use array
main {
    time.sleep(7)
    let g = governance.guidance("deployment rollback")
    print("N=" + string(array.length(g)))
    print(g)
}'
kill "$writer" 2>/dev/null; wait "$writer" 2>/dev/null
if [[ "$OUT" == *"MCP_M15: roll back"* ]] && [[ "$OUT" == *"last copy is still used"* ]] &&
   grep -q '"kept_previous_copy":"true"' <<<"$(tele "$d")"; then
    pass "M-15" "a refresh whose server fails keeps the last good copy and says so"
else fail "M-15" "last good copy not kept" "rc=$RC ${OUT:0:400}"; fi

# ---------------------------------------------------------------------------
echo ""
echo "== the server may not live where the program can write (A36, decision (b))"
# M-17: the behaviour file in the project directory, which the program may
# write -- the source is refused and its server never starts. M-17c: the same
# file in a directory the policy blocks is fetched.
d="$TEST_TMP/m17"; BEH_AT="$d/beh.json" behaviour "$d" "MCP_M17"
cfg "$d" "[$(BEH_AT="$d/beh.json" src "$d")]" "\"capabilities\": {\"filesystem\": {\"mode\": \"write\", \"blocked_paths\": [\"$HELPERS\"]}},"
run "$d" vm "$PULL"
if [[ "$OUT" == *"N=0"* ]] && [[ "$OUT" == *"the program may write in $d"* ]] && [ "$(sessions "$d")" = 0 ]; then
    pass "M-17" "an MCP source whose file sits where the program may write is refused; its server never starts"
else fail "M-17" "MCP source in a program-writable directory was run" "sessions=$(sessions "$d") rc=$RC ${OUT:0:300}"; fi
d="$TEST_TMP/m17c"; BEH_AT="$d/beh.json" behaviour "$d" "MCP_M17C"
cfg "$d" "[$(BEH_AT="$d/beh.json" src "$d")]" "\"capabilities\": {\"filesystem\": {\"mode\": \"write\", \"blocked_paths\": [\"$HELPERS\", \"$d/beh.json\", \"$d/\"]}},"
run "$d" vm "$PULL"
if [[ "$OUT" == *"MCP_M17C: roll back"* ]] && [[ "$OUT" != *"the program may write in"* ]]; then
    pass "M-17c" "control: the same file in a directory the policy blocks is fetched"
else fail "M-17c" "a protected MCP source was refused" "rc=$RC ${OUT:0:300}"; fi

# M-18: the files a source names are refused to the program one by one too --
# defence in depth beside (b): here the extra argument sits in the project
# directory, so the source is refused AND the program still cannot write it.
d="$TEST_TMP/m18"; behaviour "$d" "MCP_M18"; printf '{}\n' > "$d/notes.json"
cfg "$d" "[{\"mcp\": [\"python3\", \"$STUB\", \"$(beh_path "$d")\", \"$d/notes.json\"]}]" \
    "\"capabilities\": {\"shell\": {\"enabled\": false}, \"filesystem\": {\"mode\": \"write\", \"blocked_paths\": [\"$SRV\", \"$HELPERS\"]}},"
run "$d" vm 'use file
main {
    file.write("notes.json", "{\"x\": 1}")
    print("WROTE")
}'
if [ "$RC" = 3 ] && [[ "$OUT" == *"Writing a project guidance file is blocked: notes.json"* ]] &&
   [[ "$OUT" != *"WROTE"* ]] && [ "$(cat "$d/notes.json")" = "{}" ]; then
    pass "M-18" "the program cannot rewrite a file an MCP source's arguments name"
else fail "M-18" "MCP source file writable by the program" "rc=$RC ${OUT:0:300}"; fi
run "$d" vm 'use file
main {
    file.write("other.json", "{}")
    print("WROTE")
}'
if [ "$RC" = 0 ] && [[ "$OUT" == *"WROTE"* ]] && [ -f "$d/other.json" ]; then
    pass "M-18c" "control: another file in the same directory is still writable"
else fail "M-18c" "the refusal is wider than the source's files" "rc=$RC ${OUT:0:300}"; fi

# ---------------------------------------------------------------------------
echo ""
echo "== what a person reads: the source is named in every display"
# A source with no path (https, command, MCP) used to print its empty path:
# "Project guidance ( > No placeholders)". The marker text is the control that
# the section was delivered at all; the label is the subject.
d="$TEST_TMP/m19"; behaviour "$d" "MCP_M19"; cfg "$d" "[$(src "$d")]" "$QC"
run "$d" vm "$PLACEHOLDER"
if [ "$RC" = 3 ] && [[ "$OUT" == *"MCP_M19_PH: unfinished work"* ]] &&
   [[ "$OUT" == *"Project guidance (mcp_guidance_stub > No placeholders):"* ]]; then
    pass "M-19" "terminal: a violation's guidance names the MCP source (mcp_guidance_stub > No placeholders)"
else fail "M-19" "terminal guidance label wrong" "rc=$RC $(grep -A1 'Project guidance' <<<"$OUT" | head -2)"; fi

# M-20: the agent's request and the end-of-run block. A resource named for the
# agent is delivered by the automatic start rule with its first request.
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
export FAKE_KEY_MCP="fake-key-mcp"
d="$TEST_TMP/m20"; mkdir -p "$d/agent"
behaviour "$d" "MCP_M20" ', "pages": [[{"uri": "kb://worker", "name": "worker"}]], "texts": {"kb://worker": "MCP_M20: answer from the invoice lines, never from memory."}'
printf '{"responses": [{"content": "The total is the sum of the invoice lines.", "input_tokens": 12, "output_tokens": 24}]}\n' > "$d/agent/fx.json"
if start_stub "$d/agent/fx.json" "$d/agent" >/dev/null 2>&1; then
    cfg "$d" "[$(src "$d")]" "\"agents\": {\"worker\": {\"provider\": \"gemini\", \"model\": \"stub-model\", \"api_base\": \"http://127.0.0.1:$STUB_PORT\", \"api_key_env\": \"FAKE_KEY_MCP\", \"max_tokens\": 200, \"max_turns\": 10, \"system_prompt\": \"You answer questions about invoices.\"}},"
    run "$d" vm 'use agent
main {
    let h = agent.create("worker")
    let r = agent.send(h, "How is the total computed?")
    print("SENT")
}'
    stop_stub
    req1="$(cat "$d/agent/req_1.json" 2>/dev/null)"
    if [[ "$OUT" != *SENT* ]] || [[ "$req1" != *"MCP_M20: answer from the invoice lines"* ]]; then
        fail "M-20" "harness: the start section was not delivered -- the label is unreadable" "rc=$RC ${OUT:0:300}"
    elif [[ "$req1" == *"[Project guidance: mcp_guidance_stub > worker]"* ]] &&
         grep -q -- "-> mcp_guidance_stub > worker" <<<"$OUT"; then
        pass "M-20" "the agent's request and the end-of-run block name the MCP source"
    else fail "M-20" "agent or end-of-run label wrong" "req: $(grep -o 'Project guidance:[^]]*]' <<<"$req1" | head -1) | $(grep -A1 -- '-- Guidance' <<<"$OUT" | head -2)"; fi
else skip "M-20" "the agent stub did not start"; fi

summary
