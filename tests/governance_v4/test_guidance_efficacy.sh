#!/usr/bin/env bash
# ============================================================
# test_guidance_efficacy.sh -- did guidance help? Recorded, judged afterwards
# (Bundle 20 PR 4; docs/plan-live-guidance.md "Measuring whether it helped")
#
# The owner's decision of 2026-10-12: nothing at runtime decides on whether a
# piece of guidance helped. A run records each delivery; the NEXT run's start
# (and `naab-gov efficacy TELEMETRY`) judge, per delivery, the next three
# analysed drift turns of its handle: did the triggering signal pay again?
# A window where something else was done to the agent is undetermined; a
# holdout makes a second arm so the two can be compared.
#
#   GE-00   every event type the engine writes is classified for the window rule
#   GE-01   A199: after a tool loop the reply's trace is the send's own call
#   GE-02   the model that answered is on CDD_TURN and GUIDANCE_RESPONSE, through
#           a fallback (the stub's path routes answer per model)
#   GE-03   A200(a): recorded_sha256 is the hash of the text as recorded; a text
#           the writer sanitised says so, and replay reads it UNMEASURABLE
#   GE-04   A200(c): a candidate skipped for its cooldown is recorded
#   GE-05   the holdout: reproducible draws, held out below, delivered at or above
#   GE-06   the next run judges the first run's deliveries and names a section
#           whose trigger always came back; a run never judges its own
#   GE-07   the judgement changes no selection: the same section is delivered,
#           and no delivery is judged twice
#   GE-08   an intervention in the window makes it undetermined
#   GE-09   a delivery line whose hash does not recompute is not judged
#   GE-10   naab-gov efficacy: a report, its UNMEASURABLE readings, causation
#
# Each subject arm has its control in the same arm or the next (the "c"
# rows), run through the same harness, so a refusal-everything or a dead stub
# cannot pass an arm.
#
# KILL TABLE (each mutant applied alone to a clean tree, the suite re-run;
# the arms that turned red):
#   K1  eventClassTable() loses MANDATE_INJECTION                  -> GE-00, GE-08
#   K2  keepSendTrace() not called after the tool loop               -> GE-01
#   K3  CDD_TURN "model" = the first configured model                -> GE-02
#   K4  recorded_sha256 taken over the text before sanitising        -> GE-03
#   K5  the cooldown skip records nothing                            -> GE-04
#   K6  the holdout draw compared with > instead of <                -> GE-05
#   K7  the next run ignores from_offset (judges after the carry only) -> GE-07c
#   K8  the carry drops its guidance tallies                         -> GE-06 GE-07c
#   K9  intervenes() always false                                    -> GE-08
#   K10 the next run's judge reads lines whose hash does not recompute -> GE-09
#   K11 naab-gov efficacy exits 0 whatever it found                  -> GE-10c
#   K12 naab-gov efficacy reads lines whose hash does not recompute  -> GE-10c
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
NAAB_GOV="${NAAB_GOV:-$REPO/build/naab-gov}"
TEST_TMP="$(mktemp -d)"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
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
[ -x "$NAAB_GOV" ] || { echo "naab-gov not built: the efficacy and replay arms are UNMEASURABLE, not a pass"; exit 1; }

echo "=== guidance efficacy (Bundle 20 PR 4) ==="

# ------------------------------------------------------------------
# GE-00 -- every event type the engine writes has a class
# ------------------------------------------------------------------
# A type the table does not list is neither an intervention nor an
# observation, so a new event that changes what an agent is told would be
# judged as nothing. Read from the source (screened: string literals passed to
# writeAgentTelemetry), the same list CLAUDE.md tells a new event to update.
# Run from the repo root with relative names, and the table fed on stdin: under
# MSYS2 python3 is a native program that cannot open the shell's /tmp names.
classify_check() {  # $1 = the efficacy source; prints each written type the table lacks
    local code
    code="$(cat <<'PYEOF'
import os, re, sys
src = "src"
written = set()
for root, _, files in os.walk(src):
    for f in files:
        if f.endswith((".cpp", ".h", ".inc")):
            text = open(os.path.join(root, f), encoding="utf-8", errors="replace").read()
            written.update(re.findall(r'writeAgentTelemetry\(\s*"([A-Z_]+)"', text))
table = set(re.findall(r'\{"([A-Z_]+)", "(?:intervention|observation)"\}',
                       sys.stdin.buffer.read().decode("utf-8")))
for t in sorted(written - table):
    print(t)
print("WRITTEN %d" % len(written), file=sys.stderr)
PYEOF
)"
    (cd "$REPO" && python3 -I -c "$code" < "$1")
}
EFF_SRC="$REPO/src/runtime/guidance_efficacy.cpp"
MISSING="$(classify_check "$EFF_SRC" 2>"$TEST_TMP/ge00.err")"
NWRITTEN="$(sed -n 's/^WRITTEN //p' "$TEST_TMP/ge00.err")"
if [ -z "$MISSING" ] && [ "${NWRITTEN:-0}" -ge 40 ]; then
    pass "GE-00" "all $NWRITTEN event types the engine writes are classified intervention or observation"
