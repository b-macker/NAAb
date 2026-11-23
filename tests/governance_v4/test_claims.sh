#!/usr/bin/env bash
# ============================================================
# test_claims.sh -- claims as evidence (Bundle 20 PR 5)
#
# A reply's claims, read by include/naab/claim_reader.h, compared with what
# was observed: S19 against the tool's recorded outcome (A202), S24 against
# the validation recorded next. Every claim is written as a chained
# AGENT_CLAIM, every validation names what it settles, and `naab-gov replay`
# checks the two against each other. Each run is a real run against
# tests/helpers/agent_stub.py; no telemetry file is edited and re-sealed.
#
#   CL-00  AGENT_CLAIM records what `naab-gov claims` reads from the same reply
#   CL-01  S19 fires on a snake_case tool that failed while the reply says it
#          completed (A202: the name was split and never checked);
#          CL-01c the same reply over a tool that succeeded does not fire;
#          CL-01b ... and is counted accurate (the tool is checked at all)
#   CL-02  a negated claim ("was not successful") over a tool that succeeded fires
#   CL-03  a reply naming the tool with no status word is not counted accurate
#          (A202): the rolling accuracy stays N/A. A one-word tool, so the old
#          reading -- which never saw a snake_case name -- would check it
#   CL-04  a mixed reply is not judged (the same one-word tool)
#   CL-05  a validation settles the latest reply's claims once: turn, settles,
#          contradicts; a second validation settles nothing
#   CL-06  S24 on: a failed validation after a claimed fix costs coherence on
#          the next turn, beside S22; CL-06c S24 off (the default): S22 alone
#   CL-07  a reply that admitted the failure is not contradicted, S24 on
#   CL-08  a failed validation after the last send is scored at exit as S24;
#          CL-08c with S24 off it is not
#   CL-09  the per-agent override turns S24 on for that agent only
#   CL-10  replay: claims MATCH on a real run with its transcript, and the
#          coherence identity holds with S24's penalty listed
#   CL-11  replay on copies, each one edit, never re-sealed: a claim's direction,
#          a contradiction, a settlement with a claim removed, an id
#   CL-12  a negative weight is warned at load; CL-12c a positive one is not
#   CL-13  S24's latch is consumed while S24 is off: a reload that turns S24
#          on later charges nothing stale (A203's shape); CL-13c S24 on from the
#          start charges the same contradiction
#   CL-IO  the suite's own readers give the same answers with Windows' streams
#          injected (stdin decoded as cp1252, "\n" written as "\r\n")
#
# THE INSTRUMENT. Every Python reader takes the engine's output as BYTES and
# writes its answer as bytes; `tq` prints through a print() that writes UTF-8
# bytes. The first version read and printed text, which a native Windows
# python turns into a different answer: test_evidence_utf8.sh failed that way
# on build-windows at 46ffbbdd, and this suite failed CL-00, CL-10 and CL-11
# on Linux with the same two stream behaviours injected (it is stub-backed, so
# Windows does not run it yet).
#
# KILL TABLE (each mutant applied alone, rebuilt, the suite re-run; the arms
# that turned red):
#   M1  S19 skips the reading                                    -> CL-01b CL-01 CL-02
#   M2  S19 counts a named tool with no claim as accurate        -> CL-03 CL-04
#   M3  S19 judges a mixed reply as success                          -> CL-04
#   M4  settleClaims() keeps the claims pending (settled twice)      -> CL-05
#   M5  S24 charges any failed validation (contradiction ignored)    -> CL-07
#   M6  S24's latch consumed only when S24 is on                     -> CL-13
#   M7  no exit scoring for S24                                      -> CL-08
#   M8  AGENT_CLAIM writes the span of the sentence, not the word    -> CL-00 CL-10
#   M9  replay's settlement check accepts a partial settlement       -> CL-11
#   M10 replay's re-read skipped (always MATCH)                      -> CL-11 (the span edit)
#   M11 tq's print() writes text (the builtin)                       -> CL-IO
#   M12 CL-10's reader prints text                                    -> CL-IO
#   Not measured here: a reader that decodes stdin as text. This
#   suite's files are ASCII, which cp1252 decodes the same; EU-IO in
#   test_evidence_utf8.sh measures that half on non-ASCII data.
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
NAAB_GOV="${NAAB_GOV:-$REPO/build/naab-gov}"
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
[ -x "$NAAB_GOV" ] || { echo "naab-gov not built: the reader and replay arms are UNMEASURABLE, not a pass"; exit 1; }

