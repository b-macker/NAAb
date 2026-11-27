#!/usr/bin/env bash
# ============================================================
# test_agent_history_engine.sh -- the history a request carries is the one
# the engine recorded (A211; Bundle 21 PR 2)
#
# THE DEFECT (observed on master abf2c4cd and on 6a6b2b1b, stub)
#
# A program could write an agent handle's message list
# (h["messages"] = [...]) and the next agent.send() or agent.propose() sent
# that text to the model: the handle's anti-forge nonce covers its id and
# agent name, not the list, and the request builders read the list from the
# handle. The prompt scan reads only the new message, so a secret placed in
# the history reached the model under no_secrets (RU-09p refuses the same text
# as a prompt). A copy made with `let h2 = h` kept its own list, so sending
# through the other copy silently dropped turns. A list entry that is not a
# dict ended the run with "get: wrong index for variant".
#
# THE FIX (the owner's decisions of 2026-11-23 and 2026-11-27)
#
# The engine keeps each handle's history beside its tracker, and every
# request (send, its tool loop and final turn, propose, the step-up
# challenge) is built from it. send, propose and commit first compare the
# handle's list with it and refuse a handle whose list differs, before
# anything is sent: a catchable error, like the tampered-handle refusal, that
# names a context window and agent.reset() as the supported ways to send less
# history or start over. agent.messages() returns the engine's history.
#
#   AH-00   CONTROL: two sends; the second request carries the first exchange
#   AH-01c  CONTROL: a send on a fresh handle is sent
#   AH-01   a list written on a fresh handle: refused, nothing sent, the marker
#           in no request, and a NAAb try catches it
#   AH-01m  ... the message names a context window and agent.reset()
#   AH-01u  uncaught it ends the run with exit 1 (an ordinary error, not 3)
#   AH-02   an exchange edited after a real turn: refused, nothing more sent
#   AH-03   a pair appended after a real turn: refused
#   AH-04   the list emptied after a real turn: refused (deliberate trimming)
#   AH-05   a stale copy (`let h2 = h`, sent through h2, then h): refused
#   AH-05c  CONTROL: the same copy used alone is sent, with its history
#   AH-06   a list entry that is not a dict: refused with the history message,
#           not "wrong index for variant"
#   AH-07   agent.propose() on an edited list: refused, nothing sent
#   AH-07c  CONTROL: propose after a send carries the engine's history
#   AH-08   propose -> commit -> send: the third request carries the committed
#           exchange (commit appends to the engine's history)
#   AH-08r  agent.commit() on an edited list: refused, nothing appended
#   AH-09   agent.messages() returns the engine's history, not an edited list
#   AH-10   agent.reset() clears both: a send after reset carries no history
#   AH-11   CONTROL: agent.batch() then agent.send() on the same handle is sent
#   AH-12   CONTROL: a context window still trims, from the engine's history
#
# KILL TABLE (each mutant applied alone, naab-lang rebuilt, this suite run;
# observed 2026-11-27 on the default build; the arms that turned red):
#   K1  no comparison in agent.send()            -> AH-01 AH-01m AH-01u AH-02 AH-03 AH-04 AH-05 AH-06
#   K2  no comparison in agent.propose()         -> AH-07
#   K3  no comparison in agent.commit()          -> AH-08r
#   K4  the engine's history not appended on a send
#                                                -> AH-00 AH-04 AH-05 AH-05c AH-07c AH-09 AH-10 AH-11 AH-12
#   K5  ... not appended on a commit             -> AH-08
#   K6  agent.reset() leaves the engine's history -> AH-10
#   K7  the refusal thrown as a HARD block       -> AH-01 AH-01u AH-02 AH-03 AH-04 AH-05 AH-06 AH-07 AH-08r
#   K8  agent.messages() returns the handle's list -> AH-08r AH-09
#   K9  the send's request built from the handle's list (the comparison kept)
#                                                -> none: equivalent by construction, since the
#       comparison at the top makes the two lists equal when the request is built. K9 with K1
#       is the old behaviour (K1's arms).
# Not mutated: the step-up challenge's request and its recent prompts, now built from the
# engine's history too -- no arm stages a challenge, and a differing list is refused before
# the challenge is reached, so they read the same list K9 does.
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-${1:-$REPO/build/naab-lang}}"
TEST_TMP="$(mktemp -d)"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
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