else
    fail "GE-00" "an event type the engine writes has no class" "unclassified: $MISSING (written: ${NWRITTEN:-?})"
fi
sed '/{"MANDATE_INJECTION", "intervention"}/d' "$EFF_SRC" > "$TEST_TMP/planted_efficacy.cpp"
if [ "$(classify_check "$TEST_TMP/planted_efficacy.cpp" 2>/dev/null)" = "MANDATE_INJECTION" ]; then
    pass "GE-00c" "the check names a type removed from a copy of the table"
else
    fail "GE-00c" "the check did not flag a type removed from the table -- GE-00 measures nothing"
fi

case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;; *) WINDOWS=0 ;; esac
[ -n "${WINDIR:-}" ] && WINDOWS=1
if [ "$WINDOWS" = 1 ]; then
    skip "GE-STUB" "stub-backed arms are not run on Windows (stub_platform.sh)"
    echo ""; echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
    [ "$FAIL_COUNT" -eq 0 ]; exit $?
fi
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
export FAKE_KEY_EFF="fake-key-efficacy"

# ------------------------------------------------------------------
# Helpers
# ------------------------------------------------------------------
# A Python query over a telemetry file: tq FILE EXPR, where EXPR sees `ev`
# (the list of events, file order) and prints what it computes.
tq() {
    python3 -I - "$1" "$2" <<'PYEOF'
import hashlib, json, sys
ev = []
for line in open(sys.argv[1], encoding="utf-8"):
    try:
        ev.append(json.loads(line))
    except ValueError:
        pass
def of(t, **kw):
    return [e for e in ev if e.get("event_type") == t and all(e.get(k) == v for k, v in kw.items())]
def sha(s):
    return hashlib.sha256(s.encode("utf-8")).hexdigest()
def draw(run, o):
    return int(sha("%s:%d" % (run, o))[:13], 16) / 16.0 ** 13
exec(sys.argv[2])
PYEOF
}

# A playbook whose "Response repetition" section the automatic routes deliver
# when that signal pays, with a marker the request bodies can be searched for.
playbook() {  # $1 dir, [$2 extra line]
    mkdir -p "$1/docs"
    printf '# Team playbook\n\n## Response repetition\nEFF_REPEAT_q7: never send the same answer twice; restate it with new detail.\n%s\n' "${2:-}" \
        > "$1/docs/playbook.md"
}

REPEAT_FIXTURE="$TEST_TMP/repeat.json"
cat > "$REPEAT_FIXTURE" <<'EOF'
{"responses": [{"content": "The billing total is computed from the invoice lines.", "input_tokens": 12, "output_tokens": 24}]}
EOF

CDD='"behavioral_sequences": {"enabled": true}, "advisory_escalation": {"enabled": false},
 "context_drift": {"enabled": true, "level": "advisory", "check_interval_turns": 1, "reality_checkpoint": {"enabled": false}},'

# erun DIR FIXTURE GUIDANCE_JSON EXTRA_TOP PROGRAM_BODY [AGENT_EXTRA]
# One run in DIR (telemetry and history kept there across runs).
erun() {
    local d="$1" fx="$2" g="$3" top="$4" body="$5" ax="${6:-}"
    mkdir -p "$d/stub"
    start_stub "$fx" "$d/stub" >/dev/null 2>&1 || { echo "STUB_FAIL"; return; }
    STUB_STARTED=1
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},\n "telemetry": {"enabled": true, "output_file": "tele.jsonl", "tamper_evidence": {"enabled": true}},\n %s %s\n "agents": {"worker": {"provider": "gemini", "model": "stub-model", "api_base": "http://127.0.0.1:%s", "api_key_env": "FAKE_KEY_EFF", "max_tokens": 200, "max_turns": 60, "system_prompt": "You maintain the billing service and answer questions about invoices."%s}},\n "guidance": %s}\n' \
        "$CDD" "$top" "$STUB_PORT" "$ax" "$g" > "$d/govern.json"
    printf 'use agent\nmain {\n    let h = agent.create("worker")\n%s\n    print("RUN_DONE")\n}\n' "$body" > "$d/p.naab"
    local out; out="$( (cd "$d" && timeout 120s "$NAAB" p.naab 2>"$d/err.txt") )"
    stop_stub; STUB_STARTED=0
    printf '%s' "$out"
}
SENDS() {  # $1 = number of sends
    printf '    let i = 0\n    while i < %s { i = i + 1\n        let r = agent.send(h, "Explain the invoice total.") }' "$1"
}
G_PLAIN='{"cooldown_turns": 3, "sources": ["docs/playbook.md"]}'