echo "=== claims as evidence (Bundle 20 PR 5) ==="

case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;; *) WINDOWS=0 ;; esac
[ -n "${WINDIR:-}" ] && WINDOWS=1
if [ "$WINDOWS" = 1 ]; then
    skip "CL-STUB" "stub-backed arms are not run on Windows (stub_platform.sh)"
    echo ""; echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
    [ "$FAIL_COUNT" -eq 0 ]; exit $?
fi
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
export FAKE_KEY_CL="fake-key-claims"

# py CODE: run a reader with stdin and stdout as given. PY_PRE (and TQ_PRE for
# tq) is empty except in CL-IO, which injects Windows' stream behaviour.
PY_PRE=""; TQ_PRE=""
py() { python3 -I -c "$PY_PRE$1"; }
# tq FILE EXPR: EXPR sees `ev`, the events in file order; its print() writes
# UTF-8 bytes, so no platform's text stream changes the answer.
tq() {
    python3 -I - "$1" "$2" "$TQ_PRE" <<'PYEOF'
import json, sys
if len(sys.argv) > 3 and sys.argv[3]:
    exec(sys.argv[3])
def print(*a, sep=" ", end="\n"):
    sys.stdout.buffer.write((sep.join(str(x) for x in a) + end).encode("utf-8"))
ev = []
for line in open(sys.argv[1], encoding="utf-8", errors="strict"):
    try:
        ev.append(json.loads(line))
    except ValueError:
        pass
def of(t, **kw):
    return [e for e in ev if e.get("event_type") == t and all(e.get(k) == v for k, v in kw.items())]
exec(sys.argv[2])
PYEOF
}

# crun DIR FIXTURE SIGNALS_JSON PROGRAM_BODY [AGENT_EXTRA] [FUNCTIONS]
# One run in DIR: CDD every turn, adaptive baseline off (S19 pays on the turn
# it fires), the transcript on.
crun() {
    local d="$1" fx="$2" sig="$3" body="$4" ax="${5:-}" fns="${6:-}"
    mkdir -p "$d/stub"
    start_stub "$fx" "$d/stub" >/dev/null 2>&1 || { echo "STUB_FAIL"; return; }
    STUB_STARTED=1
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},\n "telemetry": {"enabled": true, "output_file": "tele.jsonl", "tamper_evidence": {"enabled": true},\n   "transcript": {"enabled": true, "output_file": "transcript.jsonl"}},\n "behavioral_sequences": {"enabled": true}, "advisory_escalation": {"enabled": false},\n "context_drift": {"enabled": true, "level": "advisory", "check_interval_turns": 1, "adaptive_baseline_enabled": false,\n   "reality_checkpoint": {"enabled": false}, "signals": %s},\n "agents": {"worker": {"provider": "gemini", "model": "stub-model", "api_base": "http://127.0.0.1:%s", "api_key_env": "FAKE_KEY_CL", "max_tokens": 100, "max_turns": 20, "retry": {"max_attempts": 1}%s}}}\n' \
        "$sig" "$STUB_PORT" "$ax" > "$d/govern.json"
    printf 'use agent\n%s\nmain {\n    let h = agent.create("worker")\n%s\n    print("RUN_DONE")\n}\n' "$fns" "$body" > "$d/p.naab"
    local out; out="$( (cd "$d" && timeout 120s "$NAAB" p.naab 2>"$d/err.txt") )"
    stop_stub; STUB_STARTED=0
    printf '%s' "$out"
}
reply_fixture() {  # $1 file, $2 the reply text (no double quotes)
    printf '{"responses": [{"content": "%s", "input_tokens": 5, "output_tokens": 14}]}\n' "$2" > "$1"
}
tool_fixture() {  # $1 file, $2 the final reply text, [$3 the tool, default write_file]
    printf '{"responses": [{"tool_calls": [{"name": "%s", "args": {"path": "out.txt"}}]}, {"content": "%s", "input_tokens": 5, "output_tokens": 14}]}\n' "${3:-write_file}" "$2" > "$1"
}
TOOLS=', "tools_enabled": true, "tools": ["write_file"]'
REG='    agent.register_tool("write_file", write_file, {"description": "d", "parameters": {"path": {"type": "string", "description": "p"}}})
    let r = agent.send(h, "save the report")'