echo "=== the history a request carries is the engine's (A211) ==="

case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;; *) WINDOWS=0 ;; esac
[ -n "${WINDIR:-}" ] && WINDOWS=1
if [ "$WINDOWS" = 1 ]; then
    skip "AH-STUB" "stub-backed arms are not run on Windows (stub_platform.sh)"
    echo ""; echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
    [ "$FAIL_COUNT" -eq 0 ]; exit $?
fi
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
export FAKE_KEY_AH="fake-key-agent-history"

FX='{"responses": [{"content": "r1", "input_tokens": 5, "output_tokens": 3}, {"content": "r2", "input_tokens": 5, "output_tokens": 3}, {"content": "r3", "input_tokens": 5, "output_tokens": 3}, {"content": "r4", "input_tokens": 5, "output_tokens": 3}]}'

# run_arm DIR PROGRAM [AGENT_EXTRA]: one run against a fresh stub. Exit status
# in DIR/rc, stdout+stderr in DIR/out.txt, request bodies in DIR/stub/req_N.json.
run_arm() {
    local d="$1" prog="$2" ax="${3:-}"
    mkdir -p "$d/stub"
    printf '%s\n' "$FX" > "$d/fixture.json"
    echo "NOT_RUN" > "$d/rc"
    if [ "$STUB_STARTED" = 1 ]; then stop_stub >/dev/null 2>&1; STUB_STARTED=0; fi
    start_stub "$d/fixture.json" "$d/stub" >/dev/null 2>&1 || { echo "STUB_FAIL" > "$d/rc"; return; }
    STUB_STARTED=1
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},\n "agents": {"worker": {"provider": "gemini", "model": "stub-model", "api_base": "http://127.0.0.1:%s", "api_key_env": "FAKE_KEY_AH", "max_tokens": 100, "max_turns": 20, "retry": {"max_attempts": 1}%s}}}\n' \
        "$STUB_PORT" "$ax" > "$d/govern.json"
    printf '%s\n' "$prog" > "$d/p.naab"
    (cd "$d" && timeout 60s "$NAAB" run p.naab > out.txt 2>&1); echo "$?" > "$d/rc"
    stop_stub >/dev/null 2>&1; STUB_STARTED=0
}
rc() { cat "$1/rc" 2>/dev/null; }
out_has() { grep -qF -- "$2" "$1/out.txt"; }
nreq() { ls "$1/stub" 2>/dev/null | grep -c '^req_[0-9]*\.json$'; }
req_has() { [ -f "$1/stub/req_$2.json" ] && grep -qF -- "$3" "$1/stub/req_$2.json"; }
any_req_has() { grep -qF -- "$2" "$1"/stub/req_*.json 2>/dev/null; }
# turns DIR N: the role/text sequence of request N, "user:first|model:r1|..."
turns() {
    python3 -I -c 'import json,sys
b=json.load(open(sys.argv[1],"rb"))
print("|".join("%s:%s" % (c.get("role"), "".join(p.get("text","") for p in c.get("parts",[]))) for c in b.get("contents",[])))' "$1/stub/req_$2.json" 2>/dev/null
}
why() { printf 'rc=%s reqs=%s %s' "$(rc "$1")" "$(nreq "$1")" "$(grep -m1 -E 'Error|error|RUN_|CAUGHT' "$1/out.txt" | head -c 200)"; }
HIST_MSG="message history differs"

prog() {  # the body of main; agent and handle setup prepended
    printf 'use agent\nmain {\n    let h = agent.create("worker")\n%s\n}' "$1"
}
caught() {  # wrap a statement block in try/catch that prints CAUGHT and the message
    printf '    try {\n%s\n        print("RUN_NOT_REFUSED")\n    } catch (e) {\n        print("CAUGHT " + string(e))\n    }\n    print("RUN_DONE")' "$1"
}