# ------------------------------------------------------------------
# GE-01 -- A199: the trace after a tool loop
# ------------------------------------------------------------------
# The send's own call is held 300 ms and asks for a tool; the loop's second
# call answers. The trace must be the send's call -- model, provider, key and
# its latency -- not the empty record of the loop's reply.
cat > "$TEST_TMP/tool.json" <<'EOF'
{"responses": [{"tool_calls": [{"name": "get_data", "args": {"query": "x"}}], "hold_ms": 300},
               {"content": "FINAL-ok", "input_tokens": 5, "output_tokens": 9}]}
EOF
cat > "$TEST_TMP/notool.json" <<'EOF'
{"responses": [{"content": "FINAL-ok", "input_tokens": 5, "output_tokens": 9, "hold_ms": 300}]}
EOF
TRACE_PROG='    agent.register_tool("get_data", get_data, {"description": "d", "parameters": {"query": {"type": "string", "description": "q"}}})
    let r = agent.send(h, "go")
    let tr = r.get("trace")
    print("TRACE model=[" + string(tr.get("model")) + "] provider=[" + string(tr.get("provider")) + "] key=[" + string(tr.get("api_key_env")) + "] latency=" + string(tr.get("latency_ms")) + " tools=" + string(r.get("tool_calls_made")))
    print("USAGE latency=" + string(agent.usage(h).get("total_latency_ms")))'
trace_run() {  # $1 dir, $2 fixture
    local d="$1"; mkdir -p "$d/stub"
    start_stub "$2" "$d/stub" >/dev/null 2>&1 || { echo "STUB_FAIL"; return; }
    STUB_STARTED=1
    printf '{"mode": "enforce", "security": {"sandbox_level": "elevated"},\n "agents": {"worker": {"provider": "gemini", "model": "stub-model-a", "api_base": "http://127.0.0.1:%s", "api_key_env": "FAKE_KEY_EFF", "max_tokens": 100, "max_turns": 20, "retry": {"max_attempts": 1}, "tools_enabled": true, "tools": ["get_data"]}}}\n' "$STUB_PORT" > "$d/govern.json"
    printf 'use agent\nfn get_data(query) { return "data for " + string(query) }\nmain {\n    let h = agent.create("worker")\n%s\n}\n' "$TRACE_PROG" > "$d/p.naab"
    (cd "$d" && timeout 60s "$NAAB" p.naab 2>"$d/err.txt")
    stop_stub; STUB_STARTED=0
}
trace_ok() {  # $1 output, $2 tools expected
    local lat ulat
    lat="$(sed -n 's/.*latency=\([0-9]*\).*tools=.*/\1/p' <<<"$1")"
    ulat="$(sed -n 's/^USAGE latency=\([0-9]*\).*/\1/p' <<<"$1")"
    [[ "$1" == *"model=[stub-model-a] provider=[gemini] key=[FAKE_KEY_EFF]"* ]] &&
    [[ "$1" == *"tools=$2"* ]] && [ "${lat:-0}" -ge 250 ] && [ "${ulat:-0}" -ge 250 ]
}
OUT1="$(trace_run "$TEST_TMP/ge01" "$TEST_TMP/tool.json")"
OUT1C="$(trace_run "$TEST_TMP/ge01c" "$TEST_TMP/notool.json")"
if trace_ok "$OUT1C" null; then
    pass "GE-01c" "without a tool loop the trace names the model, provider, key and the call's latency"
else
    fail "GE-01c" "the control's trace is not as expected -- GE-01 measures nothing" "$OUT1C / $(tail -3 "$TEST_TMP/ge01c/err.txt")"
fi
if trace_ok "$OUT1" 1; then
    pass "GE-01" "after a tool loop the trace is still the send's own call (model, provider, key, latency >= the 300 ms hold)"