FN_FAIL='fn write_file(path) { throw "disk full" }'
FN_OK='fn write_file(path) { return "wrote " + path }'
# A one-word tool: the old reading split snake_case names, which masked every
# other A202 defect for write_file (an outer gate hiding the inner one).
TOOLS1=', "tools_enabled": true, "tools": ["lookup"]'
REG1='    agent.register_tool("lookup", lookup, {"description": "d", "parameters": {"path": {"type": "string", "description": "p"}}})
    let r = agent.send(h, "look it up")'
FN1_FAIL='fn lookup(path) { throw "not found" }'
# The penalties of the handle's analysed CDD_TURNs, joined.
PENS='print(";".join(e.get("penalties_detail", "") for e in of("CDD_TURN", analyzed="true")))'

# ------------------------------------------------------------------
# CL-00 -- AGENT_CLAIM records what the reader reads
# ------------------------------------------------------------------
TXT0='I fixed the parser and the tests pass now, with no errors.'
reply_fixture "$TEST_TMP/c0.json" "$TXT0"
crun "$TEST_TMP/cl00" "$TEST_TMP/c0.json" '{}' '    let r = agent.send(h, "fix it")' >/dev/null
GOT0="$(tq "$TEST_TMP/cl00/tele.jsonl" 'print(" ".join("%s%s%s@%s" % (e["kind"], "+" if e["positive"] == "true" else "-", "!" if e["negated"] == "true" else "", e["span"]) for e in of("AGENT_CLAIM")))')"
printf '%s' "$TXT0" | "$NAAB_GOV" claims - --json > "$TEST_TMP/cl00/claims.json" 2>/dev/null
PY_WANT0='import json,sys
d=json.load(sys.stdin.buffer)
sys.stdout.buffer.write(" ".join("%s%s%s@%s" % (c["kind"], "+" if c["positive"] else "-", "!" if c["negated"] else "", c["span"]) for c in d["claims"]).encode("utf-8"))'
WANT0="$(py "$PY_WANT0" < "$TEST_TMP/cl00/claims.json")"
if [ -n "$WANT0" ] && [ "$GOT0" = "$WANT0" ] && [ "$(tq "$TEST_TMP/cl00/tele.jsonl" 'print(len(of("AGENT_CLAIM", claim_id="1.0.0")))')" = 1 ]; then
    pass "CL-00" "AGENT_CLAIM records the claims the reader reads from the same reply ($GOT0)"
else
    fail "CL-00" "AGENT_CLAIM does not record what the reader reads" "want [$WANT0] got [$GOT0]"
fi

# ------------------------------------------------------------------
# CL-01 -- S19 on a snake_case tool (A202)
# ------------------------------------------------------------------
tool_fixture "$TEST_TMP/c1.json" "I called write_file and it completed successfully."
crun "$TEST_TMP/cl01" "$TEST_TMP/c1.json" '{}' "$REG" "$TOOLS" "$FN_FAIL" >/dev/null
crun "$TEST_TMP/cl01c" "$TEST_TMP/c1.json" '{}' "$REG" "$TOOLS" "$FN_OK" >/dev/null
P1="$(tq "$TEST_TMP/cl01/tele.jsonl" "$PENS")"; P1C="$(tq "$TEST_TMP/cl01c/tele.jsonl" "$PENS")"
A1C="$(tq "$TEST_TMP/cl01c/tele.jsonl" 'print([e["claim_accuracy_rolling"] for e in of("RECONCILIATION_TURN")])')"
if [ "$(tq "$TEST_TMP/cl01c/tele.jsonl" 'print(len(of("CDD_TURN", analyzed="true")))')" = 1 ] && [[ "$P1C" != *claim_result* ]]; then
    pass "CL-01c" "the same claim over a tool that succeeded produces an analysed turn and does not fire"
else
    fail "CL-01c" "the control produced no analysed turn, or fired -- CL-01 measures nothing" "pens=[$P1C] $(tail -3 "$TEST_TMP/cl01c/err.txt")"