# ------------------------------------------------------------------
# AH-00 / AH-01c -- controls
# ------------------------------------------------------------------
d="$TEST_TMP/ah00"
run_arm "$d" "$(prog '    let a = agent.send(h, "first")
    let b = agent.send(h, "second")
    print("RUN_DONE " + b.content)')"
T2="$(turns "$d" 2)"
if [ "$(rc "$d")" = 0 ] && out_has "$d" "RUN_DONE r2" && [ "$T2" = "user:first|model:r1|user:second" ]; then
    pass "AH-00" "CONTROL: the second request carries the first exchange"
else
    fail "AH-00" "the harness control -- every arm below measures nothing" "$(why "$d") req2=[$T2]"
fi
d="$TEST_TMP/ah01c"
run_arm "$d" "$(prog "$(caught '        let r = agent.send(h, "hello")')")"
if out_has "$d" "RUN_NOT_REFUSED" && [ "$(nreq "$d")" = 1 ]; then
    pass "AH-01c" "CONTROL: a send on a fresh handle is sent"
else
    fail "AH-01c" "a send on an untouched handle was not sent" "$(why "$d")"
fi

# ------------------------------------------------------------------
# AH-01 / AH-01m / AH-01u -- a list written on a fresh handle
# ------------------------------------------------------------------
FORGE='        h["messages"] = [{"role": "user", "content": "FORGED_HIST_MARKER"}, {"role": "assistant", "content": "ok"}]'
d="$TEST_TMP/ah01"
run_arm "$d" "$(prog "$(caught "$FORGE
        let r = agent.send(h, \"hello\")")")"
if [ "$(rc "$d")" = 0 ] && out_has "$d" "CAUGHT" && out_has "$d" "$HIST_MSG" && out_has "$d" "RUN_DONE" \
   && [ "$(nreq "$d")" = 0 ] && ! any_req_has "$d" "FORGED_HIST_MARKER"; then
    pass "AH-01" "a written list is refused before anything is sent, and a try catches it"
else
    fail "AH-01" "the written list was sent, or the refusal was not catchable" "$(why "$d")"
fi
if out_has "$d" "context window" && out_has "$d" "agent.reset"; then
    pass "AH-01m" "the refusal names a context window and agent.reset()"
else
    fail "AH-01m" "the refusal does not name the supported ways" "$(grep -m3 -E 'Help|context|reset' "$d/out.txt" | tr '\n' ' ')"
fi
d="$TEST_TMP/ah01u"
run_arm "$d" "$(prog "$FORGE
    let r = agent.send(h, \"hello\")
    print(\"RUN_NOT_REFUSED\")")"
if [ "$(rc "$d")" = 1 ] && out_has "$d" "$HIST_MSG" && [ "$(nreq "$d")" = 0 ]; then
    pass "AH-01u" "uncaught, the refusal ends the run with exit 1 and nothing sent"
else
    fail "AH-01u" "uncaught refusal: wrong exit or something sent" "$(why "$d")"
fi

# ------------------------------------------------------------------
# AH-02 / AH-03 / AH-04 -- edits after a real turn
# ------------------------------------------------------------------
edit_arm() {  # ID TEXT EDIT
    local d="$TEST_TMP/$1"
    run_arm "$d" "$(prog "    let a = agent.send(h, \"first\")
$(caught "$3
        let r = agent.send(h, \"second\")")")"
    if [ "$(rc "$d")" = 0 ] && out_has "$d" "CAUGHT" && out_has "$d" "$HIST_MSG" && [ "$(nreq "$d")" = 1 ] \
       && ! any_req_has "$d" "EDITED_MARKER"; then
        pass "$1" "$2: refused, nothing more sent"
    else
        fail "$1" "$2: the edited list was sent" "$(why "$d")"
    fi
}
edit_arm "AH-02" "an exchange edited after a real turn" '        h["messages"] = [{"role": "user", "content": "first"}, {"role": "assistant", "content": "EDITED_MARKER"}]'
edit_arm "AH-03" "a pair appended after a real turn" '        let m = h["messages"]
        m.push({"role": "user", "content": "EDITED_MARKER"})
        m.push({"role": "assistant", "content": "ok"})
        h["messages"] = m'
edit_arm "AH-04" "the list emptied after a real turn" '        h["messages"] = []'

# ------------------------------------------------------------------
# AH-05 / AH-05c -- a stale copy
# ------------------------------------------------------------------
d="$TEST_TMP/ah05"
run_arm "$d" "$(prog "    let a = agent.send(h, \"first\")
    let h2 = h
    let b = agent.send(h2, \"second\")
$(caught '        let c = agent.send(h, "third")')")"
if [ "$(rc "$d")" = 0 ] && out_has "$d" "CAUGHT" && out_has "$d" "$HIST_MSG" && [ "$(nreq "$d")" = 2 ]; then
    pass "AH-05" "a stale copy is refused rather than sending a history that dropped a turn"
else
    fail "AH-05" "the stale copy was sent" "$(why "$d")"
fi
d="$TEST_TMP/ah05c"
run_arm "$d" "$(prog "    let a = agent.send(h, \"first\")
    let h2 = h
    let b = agent.send(h2, \"second\")
    let c = agent.send(h2, \"third\")
    print(\"RUN_DONE \" + c.content)")"
T3="$(turns "$d" 3)"
if [ "$(rc "$d")" = 0 ] && [ "$T3" = "user:first|model:r1|user:second|model:r2|user:third" ]; then
    pass "AH-05c" "CONTROL: the copy used alone is sent with its whole history"
else
    fail "AH-05c" "a copy used alone was refused or lost history" "$(why "$d") req3=[$T3]"
fi

# ------------------------------------------------------------------
# AH-06 -- an entry that is not a dict
# ------------------------------------------------------------------
d="$TEST_TMP/ah06"
run_arm "$d" "$(prog "$(caught '        h["messages"] = [1, 2]
        let r = agent.send(h, "hello")')")"
if [ "$(rc "$d")" = 0 ] && out_has "$d" "$HIST_MSG" && ! out_has "$d" "wrong index for variant" && [ "$(nreq "$d")" = 0 ]; then
    pass "AH-06" "an entry that is not a dict is refused with the history message"
else
    fail "AH-06" "a non-dict entry was not refused cleanly" "$(why "$d")"
fi

# ------------------------------------------------------------------
# AH-07 / AH-07c / AH-08 -- propose and commit
# ------------------------------------------------------------------
PROP=', "propose_candidates_max": 1'
d="$TEST_TMP/ah07"
run_arm "$d" "$(prog "$(caught "$FORGE
        let p = agent.propose(h, \"hello\")")")" "$PROP"
if [ "$(rc "$d")" = 0 ] && out_has "$d" "CAUGHT" && out_has "$d" "$HIST_MSG" && [ "$(nreq "$d")" = 0 ]; then
    pass "AH-07" "agent.propose() on a written list is refused, nothing sent"
else
    fail "AH-07" "propose sent the written list" "$(why "$d")"
fi
d="$TEST_TMP/ah07c"
run_arm "$d" "$(prog '    let a = agent.send(h, "first")
    let p = agent.propose(h, "second")
    print("RUN_DONE " + string(len(p["candidates"])))')" "$PROP"
T2="$(turns "$d" 2)"
if [ "$(rc "$d")" = 0 ] && out_has "$d" "RUN_DONE 1" && [ "$T2" = "user:first|model:r1|user:second" ]; then
    pass "AH-07c" "CONTROL: propose after a send carries the engine's history"
else
    fail "AH-07c" "propose lost the history or failed" "$(why "$d") req2=[$T2]"
fi
d="$TEST_TMP/ah08"
run_arm "$d" "$(prog '    let p = agent.propose(h, "first")
    let c = agent.commit(h, p["candidates"][0])
    let r = agent.send(h, "second")
    print("RUN_DONE " + r.content)')" "$PROP"
T2="$(turns "$d" 2)"
if [ "$(rc "$d")" = 0 ] && out_has "$d" "RUN_DONE r2" && [ "$T2" = "user:first|model:r1|user:second" ]; then
    pass "AH-08" "propose -> commit -> send: the send carries the committed exchange"
else
    fail "AH-08" "the commit did not reach the next request, or the send was refused" "$(why "$d") req2=[$T2]"
fi

d="$TEST_TMP/ah08r"
run_arm "$d" "$(prog "    let p = agent.propose(h, \"first\")
$(caught '        h["messages"] = [{"role": "user", "content": "EDITED_MARKER"}, {"role": "assistant", "content": "ok"}]
        let c = agent.commit(h, p["candidates"][0])')
    print(\"RUN_N \" + string(len(agent.messages(h))))")" "$PROP"
if [ "$(rc "$d")" = 0 ] && out_has "$d" "CAUGHT" && out_has "$d" "$HIST_MSG" && out_has "$d" "RUN_N 0"; then
    pass "AH-08r" "agent.commit() on an edited list is refused and appends nothing"
else
    fail "AH-08r" "commit accepted an edited list" "$(why "$d") $(grep -m1 RUN_N "$d/out.txt")"
fi

# ------------------------------------------------------------------
# AH-09 -- agent.messages() is the engine's history
# ------------------------------------------------------------------
d="$TEST_TMP/ah09"
run_arm "$d" "$(prog '    let a = agent.send(h, "first")
    h["messages"] = [{"role": "user", "content": "EDITED_MARKER"}]
    let m = agent.messages(h)
    print("RUN_DONE n=" + string(len(m)) + " first=" + m[0]["content"])')"
if [ "$(rc "$d")" = 0 ] && out_has "$d" "RUN_DONE n=2 first=first"; then
    pass "AH-09" "agent.messages() returns the engine's history, not the edited list"
else
    fail "AH-09" "agent.messages() returned the program's list" "$(why "$d") $(grep -m1 RUN_DONE "$d/out.txt")"
fi

# ------------------------------------------------------------------
# AH-10 -- reset clears both
# ------------------------------------------------------------------
d="$TEST_TMP/ah10"
run_arm "$d" "$(prog '    let a = agent.send(h, "first")
    agent.reset(h)
    let r = agent.send(h, "after-reset")
    print("RUN_DONE " + r.content + " n=" + string(len(agent.messages(h))))')"
T2="$(turns "$d" 2)"
if [ "$(rc "$d")" = 0 ] && out_has "$d" "RUN_DONE r2 n=2" && [ "$T2" = "user:after-reset" ]; then
    pass "AH-10" "after agent.reset() a send carries no earlier history"
else
    fail "AH-10" "reset left history behind, or the send was refused" "$(why "$d") req2=[$T2]"
fi

# ------------------------------------------------------------------
# AH-11 / AH-12 -- controls: shared handles and windowing still work
# ------------------------------------------------------------------
d="$TEST_TMP/ah11"
run_arm "$d" "$(prog '    let rs = agent.batch([h], ["batched"])
    let r = agent.send(h, "after-batch")
    print("RUN_DONE " + r.content)')"
T2="$(turns "$d" 2)"
if [ "$(rc "$d")" = 0 ] && out_has "$d" "RUN_DONE r2" && [ "$T2" = "user:batched|model:r1|user:after-batch" ]; then
    pass "AH-11" "CONTROL: a send after agent.batch() on the same handle is sent with its history"
else
    fail "AH-11" "a batch then send on one handle was refused or lost history" "$(why "$d") req2=[$T2]"
fi
d="$TEST_TMP/ah12"
run_arm "$d" "$(prog '    let a = agent.send(h, "one")
    let b = agent.send(h, "two")
    let c = agent.send(h, "three")
    print("RUN_DONE " + c.content)')" ', "context_window": 2, "context_strategy": "recent"'
T3="$(turns "$d" 3)"
if [ "$(rc "$d")" = 0 ] && out_has "$d" "RUN_DONE r3" && [ "$T3" = "user:two|model:r2|user:three" ]; then
    pass "AH-12" "CONTROL: a context window trims the engine's history"
else
    fail "AH-12" "windowing changed" "$(why "$d") req3=[$T3]"
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