else
    fail "GE-01" "the trace after a tool loop lost the send's call" "$OUT1 / $(tail -3 "$TEST_TMP/ge01/err.txt")"
fi

# ------------------------------------------------------------------
# GE-02 -- the model that answered, through a fallback
# ------------------------------------------------------------------
cat > "$TEST_TMP/fallback.json" <<'EOF'
{"path_routes": {"models/stub-primary:": {"responses": [{"status": 503, "error": "down"}]},
                 "models/stub-backup:": {"responses": [{"content": "The billing total is computed from the invoice lines.", "input_tokens": 12, "output_tokens": 24}]}}}
EOF
cat > "$TEST_TMP/nofallback.json" <<'EOF'
{"path_routes": {"models/stub-primary:": {"responses": [{"content": "The billing total is computed from the invoice lines.", "input_tokens": 12, "output_tokens": 24}]}}}
EOF
MODEL_AX=', "retry": {"max_attempts": 2, "backoff_ms": 10}'
model_run() {  # $1 dir, $2 fixture
    playbook "$1"
    erun "$1" "$2" "$G_PLAIN" "" "$(SENDS 3)" "$MODEL_AX" >/dev/null
    sed -i 's/"model": "stub-model"/"model": ["stub-primary", "stub-backup"]/' "$1/govern.json"
    # erun wrote the config with one model; run again with the list.
    rm -f "$1/tele.jsonl"; rm -rf "$1/.naab_cache"
    mkdir -p "$1/stub"
    start_stub "$2" "$1/stub" >/dev/null 2>&1 || { echo "STUB_FAIL"; return; }
    STUB_STARTED=1
    sed -i "s/127.0.0.1:[0-9]*/127.0.0.1:$STUB_PORT/" "$1/govern.json"
    (cd "$1" && timeout 120s "$NAAB" p.naab 2>"$1/err.txt")
    stop_stub; STUB_STARTED=0
}
models_of() {  # $1 dir: the CDD_TURN models, then the GUIDANCE_RESPONSE models
    tq "$1/tele.jsonl" 'print(",".join(sorted({e.get("model","") for e in of("CDD_TURN")})) + " " + ",".join(sorted({e.get("model","") for e in of("GUIDANCE_RESPONSE")})) + " fb=%d" % len(of("AGENT_FALLBACK")))'
}
out="$(model_run "$TEST_TMP/ge02" "$TEST_TMP/fallback.json")"
outc="$(model_run "$TEST_TMP/ge02c" "$TEST_TMP/nofallback.json")"
M2="$(models_of "$TEST_TMP/ge02")"; M2C="$(models_of "$TEST_TMP/ge02c")"
if [ "$M2C" = "stub-primary stub-primary fb=0" ]; then
    pass "GE-02c" "without a fallback every scored turn and guidance reply records the primary model"
else
    fail "GE-02c" "the control's models are not the primary's -- GE-02 measures nothing" "$M2C / $outc"
fi
if [ "$M2" = "stub-backup stub-backup fb=3" ]; then
    pass "GE-02" "after a fallback CDD_TURN and GUIDANCE_RESPONSE record the model that answered (stub-backup)"
else
    fail "GE-02" "the recorded model is not the one that answered" "$M2 / $out"
fi

# ------------------------------------------------------------------
# GE-03 -- A200(a): the hash of the text as recorded
# ------------------------------------------------------------------
# The telemetry writer removes absolute path prefixes from every field, so a
# section naming /home/... is recorded differently from what was delivered.
d="$TEST_TMP/ge03"; playbook "$d" "See /home/ops/runbook.md for the escalation contact."
out="$(erun "$d" "$REPEAT_FIXTURE" "$G_PLAIN" "" "$(SENDS 3)")"
dc="$TEST_TMP/ge03c"; playbook "$dc"
outc="$(erun "$dc" "$REPEAT_FIXTURE" "$G_PLAIN" "" "$(SENDS 3)")"
H3='d = of("GUIDANCE_DELIVERED")
print("n=%d rec=%d same=%d" % (len(d), sum(sha(e.get("text","")) == e.get("recorded_sha256") for e in d), sum(e.get("sha256") == e.get("recorded_sha256") for e in d)))'
R3="$(tq "$d/tele.jsonl" "$H3")"; R3C="$(tq "$dc/tele.jsonl" "$H3")"
RP3="$("$NAAB_GOV" replay --json "$d/tele.jsonl" | python3 -I -c 'import json,sys; c=json.load(sys.stdin)["checks"]["guidance"]; print("%d/%d/%d" % (c["match"], c["diverged"], c["unmeasurable"]))')"
RP3C="$("$NAAB_GOV" replay --json "$dc/tele.jsonl" | python3 -I -c 'import json,sys; c=json.load(sys.stdin)["checks"]["guidance"]; print("%d/%d/%d" % (c["match"], c["diverged"], c["unmeasurable"]))')"
if [ "$R3C" = "n=1 rec=1 same=1" ] && [ "${RP3C#*/}" = "0/0" ]; then
    pass "GE-03c" "a delivery whose text the writer leaves alone: both hashes agree and replay reads it MATCH ($RP3C)"