fi
if [[ "$A1C" == *"1.0000"* ]]; then
    pass "CL-01b" "the matching claim about write_file is counted accurate: the snake_case tool is checked at all"
else
    fail "CL-01b" "a claim about a snake_case tool was not checked" "acc=$A1C"
fi
if [[ "$P1" == *"claim_result=0.1200"* ]]; then
    pass "CL-01" "S19 charges a reply that says write_file completed when it failed"
else
    fail "CL-01" "S19 did not fire on a snake_case tool the reply misreported" "pens=[$P1]"
fi

# ------------------------------------------------------------------
# CL-02 / CL-03 / CL-04 -- negation, no claim, a mixed reply
# ------------------------------------------------------------------
tool_fixture "$TEST_TMP/c2.json" "write_file was not successful."
crun "$TEST_TMP/cl02" "$TEST_TMP/c2.json" '{}' "$REG" "$TOOLS" "$FN_OK" >/dev/null
P2="$(tq "$TEST_TMP/cl02/tele.jsonl" "$PENS")"
if [[ "$P2" == *"claim_result=0.1200"* ]]; then
    pass "CL-02" "a negated claim over a tool that succeeded is a misreport (\"not successful\")"
else
    fail "CL-02" "a negated claim was not read as one" "pens=[$P2]"
fi
tool_fixture "$TEST_TMP/c3.json" "I called lookup with the report path." lookup
crun "$TEST_TMP/cl03" "$TEST_TMP/c3.json" '{}' "$REG1" "$TOOLS1" "$FN1_FAIL" >/dev/null
A3="$(tq "$TEST_TMP/cl03/tele.jsonl" 'print([e["claim_accuracy_rolling"] for e in of("RECONCILIATION_TURN")])')"
if [ "$A3" = "['N/A']" ]; then
    pass "CL-03" "a reply that names the tool and claims nothing is not counted accurate (accuracy N/A)"
else
    fail "CL-03" "a reply with no claim was counted" "acc=$A3"
fi
tool_fixture "$TEST_TMP/c4.json" "lookup succeeded but the upload failed." lookup
crun "$TEST_TMP/cl04" "$TEST_TMP/c4.json" '{}' "$REG1" "$TOOLS1" "$FN1_FAIL" >/dev/null
P4="$(tq "$TEST_TMP/cl04/tele.jsonl" "$PENS")"; A4="$(tq "$TEST_TMP/cl04/tele.jsonl" 'print([e["claim_accuracy_rolling"] for e in of("RECONCILIATION_TURN")])')"
if [[ "$P4" != *claim_result* ]] && [ "$A4" = "['N/A']" ]; then
    pass "CL-04" "a reply that says both of one tool is not judged either way"
else
    fail "CL-04" "a mixed reply was judged" "pens=[$P4] acc=$A4"
fi

# ------------------------------------------------------------------
# CL-05 -- settlement, once
# ------------------------------------------------------------------
cat > "$TEST_TMP/c5.json" <<'EOF'
{"responses": [{"content": "I fixed the bug in the parser and the tests pass now.", "input_tokens": 5, "output_tokens": 14},
               {"content": "Looking at the parser again for the remaining case.", "input_tokens": 5, "output_tokens": 11}]}
EOF
VAL_BODY='    let r = agent.send(h, "fix the parser")
    let v = agent.record_validation(h, false, "test_parse failed")
    let v2 = agent.record_validation(h, false, "test_parse failed again")
    let r2 = agent.send(h, "try again")'
crun "$TEST_TMP/cl05" "$TEST_TMP/c5.json" '{}' "$VAL_BODY" >/dev/null
V5="$(tq "$TEST_TMP/cl05/tele.jsonl" 'print([(e["turn"], e["settles"], e["claims_only_positive"], e["contradicts"]) for e in of("VALIDATION_RECORDED")])')"
if [ "$V5" = "[('0', '1.0.0,1.0.1', 'true', 'true'), ('0', '', 'false', 'false')]" ]; then
    pass "CL-05" "the first validation settles both claims of turn 0 and records the contradiction; the second settles nothing"
else
    fail "CL-05" "settlement is not as recorded" "$V5"
fi

# ------------------------------------------------------------------
# CL-06 -- S24 on and off, mid-run
# ------------------------------------------------------------------
VAL1='    let r = agent.send(h, "fix the parser")
    let v = agent.record_validation(h, false, "test_parse failed")
    let r2 = agent.send(h, "try again")'
crun "$TEST_TMP/cl06" "$TEST_TMP/c5.json" '{"claim_validation": true}' "$VAL1" >/dev/null
crun "$TEST_TMP/cl06c" "$TEST_TMP/c5.json" '{}' "$VAL1" >/dev/null
P6="$(tq "$TEST_TMP/cl06/tele.jsonl" "$PENS")"; P6C="$(tq "$TEST_TMP/cl06c/tele.jsonl" "$PENS")"
if [[ "$P6C" == *"validation_outcome=0.1500"* ]] && [[ "$P6C" != *claim_validation* ]]; then
    pass "CL-06c" "S24 off (the default): the failed validation costs S22 alone"
else
    fail "CL-06c" "with S24 off the turn is not S22 alone" "pens=[$P6C]"
fi
if [[ "$P6" == *"validation_outcome=0.1500"* ]] && [[ "$P6" == *"claim_validation=0.1000"* ]]; then
    pass "CL-06" "S24 on: the claimed fix the validation contradicted costs coherence beside S22"
else
    fail "CL-06" "S24 did not charge the contradicted claim" "pens=[$P6]"
fi

# ------------------------------------------------------------------
# CL-07 -- a reply that admitted the failure
# ------------------------------------------------------------------
cat > "$TEST_TMP/c7.json" <<'EOF'
{"responses": [{"content": "I changed the parser, but the tests still fail on the empty case.", "input_tokens": 5, "output_tokens": 14},
               {"content": "Looking at the parser again for the remaining case.", "input_tokens": 5, "output_tokens": 11}]}
EOF
crun "$TEST_TMP/cl07" "$TEST_TMP/c7.json" '{"claim_validation": true}' "$VAL1" >/dev/null
P7="$(tq "$TEST_TMP/cl07/tele.jsonl" "$PENS")"; C7="$(tq "$TEST_TMP/cl07/tele.jsonl" 'print([e["contradicts"] for e in of("VALIDATION_RECORDED")])')"
if [[ "$P7" == *"validation_outcome=0.1500"* ]] && [[ "$P7" != *claim_validation* ]] && [ "$C7" = "['false']" ]; then
    pass "CL-07" "a reply that said the tests still fail is not contradicted: S22 charges, S24 does not"
else
    fail "CL-07" "an admitted failure was charged as a contradicted claim" "pens=[$P7] contradicts=$C7"
fi

# ------------------------------------------------------------------
# CL-08 -- exit scoring
# ------------------------------------------------------------------
VAL_END='    let r = agent.send(h, "fix the parser")
    let v = agent.record_validation(h, false, "test_parse failed")'
crun "$TEST_TMP/cl08" "$TEST_TMP/c5.json" '{"claim_validation": true}' "$VAL_END" >/dev/null
crun "$TEST_TMP/cl08c" "$TEST_TMP/c5.json" '{}' "$VAL_END" >/dev/null
X8="$(tq "$TEST_TMP/cl08/tele.jsonl" 'print(sorted((e.get("signal"), e["penalty"]) for e in of("VALIDATION_SCORED_AT_EXIT")))')"
# The control compares penalties only: `signal` is new in this PR.
X8C="$(tq "$TEST_TMP/cl08c/tele.jsonl" 'print(sorted(e["penalty"] for e in of("VALIDATION_SCORED_AT_EXIT")))')"
if [ "$X8C" = "['0.1500']" ]; then
    pass "CL-08c" "S24 off: only S22 is scored at exit"
else
    fail "CL-08c" "exit scoring with S24 off" "$X8C"
fi
if [ "$X8" = "[('claim_validation', '0.1000'), ('validation_outcome', '0.1500')]" ] &&
   grep -q 'context_drift.claim_validation' "$TEST_TMP/cl08/err.txt"; then
    pass "CL-08" "a contradicted claim settled after the last send is scored at exit as S24 and shown in the summary"
else
    fail "CL-08" "S24 was not scored at exit" "$X8 / $(grep -c claim_validation "$TEST_TMP/cl08/err.txt")"
fi