else
    fail "GE-03c" "the control delivery does not hash as expected" "$R3C replay=$RP3C / $outc"
fi
if [ "$R3" = "n=1 rec=1 same=0" ] && [ "$(cut -d/ -f2 <<<"$RP3")" = 0 ] && [ "$(cut -d/ -f3 <<<"$RP3")" = 1 ]; then
    pass "GE-03" "a text the writer sanitised: recorded_sha256 is the recorded text's hash, sha256 the delivered one's, and replay says what was delivered is not in the file"
else
    fail "GE-03" "the recorded hash does not describe the recorded text" "$R3 replay(m/d/u)=$RP3 / $out"
fi

# ------------------------------------------------------------------
# GE-04 -- A200(c): a cooldown is recorded
# ------------------------------------------------------------------
d="$TEST_TMP/ge04"; playbook "$d"
out="$(erun "$d" "$REPEAT_FIXTURE" "$G_PLAIN" "" "$(SENDS 6)")"
dc="$TEST_TMP/ge04c"; playbook "$dc"
outc="$(erun "$dc" "$REPEAT_FIXTURE" '{"cooldown_turns": 0, "sources": ["docs/playbook.md"]}' "" "$(SENDS 6)")"
H4='print("delivered=%d cooldown=%d" % (len(of("GUIDANCE_DELIVERED")), len(of("GUIDANCE_WITHHELD", reason="cooldown"))))'
R4="$(tq "$d/tele.jsonl" "$H4")"; R4C="$(tq "$dc/tele.jsonl" "$H4")"
# Six sends: the signal pays on the replies to requests 1-4, each answered
# with the next request (2-5), so four candidates.
N4C="$(sed -n 's/^delivered=\([0-9]*\) cooldown=0$/\1/p' <<<"$R4C")"
if [ "${N4C:-0}" -eq 4 ]; then
    pass "GE-04c" "with no cooldown the paid signal is answered every turn and no cooldown is recorded ($R4C)"
else
    fail "GE-04c" "the control did not deliver on every paid turn" "$R4C / $outc"
fi
COOL_LISTED=0; grep -q 'cooldown' "$d/err.txt" && COOL_LISTED=1
if [[ "$R4" =~ ^delivered=([0-9]+)\ cooldown=([0-9]+)$ ]] && [ "${BASH_REMATCH[2]}" -ge 1 ] \
   && [ $((BASH_REMATCH[1] + BASH_REMATCH[2])) -eq "${N4C:-0}" ] && [ "$COOL_LISTED" = 0 ]; then
    pass "GE-04" "every paid turn is delivered or recorded as cooling down ($R4, against $N4C candidates), and the end-of-run block does not list a cooldown as withheld"
else
    fail "GE-04" "a cooldown skip was not recorded, or was listed as withheld" "$R4 listed=$COOL_LISTED"
fi

# ------------------------------------------------------------------
# GE-05 -- the holdout
# ------------------------------------------------------------------
H5='import math
dl = of("GUIDANCE_DELIVERED", channel="agent"); ho = of("GUIDANCE_WITHHELD", reason="holdout")
bad = 0; ords = []
for e, held in [(x, False) for x in dl] + [(x, True) for x in ho]:
    if "holdout_ordinal" not in e: continue
    o = int(e["holdout_ordinal"]); ords.append(o)
    dr = draw(e["run_id"], o)
    if abs(dr - float(e["holdout_draw"])) > 6e-7: bad += 1
    if (dr < float(e["holdout"])) != held: bad += 1