# ------------------------------------------------------------------
# CL-09 -- the per-agent override
# ------------------------------------------------------------------
crun "$TEST_TMP/cl09" "$TEST_TMP/c5.json" '{}' "$VAL1" ', "context_drift_signals": {"claim_validation": true}' >/dev/null
P9="$(tq "$TEST_TMP/cl09/tele.jsonl" "$PENS")"
if [[ "$P9" == *"claim_validation=0.1000"* ]] && ! grep -q 'unknown context_drift_signals key' "$TEST_TMP/cl09/err.txt"; then
    pass "CL-09" "context_drift_signals turns S24 on for the agent it names"
else
    fail "CL-09" "the per-agent override did not reach S24" "pens=[$P9] $(grep -m1 context_drift_signals "$TEST_TMP/cl09/err.txt")"
fi

# ------------------------------------------------------------------
# CL-10 -- replay on a real run with its transcript
# ------------------------------------------------------------------
RJ="$TEST_TMP/cl06/replay.json"
(cd "$TEST_TMP/cl06" && "$NAAB_GOV" replay tele.jsonl --transcript transcript.jsonl --json > replay.json 2>/dev/null)
PY_R10='import json,sys
d=json.load(sys.stdin.buffer); c=d["checks"]
v=(c["claims"]["match"], c["claims"]["diverged"], c["claims"]["unmeasurable"], c["coherence"]["diverged"], c["coherence"]["match"], d["claims"]["settled"])
sys.stdout.buffer.write(" ".join(str(x) for x in v).encode("ascii"))'
R10="$(py "$PY_R10" < "$RJ" 2>/dev/null)"
read -r CM CD CU COD COM CS <<<"${R10:-x x x x x x}"
if [ "${CM:-0}" -ge 3 ] && [ "$CD" = 0 ] && [ "$CU" = 0 ] && [ "$COD" = 0 ] && [ "${COM:-0}" -ge 2 ] && [ "$CS" = 2 ]; then
    pass "CL-10" "replay: every claim decision MATCHes, the reply re-read from the transcript included, and coherence reconciles with S24 listed"
else
    fail "CL-10" "replay on a real run is not all MATCH" "claims m/d/u=$CM/$CD/$CU coherence d/m=$COD/$COM settled=$CS"
fi

# ------------------------------------------------------------------
# CL-11 -- edited copies, never re-sealed
# ------------------------------------------------------------------
# edit NAME PYTHON: a copy of cl06's telemetry with one event changed; prints
# the claims check's diverged count from replay on the copy.
edit_copy() {
    local d="$TEST_TMP/cl11_$1"; mkdir -p "$d"
    cp "$TEST_TMP/cl06/transcript.jsonl" "$d/"
    py "import json,sys
out=[]
for l in sys.stdin.buffer:
    e=json.loads(l)
$2
    out.append(json.dumps(e, separators=(',', ':')))
sys.stdout.buffer.write(('\n'.join(out)+'\n').encode('utf-8'))" < "$TEST_TMP/cl06/tele.jsonl" > "$d/tele.jsonl"
    (cd "$d" && "$NAAB_GOV" replay tele.jsonl --transcript transcript.jsonl --json > replay.json 2>/dev/null)
    py 'import json,sys; sys.stdout.buffer.write(str(json.load(sys.stdin.buffer)["checks"]["claims"]["diverged"]).encode("ascii"))' < "$d/replay.json" 2>/dev/null
}
E1="$(edit_copy positive '    if e.get("claim_id") == "1.0.0": e["positive"] = "false"')"
E2="$(edit_copy contradicts '    if e.get("event_type") == "VALIDATION_RECORDED": e["contradicts"] = "false"')"
E3="$(edit_copy partial '    if e.get("event_type") == "VALIDATION_RECORDED" and e.get("settles"): e["settles"] = "1.0.0"')"
E4="$(edit_copy id '    if e.get("claim_id") == "1.0.1": e["claim_id"] = "1.0.7"')"
# Only the re-read can see this one: no settlement rule reads a span.
E5="$(edit_copy span '    if e.get("claim_id") == "1.0.0": e["span"] = "3-7"')"
if [ "${E1:-0}" -ge 1 ] && [ "${E2:-0}" -ge 1 ] && [ "${E3:-0}" -ge 1 ] && [ "${E4:-0}" -ge 1 ] && [ "${E5:-0}" -ge 1 ]; then
    pass "CL-11" "each edit is a claims DIVERGED: a claim's direction, the contradiction, a partial settlement, an id, a span"