print("delivered=%d held=%d drawn=%d bad=%d contiguous=%s" % (len(dl), len(ho), len(ords), bad, sorted(ords) == list(range(len(ords)))))'
d="$TEST_TMP/ge05"; playbook "$d"
out="$(erun "$d" "$REPEAT_FIXTURE" '{"cooldown_turns": 0, "holdout": 0.5, "sources": ["docs/playbook.md"]}' "" "$(SENDS 20)")"
dc="$TEST_TMP/ge05c"; playbook "$dc"
outc="$(erun "$dc" "$REPEAT_FIXTURE" '{"cooldown_turns": 0, "sources": ["docs/playbook.md"]}' "" "$(SENDS 20)")"
R5="$(tq "$d/tele.jsonl" "$H5")"; R5C="$(tq "$dc/tele.jsonl" "$H5")"
RP5="$("$NAAB_GOV" replay --json "$d/tele.jsonl" | python3 -I -c 'import json,sys; c=json.load(sys.stdin)["checks"]["guidance"]; print("%d/%d/%d" % (c["match"], c["diverged"], c["unmeasurable"]))')"
# Twenty sends: eighteen candidates (requests 2-19), every one delivered.
# Eighteen, so that both arms are non-empty but for about 1 run in 130,000
# (2 in 2^18 at holdout 0.5; with ten it was 2 in 1,024).
N5C="$(sed -n 's/^delivered=\([0-9]*\) held=0 drawn=0 bad=0 contiguous=True$/\1/p' <<<"$R5C")"
if [ "${N5C:-0}" -eq 18 ]; then
    pass "GE-05c" "with holdout 0 nothing is drawn or held out ($R5C)"
else
    fail "GE-05c" "a run without a holdout drew or held out" "$R5C"
fi
H5D="$(sed -n 's/.*delivered=\([0-9]*\) held=\([0-9]*\) drawn=\([0-9]*\).*/\1 \2 \3/p' <<<"$R5")"
read -r n5d n5h n5n <<<"${H5D:-0 0 0}"
if [[ "$R5" == *"bad=0 contiguous=True" ]] && [ "$n5d" -ge 1 ] && [ "$n5h" -ge 1 ] && [ $((n5d + n5h)) -eq "${N5C:-0}" ] && [ "$n5n" -eq "${N5C:-0}" ] \
   && [ "$(cut -d/ -f2-3 <<<"$RP5")" = "0/0" ]; then
    pass "GE-05" "holdout 0.5: every candidate drawn ($R5), each draw recomputes outside the engine and falls on its arm's side; replay agrees ($RP5)"
else
    fail "GE-05" "the holdout arm is not what the draws say" "$R5 replay(m/d/u)=$RP5 / $out"
fi

# ------------------------------------------------------------------
# GE-06 / GE-07 -- judged by the next run, carried, named; selection unchanged
# ------------------------------------------------------------------
# Every reply repeats, so response_repetition pays on every turn after a
# delivery: each judged window is "recurred".
d="$TEST_TMP/ge06"; playbook "$d"
out1="$(erun "$d" "$REPEAT_FIXTURE" "$G_PLAIN" "" "$(SENDS 10)")"; cp "$d/err.txt" "$d/err1.txt"
out2="$(erun "$d" "$REPEAT_FIXTURE" "$G_PLAIN" "" "$(SENDS 10)")"; cp "$d/err.txt" "$d/err2.txt"
out3="$(erun "$d" "$REPEAT_FIXTURE" "$G_PLAIN" "" "$(SENDS 10)")"; cp "$d/err.txt" "$d/err3.txt"
dc="$TEST_TMP/ge06c"; playbook "$dc"
outc="$(erun "$dc" "$REPEAT_FIXTURE" "$G_PLAIN" "" "$(SENDS 10)")"
H6='runs = []
for e in ev:
    if e.get("run_id") not in runs: runs.append(e.get("run_id"))
for r in runs:
    hl = [e for e in of("HISTORY_LOADED") if e["run_id"] == r]
    ca = [e for e in of("HISTORY_CARRY") if e["run_id"] == r]
    g = ca[-1].get("guidance") if ca else None
    rec = sum(t.get("recurred", 0) for t in g) if isinstance(g, list) else -1
    print("judged=%s carried_recurred=%d delivered=%d" % (hl[-1].get("guidance_judged", "0") if hl else "-", rec, len([e for e in of("GUIDANCE_DELIVERED") if e["run_id"] == r])))'
R6="$(tq "$d/tele.jsonl" "$H6" | tr '\n' ';')"
LINE='earlier runs: .*Response repetition -- its trigger came back after all'
L1=0; grep -q "$LINE" "$d/err1.txt" && L1=1
L2=0; grep -q "$LINE" "$d/err2.txt" && L2=1
LC=0; grep -q "$LINE" "$dc/err.txt" && LC=1
if [[ "$out1" == *RUN_DONE* && "$outc" == *RUN_DONE* ]] && [ "$L1" = 0 ] && [ "$LC" = 0 ] && [[ "$R6" == "judged=-"* ]]; then
    pass "GE-06d" "a run never judges its own deliveries: the first run, and the same run in a fresh directory, name no section and judge nothing"
else
    fail "GE-06d" "a run judged its own deliveries" "line run1=$L1 fresh=$LC; $R6"
fi
J2="$(cut -d';' -f2 <<<"$R6")"
if [[ "$out2" == *RUN_DONE* ]] && [[ "$J2" =~ ^judged=([0-9]+)\ carried_recurred=([0-9]+) ]] \
   && [ "${BASH_REMATCH[1]}" -ge 2 ] && [ "${BASH_REMATCH[2]}" -ge 2 ] && [ "$L2" = 1 ]; then
    pass "GE-06" "the second run judged the first run's deliveries ($J2), carried the section's totals, and named the section whose trigger always came back"
else
    fail "GE-06" "the next run did not judge, carry or name" "$R6 line=$L2 / $(tail -4 "$d/err2.txt")"
fi
D3="$(cut -d';' -f3 <<<"$R6")"
D1="$(cut -d';' -f1 <<<"$R6")"
if [[ "$out3" == *RUN_DONE* ]] && [ "${D3##*delivered=}" = "${D1##*delivered=}" ] && [ "${D3##*delivered=}" -ge 1 ] \
   && grep -q EFF_REPEAT_q7 "$d/stub/req_3.json" 2>/dev/null; then
    pass "GE-07" "after the section was named as never resolving, the third run delivered it exactly as the first did (${D3##*delivered=} deliveries): nothing at runtime decides on outcomes"
else
    fail "GE-07" "a judged outcome changed what was delivered" "run1 ${D1##*delivered=} run3 ${D3##*delivered=}"
fi
# The third run judges the second run's deliveries only: the first run's are in
# the carried totals already, so judging them again would count them twice.
J2N="$(sed -n 's/^judged=\([0-9]*\) .*/\1/p' <<<"$J2")"
if [[ "$D3" =~ ^judged=([0-9]+)\ carried_recurred=([0-9]+) ]] && [ -n "$J2N" ] \
   && [ "${BASH_REMATCH[1]}" -eq "$J2N" ] && [ "${BASH_REMATCH[2]}" -eq $((2 * J2N)) ]; then
    pass "GE-07c" "the third run judged only the second run's deliveries and carried both runs' totals once ($D3)"
else
    fail "GE-07c" "a delivery was judged twice, or the totals were not carried" "run2 $J2 run3 $D3"
fi

# ------------------------------------------------------------------
# GE-08 -- an intervention in the window
# ------------------------------------------------------------------
# A mandate reminder on every turn lands inside every window, so no window
# can be read as the guidance's effect.
d="$TEST_TMP/ge08"; playbook "$d"
out="$(erun "$d" "$REPEAT_FIXTURE" "$G_PLAIN" '"circuit_breaker": {"mandate_reinforcement_enabled": true, "mandate_reinforcement_interval": 1},' "$(SENDS 10)")"
EFF8="$("$NAAB_GOV" efficacy --json "$d/tele.jsonl")"
EFF6="$("$NAAB_GOV" efficacy --json "$TEST_TMP/ge06c/tele.jsonl")"
sum_of() { python3 -I -c 'import json,sys; d=json.load(sys.stdin); s=d["sections"]; print("%d/%d/%d" % (sum(x["delivered"]["undetermined"] for x in s), sum(x["delivered"]["recurred"] for x in s), sum(x["delivered"]["total"] for x in s)))' <<<"$1"; }
S8="$(sum_of "$EFF8")"; S6="$(sum_of "$EFF6")"
MI="$(tq "$d/tele.jsonl" 'print(len(of("MANDATE_INJECTION")))')"
if [ "$(cut -d/ -f1 <<<"$S6")" = 0 ] && [ "$(cut -d/ -f2 <<<"$S6")" -ge 1 ]; then
    pass "GE-08c" "without a reminder the windows are judged: recurred $(cut -d/ -f2 <<<"$S6") of $(cut -d/ -f3 <<<"$S6")"
else
    fail "GE-08c" "the control windows were not judged recurred -- GE-08 measures nothing" "$S6"