else
    fail "CL-11" "an edited copy was not caught by the claims check" "positive=$E1 contradicts=$E2 partial=$E3 id=$E4 span=$E5"
fi

# ------------------------------------------------------------------
# CL-12 -- a negative weight is said
# ------------------------------------------------------------------
crun "$TEST_TMP/cl12" "$TEST_TMP/c0.json" '{}' '    let r = agent.send(h, "x")' >/dev/null
py 'import json,sys
c=json.load(sys.stdin.buffer); c["context_drift"]["weights"]={"claim_result_reconciliation": -0.1}; sys.stdout.buffer.write(json.dumps(c).encode("utf-8"))' < "$TEST_TMP/cl12/govern.json" > "$TEST_TMP/cl12/neg.json"
py 'import json,sys
c=json.load(sys.stdin.buffer); c["context_drift"]["weights"]={"claim_result_reconciliation": 0.2}; sys.stdout.buffer.write(json.dumps(c).encode("utf-8"))' < "$TEST_TMP/cl12/govern.json" > "$TEST_TMP/cl12/pos.json"
printf 'main {\n    print("ok")\n}\n' > "$TEST_TMP/cl12/q.naab"
W_NEG="$( (cd "$TEST_TMP/cl12" && cp neg.json govern.json && "$NAAB" q.naab 2>&1) )"
W_POS="$( (cd "$TEST_TMP/cl12" && cp pos.json govern.json && "$NAAB" q.naab 2>&1) )"
if [[ "$W_POS" == *ok* ]] && [[ "$W_POS" != *"is negative"* ]]; then
    pass "CL-12c" "a positive weight loads without the warning"
else
    fail "CL-12c" "the control run" "$(tail -2 <<<"$W_POS")"
fi
if [[ "$W_NEG" == *'"context_drift.weights.claim_result_reconciliation" is negative'* ]]; then
    pass "CL-12" "a negative weight is warned at load: the signal reads on and never pays"
else
    fail "CL-12" "a negative weight loaded silently" "$(grep -m2 'Warning' <<<"$W_NEG")"
fi

# ------------------------------------------------------------------
# CL-13 -- S24's latch is consumed while S24 is off (A203's shape)
# ------------------------------------------------------------------
# send (claims a fix) -> failed validation -> send (S24 off: the latch is
# consumed, nothing charged) -> an operator turns S24 on -> send. Nothing may
# charge on the last turn: the contradiction was consumed before S24 was on.
source "$SCRIPT_DIR/../helpers/config_swap.sh"
SWAP_PROG_BODY='    let r = agent.send(h, "fix the parser")
    let v = agent.record_validation(h, false, "test_parse failed")
    let r2 = agent.send(h, "try again")
    let sw = <<python
import time, os
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
time.sleep(1.1)
_swap("RUNDIR/on.json", "RUNDIR/govern.json")
print("swapped")
>>
    let r3 = agent.send(h, "and again")'