fi
if [ "${MI:-0}" -ge 5 ] && [ "$(cut -d/ -f1 <<<"$S8")" = "$(cut -d/ -f3 <<<"$S8")" ] && [ "$(cut -d/ -f3 <<<"$S8")" -ge 1 ]; then
    pass "GE-08" "with a reminder in every window ($MI injections) every delivery is undetermined ($S8 undetermined/recurred/total)"
else
    fail "GE-08" "a window with an intervention was judged" "undetermined/recurred/total=$S8 injections=$MI"
fi

# ------------------------------------------------------------------
# GE-09 -- a line whose hash does not recompute is not judged
# ------------------------------------------------------------------
# One character of the first delivery's text is changed in place (same
# length, so the anchor's offsets still hold); the line is not re-sealed.
d="$TEST_TMP/ge09"; playbook "$d"
erun "$d" "$REPEAT_FIXTURE" "$G_PLAIN" "" "$(SENDS 10)" >/dev/null
python3 -I - "$d/tele.jsonl" <<'PYEOF'
import sys
p = sys.argv[1]
lines = open(p, "rb").read().split(b"\n")
for i, l in enumerate(lines):
    if b'"GUIDANCE_DELIVERED"' in l and b"EFF_REPEAT_q7" in l:
        lines[i] = l.replace(b"EFF_REPEAT_q7", b"EFF_REPEAT_q8", 1)
        break
open(p, "wb").write(b"\n".join(lines))
PYEOF
out="$(erun "$d" "$REPEAT_FIXTURE" "$G_PLAIN" "" "$(SENDS 10)")"
R9="$(tq "$d/tele.jsonl" 'hl = of("HISTORY_LOADED"); print(hl[-1].get("guidance_judged", "0") if hl else "-")')"
JC="$(cut -d';' -f2 <<<"$R6" | sed -n 's/^judged=\([0-9]*\).*/\1/p')"
if [[ "$out" == *RUN_DONE* ]] && [ -n "$JC" ] && [ "$R9" != "-" ] && [ "$R9" -eq $((JC - 1)) ]; then
    pass "GE-09" "the edited delivery is not judged: $R9 judged against $JC for the same run unedited (GE-06)"
else
    fail "GE-09" "an edited delivery line was judged, or the run did not read history" "judged=$R9 unedited=$JC / $(tail -3 "$d/err.txt")"
fi

# ------------------------------------------------------------------
# GE-10 -- naab-gov efficacy
# ------------------------------------------------------------------
"$NAAB_GOV" efficacy "$TEST_TMP/ge06/tele.jsonl" > "$TEST_TMP/eff6.txt" 2>&1; rc6=$?
"$NAAB_GOV" efficacy "$TEST_TMP/ge05/tele.jsonl" > "$TEST_TMP/eff5.txt" 2>&1; rc5=$?
printf '{"event_type": "RunStart"}\n' > "$TEST_TMP/empty.jsonl"
"$NAAB_GOV" efficacy "$TEST_TMP/empty.jsonl" > "$TEST_TMP/effe.txt" 2>&1; rce=$?
"$NAAB_GOV" efficacy --hmac-key wrong-key "$TEST_TMP/ge06/tele.jsonl" > "$TEST_TMP/effk.txt" 2>&1; rck=$?
"$NAAB_GOV" efficacy --bogus "$TEST_TMP/ge06/tele.jsonl" > /dev/null 2>&1; rcu=$?
if [ "$rc6" = 0 ] && grep -q 'Causation: UNMEASURABLE' "$TEST_TMP/eff6.txt" && grep -q 'Response repetition' "$TEST_TMP/eff6.txt" \
   && [ "$rc5" = 0 ] && grep -q 'Causation: arms recorded' "$TEST_TMP/eff5.txt" && grep -q 'held out [0-9]' "$TEST_TMP/eff5.txt"; then
    pass "GE-10" "a report (exit 0): causation UNMEASURABLE without a holdout, both arms counted with one"
else
    fail "GE-10" "the report is not as expected" "rc=$rc6/$rc5 $(head -c 400 "$TEST_TMP/eff5.txt")"
fi
if [ "$rce" = 2 ] && [ "$rck" = 2 ] && grep -q 'UNMEASURABLE' "$TEST_TMP/effk.txt" && [ "$rcu" = 4 ]; then
    pass "GE-10c" "nothing to judge and a key the chain was not sealed with read UNMEASURABLE (exit 2), never a report; a bad option is usage (4)"
else
    fail "GE-10c" "an unmeasurable file read as a report" "empty=$rce wrong-key=$rck usage=$rcu"
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