swap_run() {  # $1 dir, $2 signals JSON the run starts with
    local d="$1"; mkdir -p "$d/stub"
    start_stub "$TEST_TMP/c5.json" "$d/stub" >/dev/null 2>&1 || { echo "STUB_FAIL"; return; }
    STUB_STARTED=1
    for f in govern.json on.json; do
        local sig="$2"; [ "$f" = on.json ] && sig='{"claim_validation": true}'
        printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},\n "telemetry": {"enabled": true, "output_file": "tele.jsonl", "tamper_evidence": {"enabled": true}},\n "behavioral_sequences": {"enabled": true}, "advisory_escalation": {"enabled": false},\n "context_drift": {"enabled": true, "level": "advisory", "check_interval_turns": 1, "adaptive_baseline_enabled": false,\n   "reality_checkpoint": {"enabled": false}, "signals": %s},\n "agents": {"worker": {"provider": "gemini", "model": "stub-model", "api_base": "http://127.0.0.1:%s", "api_key_env": "FAKE_KEY_CL", "max_tokens": 100, "max_turns": 20, "retry": {"max_attempts": 1}}}}\n' \
            "$sig" "$STUB_PORT" > "$d/$f"
    done
    # The operator resolves paths from its own directory: absolute ones.
    printf 'use agent\nmain {\n    let h = agent.create("worker")\n%s\n    print("RUN_DONE")\n}\n' "${SWAP_PROG_BODY//RUNDIR/$d}" > "$d/p.naab"
    start_swap_operator "$d"
    (cd "$d" && timeout 120s "$NAAB" p.naab > out.txt 2>"$d/err.txt")
    stop_swap_operators
    stop_stub; STUB_STARTED=0
}
swap_run "$TEST_TMP/cl13" '{}'
swap_run "$TEST_TMP/cl13c" '{"claim_validation": true}'
T13="$(tq "$TEST_TMP/cl13/tele.jsonl" 'print([e.get("penalties_detail", "") for e in of("CDD_TURN", analyzed="true")], len([e for e in of("CONFIG_ADJUSTMENT") if e.get("accepted") == "true"]))')"
T13C="$(tq "$TEST_TMP/cl13c/tele.jsonl" 'print([e.get("penalties_detail", "") for e in of("CDD_TURN", analyzed="true")])')"
if [[ "$T13C" == *"claim_validation=0.1000"* ]] && grep -q RUN_DONE "$TEST_TMP/cl13c/out.txt"; then
    pass "CL-13c" "S24 on from the start charges the contradiction on the next turn"
else
    fail "CL-13c" "the control did not charge -- CL-13 measures nothing" "$T13C $(tail -2 "$TEST_TMP/cl13c/err.txt")"
fi
if grep -q RUN_DONE "$TEST_TMP/cl13/out.txt" && [[ "$T13" == *"] 1" ]] && [[ "$T13" != *claim_validation* ]]; then
    pass "CL-13" "the reload turned S24 on, and the contradiction consumed while it was off charged nothing"
else
    fail "CL-13" "a contradiction consumed while S24 was off charged after the reload, or the reload did not land" "$T13 $(tail -2 "$TEST_TMP/cl13/err.txt")"
fi

# ------------------------------------------------------------------
# CL-IO -- the readers do not depend on the platform's streams
# ------------------------------------------------------------------
# CL-00's two readers, CL-10's and one CL-11 edit again, over the same files,
# with stdin decoded as cp1252 and "\n" written as "\r\n" -- what a native
# Windows python does under MSYS2. reconfigure, not a new wrapper: a replaced
# wrapper that is collected closes the stream under it. tq reads its program
# from stdin, so it gets the stdout half only.
WIN_OUT='import sys
sys.stdout.reconfigure(newline="\r\n")
'
WIN_PRE='import sys
sys.stdin.reconfigure(encoding="cp1252", errors="replace")
sys.stdout.reconfigure(newline="\r\n")
'
EXPR0='print(" ".join("%s%s%s@%s" % (e["kind"], "+" if e["positive"] == "true" else "-", "!" if e["negated"] == "true" else "", e["span"]) for e in of("AGENT_CLAIM")))'
plain_io() {
    printf '%s|%s|%s|%s' "$(tq "$TEST_TMP/cl00/tele.jsonl" "$EXPR0")" "$(py "$PY_WANT0" < "$TEST_TMP/cl00/claims.json")" \
        "$(py "$PY_R10" < "$RJ" 2>/dev/null)" "$(edit_copy io_positive '    if e.get("claim_id") == "1.0.0": e["positive"] = "false"')"
}
IO_PLAIN="$(plain_io)"
PY_PRE="$WIN_PRE"; TQ_PRE="$WIN_OUT"
IO_SHAPED="$(plain_io)"
PY_PRE=""; TQ_PRE=""
if [ "$IO_PLAIN" != "$GOT0|$WANT0|$R10|$E1" ] || [ -z "$GOT0" ] || [ -z "$R10" ]; then
    fail "CL-IO" "UNMEASURABLE: the plain readers did not repeat CL-00, CL-10 and CL-11, so there is nothing to compare" "$IO_PLAIN"
elif [ "$IO_SHAPED" = "$IO_PLAIN" ]; then
    pass "CL-IO" "the readers answer the same with Windows' streams injected (cp1252 stdin, CRLF stdout)"
else
    fail "CL-IO" "a reader depends on the platform's streams" "plain=[$IO_PLAIN] windows-shaped=[$IO_SHAPED]"
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
